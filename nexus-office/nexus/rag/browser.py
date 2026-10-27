"""Leer paginas como un navegador: Chrome, Edge, Chromium o Brave sin interfaz ejecutan el JavaScript y Nexus lee el DOM.

Muchas webs (React, Vue, Angular, tiendas, documentacion moderna...) mandan un HTML casi vacio y pintan el contenido con
JavaScript; con una descarga normal el RAG solo veia «Enable JavaScript». Aqui:

* ``find()`` busca un navegador del equipo (o el que diga ``rag.browser_path``), sin instalar nada: Windows trae Edge,
  y en Linux/macOS vale cualquier Chromium. Si no hay ninguno, todo sigue funcionando con la descarga normal.
* ``needs_browser(html)`` decide si una pagina necesita navegador (poco texto y mucho script, raices de SPA, avisos de
  «activa JavaScript»).
* ``render(url)`` abre la pagina en modo ``--headless``, deja correr el JavaScript un tiempo y devuelve el DOM final.
* ``maybe_render(url, html)`` es lo que usan el rastreador, «URL a PDF» y la herramienta web de los agentes, segun
  ``rag.browser``: ``auto`` (solo si hace falta), ``siempre`` o ``nunca``.

Seguridad: el navegador sale a internet a traves de un proxy local que solo deja pasar direcciones publicas (salvo que
el administrador permita las privadas con ``rag.allow_private_urls``), asi una pagina no puede usar el JavaScript para
llegar a la red local. Perfil temporal y vacio en cada lectura: sin cookies ni sesiones del usuario.
"""
from __future__ import annotations

import ipaddress
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import threading
from pathlib import Path
from urllib.parse import urlparse

from .. import log

L = log.get("navegador")

MODES = ("auto", "siempre", "nunca")
TIMEOUT_S = 40
BUDGET_MS = 8000          # tiempo virtual que se deja al JavaScript antes de leer el DOM

_NAMES = ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "chrome", "microsoft-edge",
          "microsoft-edge-stable", "msedge", "brave-browser", "brave")
_found: dict[str, str | None] = {}
_slots = threading.BoundedSemaphore(2)   # como mucho dos navegadores a la vez (un rastreo pide muchas paginas en paralelo)
_free, _free_lock = [1, 0], threading.Lock()   # un perfil por hueco: dos Chrome no pueden compartir perfil


def _candidates() -> list[str]:
    out: list[str] = []
    if sys.platform == "win32":
        for base in filter(None, (os.environ.get("PROGRAMFILES(X86)"), os.environ.get("PROGRAMFILES"),
                                  os.environ.get("LOCALAPPDATA"))):
            out += [rf"{base}\Microsoft\Edge\Application\msedge.exe", rf"{base}\Google\Chrome\Application\chrome.exe",
                    rf"{base}\BraveSoftware\Brave-Browser\Application\brave.exe", rf"{base}\Chromium\Application\chrome.exe"]
    elif sys.platform == "darwin":
        out += ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                "/Applications/Chromium.app/Contents/MacOS/Chromium",
                "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"]
    out += [w for n in _NAMES if (w := shutil.which(n))]
    # Chromium de Playwright, si esta instalado (equipos de desarrollo y CI)
    for root in filter(None, (os.environ.get("PLAYWRIGHT_BROWSERS_PATH"), "/opt/pw-browsers",
                              str(Path.home() / ".cache" / "ms-playwright"),
                              str(Path(os.environ.get("LOCALAPPDATA", "")) / "ms-playwright") if sys.platform == "win32" else "")):
        p = Path(root)
        if p.is_dir():
            out += [str(x) for x in sorted(p.glob("chromium_headless_shell-*/chrome-headless-shell-*/chrome-headless-shell*"), reverse=True)
                    if x.name in ("chrome-headless-shell", "chrome-headless-shell.exe")]
            out += [str(x) for x in sorted(p.glob("chromium-*/chrome-*/chrome*"), reverse=True)
                    if x.name in ("chrome", "chrome.exe")]
    return out


def find(preferred: str = "") -> str | None:
    """Ruta de un navegador Chromium del equipo (cacheada); None si no hay (Android, servidores minimos...)."""
    if os.environ.get("NEXUS_ANDROID"):
        return None
    key = preferred or ""
    if key in _found:
        return _found[key]
    exe = None
    for c in ([preferred] if preferred else []) + _candidates():
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            exe = c
            break
    _found[key] = exe
    if exe:
        L.info("Navegador para leer paginas con JavaScript: %s", exe)
    return exe


