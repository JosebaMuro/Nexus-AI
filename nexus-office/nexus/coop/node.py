"""Nodo cooperativo: LAN virtual en malla entre instancias de Nexus-Office a traves de internet.

* Transporte: WebSocket (puerto propio, separado del webui que sigue en localhost).
* Topologia: malla. Un nodo tras NAT solo necesita conectar hacia FUERA a cualquier nodo
  accesible (con puerto abierto, Tailscale/ZeroTier, tunel...). Los mensajes se reenvian por
  inundacion con TTL y deduplicacion, asi todos los nodos se ven aunque no haya enlace directo.
* Direccionamiento: cada nodo tiene una IP virtual 10.77.x.y estable derivada de su id.
* Servicios: presencia, chat de LAN, ping, inferencia LLM remota, agentes remotos, memoria
  compartida y propuestas de herramientas (siempre con aprobacion local).
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import queue
import threading
import time
import uuid
from collections import OrderedDict, deque

import psutil

from .. import __version__, log, paths
from .crypto import Cipher, make_invite, read_invite

L = log.get("coop")
MAX_FRAME = 16 * 1024 * 1024


def node_identity() -> dict:
    f = paths.data_dir() / "coop_node.json"
    if f.exists():
        return json.loads(f.read_text())
    ident = {"node_id": uuid.uuid4().hex[:16]}
    f.write_text(json.dumps(ident))
    return ident


def virtual_ip(network_id: str, node_id: str) -> str:
    h = hashlib.sha256(f"{network_id}:{node_id}".encode()).digest()
    return f"10.77.{h[0]}.{1 + h[1] % 254}"


def overlay_addresses() -> list[dict]:
    """Direcciones locales utiles para que otros conecten (incluye Tailscale/ZeroTier si existen)."""
    out = []
    for iface, addrs in psutil.net_if_addrs().items():
        for a in addrs:
            if getattr(a.family, "name", "") != "AF_INET" or a.address.startswith("127."):
                continue
            kind = "lan"
            if a.address.startswith("100.") and 64 <= int(a.address.split(".")[1]) <= 127:
                kind = "tailscale"
            elif "zerotier" in iface.lower() or iface.lower().startswith("zt"):
                kind = "zerotier"
            elif not a.address.startswith(("10.", "192.168.", "172.", "169.254.")):
                kind = "publica"
            out.append({"iface": iface, "ip": a.address, "kind": kind})
    return out


class CoopNode:
    def __init__(self, services) -> None:
        self.s = services
        self.cfg = services.config
        self.ident = node_identity()
        self.node_id = self.ident["node_id"]
        self.loop: asyncio.AbstractEventLoop | None = None
        self.thread: threading.Thread | None = None
        self.cipher: Cipher | None = None
        self.links: dict[str, object] = {}          # node_id vecino -> websocket
        self.link_urls: dict[str, str] = {}         # url saliente -> estado
        self.peers: dict[str, dict] = {}            # node_id -> info de presencia
        self.seen: OrderedDict[str, float] = OrderedDict()
        self.pending: dict[str, asyncio.Queue] = {}
        self.chat: deque = deque(maxlen=200)
        self.listeners: list = []                   # callbacks(evento) hacia la UI
        self.running = False
        self._server = None
        self._stop: asyncio.Event | None = None

    # ------------------------------------------------------------ configuracion
    @property
    def network_id(self) -> str:
        return self.cfg.get("coop.network_id", "")

    @property
    def name(self) -> str:
        return self.cfg.get("coop.node_name") or self.cfg.get("user.assistant_name", "Nexus-Office") + "-" + self.node_id[:4]

    @property
    def vip(self) -> str:
        return virtual_ip(self.network_id, self.node_id) if self.network_id else ""

    def create_network(self, name: str = "") -> dict:
        net = name or "red-" + uuid.uuid4().hex[:6]
        self.cfg.set("coop.network_id", net, save=False)
        self.cfg.set("coop.passphrase", uuid.uuid4().hex + uuid.uuid4().hex)
        return {"network_id": net}

    def invite(self, public_urls: list[str] | None = None) -> str:
        urls = public_urls or self.cfg.get("coop.public_urls", []) or []
        return make_invite(self.network_id, self.cfg.get("coop.passphrase"), urls)

    def join(self, code: str) -> dict:
        data = read_invite(code)
        self.cfg.set("coop.network_id", data["net"], save=False)
        self.cfg.set("coop.passphrase", data["key"], save=False)
        peers = list(dict.fromkeys((self.cfg.get("coop.peers") or []) + data.get("peers", [])))
        self.cfg.set("coop.peers", peers)
        return {"network_id": data["net"], "peers": peers}

    # ------------------------------------------------------------ ciclo de vida
    def start(self) -> None:
        if self.running:
            return
        if not (self.network_id and self.cfg.get("coop.passphrase")):
            raise RuntimeError("Crea una red o unete con un codigo de invitacion primero")
        self.cipher = Cipher(self.network_id, self.cfg.get("coop.passphrase"))
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._thread_main, daemon=True, name="nexus-coop")
        self.running = True
        self.thread.start()

    def stop(self) -> None:
        if self.running and self.loop:
            self.loop.call_soon_threadsafe(self._stop.set)
            self.thread.join(5)
        self.running = False
        self.links.clear()
        self.peers.clear()

    def _thread_main(self) -> None:
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self._main())
        except Exception as e:  # noqa: BLE001
            L.error("Nodo cooperativo detenido: %s", e)
        finally:
            self.running = False

    async def _main(self) -> None:
        from websockets.asyncio.server import serve
        self._stop = asyncio.Event()
        tasks = []
        if self.cfg.get("coop.listen", False):
            host, port = self.cfg.get("coop.listen_host", "0.0.0.0"), int(self.cfg.get("coop.listen_port", 8766))
            self._server = await serve(self._inbound, host, port, max_size=MAX_FRAME, ping_interval=20)
            L.info("LAN virtual escuchando en %s:%s (IP virtual %s)", host, port, self.vip)
        for url in self.cfg.get("coop.peers", []) or []:
            tasks.append(asyncio.create_task(self._outbound(url)))
        tasks.append(asyncio.create_task(self._announce_loop()))
        await self._stop.wait()
        for t in tasks:
            t.cancel()
        for ws in list(self.links.values()):
            await ws.close()
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        await asyncio.gather(*tasks, return_exceptions=True)

    # ------------------------------------------------------------ enlaces
    async def _inbound(self, ws) -> None:
        await self._serve_link(ws, inbound=True)

    async def _outbound(self, url: str) -> None:
        from websockets.asyncio.client import connect
        delay = 2
        while True:
            try:
                self.link_urls[url] = "conectando"
                async with connect(url, max_size=MAX_FRAME, open_timeout=10, ping_interval=20) as ws:
                    self.link_urls[url] = "conectado"
                    delay = 2
                    await self._serve_link(ws, inbound=False)
            except asyncio.CancelledError:
                return
            except Exception as e:  # noqa: BLE001
                self.link_urls[url] = f"error: {str(e)[:80]}"
            await asyncio.sleep(delay)
            delay = min(60, delay * 2)

    async def _serve_link(self, ws, inbound: bool) -> None:
        neighbor = None
        await ws.send(self.cipher.seal(self._envelope("hello", self._presence(), dst="*", ttl=1)))
        try:
            async for frame in ws:
                try:
                    msg = self.cipher.open(frame)
                except Exception:  # noqa: BLE001
                    L.warning("Trama no autenticada descartada (%s)", "entrante" if inbound else "saliente")
                    await ws.close(code=1008, reason="auth")
                    return
                if msg["type"] == "hello" and not neighbor:
                    neighbor = msg["src"]
                    self.links[neighbor] = ws
                    self._update_peer(msg, hops=1, via=neighbor)
                    self._notify({"type": "peer_join", "peer": self.peers.get(neighbor)})
                    continue
                await self._receive(msg, ws)
        finally:
            if neighbor and self.links.get(neighbor) is ws:
                self.links.pop(neighbor, None)
                if neighbor in self.peers:
                    self.peers[neighbor]["direct"] = False
                self._notify({"type": "peer_leave", "node": neighbor})

    # ------------------------------------------------------------ mensajeria
    def _envelope(self, mtype: str, body: dict, dst: str = "*", ttl: int | None = None, rid: str = "") -> dict:
        return {"id": uuid.uuid4().hex, "src": self.node_id, "dst": dst, "ttl": ttl or int(self.cfg.get("coop.max_hops", 6)),
                "ts": time.time(), "type": mtype, "body": body, "rid": rid}

    async def _send(self, msg: dict, exclude=None) -> None:
        self._mark_seen(msg["id"])
        frame = self.cipher.seal(msg)
        targets = [self.links[msg["dst"]]] if msg["dst"] in self.links else list(self.links.values())
        for ws in targets:
            if ws is exclude:
                continue
            try:
                await ws.send(frame)
            except Exception:  # noqa: BLE001
                pass

    def _mark_seen(self, mid: str) -> bool:
        if mid in self.seen:
            return False
        self.seen[mid] = time.time()
        while len(self.seen) > 5000:
            self.seen.popitem(last=False)
        return True

    async def _receive(self, msg: dict, ws) -> None:
        if abs(time.time() - msg.get("ts", 0)) > 300 or not self._mark_seen(msg["id"]):
            return  # repetido o fuera de ventana temporal (anti-replay)
        if msg["dst"] != self.node_id and msg["ttl"] > 1:  # reenvio en la malla
            fwd = {**msg, "ttl": msg["ttl"] - 1}
            frame = self.cipher.seal(fwd)
            targets = [self.links[msg["dst"]]] if msg["dst"] in self.links else [w for w in self.links.values() if w is not ws]
            for w in targets:
                try:
                    await w.send(frame)
                except Exception:  # noqa: BLE001
                    pass
        if msg["dst"] not in ("*", self.node_id):
            return
        handler = getattr(self, f"_on_{msg['type'].replace('.', '_')}", None)
        if handler:
            try:
                await handler(msg)
            except Exception as e:  # noqa: BLE001
                L.warning("Error procesando %s: %s", msg["type"], e)
                if msg.get("rid") and msg["type"].startswith("req"):
                    await self._send(self._envelope("res", {"error": str(e), "final": True}, dst=msg["src"], rid=msg["rid"]))

    # ------------------------------------------------------------ presencia
    def _presence(self) -> dict:
        prof = self.s.profile
        llm = self.s.llm.status()
        return {"name": self.name, "vip": self.vip, "version": __version__,
                "hardware": {"tier": prof["tier"], "accelerator": prof["accelerator"], "device": prof["device_name"],
                             "ram_gb": prof["ram_gb"], "vram_gb": prof["vram_gb"]},
                "llm": {"backend": llm["backend"], "model": llm["model"]},
                "agents": [a.id for a in self.s.agents.list()],
                "offers": {"inference": self.cfg.get("coop.allow_remote_inference", True),
                           "agents": self.cfg.get("coop.allow_remote_agents", False),
                           "memory": self.cfg.get("coop.share_memory", False),
                           "tools": self.cfg.get("coop.share_tools", True)}}

    def _update_peer(self, msg: dict, hops: int, via: str) -> None:
        info = msg["body"]
        self.peers[msg["src"]] = {**info, "node_id": msg["src"], "last_seen": time.time(), "hops": hops,
                                  "via": via, "direct": msg["src"] in self.links}

    async def _announce_loop(self) -> None:
        while True:
            await self._send(self._envelope("announce", self._presence()))
            for nid, p in list(self.peers.items()):
                if time.time() - p["last_seen"] > 90:
                    self.peers.pop(nid, None)
                    self._notify({"type": "peer_leave", "node": nid})
            await asyncio.sleep(15)

    async def _on_announce(self, msg: dict) -> None:
        hops = int(self.cfg.get("coop.max_hops", 6)) - msg["ttl"] + 1
        new = msg["src"] not in self.peers
        self._update_peer(msg, hops=hops, via="")
        if new:
            self._notify({"type": "peer_join", "peer": self.peers[msg["src"]]})

    # ------------------------------------------------------------ chat y ping
    async def _on_chat(self, msg: dict) -> None:
        item = {"from": msg["body"].get("name"), "node": msg["src"], "text": msg["body"].get("text", "")[:2000], "ts": msg["ts"]}
        self.chat.append(item)
        self._notify({"type": "lan_chat", "message": item})

    async def _on_req_ping(self, msg: dict) -> None:
        await self._send(self._envelope("res", {"pong": True, "final": True, "name": self.name}, dst=msg["src"], rid=msg["rid"]))

    # ------------------------------------------------------------ servicios remotos
    async def _respond_stream(self, msg: dict, gen_factory) -> None:
        """Ejecuta un generador bloqueante en un hilo y devuelve los trozos al solicitante."""
        q: queue.Queue = queue.Queue()

        def work():
            try:
                for piece in gen_factory():
                    q.put(("chunk", piece))
                q.put(("end", None))
            except Exception as e:  # noqa: BLE001
                q.put(("error", str(e)))

        threading.Thread(target=work, daemon=True).start()
        buf = ""
        last = time.time()
        while True:
            kind, val = await asyncio.get_running_loop().run_in_executor(None, q.get)
            if kind == "chunk":
                buf += val
                if time.time() - last > 0.15:
                    await self._send(self._envelope("res", {"chunk": buf}, dst=msg["src"], rid=msg["rid"]))
                    buf, last = "", time.time()
                continue
            if buf:
                await self._send(self._envelope("res", {"chunk": buf}, dst=msg["src"], rid=msg["rid"]))
            await self._send(self._envelope("res", {"final": True, **({"error": val} if kind == "error" else {})},
                                            dst=msg["src"], rid=msg["rid"]))
            return

    async def _on_req_llm(self, msg: dict) -> None:
        if not self.cfg.get("coop.allow_remote_inference", True):
            raise PermissionError("este nodo no ofrece inferencia remota")
        b = msg["body"]
        self._notify({"type": "remote_use", "from": self.peers.get(msg["src"], {}).get("name", msg["src"]), "what": "LLM"})
        await self._respond_stream(msg, lambda: self.s.llm.stream(b["messages"], "default",
                                                                  **{k: v for k, v in b.get("opts", {}).items()
                                                                     if k in ("temperature", "max_tokens")}))

    async def _on_req_agent(self, msg: dict) -> None:
        if not self.cfg.get("coop.allow_remote_agents", False):
            raise PermissionError("este nodo no ofrece agentes remotos")
        b = msg["body"]
        who = self.peers.get(msg["src"], {}).get("name", msg["src"])
        self._notify({"type": "remote_use", "from": who, "what": f"agente {b.get('agent')}"})

        def gen():
            sid = self.s.memory.ensure_session(None, f"[LAN] {who}")
            res = self.s.agents.run(b.get("agent", "auto"), b["text"], sid, remote=True, save=False,
                                    emit=lambda e: self._notify({**e, "remote_from": who}) if e["type"] == "approval_required" else None)
            yield res["answer"]

        await self._respond_stream(msg, gen)

    async def _on_share_memory(self, msg: dict) -> None:
        if not self.cfg.get("coop.share_memory", False):
            return
        who = self.peers.get(msg["src"], {}).get("name", msg["src"])
        for fact in msg["body"].get("facts", [])[:50]:
            self.s.memory.remember(str(fact)[:500], "note", 0.4, source=f"peer:{who}")
        self._notify({"type": "memory_shared", "from": who, "count": len(msg["body"].get("facts", []))})

    async def _on_share_tool(self, msg: dict) -> None:
        if not self.cfg.get("coop.share_tools", True):
            return
        b = msg["body"]
        who = self.peers.get(msg["src"], {}).get("name", msg["src"])
        prop = self.s.proposals.create(b["name"], b.get("description", ""), b["code"], risk=b.get("risk", "medium"),
                                       rationale=f"Compartida por el nodo {who}. {b.get('rationale', '')}", author=f"peer:{who}")
        self._notify({"type": "tool_shared", "from": who, "proposal": prop["id"]})

    async def _on_res(self, msg: dict) -> None:
        q = self.pending.get(msg.get("rid"))
        if q:
            await q.put(msg["body"])

    # ------------------------------------------------------------ API sincrona (para el servidor y el LLM)
    def _run(self, coro, timeout: float = 15):
        if not self.running:
            raise RuntimeError("El modo cooperativo no esta activo")
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def resolve(self, ref: str) -> str:
        for nid, p in self.peers.items():
            if ref in (nid, p.get("name"), p.get("vip")):
                return nid
        raise KeyError(f"nodo no encontrado en la LAN virtual: {ref}")

    def request_stream(self, ref: str, mtype: str, body: dict, timeout: float = 300):
        nid = self.resolve(ref)
        rid = uuid.uuid4().hex
        out: queue.Queue = queue.Queue()

        async def go():
            q: asyncio.Queue = asyncio.Queue()
            self.pending[rid] = q
            try:
                await self._send(self._envelope(mtype, body, dst=nid, rid=rid))
                while True:
                    item = await asyncio.wait_for(q.get(), timeout)
                    out.put(item)
                    if item.get("final"):
                        return
            except asyncio.TimeoutError:
                out.put({"error": "tiempo agotado", "final": True})
            finally:
                self.pending.pop(rid, None)

        asyncio.run_coroutine_threadsafe(go(), self.loop)
        while True:
            item = out.get(timeout=timeout + 5)
            if item.get("error"):
                raise RuntimeError(f"nodo remoto: {item['error']}")
            if item.get("chunk"):
                yield item["chunk"]
            if item.get("final"):
                return

    def remote_llm(self, ref: str, messages: list[dict], opts: dict):
        yield from self.request_stream(ref, "req.llm", {"messages": messages, "opts": opts})

    def remote_agent(self, ref: str, agent: str, text: str) -> str:
        return "".join(self.request_stream(ref, "req.agent", {"agent": agent, "text": text}, timeout=600))

    def ping(self, ref: str) -> float:
        t0 = time.time()
        list(self.request_stream(ref, "req.ping", {}, timeout=10))
        return round((time.time() - t0) * 1000, 1)

    def send_chat(self, text: str) -> dict:
        item = {"from": self.name, "node": self.node_id, "text": text[:2000], "ts": time.time(), "own": True}
        self.chat.append(item)
        self._run(self._send(self._envelope("chat", {"name": self.name, "text": text[:2000]})))
        return item

    def share_facts(self, facts: list[str]) -> None:
        self._run(self._send(self._envelope("share.memory", {"facts": facts[:50]})))

    def share_proposal(self, prop: dict) -> None:
        self._run(self._send(self._envelope("share.tool", {k: prop[k] for k in ("name", "description", "code", "rationale")}
                                            | {"risk": prop.get("declared_risk", "medium")})))

    def _notify(self, event: dict) -> None:
        for cb in list(self.listeners):
            try:
                cb({"type": "coop", "event": event})
            except Exception:  # noqa: BLE001
                pass

    def status(self) -> dict:
        port = int(self.cfg.get("coop.listen_port", 8766))
        return {"running": self.running, "node_id": self.node_id, "name": self.name, "vip": self.vip,
                "network_id": self.network_id, "configured": bool(self.network_id and self.cfg.get("coop.passphrase")),
                "listen": self.cfg.get("coop.listen", False), "port": port,
                "links": list(self.links.keys()), "outbound": self.link_urls,
                "peers": sorted(self.peers.values(), key=lambda p: p.get("name", "")),
                "chat": list(self.chat)[-50:],
                "addresses": [{**a, "url": f"ws://{a['ip']}:{port}"} for a in overlay_addresses()]}
