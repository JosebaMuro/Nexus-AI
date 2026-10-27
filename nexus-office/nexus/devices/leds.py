"""Luces LED NexusLED: firmware propio para ESP32 y tiras WS2812/SK6812 (carpeta ``firmware/nexusled``).

Protocolo (el mismo que el firmware y su emulador):

* Descubrimiento: UDP 4210, se envia ``NEXUSLED?`` por difusion y cada dispositivo contesta con un JSON
  ``{"nexusled": api, "name", "id", "version", "leds", "auth", "http_port"}``.
* Estado de Nexus en directo: UDP ``N X L 2 | estado | nivel (0-254, 255 = simulado) | R G B | ttl`` (rapido, sin
  respuesta) y, de respaldo, HTTP ``POST /api/nexus {"status": ..., "level": ...}``.
* Control: HTTP JSON ``/api/info``, ``/api/state`` (GET/POST: on, bri, fx, pal, col, preset...), ``/api/effects``,
  ``/api/palettes``, ``/api/presets``. Si el dispositivo tiene clave, va en la cabecera ``X-Nexus-Token``.

Los dispositivos se guardan en ``data/dispositivos/leds.json``.
"""
from __future__ import annotations

import json
import socket
import threading
import time
from pathlib import Path

import httpx

from .. import log, paths

L = log.get("leds")
UDP_PORT = 4210
# mismo orden que NEXUS_STATES del firmware (nl_effects.h)
ESTADOS = ("none", "idle", "listening", "thinking", "tool", "speaking", "success", "error", "alert")
NOMBRES = {"none": "Sin estado", "idle": "En línea", "listening": "Escuchando", "thinking": "Pensando",
           "tool": "Usando herramienta", "speaking": "Hablando", "success": "Hecho", "error": "Error", "alert": "Aviso"}


class Leds:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or paths.sub("dispositivos") / "leds.json"
        self._lock = threading.Lock()
        self.devices: list[dict] = []
        self._sock: socket.socket | None = None
        self.load()

    # ------------------------------------------------------------ registro
    def load(self) -> None:
        try:
            self.devices = json.loads(self.path.read_text(encoding="utf-8")).get("devices", [])
        except (OSError, ValueError):
            self.devices = []

    def save(self) -> None:
        self.path.write_text(json.dumps({"devices": self.devices}, ensure_ascii=False, indent=1), encoding="utf-8")

    def list(self) -> list[dict]:
        return [{k: v for k, v in d.items() if k != "token"} | {"auth": bool(d.get("token"))} for d in self.devices]

    def get(self, did: str) -> dict:
        for d in self.devices:
            if d["id"] == did:
                return d
        raise KeyError(f"dispositivo LED desconocido: {did}")

    def add(self, host: str, port: int = 80, name: str = "", token: str = "", did: str = "") -> dict:
        host = host.strip()
        if ":" in host and host.count(":") == 1:
            host, p = host.split(":")
            port = int(p)
        info = {}
        try:
            info = self._http({"host": host, "port": port, "token": token}, "GET", "/api/info", timeout=4)
        except Exception as e:   # noqa: BLE001 - se puede anadir apagado y probar luego
            L.info("NexusLED %s:%s no responde al anadirlo: %s", host, port, e)
        did = did or info.get("id") or f"{host}:{port}"
        with self._lock:
            self.devices = [d for d in self.devices if d["id"] != did]
            dev = {"id": did, "name": name or info.get("name") or f"NexusLED {host}", "host": host, "port": int(port),
                   "token": token, "enabled": True, "leds": info.get("leds", 0), "version": info.get("version", ""),
                   "follow_state": True}
            self.devices.append(dev)
            self.save()
        return {k: v for k, v in dev.items() if k != "token"}

    def update(self, did: str, **kw) -> dict:
        with self._lock:
            d = self.get(did)
            for k in ("name", "enabled", "follow_state", "token", "host", "port"):
                if k in kw and kw[k] is not None:
                    d[k] = kw[k]
            self.save()
        return {k: v for k, v in d.items() if k != "token"}

    def remove(self, did: str) -> None:
        with self._lock:
            self.devices = [d for d in self.devices if d["id"] != did]
            self.save()

    # ------------------------------------------------------------ red
    def discover(self, timeout: float = 1.5, hosts: list[str] | None = None) -> list[dict]:
        """Busca NexusLED en la red local (difusion UDP) y en ``hosts`` concretos."""
        found: dict[str, dict] = {}
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.settimeout(0.25)
            targets = ["255.255.255.255"] + list(hosts or [])
            for _ in range(2):
                for t in targets:
                    host, _, port = t.partition(":")
                    try:
                        s.sendto(b"NEXUSLED?", (host, int(port or UDP_PORT)))
                    except OSError:
                        pass
                end = time.time() + timeout / 2
                while time.time() < end:
                    try:
                        data, addr = s.recvfrom(2048)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    try:
                        info = json.loads(data.decode("utf-8", "replace"))
                    except ValueError:
                        continue
                    if "nexusled" in info:
                        info["host"] = addr[0]
                        info["udp_port"] = addr[1]
                        info["added"] = any(d["id"] == info.get("id") for d in self.devices)
                        found[info.get("id") or addr[0]] = info
        finally:
            s.close()
        return list(found.values())

    def _http(self, d: dict, method: str, path: str, body: dict | None = None, timeout: float = 3.0) -> dict:
        headers = {"X-Nexus-Token": d["token"]} if d.get("token") else {}
        url = f"http://{d['host']}:{d.get('port', 80)}{path}"
        r = httpx.request(method, url, json=body, headers=headers, timeout=timeout)
        if r.status_code >= 400:
            try:
                msg = r.json().get("error") or r.text
            except ValueError:
                msg = r.text
            raise ValueError(f"NexusLED {d['host']}: {msg}")
        return r.json() if r.content else {}

    def call(self, did: str, method: str, path: str, body: dict | None = None) -> dict:
        if not path.startswith("/api/"):
            raise ValueError("ruta no permitida")
        return self._http(self.get(did), method, path, body)

    def send_state(self, state: str, level: float | None = None, color: tuple[int, int, int] | None = None,
                   ttl: int = 0) -> int:
        """Envia el estado de Nexus a todas las tiras activas (UDP, sin esperar). Devuelve a cuantas."""
        if state not in ESTADOS:
            state = "idle"
        idx = ESTADOS.index(state)
        lv = 255 if level is None else max(0, min(254, int(level * 254)))
        r, g, b = color or (0, 0, 0)
        pkt = bytes([ord("N"), ord("X"), ord("L"), 2, idx, lv, r, g, b, max(0, min(255, ttl))])
        n = 0
        with self._lock:
            if self._sock is None:
                self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for d in self.devices:
                if not d.get("enabled", True) or not d.get("follow_state", True):
                    continue
                try:
                    self._sock.sendto(pkt, (d["host"], int(d.get("udp_port") or UDP_PORT)))
                    n += 1
                except OSError as e:
                    L.debug("NexusLED %s: %s", d["host"], e)
        return n