_SPA = re.compile(r'<div[^>]+id=["\'](root|app|__next|__nuxt|svelte|main-app|ember\w*)["\'][^>]*>\s*</div>'
                  r'|<app-root|ng-version=|data-reactroot|window\.__NUXT__|__NEXT_DATA__', re.I)
_NEEDS_JS = re.compile(r"(enable|activa|habilita|turn on)\s+(javascript|js)|requires? javascript|necesita javascript"
                       r"|javascript (is )?(required|disabled)", re.I)


def needs_browser(raw: str) -> bool:
    """La pagina parece pintarse con JavaScript: poco texto visible y muchos scripts, una raiz de SPA vacia o un aviso."""
    if not raw or "<" not in raw[:2000]:
        return False
    body = re.sub(r"(?is)<(script|style|noscript|svg|template|head)\b.*?</\1>", " ", raw)
    words = len(re.sub(r"<[^>]+>", " ", body).split())
    scripts = len(re.findall(r"(?i)<script\b", raw))
    if words < 60 and scripts >= 2:
        return True
    if _SPA.search(raw) and words < 300:
        return True
    noscript = " ".join(re.findall(r"(?is)<noscript\b[^>]*>(.*?)</noscript>", raw))
    return bool(_NEEDS_JS.search(noscript)) and words < 400


# ------------------------------------------------------------------------------------------ proxy de salida
def _host_ok(host: str) -> bool:
    try:
        infos = socket.getaddrinfo(host, None)
    except OSError:
        return False
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        if ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_reserved or ip.is_multicast or ip.is_unspecified:
            return False
    return True


