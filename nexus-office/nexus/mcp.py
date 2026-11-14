"""MCP (Model Context Protocol) en Agente IA, en los dos sentidos.

* Servidor: las herramientas de Agente IA para otros programas con MCP (asistentes de escritorio, editores, otros
  agentes). Transportes: HTTP (POST /mcp, JSON-RPC 2.0, una peticion o un lote) y stdio (``nexus mcp`` lee JSON-RPC
  por la entrada estandar y lo pasa al /mcp del servidor local). Cada llamada pasa por el supervisor de herramientas
  como cualquier agente: politica por riesgo, aprobaciones y auditoria. Fuera del propio equipo hace falta mcp.token.

* Cliente: los agentes usan herramientas de servidores MCP externos (mcp.servidores: una orden que se arranca con
  stdio o una URL HTTP). Entran en el registro como ``mcp_<servidor>_<nombre>`` con el riesgo que diga la
  configuracion (medio por defecto).
"""
from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.request

from . import __version__, log
from .tools.base import ToolContext, ToolSpec

L = log.get("mcp")
PROTOCOLO = "2025-06-18"
VERSIONES = ("2025-06-18", "2025-03-26", "2024-11-05")


# ---------------------------------------------------------------- servidor
def _error(mid, code: int, msg: str) -> dict:
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": msg}}


def resultado_mcp(r) -> dict:
    """Resultado de una herramienta de Agente IA en formato MCP (content[] de texto; isError si fallo)."""
    es_error = isinstance(r, dict) and bool(r.get("error"))
    texto = r if isinstance(r, str) else json.dumps(r, ensure_ascii=False, default=str)
    out = {"content": [{"type": "text", "text": texto}], "isError": es_error}
    if isinstance(r, dict) and not es_error:
        out["structuredContent"] = json.loads(json.dumps(r, ensure_ascii=False, default=str))
    return out


def _uno(s, m: dict, cliente: str, remoto: bool):
    if not isinstance(m, dict) or m.get("jsonrpc") != "2.0" or not isinstance(m.get("method"), str):
        return _error(m.get("id") if isinstance(m, dict) else None, -32600, "Petición JSON-RPC no válida")
    mid, metodo, params = m.get("id"), m["method"], m.get("params") or {}
    if "id" not in m:   # notificacion: sin respuesta
        return None
    if metodo == "initialize":
        pedida = str(params.get("protocolVersion") or "")
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": pedida if pedida in VERSIONES else PROTOCOLO,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "agente-ia", "title": "Agente IA", "version": __version__},
            "instructions": "Herramientas de Agente IA (oficina, documentos, memoria, imagen, bolsa simulada, web). "
                            "Las de riesgo medio o alto piden aprobación en la interfaz de Agente IA."}}
    if metodo == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    if metodo == "tools/list":
        ocultas = set(s.config.get("mcp.ocultas", []) or [])
        tools = [{"name": t.name, "description": t.description, "inputSchema": t.parameters or {"type": "object"},
                  "annotations": {"readOnlyHint": t.risk == "low", "destructiveHint": t.risk == "high"}}
                 for t in sorted(s.tools.for_agent(["*"]), key=lambda t: t.name)
                 if t.name not in ocultas and not t.name.startswith("mcp_")]   # no se reexportan las de otros MCP
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": tools}}
    if metodo == "tools/call":
        nombre = str(params.get("name") or "")
        spec = s.tools.get(nombre)
        if not spec or not spec.enabled or nombre.startswith("mcp_") or nombre in (s.config.get("mcp.ocultas", []) or []):
            return _error(mid, -32602, f"Herramienta desconocida: {nombre}")
        args = params.get("arguments") or {}
        if not isinstance(args, dict):
            return _error(mid, -32602, "arguments debe ser un objeto")
        ctx = ToolContext(services=s, agent=f"mcp:{cliente}"[:40], session_id="mcp", remote=remoto)
        return {"jsonrpc": "2.0", "id": mid, "result": resultado_mcp(s.supervisor.execute(nombre, args, ctx))}
    if metodo in ("resources/list", "prompts/list", "resources/templates/list"):
        clave = {"resources/list": "resources", "prompts/list": "prompts"}.get(metodo, "resourceTemplates")
        return {"jsonrpc": "2.0", "id": mid, "result": {clave: []}}
    return _error(mid, -32601, f"Método no admitido: {metodo}")


