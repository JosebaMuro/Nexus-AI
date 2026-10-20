"""API REST + WebSocket del webui."""
from __future__ import annotations

import asyncio
import json
import re
import secrets
import shutil
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Body, Depends, FastAPI, File, Form, Header, HTTPException, Request, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles

from .. import __version__, hardware, log, paths
from ..core import Services
from ..models.recommended import STT_MODELS
from ..runtimes import RUNTIMES, candidates

L = log.get("server")


class Hub:
    """Difunde eventos (aprobaciones, coop, descargas) a todas las UIs conectadas."""

    def __init__(self) -> None:
        self.sockets: set[WebSocket] = set()
        self.loop: asyncio.AbstractEventLoop | None = None

    def broadcast(self, event: dict) -> None:
        if not self.loop:
            return
        for ws in list(self.sockets):
            asyncio.run_coroutine_threadsafe(_safe_send(ws, event), self.loop)


async def _safe_send(ws: WebSocket, event: dict) -> None:
    try:
        await ws.send_text(json.dumps(event, ensure_ascii=False, default=str))
    except Exception:  # noqa: BLE001
        pass


def create_app(services: Services | None = None) -> FastAPI:
    hub = Hub()
    admin_tokens: dict[str, float] = {}
    state: dict = {}

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        hub.loop = asyncio.get_running_loop()
        s = services or await asyncio.to_thread(Services)
        state["s"] = s
        s.supervisor.listeners.append(hub.broadcast)
        s.coop.listeners.append(hub.broadcast)
        s.lounge.listeners.append(hub.broadcast)
        s.kg_jobs.listeners.append(hub.broadcast)
        s.remote.server = getattr(app.state, "remote_server", None)
        s.start()
        yield
        await asyncio.to_thread(s.shutdown)

    app = FastAPI(title="Nexus-Office", version=__version__, lifespan=lifespan)

    app.state.nexus_state = state           # el puerto remoto (RemoteGate) lee los servicios desde aqui

    def S() -> Services:
        return state["s"]

    # ------------------------------------------------------------------ acceso desde la red local
    LOCAL = {"127.0.0.1", "::1", "localhost", "testclient"}
    OPEN_PATHS = ("/api/ping", "/sw.js", "/static/manifest.webmanifest", "/static/icon.svg", "/connect")

    def lan_key() -> str:
        cfg = S().config
        if not cfg.get("server.lan_key"):
            cfg.set("server.lan_key", secrets.token_urlsafe(9))
        return cfg.get("server.lan_key")

    def lan_ok(host: str, headers, query, cookies) -> bool:
        if host in LOCAL:
            return True
        supplied = headers.get("x-nexus-key") or query.get("key") or cookies.get("nexus_key")
        return bool(supplied) and secrets.compare_digest(supplied, lan_key())

    @app.middleware("http")
    async def lan_guard(request, call_next):
        from fastapi.responses import HTMLResponse, JSONResponse
        host = request.client.host if request.client else "127.0.0.1"
        if request.scope.get("nexus_remote"):
            return await call_next(request)          # puerto remoto: ya autenticado por RemoteGate (dispositivo emparejado)
        if "s" not in state or request.url.path.startswith(OPEN_PATHS) or lan_ok(host, request.headers, request.query_params, request.cookies):
            resp = await call_next(request)
            if request.query_params.get("key") and host not in LOCAL:
                resp.set_cookie("nexus_key", request.query_params["key"], httponly=True, samesite="lax", max_age=31536000)
            return resp
        if request.url.path == "/":
            return HTMLResponse("<!doctype html><meta name=viewport content='width=device-width'><body style='background:#04060c;color:#d8f6ff;"
                                "font-family:sans-serif;padding:24px'><h2 style='color:#00f0ff'>Nexus-Office</h2><p>Introduce la clave de red "
                                "(en el PC: Red COOP &gt; Conectar dispositivos).</p><form><input name=key autofocus style='font-size:18px;padding:8px'>"
                                " <button style='font-size:18px;padding:8px'>Entrar</button></form>", status_code=401)
        return JSONResponse({"detail": "Clave de red requerida"}, status_code=401)

    @app.get("/api/ping")
    def ping():
        s = state.get("s")
        return {"nexus": True, "version": __version__, "name": s.config.get("user.assistant_name") if s else "Nexus-Office"}

    @app.get("/api/lan")
    def lan_info(request: Request):
        if (request.client.host if request.client else "") not in LOCAL:
            raise HTTPException(403, "Solo desde el propio equipo")
        from ..coop.node import overlay_addresses
        port = S().config.get("server.port", 8775)
        bound = S().config.get("server.host", "127.0.0.1")
        return {"key": lan_key(), "listening_lan": bound not in ("127.0.0.1", "localhost"), "port": port,
                "urls": [f"http://{a['ip']}:{port}/?key={lan_key()}" for a in sorted(overlay_addresses(), key=lambda a: a["kind"] != "lan")
                         if not a["ip"].startswith("169.254.")]}

    @app.post("/api/lan/enable", dependencies=[Depends(lambda x_admin_token=Header(default=None): require_admin(x_admin_token))])
    def lan_enable(enabled: bool = Body(..., embed=True)):
        S().config.set("server.host", "0.0.0.0" if enabled else "127.0.0.1")
        lan_key()
        return {"host": S().config.get("server.host"), "restart_required": True}

    @app.get("/api/package/termux")
    def package_termux():
        """Paquete para instalar Nexus-Office en Android/Termux directamente desde este equipo."""
        import io
        import tarfile
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for name in ("nexus", "requirements.txt", "install.sh", "start.sh", "install-termux.sh", "README.md", "LICENSE"):
                p = paths.ROOT / name
                if p.exists():
                    tar.add(p, arcname=f"Nexus-Office/{name}", filter=lambda ti: None if "__pycache__" in ti.name else ti)
        return Response(buf.getvalue(), media_type="application/gzip",
                        headers={"Content-Disposition": f"attachment; filename=Nexus-Office-{__version__}-termux.tar.gz"})

    def is_admin(token: str | None) -> bool:
        exp = admin_tokens.get(token or "")
        return bool(exp and exp > time.time())

    def require_admin(x_admin_token: str | None = Header(default=None)) -> bool:
        if not is_admin(x_admin_token):
            raise HTTPException(403, "Requiere modo administrador")
        return True

    def admin_flag(x_admin_token: str | None = Header(default=None)) -> bool:
        return is_admin(x_admin_token)

    # ------------------------------------------------------------------ estaticos
    class RevalidatedStatic(StaticFiles):
        """Los estaticos se revalidan (ETag) en cada carga: tras actualizar Nexus-Office no quedan versiones viejas."""
        async def get_response(self, path, scope):
            resp = await super().get_response(path, scope)
            resp.headers["Cache-Control"] = "no-cache"
            return resp

    app.mount("/static", RevalidatedStatic(directory=str(paths.WEB)), name="static")
    app.mount("/outputs", StaticFiles(directory=str(paths.outputs_dir())), name="outputs")

    @app.get("/")
    def index():
        return FileResponse(paths.WEB / "index.html", headers={"Cache-Control": "no-cache"})

    @app.get("/sw.js")
    def service_worker():  # en la raiz para que su ambito cubra toda la app (PWA)
        return FileResponse(paths.WEB / "sw.js", media_type="text/javascript", headers={"Cache-Control": "no-cache"})

    # ------------------------------------------------------------------ estado general
    @app.get("/api/status")
    def status(admin: bool = Depends(admin_flag)):
        s = S()
        mem = s.memory.stats()
        return {"version": __version__, "name": s.config.get("user.assistant_name"), "admin": admin,
                "has_pin": s.config.has_pin(), "profile": {k: s.profile[k] for k in ("id", "tier", "accelerator", "device_name", "ram_gb", "vram_gb")},
                "npu": s.profile["npu"], "llm": s.llm.status(), "image": s.images.status(), "voice": s.voice.info(),
                "memory": mem, "graph": s.graph.stats(), "rag": s.rag.stats(), "tuning": s.tuning_reasons,
                "assist": s.assist.info(),
                "coop": {"running": s.coop.running, "peers": len(s.coop.peers), "vip": s.coop.vip},
                "pending_approvals": len(s.supervisor.list_pending()),
                "pending_proposals": len(s.proposals.list("pendiente")),
                "suggest_smaller_model": s.profile["llm"].get("suggest_smaller_model", False)}

    # ------------------------------------------------------------------ admin
    @app.post("/api/admin/setup")
    def admin_setup(pin: str = Body(..., embed=True)):
        s = S()
        if s.config.has_pin():
            raise HTTPException(400, "Ya existe un PIN; inicia sesion")
        s.config.set_pin(pin)
        tok = secrets.token_hex(16)
        admin_tokens[tok] = time.time() + 8 * 3600
        return {"token": tok}

    @app.post("/api/admin/login")
    def admin_login(request: Request, pin: str = Body(..., embed=True)):
        host = request.client.host if request.client else ""
        key = "pin:" + host
        # limite de intentos para la red local y el puerto remoto (en el propio equipo no tiene sentido bloquear)
        lim = S().remote.limiter if (host not in LOCAL or request.scope.get("nexus_remote")) else None
        if lim and lim.blocked(key):
            raise HTTPException(429, "Demasiados intentos fallidos: espera 15 minutos")
        if not S().config.check_pin(pin):
            if lim:
                lim.fail(key)
            time.sleep(1)
            raise HTTPException(401, "PIN incorrecto")
        if lim:
            lim.reset(key)
        tok = secrets.token_hex(16)
        admin_tokens[tok] = time.time() + 8 * 3600
        return {"token": tok}

    @app.post("/api/admin/logout")
    def admin_logout(x_admin_token: str | None = Header(default=None)):
        admin_tokens.pop(x_admin_token or "", None)
        return {"ok": True}

    @app.post("/api/admin/pin", dependencies=[Depends(require_admin)])
    def admin_change_pin(pin: str = Body(..., embed=True)):
        S().config.set_pin(pin)
        return {"ok": True}

    @app.post("/api/admin/shutdown", dependencies=[Depends(require_admin)])
    def admin_shutdown():
        """Apagado limpio: guarda el resumen de rendimiento para el autoajuste del siguiente arranque."""
        import signal
        import threading
        threading.Timer(0.5, lambda: signal.raise_signal(signal.SIGINT)).start()
        return {"ok": True}

    @app.get("/api/admin/logs", dependencies=[Depends(require_admin)])
    def admin_logs(n: int = 200):
        return {"lines": log.recent(n)}

    # ------------------------------------------------------------------ configuracion
    @app.get("/api/config")
    def get_cfg(admin: bool = Depends(admin_flag)):
        return S().config.view(admin)

    @app.put("/api/config")
    def put_cfg(changes: dict = Body(...), admin: bool = Depends(admin_flag)):
        s = S()
        try:
            applied = s.config.update(changes, admin)
        except PermissionError as e:
            raise HTTPException(403, str(e)) from e
        if any(k.startswith(("llm.", "hardware.")) for k in applied):
            s.llm.rebuild()
        if any(k.startswith("tools.") for k in applied):
            s.tools.sandbox_community = s.config.get("tools.sandbox_community_tools", True)
            s.tools.reload()
        return {"applied": applied}

    # ------------------------------------------------------------------ hardware / rendimiento
    @app.get("/api/hardware")
    def hw():
        s = S()
        return {"hardware": s.hw.to_dict(), "profile": s.profile, "tuning": s.tuning_reasons,
                "tuning_state": hardware.load_state(), "history": hardware.load_history()[-15:]}

    @app.get("/api/monitor")
    def monitor():
        return S().monitor.live()

    @app.get("/api/android/autoajuste")
    def android_tuning():
        """App Android autonoma: nucleos, hilos medidos, limites por temperatura o bateria y consejo de modelo."""
        t = getattr(S(), "android_tuner", None)
        if t is None:
            raise HTTPException(404, "El autoajuste en vivo solo funciona en la app Android autonoma")
        return t.report()

    @app.post("/api/android/autoajuste/medir")
    def android_tuning_reset():
        """Vuelve a medir los hilos desde cero (p. ej. tras cambiar de funda o de ajustes de rendimiento)."""
        t = getattr(S(), "android_tuner", None)
        if t is None:
            raise HTTPException(404, "El autoajuste en vivo solo funciona en la app Android autonoma")
        t.state["trials"], t.state["best"] = {}, None
        return t.step()

    @app.post("/api/android/autoajuste/modelo/{mid}")
    def android_tuning_model(mid: str):
        """Acepta el consejo de modelo: lo activa, o lo descarga y lo activa al terminar."""
        t = getattr(S(), "android_tuner", None)
        if t is None:
            raise HTTPException(404, "El autoajuste en vivo solo funciona en la app Android autonoma")
        try:
            return t.switch_model(mid)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.post("/api/hardware/redetect", dependencies=[Depends(require_admin)])
    def redetect():
        s = S()
        s.hw = hardware.detect(ollama_url=s.config.get("llm.ollama_url"))
        hardware.save(s.hw)
        s.profile.clear()
        s.profile.update(hardware.build_profile(s.hw, s.config.get("hardware.profile_override", "")))
        s.llm.rebuild()
        return {"hardware": s.hw.to_dict(), "profile": s.profile}

    @app.get("/api/service")
    async def service_status():
        from .. import service
        from ..hardware.profiles import android
        if android():
            return {"kind": "android", "registered": "", "supported": False}
        return {"kind": service.kind(), "registered": await asyncio.to_thread(service.registered), "supported": True,
                "admin_os": service.is_admin(), "port": S().config.get("server.port", 8775)}

    @app.post("/api/service", dependencies=[Depends(require_admin)])
    async def service_action(action: str = Body(..., embed=True), red: bool = Body(False, embed=True)):
        """Instala o quita el arranque automatico; en un servidor ya en marcha el servicio toma el relevo al reiniciar."""
        from .. import service
        cfg = S().config
        try:
            if action == "instalar":
                host = "0.0.0.0" if red else cfg.get("server.host", "127.0.0.1")
                return {"ok": True, "detail": await asyncio.to_thread(service.install, host, int(cfg.get("server.port", 8775)))}
            if action == "quitar":
                return {"ok": True, "detail": ", ".join(await asyncio.to_thread(service.uninstall)) or "no habia servicio"}
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, str(e)) from e
        raise HTTPException(400, "accion: instalar | quitar")

    @app.get("/api/downloads")
    def downloads_list():
        from .. import downloads
        return downloads.listing()

    @app.get("/descargas/{rel:path}")
    def downloads_get(rel: str):
        from .. import downloads
        f = downloads.resolve(rel)
        if not f:
            raise HTTPException(404, "no existe")
        return FileResponse(f, filename=f.name, media_type="application/vnd.android.package-archive" if f.suffix == ".apk" else None)

    @app.get("/api/drivers")
    async def drivers_report():
        from ..hardware import drivers
        return await asyncio.to_thread(drivers.report, S().profile["accelerator"])

    @app.post("/api/drivers/update", dependencies=[Depends(require_admin)])
    def drivers_update(option: str = Body(..., embed=True)):
        from ..hardware import drivers
        try:
            return drivers.launch(option)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/runtimes")
    def runtimes():
        s = S()
        from ..runtimes import bundled_variants
        return {"installed": s.runtimes.installed(), "progress": s.runtimes.progress,
                "bundled": {rt: bundled_variants(rt) for rt in RUNTIMES},
                "recommended": {rt: candidates(s.profile, rt) for rt in RUNTIMES},
                "descriptions": {k: v["desc"] for k, v in RUNTIMES.items()},
                "active": {"llama": getattr(s.llm.backends.get("llamaserver"), "variant", "")}}

    @app.post("/api/runtimes/install")
    def runtime_install(runtime: str = Body(...), variant: str = Body(...), admin: bool = Depends(admin_flag)):
        s = S()
        rec = candidates(s.profile, runtime)
        if not admin and variant != rec[0]:
            raise HTTPException(403, "En modo usuario solo se instala la variante recomendada")

        def work():
            try:
                s.runtimes.install(runtime, variant)
                s.llm.rebuild()
            except Exception as e:  # noqa: BLE001
                L.error("Instalacion de runtime fallida: %s", e)

        import threading
        threading.Thread(target=work, daemon=True).start()
        return {"started": True}

    # ------------------------------------------------------------------ sesiones / memoria
    @app.get("/api/sessions")
    def sessions():
        return S().memory.sessions()

    @app.get("/api/sessions/{sid}")
    def session_messages(sid: str):
        return S().memory.history(sid, 200)

    @app.delete("/api/sessions/{sid}")
    def delete_session(sid: str):
        S().memory.delete_session(sid)
        return {"ok": True}

    @app.post("/api/feedback")
    def feedback(message_id: int = Body(...), score: int = Body(...), comment: str = Body("")):
        s = S()
        res = s.learner.feedback(message_id, score, comment)
        msg = s.db.one("SELECT session_id, agent FROM messages WHERE id=?", (message_id,))
        if msg:
            q = s.db.one("SELECT content FROM messages WHERE session_id=? AND id<? AND role='user' ORDER BY id DESC LIMIT 1",
                         (msg["session_id"], message_id))
            s.lounge.on_feedback(msg["agent"], q["content"] if q else "", score)
        return res

    # ------------------------------------------------------------------ salon virtual
    @app.get("/api/lounge")
    def lounge():
        from dataclasses import asdict
        s = S()
        return {"messages": s.lounge.history(150), "skills": s.lounge.skills(), "proposals": s.lounge.proposals(),
                "gaps": s.lounge.detect_gaps(), "agents": [asdict(a) for a in s.agents.list()]}

    @app.post("/api/lounge/meet")
    def lounge_meet(topic: str = Body("", embed=True)):
        import threading
        threading.Thread(target=S().lounge.meet, kwargs={"topic": topic}, daemon=True).start()
        return {"started": True}

    @app.post("/api/lounge/say")
    def lounge_say(text: str = Body(..., embed=True)):
        S().lounge.user_says(text)
        return {"ok": True}

    @app.post("/api/lounge/proposals/{pid}", dependencies=[Depends(require_admin)])
    def lounge_proposal(pid: int, approve: bool = Body(...), note: str = Body("")):
        try:
            return S().lounge.resolve_proposal(pid, approve, note)
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/memory")
    def memories(kind: str | None = None, limit: int = 100, offset: int = 0):
        s = S()
        return {"items": s.memory.list(kind, limit, offset), "stats": s.memory.stats()}

    @app.get("/api/memory/search")
    def memory_search(q: str, k: int = 10):
        return S().memory.recall(q, k)

    @app.post("/api/memory")
    def memory_add(content: str = Body(...), kind: str = Body("note")):
        s = S()
        mid = s.memory.remember(content, kind, 0.8, source="manual")
        s.learner.learn_text(content)   # lo comun al general; el saber hacer, al especialista que corresponda
        hub.broadcast({"type": "graph_update"})
        return {"id": mid}

    @app.delete("/api/memory/{mid}")
    def memory_delete(mid: int):
        S().memory.forget(mid)
        return {"ok": True}

    @app.get("/api/graph")
    def graph(limit: int = 220, scope: str = "*"):
        """scope="*": el general con el subgrafo de cada agente dentro; "": solo el general; id: solo ese agente."""
        s = S()
        if scope not in ("*", "") and not s.agents.get(scope):
            raise HTTPException(404, "Agente desconocido")
        return s.graph.to_json(limit, scope)

    @app.get("/api/graph/scopes")
    def graph_scopes():
        """Grafo general y subgrafo de cada agente, con sus tamanos y documentos RAG propios."""
        s = S()
        sizes = {r["scope"]: r for r in s.graph.scopes()}
        docs = s.rag.stats()["by_scope"]
        out = [{"id": "", "name": "General", "icon": "◎", "color": "", **_sizes(sizes.get("")), "documents": docs.get("general", 0)}]
        for a in s.agents.list():
            out.append({"id": a.id, "name": a.name, "icon": a.icon, "color": a.color, **_sizes(sizes.get(a.id)),
                        "documents": docs.get(a.id, 0)})
        return out

    def _sizes(r: dict | None) -> dict:
        return {"entities": r["entities"] if r else 0, "relations": r["relations"] if r else 0}

    def rag_target(agent: str) -> str:
        agent = (agent or "").strip()
        if agent in ("", "general"):
            return ""
        if not S().agents.get(agent):
            raise HTTPException(400, f"Agente desconocido: {agent}")
        return agent

    @app.delete("/api/graph/{eid}", dependencies=[Depends(require_admin)])
    def graph_delete(eid: int):
        S().graph.delete_entity(eid)
        return {"ok": True}

    # ------------------------------------------------------------------ gestor de conocimiento (Ajustes)
    def kg_scope(scope: str | None) -> str | None:
        """None o "*": todo; "" o "general": el grafo general; un id: el subgrafo de ese agente."""
        if scope is None or scope == "*":
            return None
        if scope in ("", "general"):
            return ""
        if not S().agents.get(scope):
            raise HTTPException(400, f"Agente desconocido: {scope}")
        return scope

    @app.get("/api/kg/overview")
    def kg_overview():
        s = S()
        return {**s.organizer.overview(), "stats": s.graph.stats(), "scopes": graph_scopes(),
                "memory": s.memory.stats(), "embeddings": s.embed_worker.status(), "jobs": s.kg_jobs.list(),
                "embed_models": [m for m in s.models.catalog.list() if m["kind"] == "embed"],
                "embed_active": s.config.get("memory.embed_model", "")}

    @app.get("/api/kg/nodes")
    def kg_nodes(scope: str | None = None, q: str = "", category: str = "", type: str = "", limit: int = 60,
                 offset: int = 0, semantic: bool = False):
        s = S()
        sc = kg_scope(scope)
        if semantic and q.strip():
            try:
                items = s.organizer.semantic_search(q, sc, min(limit, 200))
            except Exception as e:  # noqa: BLE001
                raise HTTPException(400, f"Búsqueda semántica no disponible: {e}") from e
            return {"total": len(items), "items": items, "semantic": True}
        return s.graph.search_nodes(sc, q, category, type, min(max(limit, 1), 500), max(offset, 0))

    @app.get("/api/kg/nodes/{eid}")
    def kg_node(eid: int):
        n = S().graph.node(eid)
        if not n:
            raise HTTPException(404, "Nodo no encontrado")
        return n

    @app.post("/api/kg/nodes", dependencies=[Depends(require_admin)])
    def kg_node_add(data: dict = Body(...)):
        s = S()
        try:
            n = s.graph.add_entity(kg_scope(data.get("scope", "")) or "", str(data.get("name", "")),
                                   str(data.get("type") or "concepto"), str(data.get("category") or ""),
                                   str(data.get("note") or ""))
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return n

    @app.patch("/api/kg/nodes/{eid}", dependencies=[Depends(require_admin)])
    def kg_node_edit(eid: int, data: dict = Body(...)):
        s = S()
        try:
            n = s.graph.update_entity(eid, data.get("name"), data.get("type"), data.get("category"), data.get("note"),
                                      data.get("weight"))
            if "scope" in data and data["scope"] is not None:
                n = s.graph.move_entity(n["id"], kg_scope(data["scope"]) or "", data.get("category") or "")
        except KeyError:
            raise HTTPException(404, "Nodo no encontrado") from None
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return n

    @app.delete("/api/kg/nodes/{eid}", dependencies=[Depends(require_admin)])
    def kg_node_delete(eid: int):
        S().graph.delete_entity(eid)
        hub.broadcast({"type": "graph_update"})
        return {"ok": True}

    @app.post("/api/kg/merge", dependencies=[Depends(require_admin)])
    def kg_merge(src: int = Body(...), dst: int = Body(...)):
        try:
            n = S().graph.merge_entities(src, dst)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return n

    @app.post("/api/kg/relations", dependencies=[Depends(require_admin)])
    def kg_relation_add(src: int = Body(...), dst: int = Body(...), rel: str = Body("relacionado_con"),
                        weight: float = Body(1.0)):
        try:
            r = S().graph.add_relation(src, dst, rel, weight)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return r

    @app.delete("/api/kg/relations/{rid}", dependencies=[Depends(require_admin)])
    def kg_relation_delete(rid: int):
        S().graph.delete_relation(rid)
        hub.broadcast({"type": "graph_update"})
        return {"ok": True}

    @app.get("/api/kg/formats")
    def kg_formats():
        from ..memory.kg_io import FORMATS, IMPORT_EXT
        return {"formats": [{"name": n, "extensions": e, "description": d} for n, e, d in FORMATS],
                "extensions": IMPORT_EXT}

    @app.post("/api/kg/import", dependencies=[Depends(require_admin)])
    async def kg_import(file: UploadFile | None = File(None), files: list[UploadFile] | None = File(None),
                        target: str = Form("")):
        """Inserta otro grafo de conocimiento en cualquier formato (se detecta por el contenido). Varios ficheros
        (``files``) se unen en un solo grafo: p. ej. nodes.csv + edges.csv de Neo4j o Gephi."""
        from ..memory.kg_io import GraphFormatError
        uploads = ([file] if file else []) + list(files or [])
        if not uploads:
            raise HTTPException(400, "Falta el fichero del grafo")
        items, total = [], 0
        for f in uploads[:50]:
            data = await f.read()
            total += len(data)
            if total > 300 * 1024 * 1024:
                raise HTTPException(400, "El grafo supera 300 MB")
            items.append(((f.filename or "grafo.json").replace("\\", "/").rsplit("/", 1)[-1], data))
        try:
            if len(items) == 1:
                return S().kg_jobs.import_graph(items[0][1], items[0][0], target)
            return S().kg_jobs.import_graphs(items, target)
        except (GraphFormatError, ValueError) as e:
            raise HTTPException(400, str(e)) from e
        except Exception as e:  # noqa: BLE001 (XML mal formado, etc.)
            raise HTTPException(400, f"No se pudo leer el grafo: {e}") from e

    @app.post("/api/kg/archive", dependencies=[Depends(require_admin)])
    async def kg_archive(file: UploadFile = File(...), target: str = Form("auto"), reason: bool = Form(True),
                         rag: bool = Form(True), max_reason: int = Form(-1)):
        """Lee un ZIP (o RAR, 7z, tar), lo razona con el LLM y lo procesa en segundo plano: grafos, biblioteca,
        temas, hechos y procedimientos, cada cosa en el grafo general o en el del especialista."""
        import tempfile
        name = (file.filename or "archivo.zip").replace("\\", "/").rsplit("/", 1)[-1]
        fd, tmp = tempfile.mkstemp(suffix="_" + re.sub(r"[^A-Za-z0-9._-]+", "_", name)[-60:], dir=paths.sub("cache"))
        with open(fd, "wb") as out:
            while chunk := await file.read(1024 * 1024):
                out.write(chunk)
        try:
            return S().kg_jobs.process_archive(Path(tmp), name, target, reason, rag, None if max_reason < 0 else max_reason)
        except ValueError as e:
            Path(tmp).unlink(missing_ok=True)
            raise HTTPException(400, str(e)) from e

    @app.get("/api/kg/export")
    def kg_export(scope: str | None = None, format: str = "json"):
        from ..memory.kg_io import export_graph
        if format not in ("json", "graphml", "csv"):
            raise HTTPException(400, "format: json, graphml o csv")
        sc = kg_scope(scope)
        data, mime, ext = export_graph(S().graph, sc, format)
        name = f"nexus-conocimiento-{'todo' if sc is None else sc or 'general'}.{ext}"
        return Response(data, media_type=mime, headers={"Content-Disposition": f"attachment; filename={name}"})

    @app.post("/api/kg/reorganize", dependencies=[Depends(require_admin)])
    def kg_reorganize(apply: bool = Body(False, embed=True)):
        return S().kg_jobs.reorganize(apply)

    @app.post("/api/kg/duplicates", dependencies=[Depends(require_admin)])
    def kg_duplicates(scope: str | None = Body(None, embed=True)):
        return S().kg_jobs.duplicates(kg_scope(scope))

    @app.get("/api/kg/jobs")
    def kg_jobs():
        return S().kg_jobs.list()

    @app.get("/api/kg/jobs/{jid}")
    def kg_job(jid: str):
        j = S().kg_jobs.get(jid)
        if not j:
            raise HTTPException(404, "Trabajo no encontrado")
        return j

    @app.get("/api/kg/memories")
    def kg_memories(scope: str | None = None, kind: str | None = None, limit: int = 100, offset: int = 0):
        return S().memory.list(kind, min(limit, 500), offset, kg_scope(scope))

    @app.patch("/api/kg/memories/{mid}", dependencies=[Depends(require_admin)])
    def kg_memory_move(mid: int, scope: str = Body("", embed=True)):
        S().memory.move(mid, kg_scope(scope) or "")
        return {"ok": True}

    @app.get("/api/kg/embeddings")
    def kg_embeddings():
        return S().embed_worker.status()

    @app.post("/api/kg/embeddings/model", dependencies=[Depends(require_admin)])
    def kg_embeddings_model(model: str = Body("", embed=True)):
        """Activa un modelo de embeddings del catalogo ("" = sin modelo: hashing). Se recalcula en segundo plano."""
        s = S()
        if model:
            entry = s.models.catalog.get(model)
            if not entry or entry.get("kind") != "embed":
                raise HTTPException(400, "No es un modelo de embeddings instalado")
        s.config.set("memory.embed_model", model)
        s.set_embedder()
        return s.embed_worker.status()

    @app.post("/api/kg/embeddings/reindex", dependencies=[Depends(require_admin)])
    def kg_embeddings_reindex():
        s = S()
        s.memory._cache = s.rag._cache = None
        s.embed_worker.start()
        return s.embed_worker.status()

    # ------------------------------------------------------------------ biblioteca RAG
    @app.get("/api/rag")
    def rag_list(agent: str | None = None):
        s = S()
        return {"documents": s.rag.list(None if agent is None else rag_target(agent)), "stats": s.rag.stats()}

    @app.get("/api/rag/search")
    def rag_search(q: str, k: int = 6, agent: str | None = None):
        return S().rag.search(q, k, agent=None if agent is None else rag_target(agent))

    RAG_FORMATS = ("Formatos admitidos: PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, texto, código "
                   "fuente y archivos ZIP, RAR, 7z o tar.gz")

    async def save_upload(file: UploadFile, folder: Path) -> tuple[Path, str]:
        from ..rag.readers import supported
        name = (file.filename or "documento.txt").replace("\\", "/").rsplit("/", 1)[-1] or "documento.txt"
        if not supported(name):
            raise HTTPException(400, f"{name}: formato no admitido. {RAG_FORMATS}")
        tmp = folder / f"{len(list(folder.iterdir()))}_{name}"
        with open(tmp, "wb") as out:   # por trozos: un ZIP de proyecto puede pesar cientos de MB
            while chunk := await file.read(1024 * 1024):
                out.write(chunk)
        return tmp, name

    @app.get("/api/rag/formats")
    def rag_formats():
        from ..rag.archive import find_extractor
        from ..rag.readers import ARCHIVES, DOCUMENTS
        return {"documents": sorted(DOCUMENTS), "archives": list(ARCHIVES),
                "extractors": {k: find_extractor(k)[0] for k in ("rar", "7z")}}

    @app.post("/api/rag/upload")
    async def rag_upload(file: UploadFile = File(...), agent: str = Form("")):
        import tempfile
        target = rag_target(agent)
        folder = Path(tempfile.mkdtemp(dir=paths.sub("cache")))
        try:
            tmp, name = await save_upload(file, folder)
        except Exception:
            shutil.rmtree(folder, ignore_errors=True)
            raise
        try:
            doc = await asyncio.to_thread(lambda: S().rag.ingest_file(tmp, "", name, agent=target))
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, f"No se pudo indexar: {e}") from e
        finally:
            shutil.rmtree(tmp.parent, ignore_errors=True)
        return doc

    @app.post("/api/rag/import")
    async def rag_import(files: list[UploadFile] = File(...), agent: str = Form("")):
        """Varios documentos y/o comprimidos de una vez; se leen en paralelo (un fichero por nucleo).

        agent: "" = grafo general (todos los agentes); un id = solo el grafo individual de ese agente."""
        import tempfile
        target = rag_target(agent)
        folder = Path(tempfile.mkdtemp(dir=paths.sub("cache")))
        try:
            items = [await save_upload(f, folder) for f in files[:200]]
            t0 = time.time()
            res = await asyncio.to_thread(S().rag.ingest_many, items, hub.broadcast, target)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        hub.broadcast({"type": "graph_update"})
        return {"results": res, "ok": sum(1 for r in res if "doc" in r), "failed": sum(1 for r in res if "error" in r),
                "seconds": round(time.time() - t0, 2), "agent": target}

    @app.post("/api/rag/url")
    async def rag_url(url: str = Body(...), agent: str = Body("")):
        """Peticion explicita del usuario desde la Biblioteca: URL -> PDF -> RAG -> conocimiento."""
        from ..rag.url_pdf import process_url
        target = rag_target(agent)
        try:
            res = await asyncio.to_thread(process_url, S(), url, hub.broadcast, 8, target)
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return res

    @app.post("/api/rag/crawl")
    async def rag_crawl(url: str = Body(...), max_pages: int = Body(40), max_depth: int = Body(3),
                        scope: str = Body("path"), agent: str = Body(""), background: bool = Body(False),
                        batch_pages: int = Body(0)):
        """Rastreo de un sitio de documentacion -> biblioteca RAG -> conocimiento (general o de un agente).

        background=true: lo hace en segundo plano por lotes reanudables (GET /api/rag/crawls para el progreso).
        Sin el, si max_pages no cabe en un lote, tambien va por lotes, pero espera a terminar."""
        from ..rag import crawl_jobs
        from ..rag.crawler import crawl_site
        if scope not in ("path", "domain"):
            raise HTTPException(400, "scope debe ser 'path' o 'domain'")
        if not re.match(r"^https?://", url or "", re.I):
            raise HTTPException(400, "la URL debe empezar por http:// o https://")
        target = rag_target(agent)
        if background:
            pages, chars = crawl_jobs.auto_batch(S().profile)
            job = crawl_jobs.create(url, max_pages, max_depth, scope, target, True,
                                    max(1, min(batch_pages or pages, 500)), chars)
            crawl_jobs.run_background(S(), job["id"], emit=hub.broadcast)
            return {"job": crawl_jobs.public(crawl_jobs.get(job["id"]))}
        try:
            res = await asyncio.to_thread(lambda: crawl_site(S(), url, max_pages, max_depth, scope, hub.broadcast,
                                                             agent=target))
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, str(e)) from e
        hub.broadcast({"type": "graph_update"})
        return res

    @app.get("/api/rag/crawls")
    def rag_crawls():
        """Rastreos por lotes: estado, lotes indexados y paginas pendientes."""
        from ..rag import crawl_jobs
        return crawl_jobs.list_jobs()

    @app.post("/api/rag/crawls/{job_id}/resume")
    def rag_crawl_resume(job_id: str):
        from ..rag import crawl_jobs
        job = crawl_jobs.get(job_id)
        if not job:
            raise HTTPException(404, "Rastreo no encontrado")
        if job["status"] != crawl_jobs.DONE and not crawl_jobs.is_running(job_id):
            crawl_jobs.run_background(S(), job_id, emit=hub.broadcast)
        return crawl_jobs.public(crawl_jobs.get(job_id))

    @app.post("/api/rag/crawls/{job_id}/pause")
    def rag_crawl_pause(job_id: str):
        from ..rag import crawl_jobs
        if not crawl_jobs.get(job_id):
            raise HTTPException(404, "Rastreo no encontrado")
        return {"stopping": crawl_jobs.stop(job_id)}

    @app.delete("/api/rag/crawls/{job_id}")
    def rag_crawl_delete(job_id: str, docs: bool = False):
        """Borra el trabajo (y con docs=true tambien los lotes ya indexados en la biblioteca)."""
        from ..rag import crawl_jobs
        crawl_jobs.wait(job_id, 0) if not crawl_jobs.stop(job_id) else crawl_jobs.wait(job_id, 30)
        if not crawl_jobs.delete(job_id, docs, S().rag):
            raise HTTPException(404, "Rastreo no encontrado")
        hub.broadcast({"type": "graph_update"})
        return {"deleted": job_id}

    @app.get("/api/rag/{doc_id}/file")
    def rag_file(doc_id: int):
        d = S().rag.get(doc_id)
        if not d or not d["path"] or not Path(d["path"]).exists():
            raise HTTPException(404, "Documento no disponible")
        return FileResponse(d["path"], filename=Path(d["path"]).name.split("_", 1)[-1])

    @app.patch("/api/rag/{doc_id}")
    def rag_assign(doc_id: int, agent: str = Body("", embed=True)):
        """Mueve un documento al grafo general o al grafo individual de un agente."""
        try:
            doc = S().rag.assign(doc_id, rag_target(agent))
        except KeyError:
            raise HTTPException(404, "Documento no encontrado") from None
        hub.broadcast({"type": "graph_update"})
        return doc

    @app.delete("/api/rag/{doc_id}")
    def rag_delete(doc_id: int):
        S().rag.delete(doc_id)
        hub.broadcast({"type": "graph_update"})
        return {"ok": True}

    @app.post("/api/memory/consolidate")
    def consolidate(session_id: str = Body(..., embed=True)):
        s = S()
        summ = s.learner.consolidate(session_id, (lambda p: s.llm.complete(p, max_tokens=200)) if s.llm.has_real_llm() else None)
        return {"summary": summ}

    # ------------------------------------------------------------------ multimedia
    @app.post("/api/image/generate")
    async def image_generate(prompt: str = Body(...), negative: str = Body(""), seed: int | None = Body(None)):
        return await asyncio.to_thread(S().images.generate, prompt, negative, None, None, seed)

    @app.post("/api/image/analyze")
    async def image_analyze(file: UploadFile = File(...), question: str = Form("")):
        data = await file.read()
        return await asyncio.to_thread(S().images.analyze, data, question)

    @app.post("/api/music/generate")
    async def music_generate(prompt: str = Body(...), seconds: float = Body(20)):
        res = await asyncio.to_thread(S().music.generate, prompt, seconds)
        return {k: v for k, v in res.items() if k != "path"}

    @app.get("/api/outputs")
    def outputs(limit: int = 60):
        out = {}
        for kind in ("images", "music", "docs"):
            files = sorted(paths.sub("outputs", kind).glob("*.*"), key=lambda p: p.stat().st_mtime, reverse=True)
            out[kind] = [f"/outputs/{kind}/{f.name}" for f in files if f.suffix in (".png", ".jpg", ".wav", ".mid", ".pdf")][:limit]
        return out

    @app.post("/api/prompt/enhance")
    async def prompt_enhance(text: str = Body(...), target: str = Body("image"), style: str = Body(""), use_llm: bool = Body(True)):
        from ..prompt_enhancer import enhance
        return await asyncio.to_thread(enhance, S(), text, target, style, use_llm)

    @app.get("/api/prompt/styles")
    def prompt_styles():
        from ..prompt_enhancer import STYLES
        return {k: v for k, v in STYLES.items() if k}

    @app.post("/api/avatar/upload")
    async def avatar_upload(file: UploadFile = File(...)):
        import re as _re
        name = (file.filename or "avatar.glb").replace("\\", "/").rsplit("/", 1)[-1]
        if not name.lower().endswith((".glb", ".gltf", ".vrm")):
            raise HTTPException(400, "Formatos admitidos: GLB, GLTF (embebido) o VRM")
        data = await file.read()
        if len(data) > 200_000_000:
            raise HTTPException(400, "Modelo demasiado grande (max. 200 MB)")
        safe = _re.sub(r"[^A-Za-z0-9._-]+", "_", name)
        dest = paths.sub("outputs", "avatars") / f"{int(time.time())}_{safe}"
        dest.write_bytes(data)
        return {"url": f"/outputs/avatars/{dest.name}", "size": len(data)}

    @app.get("/api/voice")
    def voice_info():
        return S().voice.info()

    @app.post("/api/voice/tts")
    async def tts(text: str = Body(...), rate: float = Body(1.0)):
        res = await asyncio.to_thread(S().voice.tts, text, rate)
        if not res:  # sin motor local: la UI usa la voz del navegador
            return Response(status_code=204, headers={"X-TTS-Engine": "browser"})
        return Response(res[0], media_type="audio/wav", headers={"X-TTS-Engine": res[1]})

    @app.post("/api/voice/stt")
    async def stt(file: UploadFile = File(...)):
        data = await file.read()
        suffix = "." + (file.filename or "a.webm").rsplit(".", 1)[-1]
        return await asyncio.to_thread(S().voice.stt, data, suffix)

    # ------------------------------------------------------------------ agentes
    @app.get("/api/agents")
    def agents():
        from dataclasses import asdict
        return [asdict(a) for a in S().agents.agents.values()]

    @app.put("/api/agents", dependencies=[Depends(require_admin)])
    def agent_save(data: dict = Body(...)):
        from dataclasses import asdict
        try:
            return asdict(S().agents.save(data))
        except (ValueError, TypeError) as e:
            raise HTTPException(400, str(e)) from e

    @app.delete("/api/agents/{aid}", dependencies=[Depends(require_admin)])
    def agent_delete(aid: str):
        try:
            S().agents.delete(aid)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        return {"ok": True}

    # ------------------------------------------------------------------ oficina (perfiles de pyme)
    @app.get("/api/office/profiles")
    def office_profiles():
        from .. import office
        return office.Office(S().config).list()

    @app.post("/api/office/profile", dependencies=[Depends(require_admin)])
    def office_apply(data: dict = Body(...)):
        from .. import office
        s = S()
        o = office.Office(s.config)
        try:
            if data.get("id"):
                prof = o.apply(str(data["id"]), reset_agents=bool(data.get("reset_agents")))
            else:
                o.clear()
                prof = None
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        s.agents.reload()
        for aid in s.agents.agents:
            s.graph.forget_view(aid)   # nombres nuevos: el ancla de cada subgrafo los sigue
        seeded = o.seed_graphs(s.graph, s.agents.ids())
        packs = office.install_packs(s)   # manual de GIMP... en los agentes nuevos que lo usan
        return {"profile": prof, "seeded": seeded, "packs": packs, "agents": s.agents.ids()}

    # ------------------------------------------------------------------ mesa de bolsa (oficina bolsa-inversiones)
    def _bolsa(fn, *a, **kw):
        try:
            return fn(*a, **kw)
        except PermissionError as e:
            raise HTTPException(403, str(e)) from e
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/bolsa")
    def bolsa_state(cartera: int = 0):
        from ..office import bolsa
        b = bolsa.get()
        res = _bolsa(b.resumen, cartera or None)
        return {"aviso": bolsa.AVISO, "confirmacion": bolsa.CONFIRMACION, "mercados": b.mercados(),
                "carteras": b.carteras(), "tarifas": {k: v["nombre"] for k, v in bolsa.TARIFAS.items()},
                "tipos": list(bolsa.ORDER_TYPES), "resumen": res,
                "ordenes": b.ordenes(res["cartera"]), "operaciones": b.operaciones(res["cartera"], 50)}

    @app.get("/api/bolsa/instrumentos")
    def bolsa_instruments(q: str = ""):
        from ..office import bolsa
        return bolsa.get().instrumentos(q)

    @app.get("/api/bolsa/cotizacion/{simbolo}")
    def bolsa_quote(simbolo: str):
        from ..office import bolsa
        b = bolsa.get()
        return {**_bolsa(b.precio, simbolo), "ticks": b.ticks(simbolo)}

    @app.post("/api/bolsa/costes")
    def bolsa_fees(data: dict = Body(...)):
        from ..office import bolsa
        b = bolsa.get()
        q = _bolsa(b.precio, str(data.get("simbolo", "")))
        lado = data.get("lado", "compra")
        px = float(data.get("precio") or (q["venta"] if lado == "compra" else q["compra"]))
        return _bolsa(b.costes, data.get("cartera") or None, q["simbolo"], lado, float(data.get("cantidad") or 0), px)

    @app.post("/api/bolsa/orden")
    def bolsa_order(data: dict = Body(...)):
        from ..office import bolsa
        return _bolsa(bolsa.get().ordenar, str(data.get("simbolo", "")), str(data.get("lado", "")),
                      data.get("cantidad", 0), data.get("tipo", "mercado"), data.get("limite"), data.get("stop"),
                      data.get("validez", "dia"), data.get("cartera") or None, origen="usuario")

    @app.post("/api/bolsa/orden/{oid}/cancelar")
    def bolsa_cancel(oid: int):
        from ..office import bolsa
        return _bolsa(bolsa.get().cancelar, oid)

    @app.post("/api/bolsa/orden/{oid}/confirmar", dependencies=[Depends(require_admin)])
    def bolsa_confirm(oid: int, confirmacion: str = Body("", embed=True)):
        """Solo una persona confirma una orden de cartera real; no se envia a ningun broker."""
        from ..office import bolsa
        return _bolsa(bolsa.get().confirmar, oid, confirmacion)

    @app.post("/api/bolsa/orden/{oid}/ejecucion", dependencies=[Depends(require_admin)])
    def bolsa_fill(oid: int, data: dict = Body(...)):
        from ..office import bolsa
        return _bolsa(bolsa.get().registrar_ejecucion, oid, data.get("precio", 0), data.get("comision_eur") or 0,
                      data.get("impuestos_eur"), data.get("fx") or None)

    @app.post("/api/bolsa/cartera")
    def bolsa_portfolio(data: dict = Body(...), admin: bool = Depends(admin_flag)):
        from ..office import bolsa
        if data.get("modo") == "real" and not admin:
            raise HTTPException(403, "Una cartera real requiere modo administrador")
        return _bolsa(bolsa.get().crear_cartera, str(data.get("nombre") or "Cartera simulada"),
                      data.get("capital", 10000), str(data.get("tarifa") or "broker-online"),
                      str(data.get("modo") or "simulado"))

    @app.post("/api/bolsa/cartera/{cid}/reiniciar")
    def bolsa_reset(cid: int, data: dict = Body(default={})):
        from ..office import bolsa
        return _bolsa(bolsa.get().reiniciar, cid, data.get("capital"))

    @app.post("/api/bolsa/instrumento")
    def bolsa_instrument(data: dict = Body(...)):
        from ..office import bolsa
        return _bolsa(bolsa.get().anadir_instrumento, str(data.get("simbolo", "")), str(data.get("nombre", "")),
                      str(data.get("mercado", "")), data.get("precio", 0), str(data.get("divisa", "")),
                      str(data.get("tipo") or "acción"), data.get("vol") or 0.25, data.get("spread_bps") or 5,
                      data.get("adv") or 1e6, data.get("impuesto_pct") or 0)

    @app.post("/api/bolsa/importar")
    def bolsa_import(data: dict = Body(...)):
        from ..office import bolsa
        return _bolsa(bolsa.get().importar_csv, str(data.get("simbolo", "")), str(data.get("csv", "")))

    @app.get("/api/bolsa/fiscal")
    def bolsa_tax(cartera: int = 0, anio: int = 0):
        from ..office import bolsa
        return _bolsa(bolsa.get().informe_fiscal, cartera or None, anio or None)

    # ------------------------------------------------------------------ herramientas
    @app.get("/api/tools")
    def tools():
        s = S()
        return {"tools": s.tools.list(), "policy": s.config.get("tools.policy"), "pending": s.supervisor.list_pending()}

    @app.post("/api/tools/{name}/enabled", dependencies=[Depends(require_admin)])
    def tool_enable(name: str, enabled: bool = Body(..., embed=True)):
        S().tools.set_enabled(name, enabled)
        return {"ok": True}

    @app.post("/api/approvals/{pid}")
    def approval(pid: str, approve: bool = Body(..., embed=True), admin: bool = Depends(admin_flag)):
        try:
            ok = S().supervisor.resolve(pid, approve, admin)
        except PermissionError as e:
            raise HTTPException(403, str(e)) from e
        hub.broadcast({"type": "approval_resolved", "id": pid, "approved": approve})
        return {"ok": ok}

    @app.get("/api/tools/audit", dependencies=[Depends(require_admin)])
    def audit(limit: int = 100):
        return S().supervisor.audit_log(limit)

    @app.get("/api/proposals")
    def proposals(status: str | None = None, admin: bool = Depends(admin_flag)):
        items = S().proposals.list(status)
        if not admin:
            items = [{k: v for k, v in p.items() if k != "code"} for p in items]
        return items

    @app.post("/api/proposals", dependencies=[Depends(require_admin)])
    def proposal_create(data: dict = Body(...)):
        try:
            return S().proposals.create(data["name"], data.get("description", ""), data["code"],
                                        risk=data.get("risk", "medium"), rationale=data.get("rationale", ""), author="admin")
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e)) from e

    @app.put("/api/proposals/{pid}", dependencies=[Depends(require_admin)])
    def proposal_update(pid: str, code: str = Body(..., embed=True)):
        return S().proposals.update_code(pid, code)

    @app.post("/api/proposals/{pid}/test", dependencies=[Depends(require_admin)])
    async def proposal_test(pid: str, args: dict = Body({}, embed=True)):
        return await asyncio.to_thread(S().proposals.test, pid, args)

    @app.post("/api/proposals/{pid}/approve", dependencies=[Depends(require_admin)])
    def proposal_approve(pid: str, note: str = Body("", embed=True)):
        try:
            return S().proposals.approve(pid, "admin", note)
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e)) from e

    @app.post("/api/proposals/{pid}/reject", dependencies=[Depends(require_admin)])
    def proposal_reject(pid: str, reason: str = Body(..., embed=True)):
        return S().proposals.reject(pid, reason)

    @app.delete("/api/tools/community/{name}", dependencies=[Depends(require_admin)])
    def tool_uninstall(name: str):
        return {"ok": S().proposals.uninstall(name)}

    # ------------------------------------------------------------------ modelos
    @app.get("/api/models")
    def models():
        s = S()
        return {"catalog": s.models.catalog.list(), "active": s.models.active(), "jobs": list(s.models.jobs.values()),
                "sources": [{"id": k, "name": v.name, "url": v.url} for k, v in s.models.sources.items()],
                "stt_models": STT_MODELS, "ollama": s.llm.backends["ollama"].models() if s.llm.is_available("ollama") else []}

    @app.get("/api/models/recommended")
    def models_recommended():
        s = S()
        return {"items": s.models.recommended(), "profile": {"tier": s.profile["tier"], **s.profile["llm"]}}

    @app.get("/api/models/search")
    async def models_search(source: str, q: str, kind: str = ""):
        try:
            return await asyncio.to_thread(S().models.search, source, q, kind)
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"{source}: {e}") from e

    @app.get("/api/models/files")
    async def models_files(source: str, repo: str):
        try:
            return await asyncio.to_thread(S().models.files, source, repo)
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"{source}: {e}") from e

    @app.post("/api/models/download")
    def models_download(source: str = Body(...), repo: str = Body(...), file: str = Body(""), kind: str = Body(""),
                        extras: bool = Body(True)):
        """Cualquier modelo de cualquier repositorio (las recomendaciones son orientativas)."""
        return {"job": S().models.download(source, repo, file, kind, extras)}

    @app.post("/api/models/download-repo")
    def models_download_repo(source: str = Body(...), repo: str = Body(...), kind: str = Body("")):
        if source not in ("huggingface", "modelscope"):
            raise HTTPException(400, "La descarga de repositorios completos esta disponible para Hugging Face y ModelScope")
        return {"job": S().models.download_repo(source, repo, kind)}

    @app.post("/api/models/recommended/{rid}")
    def models_download_rec(rid: str):
        return {"job": S().models.download_recommended(rid)}

    @app.post("/api/models/cancel/{jid}")
    def models_cancel(jid: str):
        S().models.cancel(jid)
        return {"ok": True}

    @app.post("/api/models/import", dependencies=[Depends(require_admin)])
    def models_import(path: str = Body(...), copy_file: bool = Body(False), kind: str = Body("")):
        try:
            return S().models.import_local(path, copy_file, kind)
        except FileNotFoundError as e:
            raise HTTPException(404, f"No existe: {e}") from e

    @app.post("/api/models/activate/{mid}")
    def models_activate(mid: str):
        s = S()
        try:
            entry = s.models.activate(mid)
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e)) from e
        s.llm.rebuild()
        return entry

    @app.post("/api/models/activate-spec", dependencies=[Depends(require_admin)])
    def models_activate_spec(spec: str = Body(..., embed=True)):
        """Activa un modelo externo: 'ollama:qwen3-vl:2b', 'openai:gpt-4o-mini', 'peer:<nodo>'."""
        s = S()
        s.config.set("llm.default_model", spec)
        s.llm.rebuild()
        return s.llm.status()

    @app.delete("/api/models/{mid}", dependencies=[Depends(require_admin)])
    def models_delete(mid: str, files: bool = False):
        S().models.delete(mid, files)
        return {"ok": True}

    # ------------------------------------------------------------------ cooperativo
    @app.get("/api/coop")
    def coop_status():
        return S().coop.status()

    @app.post("/api/coop/create", dependencies=[Depends(require_admin)])
    def coop_create(name: str = Body("", embed=True)):
        return S().coop.create_network(name)

    @app.post("/api/coop/join")
    def coop_join(code: str = Body(..., embed=True), admin: bool = Depends(admin_flag)):
        try:
            return S().coop.join(code)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/coop/invite", dependencies=[Depends(require_admin)])
    def coop_invite():
        return {"code": S().coop.invite()}

    @app.post("/api/coop/start")
    def coop_start():
        s = S()
        try:
            s.coop.start()
        except RuntimeError as e:
            raise HTTPException(400, str(e)) from e
        s.config.set("coop.enabled", True)
        return s.coop.status()

    @app.post("/api/coop/stop")
    def coop_stop():
        s = S()
        s.coop.stop()
        s.config.set("coop.enabled", False)
        return s.coop.status()

    @app.post("/api/coop/chat")
    def coop_chat(text: str = Body(..., embed=True)):
        try:
            return S().coop.send_chat(text)
        except RuntimeError as e:
            raise HTTPException(400, str(e)) from e

    @app.post("/api/coop/ping/{node}")
    async def coop_ping(node: str):
        try:
            return {"ms": await asyncio.to_thread(S().coop.ping, node)}
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, str(e)) from e

    @app.post("/api/coop/agent")
    async def coop_agent(node: str = Body(...), agent: str = Body("auto"), text: str = Body(...)):
        try:
            return {"answer": await asyncio.to_thread(S().coop.remote_agent, node, agent, text)}
        except Exception as e:  # noqa: BLE001
            raise HTTPException(400, str(e)) from e

    @app.post("/api/coop/share-proposal/{pid}", dependencies=[Depends(require_admin)])
    def coop_share_proposal(pid: str):
        s = S()
        s.coop.share_proposal(s.proposals.get(pid))
        return {"ok": True}

    @app.post("/api/coop/share-facts", dependencies=[Depends(require_admin)])
    def coop_share_facts():
        s = S()
        facts = [m["content"] for m in s.memory.list("fact", 30)]
        s.coop.share_facts(facts)
        return {"shared": len(facts)}

    # ------------------------------------------------------------------ acceso remoto por internet
    admin_dep = [Depends(lambda x_admin_token=Header(default=None): require_admin(x_admin_token))]

    @app.post("/api/remote/pair")
    def remote_pair(request: Request, code: str = Body(...), name: str = Body("")):
        """Canjea un codigo de emparejamiento por la llave del dispositivo (cookie segura + JSON para apps)."""
        from fastapi.responses import JSONResponse

        from ..remote import COOKIE
        ip = request.client.host if request.client else ""
        r = S().remote
        if r.limiter.blocked("pair:" + ip):
            raise HTTPException(429, "Demasiados intentos fallidos: espera 15 minutos")
        res = r.devices.pair(code, name, ip)
        if not res:
            r.limiter.fail("pair:" + ip)
            L.warning("Emparejamiento remoto rechazado desde %s", ip)
            raise HTTPException(401, "Codigo incorrecto o caducado (pide uno nuevo en tu Nexus-Office)")
        token, dev = res
        r.limiter.reset("pair:" + ip)
        L.info("Dispositivo emparejado: %s (%s)", dev["name"], ip)
        hub.broadcast({"type": "remote", "event": "paired", "device": dev})
        resp = JSONResponse({"ok": True, "device": dev, "token": token})
        resp.set_cookie(COOKIE, token, max_age=31536000, httponly=True, secure=bool(request.scope.get("nexus_remote")),
                        samesite="strict", path="/")
        return resp

    @app.get("/api/remote", dependencies=admin_dep)
    def remote_status():
        return S().remote.status()

    @app.post("/api/remote/enable", dependencies=admin_dep)
    def remote_enable(enabled: bool = Body(...), port: int = Body(8443)):
        if not 1024 <= port <= 65535 or port == S().config.get("server.port", 8775):
            raise HTTPException(400, "Puerto no valido (1024-65535 y distinto del puerto local)")
        cfg = S().config
        cfg.set("remote.enabled", enabled)
        cfg.set("remote.port", port)
        S().remote.cert()                                # crea el certificado ya para poder mostrar la huella
        running = S().remote.listening_port
        return {**S().remote.status(), "restart_required": (running != port) if enabled else running is not None}

    @app.post("/api/remote/pairing", dependencies=admin_dep)
    def remote_pairing(name: str = Body("", embed=True)):
        r = S().remote
        p = r.devices.new_pairing(name)
        urls = r.urls()
        return {**p, "invite": r.invite(p, name), "urls": urls, "fingerprint": r.status()["fingerprint"],
                "links": [f"{u['url']}/?pair={p['code']}" for u in urls]}

    @app.delete("/api/remote/devices/{dev_id}", dependencies=admin_dep)
    def remote_revoke(dev_id: str):
        if not S().remote.devices.revoke(dev_id):
            raise HTTPException(404, "Dispositivo no encontrado")
        for w in list(hub.sockets):                      # corta al momento sus conexiones abiertas
            if w.scope.get("nexus_device") == dev_id and hub.loop:
                asyncio.run_coroutine_threadsafe(w.close(code=4401), hub.loop)
        return {"ok": True}

    @app.post("/api/remote/upnp", dependencies=admin_dep)
    def remote_upnp(open: bool = Body(..., embed=True)):
        from ..remote.upnp import Gateway, is_cgnat_or_private
        r = S().remote
        try:
            gw = Gateway.discover()
            ext = gw.external_ip()
            if open:
                internal = gw.add_mapping(r.port())
                r.upnp = {"mapped": True, "external_ip": ext, "internal": internal, "error": "", "cgnat": is_cgnat_or_private(ext)}
            else:
                gw.delete_mapping(r.port())
                r.upnp = {"mapped": False, "external_ip": ext, "error": "", "cgnat": is_cgnat_or_private(ext)}
        except Exception as e:  # noqa: BLE001
            r.upnp = {**r.upnp, "error": str(e)}
        return r.status()

    @app.post("/api/remote/public-ip", dependencies=admin_dep)
    def remote_public_ip():
        """IP publica vista desde internet (servicio externo ipify; solo cuando el administrador lo pide)."""
        import httpx

        from ..remote.upnp import is_cgnat_or_private
        r = S().remote
        try:
            r.public_ip = httpx.get("https://api.ipify.org", timeout=6).text.strip()
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"No se pudo consultar la IP publica: {e}") from e
        ext = r.upnp.get("external_ip")
        r.behind_cgnat = (bool(ext) and ext != r.public_ip) or is_cgnat_or_private(ext or r.public_ip)
        return r.status()

    # ------------------------------------------------------------------ VPN cifrada (WireGuard)
    @app.get("/api/vpn", dependencies=admin_dep)
    def vpn_status():
        return S().vpn.status()

    @app.post("/api/vpn/enable", dependencies=admin_dep)
    def vpn_enable(enabled: bool = Body(...), port: int = Body(51820)):
        """Activa la VPN: genera las claves y el tunel del PC. Nexus-Office debe escuchar en la red (clave de red) para
        que el movil lo alcance por la direccion del tunel; si no lo hacia, hace falta reiniciar."""
        if not 1 <= port <= 65535:
            raise HTTPException(400, "Puerto no valido")
        cfg, v = S().config, S().vpn
        cfg.set("vpn.enabled", enabled)
        cfg.set("vpn.port", port)
        restart = False
        if enabled:
            v.write_conf()
            if cfg.get("server.host", "127.0.0.1") in ("127.0.0.1", "localhost"):
                cfg.set("server.host", "0.0.0.0")
                restart = True
            lan_key()
        return {**v.status(), "restart_required": restart}

    @app.post("/api/vpn/peers", dependencies=admin_dep)
    def vpn_add_peer(name: str = Body(""), lookup_public_ip: bool = Body(False)):
        """Nuevo dispositivo: devuelve el codigo NEXUSV1:... para la app (incluye su clave privada: se muestra una vez)."""
        r, v = S().remote, S().vpn
        if not S().config.get("vpn.enabled", False):
            raise HTTPException(400, "Activa primero la VPN")
        public_ip = r.upnp.get("external_ip") or r.public_ip
        if lookup_public_ip and not public_ip:
            import httpx
            try:
                public_ip = r.public_ip = httpx.get("https://api.ipify.org", timeout=6).text.strip()
            except Exception as e:  # noqa: BLE001
                L.info("VPN: sin IP publica (%s)", e)
        try:
            return v.add_peer(name, lan_key(), public_ip)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.delete("/api/vpn/peers/{peer_id}", dependencies=admin_dep)
    def vpn_remove_peer(peer_id: str):
        if not S().vpn.remove_peer(peer_id):
            raise HTTPException(404, "Dispositivo no encontrado")
        return {**S().vpn.status(), "restart_tunnel": True}

    @app.get("/api/vpn/server.conf", dependencies=admin_dep)
    def vpn_server_conf():
        from ..remote.vpn import IFACE
        return Response(S().vpn.server_conf(), media_type="text/plain; charset=utf-8",
                        headers={"Content-Disposition": f"attachment; filename={IFACE}.conf"})

    @app.post("/api/vpn/apply", dependencies=admin_dep)
    def vpn_apply():
        return {**S().vpn.apply(), "tunnel_up": S().vpn.tunnel_up()}

    @app.post("/api/vpn/upnp", dependencies=admin_dep)
    def vpn_upnp(open: bool = Body(..., embed=True)):
        from ..remote.upnp import Gateway
        port = S().vpn.port()
        try:
            gw = Gateway.discover()
            if open:
                internal = gw.add_mapping(port, protocol="UDP")
                return {"mapped": True, "external_ip": gw.external_ip(), "internal": internal, "port": port}
            gw.delete_mapping(port, protocol="UDP")
            return {"mapped": False, "port": port}
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"UPnP: {e}") from e

    @app.get("/api/remote/router", dependencies=admin_dep)
    def remote_router():
        """Asistente del router: puerta de enlace, pagina de configuracion, marca, UPnP y la regla exacta a crear."""
        from ..remote import router
        r = S().remote
        return {**router.detect(r.port()), "enabled": bool(S().config.get("remote.enabled", False)),
                "listening": r.listening_port is not None, "port": r.port()}

    @app.post("/api/remote/check", dependencies=admin_dep)
    def remote_check():
        """Comprueba desde este equipo si el puerto responde en la IP publica (ipify, solo a peticion)."""
        import httpx

        from ..remote import router
        from ..remote.upnp import is_cgnat_or_private
        r = S().remote
        try:
            r.public_ip = httpx.get("https://api.ipify.org", timeout=6).text.strip()
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"No se pudo consultar la IP publica: {e}") from e
        ext = r.upnp.get("external_ip")
        r.behind_cgnat = (bool(ext) and ext != r.public_ip) or is_cgnat_or_private(ext or r.public_ip)
        ok = r.listening_port is not None and router.reachable_from_outside(r.public_ip, r.port())
        return {"public_ip": r.public_ip, "port": r.port(), "listening": r.listening_port is not None,
                "reachable": ok, "behind_cgnat": r.behind_cgnat, "url": f"https://{r.public_ip}:{r.port()}"}

    @app.post("/api/remote/certificate", dependencies=admin_dep)
    def remote_new_cert():
        """Certificado nuevo: invalida la huella anclada (habra que volver a emparejar las apps)."""
        S().remote.cert(regenerate=True)
        return {**S().remote.status(), "restart_required": True}

    # ------------------------------------------------------------------ websocket
    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket):
        if not ws.scope.get("nexus_remote") and not lan_ok(ws.client.host if ws.client else "127.0.0.1", ws.headers, ws.query_params, ws.cookies):
            await ws.close(code=4401)
            return
        await ws.accept()
        hub.sockets.add(ws)
        loop = asyncio.get_running_loop()
        s = S()

        def emit(event: dict) -> None:
            asyncio.run_coroutine_threadsafe(_safe_send(ws, event), loop)

        async def run_chat(data: dict) -> None:
            sid = s.memory.ensure_session(data.get("session_id"), data.get("text", "")[:50])
            emit({"type": "session", "session_id": sid})
            images = [i.split(",", 1)[-1] for i in data.get("images", [])][:4]
            try:
                await asyncio.to_thread(s.agents.run, data.get("agent", "auto"), data.get("text", ""), sid, images, emit,
                                        live=bool(data.get("live")))
            except Exception as e:  # noqa: BLE001
                L.exception("Error en el chat")
                emit({"type": "error", "message": str(e)})
                emit({"type": "state", "state": "idle"})

        try:
            await _safe_send(ws, {"type": "hello", "version": __version__})
            while True:
                data = json.loads(await ws.receive_text())
                if data.get("type") == "chat":
                    asyncio.create_task(run_chat(data))
                elif data.get("type") == "ping":
                    await _safe_send(ws, {"type": "pong"})
        except WebSocketDisconnect:
            pass
        finally:
            hub.sockets.discard(ws)

    return app