class _GuardProxy:
    """Proxy HTTP minimo (CONNECT para https y GET absoluto para http) que solo conecta con direcciones publicas."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        self.blocked: list[str] = []
        self._stop = False
        threading.Thread(target=self._serve, daemon=True, name="nexus-navegador-proxy").start()

    def close(self) -> None:
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass

    def _serve(self) -> None:
        while not self._stop:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(c,), daemon=True).start()

    def _handle(self, c: socket.socket) -> None:
        up = None
        try:
            c.settimeout(TIMEOUT_S)
            head = b""
            while b"\r\n\r\n" not in head and len(head) < 65536:
                chunk = c.recv(4096)
                if not chunk:
                    return
                head += chunk
            line = head.split(b"\r\n", 1)[0].decode("latin-1")
            method, target, _ = (line.split(" ") + ["", ""])[:3]
            if method == "CONNECT":
                host, _, port = target.rpartition(":")
                port = int(port or 443)
            else:
                u = urlparse(target)
                host, port = u.hostname or "", u.port or 80
            host = host.strip("[]")
            if not host or not _host_ok(host):
                self.blocked.append(host)
                c.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            up = socket.create_connection((host, port), timeout=TIMEOUT_S)
            if method == "CONNECT":
                c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                rest = head.split(b"\r\n\r\n", 1)[1]
            else:   # peticion http: se reenvia con la ruta relativa
                u = urlparse(target)
                path = (u.path or "/") + (f"?{u.query}" if u.query else "")
                rest = head.replace(target.encode("latin-1"), path.encode("latin-1"), 1)
            if rest:
                up.sendall(rest)
            self._pipe(c, up)
        except (OSError, ValueError):
            pass
        finally:
            for s in (c, up):
                try:
                    s and s.close()
                except OSError:
                    pass

    @staticmethod
    def _pipe(a: socket.socket, b: socket.socket) -> None:
        socks = [a, b]
        while True:
            r, _, x = select.select(socks, [], socks, TIMEOUT_S)
            if x or not r:
                return
            for s in r:
                data = s.recv(65536)
                if not data:
                    return
                (b if s is a else a).sendall(data)


# ------------------------------------------------------------------------------------------ lectura
def render(url: str, exe: str | None = None, budget_ms: int = BUDGET_MS, allow_private: bool | None = None) -> str:
    """DOM final de la pagina tras ejecutar su JavaScript. Lanza RuntimeError si no hay navegador o falla."""
    exe = exe or find()
    if not exe:
        raise RuntimeError("no hay ningun navegador Chrome, Edge o Chromium en este equipo")
    if allow_private is None:
        from ..tools.builtin.web_tools import _allow_private
        allow_private = _allow_private()
    with _slots:
        return _render(url, exe, budget_ms, allow_private)


def _profile() -> Path:
    """Perfil propio de Nexus (nunca el del usuario), uno por hueco. Se reutiliza porque Chrome tarda mucho (o se
    queda colgado en algunos equipos) la primera vez que prepara un perfil nuevo; antes de cada lectura se borran
    cookies, almacenamiento e historial, asi que cada pagina se abre como en una ventana privada."""
    with _free_lock:
        slot = _free.pop()
    from .. import paths
    prof = paths.sub("navegador", f"perfil{slot}")
    for name in ("Cookies", "Cookies-journal", "History", "History-journal", "Local Storage", "Session Storage",
                 "IndexedDB", "Service Worker", "Cache", "Code Cache", "Sessions"):
        target = prof / "Default" / name
        shutil.rmtree(target, ignore_errors=True) if target.is_dir() else target.unlink(missing_ok=True)
    return prof


def _release(prof: Path) -> None:
    with _free_lock:
        _free.append(int(prof.name.removeprefix("perfil")))


def _render(url: str, exe: str, budget_ms: int, allow_private: bool) -> str:
    proxy = None if allow_private else _GuardProxy()
    prof = _profile()
    shell = "headless-shell" in Path(exe).name or "headless_shell" in exe
    try:
        cmd = [exe] + ([] if shell else ["--headless=new"]) + [
            "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--mute-audio", "--disable-extensions",
            "--disable-background-networking", "--disable-component-update", "--disable-sync", "--hide-scrollbars",
            "--blink-settings=imagesEnabled=false", f"--user-data-dir={prof}", f"--virtual-time-budget={int(budget_ms)}",
            "--window-size=1280,2000", "--lang=es-ES", "--dump-dom",
            # sin Llavero de macOS ni almacen de claves del escritorio: sin ellos se queda esperando y agota el tiempo
            "--use-mock-keychain", "--password-store=basic"]
        if proxy:
            # todo pasa por el proxy, incluso localhost (<-loopback quita la excepcion por defecto de Chromium)
            cmd += [f"--proxy-server=http://127.0.0.1:{proxy.port}", "--proxy-bypass-list=<-loopback>"]
        if sys.platform.startswith("linux") and hasattr(os, "geteuid") and os.geteuid() == 0:
            cmd.append("--no-sandbox")   # Chromium no arranca como root con sandbox (contenedores, Termux con proot)
        cmd.append(url)
        r = None
        for intento in range(2):   # Chrome a veces se queda colgado al arrancar (sobre todo con un perfil nuevo): un reintento
            try:
                r = subprocess.run(cmd, capture_output=True, timeout=(20 if not intento else TIMEOUT_S) + budget_ms / 1000,
                                   errors="replace", text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                break
            except subprocess.TimeoutExpired as e:
                if intento:
                    raise RuntimeError("el navegador ha tardado demasiado") from e
            except OSError as e:
                raise RuntimeError(f"no se pudo abrir el navegador: {e}") from e
    finally:
        _release(prof)
        if proxy:
            proxy.close()
    html = r.stdout or ""
    if "<" not in html:
        raise RuntimeError(f"el navegador no devolvio la pagina ({(r.stderr or '').strip()[-200:]})")
    if proxy and proxy.blocked:
        L.info("Navegador: bloqueadas %d conexiones a la red local (%s)", len(proxy.blocked), ", ".join(sorted(set(proxy.blocked))[:5]))
    return html


def mode() -> str:
    try:
        from ..config import get_config
        m = str(get_config().get("rag.browser", "auto") or "auto")
    except Exception:  # noqa: BLE001
        m = "auto"
    return m if m in MODES else "auto"


def _path() -> str:
    try:
        from ..config import get_config
        return str(get_config().get("rag.browser_path", "") or "")
    except Exception:  # noqa: BLE001
        return ""


def _words(raw: str) -> int:
    body = re.sub(r"(?is)<(script|style|noscript|svg|template|head)\b.*?</\1>", " ", raw)
    return len(re.sub(r"<[^>]+>", " ", body).split())


def maybe_render(url: str, raw: str, how: str | None = None) -> tuple[str, bool]:
    """(html, leida_con_navegador). Usa el navegador si el modo lo pide y hay uno; si no, devuelve lo descargado."""
    how = how or mode()
    if how == "nunca" or (how == "auto" and not needs_browser(raw)):
        return raw, False
    exe = find(_path())
    if not exe:
        return raw, False
    try:
        dom = render(url, exe)
    except RuntimeError as e:
        L.info("No se pudo leer %s con el navegador: %s", url, e)
        return raw, False
    if how == "auto" and _words(dom) <= _words(raw):
        return raw, False
    return dom, True


def info() -> dict:
    exe = find(_path())
    return {"modo": mode(), "navegador": exe or "", "disponible": bool(exe)}