def atender(s, mensaje, cliente: str = "local", remoto: bool = False):
    """Atiende un mensaje JSON-RPC o un lote. Devuelve None si solo habia notificaciones."""
    if isinstance(mensaje, list):
        if not mensaje:
            return _error(None, -32600, "Lote vacío")
        out = [r for r in (_uno(s, m, cliente, remoto) for m in mensaje) if r is not None]
        return out or None
    return _uno(s, mensaje, cliente, remoto)


def main_stdio(url: str = "", token: str = "") -> int:
    """``nexus mcp [--url http://127.0.0.1:8775] [--token T]``: puente stdio -> HTTP."""
    from .config import get_config
    cfg = get_config()
    url = (url or f"http://127.0.0.1:{cfg.get('server.port', 8775)}").rstrip("/") + "/mcp"
    token = token or cfg.get("mcp.token", "")
    for linea in sys.stdin:
        linea = linea.strip()
        if not linea:
            continue
        try:
            msg = json.loads(linea)
        except ValueError:
            resp = _error(None, -32700, "JSON no válido")
        else:
            try:
                req = urllib.request.Request(url, json.dumps(msg).encode(), {"Content-Type": "application/json",
                                             **({"Authorization": f"Bearer {token}"} if token else {})})
                with urllib.request.urlopen(req, timeout=600) as r:
                    cuerpo = r.read()
                resp = json.loads(cuerpo) if cuerpo.strip() else None
            except (OSError, ValueError) as e:
                mid = msg.get("id") if isinstance(msg, dict) else None
                resp = _error(mid, -32000, f"Agente IA no responde en {url}: {e}") if mid is not None else None
        if resp is not None:
            sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
            sys.stdout.flush()
    return 0


