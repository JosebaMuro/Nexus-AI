"""Acceso remoto por internet: HTTPS/WSS (TLS 1.3) en un puerto propio + dispositivos emparejados.

El puerto local (HTTP, 8775) sigue igual. El puerto remoto (HTTPS, 8443 por defecto) envuelve la misma
aplicacion con ``RemoteGate``: todo lo que entra por el exige la llave de un dispositivo emparejado,
venga de donde venga (tambien desde 127.0.0.1, para que un tunel o proxy no pueda saltarselo).
"""
from __future__ import annotations

import base64
import json
from pathlib import Path

from .. import log, paths
from .devices import DeviceStore, RateLimiter
from .tlscert import ensure_cert, pretty

L = log.get("remote")
COOKIE = "nexus_dev"
OPEN_EXACT = {"/api/ping", "/api/remote/pair", "/sw.js", "/favicon.ico"}
OPEN_PREFIX = ("/static/",)      # la interfaz es codigo publico; datos, salidas y API van protegidos
SECURITY_HEADERS = [(b"x-content-type-options", b"nosniff"), (b"referrer-policy", b"no-referrer"),
                    (b"x-frame-options", b"DENY"), (b"cache-control", b"no-store")]


class RemoteAccess:
    def __init__(self, config) -> None:
        self.cfg = config
        self.folder: Path = paths.sub("remote")
        self.devices = DeviceStore(self.folder)
        self.limiter = RateLimiter()
        self.server = None                         # uvicorn.Server del puerto remoto (lo fija el arranque)
        self.public_ip = ""
        self.behind_cgnat = False
        self.upnp = {"mapped": False, "external_ip": "", "error": ""}
        self._fp = ""

    @property
    def listening_port(self) -> int | None:
        srv = self.server
        return srv.config.port if srv is not None and srv.started and not srv.should_exit else None

    # ---------------------------------------------------------------- certificado
    def cert(self, regenerate: bool = False) -> tuple[Path, Path, str]:
        from ..coop.node import overlay_addresses
        names = [a["ip"] for a in overlay_addresses()] + ([self.public_ip] if self.public_ip else [])
        cert, key, fp = ensure_cert(self.folder, names, regenerate)
        self._fp = fp
        return cert, key, fp

    @property
    def fingerprint(self) -> str:
        if not self._fp:
            self.cert()
        return self._fp

    # ---------------------------------------------------------------- direcciones e invitaciones
    def port(self) -> int:
        return int(self.cfg.get("remote.port", 8443))

    def urls(self) -> list[dict]:
        from ..coop.node import overlay_addresses
        p, out = self.port(), []
        ext = self.upnp.get("external_ip") or self.public_ip
        if ext:
            out.append({"kind": "internet", "url": f"https://{ext}:{p}"})
        for a in sorted(overlay_addresses(), key=lambda a: a["kind"] != "tailscale"):
            if not a["ip"].startswith("169.254."):
                out.append({"kind": a["kind"], "url": f"https://{a['ip']}:{p}"})
        return out

    def invite(self, pairing: dict, name: str = "") -> str:
        """Codigo para la app Android: direcciones + huella a anclar + codigo de un solo uso."""
        body = {"u": [u["url"] for u in self.urls()], "f": self.fingerprint, "c": pairing["code"], "n": name}
        raw = json.dumps(body, separators=(",", ":")).encode()
        return "NEXUSR1:" + base64.urlsafe_b64encode(raw).decode().rstrip("=")

    def status(self) -> dict:
        return {"enabled": bool(self.cfg.get("remote.enabled", False)), "port": self.port(),
                "listening": self.listening_port is not None, "fingerprint": pretty(self.fingerprint),
                "urls": self.urls(), "upnp": self.upnp, "public_ip": self.public_ip, "behind_cgnat": self.behind_cgnat, "devices": self.devices.list(),
                "crypto": "TLS 1.3 · X25519MLKEM768 post-cuántico (o X25519) · AES-GCM / ChaCha20-Poly1305 · ECDSA P-256 anclado"}


def read_invite(code: str) -> dict:
    code = code.strip()
    if not code.startswith("NEXUSR1:"):
        raise ValueError("codigo de acceso remoto no valido")
    b = code[8:]
    return json.loads(base64.urlsafe_b64decode(b + "=" * (-len(b) % 4)))