# ---------------------------------------------------------------- cliente
class CanalStdio:
    """Servidor MCP arrancado como proceso hijo: JSON-RPC por lineas en stdin/stdout."""

    def __init__(self, orden: list[str], env: dict | None = None) -> None:
        self.proc = subprocess.Popen(orden, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     env={**os.environ, **(env or {})}, text=True, encoding="utf-8", bufsize=1,
                                     creationflags=0x08000000 if os.name == "nt" else 0)
        self._esperas: dict = {}
        self._mu = threading.Lock()
        threading.Thread(target=self._leer, name="nexus-mcp-lee", daemon=True).start()

    def _leer(self) -> None:
        try:
            for linea in self.proc.stdout:
                try:
                    msg = json.loads(linea)
                except ValueError:
                    continue
                if isinstance(msg, dict) and "id" in msg and ("result" in msg or "error" in msg):
                    with self._mu:
                        q = self._esperas.pop(msg["id"], None)
                    if q is not None:
                        q.put(msg)
        except (OSError, ValueError):
            pass
        with self._mu:   # proceso terminado: se despierta a quien espere
            for q in self._esperas.values():
                q.put(None)
            self._esperas.clear()

    def pedir(self, peticion: dict, timeout_s: float) -> dict:
        q: queue.Queue = queue.Queue()
        with self._mu:
            self._esperas[peticion["id"]] = q
        self.avisar(peticion)
        try:
            msg = q.get(timeout=timeout_s)
        except queue.Empty:
            with self._mu:
                self._esperas.pop(peticion["id"], None)
            raise TimeoutError(f"el servidor MCP no respondió en {timeout_s:.0f} s") from None
        if msg is None:
            raise ConnectionError("el servidor MCP se cerró")
        return msg

    def avisar(self, notificacion: dict) -> None:
        try:
            self.proc.stdin.write(json.dumps(notificacion, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError) as e:
            raise ConnectionError(f"no se pudo escribir al servidor MCP: {e}") from None

    def vivo(self) -> bool:
        return self.proc.poll() is None

    def cerrar(self) -> None:
        try:
            self.proc.terminate()
        except OSError:
            pass


class CanalHttp:
    """Servidor MCP por HTTP (Streamable HTTP: respuesta JSON o text/event-stream)."""

    def __init__(self, url: str, cabeceras: dict | None = None) -> None:
        self.url, self.cabeceras, self.sesion = url, dict(cabeceras or {}), ""

    def _post(self, cuerpo: dict, timeout_s: float):
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", **self.cabeceras}
        if self.sesion:
            h["Mcp-Session-Id"] = self.sesion
        req = urllib.request.Request(self.url, json.dumps(cuerpo).encode(), h)
        with urllib.request.urlopen(req, timeout=timeout_s) as r:
            self.sesion = r.headers.get("Mcp-Session-Id") or self.sesion
            return r.headers.get("Content-Type", ""), r.read().decode("utf-8", "replace")

    def pedir(self, peticion: dict, timeout_s: float) -> dict:
        try:
            tipo, texto = self._post(peticion, timeout_s)
        except urllib.error.HTTPError as e:
            raise ConnectionError(f"HTTP {e.code} del servidor MCP") from None
        except OSError as e:
            raise ConnectionError(f"servidor MCP inalcanzable: {e}") from None
        if "text/event-stream" in tipo:
            for bloque in texto.split("\n\n"):
                datos = "\n".join(x[5:].lstrip() for x in bloque.split("\n") if x.startswith("data:"))
                try:
                    msg = json.loads(datos)
                except ValueError:
                    continue
                if isinstance(msg, dict) and msg.get("id") == peticion["id"]:
                    return msg
            raise ConnectionError("el servidor MCP no devolvió la respuesta")
        msg = json.loads(texto)
        if isinstance(msg, list):
            msg = next((x for x in msg if isinstance(x, dict) and x.get("id") == peticion["id"]), {})
        return msg

    def avisar(self, notificacion: dict) -> None:
        try:
            self._post(notificacion, 15)
        except (OSError, ValueError):
            pass

    def vivo(self) -> bool:
        return True

    def cerrar(self) -> None:
        pass


class Cliente:
    def __init__(self, nombre: str, canal) -> None:
        self.nombre, self.canal = nombre, canal
        self.info_servidor: dict = {}
        self._n = 0
        self._mu = threading.Lock()

    def _pedir(self, metodo: str, params: dict | None = None, timeout_s: float = 30):
        with self._mu:
            self._n += 1
            mid = self._n
        msg = self.canal.pedir({"jsonrpc": "2.0", "id": mid, "method": metodo, **({"params": params} if params is not None
                                                                                else {})}, timeout_s)
        if msg.get("error"):
            raise RuntimeError(str(msg["error"].get("message") or msg["error"]))
        return msg.get("result") or {}

    def iniciar(self) -> dict:
        r = self._pedir("initialize", {"protocolVersion": PROTOCOLO, "capabilities": {},
                                       "clientInfo": {"name": "agente-ia", "version": __version__}})
        self.info_servidor = r.get("serverInfo") or {}
        self.canal.avisar({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return r

    def herramientas(self) -> list[dict]:
        out, cursor = [], None
        for _ in range(50):   # todas las paginas
            r = self._pedir("tools/list", {"cursor": cursor} if cursor else {})
            out += r.get("tools") or []
            cursor = r.get("nextCursor")
            if not cursor:
                break
        return out

    def llamar(self, herramienta: str, args: dict, timeout_s: float = 120) -> dict:
        return self._pedir("tools/call", {"name": herramienta, "arguments": args or {}}, timeout_s)

    def vivo(self) -> bool:
        return self.canal.vivo()


def nombre_herramienta(servidor: str, herramienta: str) -> str:
    """Nombre valido para el registro: mcp_<servidor>_<herramienta> con [a-z0-9_]."""
    limpio = lambda x: re.sub(r"_+", "_", re.sub(r"[^a-z0-9]", "_", x.lower())).strip("_")  # noqa: E731
    return f"mcp_{limpio(servidor)[:20]}_{limpio(herramienta)[:40]}"


def resultado_a_json(r: dict) -> dict:
    """Texto de un resultado MCP (content[]) para el agente; {"error"} si isError."""
    partes = []
    for c in r.get("content") or []:
        if c.get("type") == "text":
            partes.append(c.get("text", ""))
        elif c.get("type") in ("image", "audio"):
            partes.append(f"[{c['type']} {c.get('mimeType', '')}]")
        elif c.get("type") == "resource":
            partes.append((c.get("resource") or {}).get("text") or f"[recurso {(c.get('resource') or {}).get('uri', '')}]")
    texto = "\n".join(p for p in partes if p)[:20000]
    if r.get("isError"):
        return {"error": texto or "el servidor MCP devolvió un error"}
    if isinstance(r.get("structuredContent"), dict):
        return {"resultado": r["structuredContent"], **({"texto": texto} if texto else {})}
    return {"resultado": texto}


class Gestor:
    """Clientes de mcp.servidores y sus herramientas en el registro."""

    def __init__(self, services) -> None:
        self.s = services
        self._mu = threading.Lock()
        self.clientes: dict[str, Cliente] = {}
        self._estado: dict = {}
        self._specs: dict[str, ToolSpec] = {}
        reg = services.tools
        if not hasattr(reg, "_reload_base"):   # las externas sobreviven a reload() del registro
            reg._reload_base = reg.reload

            def reload_con_externas(_reg=reg, _g=self):
                _reg._reload_base()
                with _reg._lock:
                    _reg.tools.update({k: v for k, v in _g._specs.items() if k not in _reg.tools})
            reg.reload = reload_con_externas

    def conectar(self) -> None:
        """(Re)conecta todos en segundo plano y registra sus herramientas."""
        threading.Thread(target=self.conectar_ya, name="nexus-mcp", daemon=True).start()

    def conectar_ya(self) -> None:
        servidores = self.s.config.get("mcp.servidores", []) or []
        with self._mu:
            for c in self.clientes.values():
                c.canal.cerrar()
            self.clientes, estado, specs = {}, {}, {}
            for sv in servidores:
                if not isinstance(sv, dict) or not sv.get("nombre"):
                    continue
                nombre = str(sv["nombre"])
                if sv.get("activo") is False:
                    estado[nombre] = {"conectado": False, "error": "", "herramientas": [], "activo": False}
                    continue
                try:
                    if sv.get("url"):
                        canal = CanalHttp(str(sv["url"]), sv.get("cabeceras") or {})
                    else:
                        orden = sv.get("orden") or []
                        if isinstance(orden, str):
                            import shlex
                            orden = shlex.split(orden, posix=os.name != "nt")
                        if not orden:
                            raise ValueError("falta la orden o la URL")
                        canal = CanalStdio([str(x) for x in orden], sv.get("env") or {})
                    cli = Cliente(nombre, canal)
                    cli.iniciar()
                    nombres = []
                    riesgo = sv.get("riesgo", "medium") if sv.get("riesgo") in ("low", "medium", "high") else "medium"
                    for t in cli.herramientas():
                        reg = nombre_herramienta(nombre, t.get("name", ""))
                        specs[reg] = ToolSpec(
                            name=reg, description=f"[MCP {nombre}] {t.get('description') or t.get('name')}"[:600],
                            parameters=t.get("inputSchema") or {"type": "object", "properties": {}}, risk=riesgo,
                            permissions=["network"], author=nombre, source="mcp",
                            func=lambda args, ctx, _c=cli, _n=t.get("name", ""): resultado_a_json(_c.llamar(_n, args)),
                            timeout=float(sv.get("timeout_s", 120)))
                        nombres.append(reg)
                    self.clientes[nombre] = cli
                    estado[nombre] = {"conectado": True, "error": "", "herramientas": nombres, "activo": True,
                                      "servidor": cli.info_servidor}
                except Exception as e:  # noqa: BLE001
                    L.warning("Servidor MCP %s: %s", nombre, e)
                    estado[nombre] = {"conectado": False, "error": f"{type(e).__name__}: {e}", "herramientas": [],
                                      "activo": True}
            reg = self.s.tools
            with reg._lock:
                for k in list(reg.tools):
                    if reg.tools[k].source == "mcp":
                        del reg.tools[k]
                for k, v in specs.items():
                    if k not in reg.tools:
                        v.enabled = k not in reg._disabled
                        reg.tools[k] = v
            self._specs, self._estado = specs, estado

    def estado(self) -> dict:
        with self._mu:
            for n, c in self.clientes.items():
                if n in self._estado and not c.vivo():
                    self._estado[n].update(conectado=False, error="el servidor se cerró")
            return json.loads(json.dumps(self._estado, default=str))

    def cerrar(self) -> None:
        with self._mu:
            for c in self.clientes.values():
                c.canal.cerrar()