def _cookies(scope) -> dict:
    for k, v in scope.get("headers", []):
        if k == b"cookie":
            out = {}
            for part in v.decode(errors="ignore").split(";"):
                n, _, val = part.strip().partition("=")
                out[n] = val
            return out
    return {}


def _header(scope, name: bytes) -> str:
    return next((v.decode(errors="ignore") for k, v in scope.get("headers", []) if k == name), "")


PAIR_PAGE = (Path(__file__).parent / "pair.html")


class RemoteGate:
    """Middleware ASGI del puerto remoto (HTTP y WebSocket)."""

    def __init__(self, app, get_remote) -> None:
        self.app, self.get_remote = app, get_remote

    async def __call__(self, scope, receive, send):
        if scope["type"] == "lifespan":          # el ciclo de vida lo lleva el puerto local: aqui solo se confirma
            while True:
                msg = await receive()
                if msg["type"] == "lifespan.startup":
                    await send({"type": "lifespan.startup.complete"})
                elif msg["type"] == "lifespan.shutdown":
                    return await send({"type": "lifespan.shutdown.complete"})
        scope = dict(scope)
        scope["nexus_remote"] = True
        ra = self.get_remote()
        if ra is None:
            return await self._deny(scope, send, 503, "Nexus-Office esta arrancando")
        path = scope.get("path", "/")
        ip = (scope.get("client") or ("", 0))[0]
        token = _cookies(scope).get(COOKIE) or _header(scope, b"x-nexus-device")
        dev = ra.devices.check(token, ip)
        if dev:
            scope["nexus_device"] = dev
        elif not (path in OPEN_EXACT or path.startswith(OPEN_PREFIX)):
            if scope["type"] == "http" and path == "/":
                return await self._html(send, PAIR_PAGE.read_text(encoding="utf-8").replace("{{FP}}", pretty(ra.fingerprint)))
            return await self._deny(scope, send, 401, "Dispositivo no emparejado")

        async def secured_send(msg):
            if msg["type"] == "http.response.start":
                msg = {**msg, "headers": list(msg.get("headers", [])) + [h for h in SECURITY_HEADERS
                                                                           if not path.startswith(OPEN_PREFIX) or h[0] != b"cache-control"]}
            await send(msg)
        return await self.app(scope, receive, secured_send)

    @staticmethod
    async def _deny(scope, send, status: int, detail: str):
        if scope["type"] == "websocket":
            return await send({"type": "websocket.close", "code": 4400 + (status % 100)})
        body = json.dumps({"detail": detail}).encode()
        await send({"type": "http.response.start", "status": status,
                    "headers": [(b"content-type", b"application/json"), *SECURITY_HEADERS]})
        await send({"type": "http.response.body", "body": body})

    @staticmethod
    async def _html(send, html: str):
        await send({"type": "http.response.start", "status": 401,
                    "headers": [(b"content-type", b"text/html; charset=utf-8"), *SECURITY_HEADERS]})
        await send({"type": "http.response.body", "body": html.encode()})


def server_config(app, state: dict, cfg):
    """uvicorn.Config del puerto remoto: TLS 1.3 endurecido, sin lifespan (lo lleva el servidor local)."""
    import uvicorn

    from .tlscert import harden
    ra = RemoteAccess(cfg)            # mismo certificado que el servicio (data/remote/), creado antes de arrancar
    cert, key, fp = ra.cert()
    conf = uvicorn.Config(RemoteGate(app, lambda: state["s"].remote if "s" in state else None),
                          host=cfg.get("remote.host", "0.0.0.0"), port=ra.port(), ssl_certfile=str(cert), ssl_keyfile=str(key),
                          lifespan="off", log_level="warning", proxy_headers=False, server_header=False,
                          timeout_graceful_shutdown=3)   # un movil sin cobertura no confirma el cierre TLS: no esperar 30 s
    conf.load()
    harden(conf.ssl)
    L.info("Acceso remoto HTTPS (TLS 1.3) en el puerto %s · huella %s", ra.port(), pretty(fp))
    return conf
