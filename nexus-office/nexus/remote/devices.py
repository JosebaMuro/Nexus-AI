"""Dispositivos autorizados para el acceso remoto.

* Codigo de emparejamiento: de un solo uso, caduca a los 10 minutos, 60 bits de azar (XXXX-XXXX-XXXX).
* Al emparejar, el dispositivo recibe una llave aleatoria de 256 bits; aqui solo se guarda su SHA-256,
  asi que una copia de este fichero no sirve para entrar. Cada dispositivo se puede revocar.
* Limitador por IP: demasiados fallos (codigo o PIN) bloquean temporalmente esa direccion.
"""
from __future__ import annotations

import hashlib
import json
import secrets
import threading
import time
from pathlib import Path

ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"   # sin 0/O/1/I/L para dictarlo sin errores
PAIR_TTL = 600
SEEN_WRITE_EVERY = 60


def _h(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def normalize_code(code: str) -> str:
    return "".join(c for c in (code or "").upper() if c.isalnum())


class RateLimiter:
    """Fallos por clave (IP) en una ventana; al superar el maximo, bloqueo hasta que caduquen."""

    def __init__(self, max_fails: int = 8, window_s: int = 900) -> None:
        self.max_fails, self.window = max_fails, window_s
        self.fails: dict[str, list[float]] = {}
        self.lock = threading.Lock()

    def _recent(self, key: str) -> list[float]:
        now = time.time()
        lst = [t for t in self.fails.get(key, []) if now - t < self.window]
        self.fails[key] = lst
        return lst

    def blocked(self, key: str) -> bool:
        with self.lock:
            return len(self._recent(key)) >= self.max_fails

    def fail(self, key: str) -> None:
        with self.lock:
            self._recent(key).append(time.time())

    def reset(self, key: str) -> None:
        with self.lock:
            self.fails.pop(key, None)


class DeviceStore:
    def __init__(self, folder: Path) -> None:
        self.path = folder / "devices.json"
        self.lock = threading.Lock()
        self.pending: dict[str, dict] = {}              # codigo normalizado -> {name, expires}
        self.data = {"devices": {}}                     # id -> {name, token_hash, created, last_seen, last_ip}
        if self.path.exists():
            try:
                self.data = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                pass
        self.by_hash = {d["token_hash"]: i for i, d in self.data["devices"].items()}
        self._last_write = 0.0

    def _save(self) -> None:
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=1), encoding="utf-8")
        tmp.replace(self.path)
        self._last_write = time.time()

    # ---------------------------------------------------------------- emparejamiento
    def new_pairing(self, name: str = "") -> dict:
        raw = "".join(secrets.choice(ALPHABET) for _ in range(12))
        code = f"{raw[:4]}-{raw[4:8]}-{raw[8:]}"
        with self.lock:
            now = time.time()
            self.pending = {c: p for c, p in self.pending.items() if p["expires"] > now}
            self.pending[normalize_code(code)] = {"name": name.strip()[:40], "expires": now + PAIR_TTL}
        return {"code": code, "expires": int(now + PAIR_TTL), "ttl": PAIR_TTL}

    def pair(self, code: str, device_name: str, ip: str) -> tuple[str, dict] | None:
        """Canjea el codigo (una sola vez) por (llave, dispositivo). None si no es valido o ha caducado."""
        with self.lock:
            p = self.pending.pop(normalize_code(code), None)
            if not p or p["expires"] < time.time():
                return None
            token = secrets.token_urlsafe(32)
            dev_id = secrets.token_hex(6)
            dev = {"name": (device_name or p["name"] or "Dispositivo").strip()[:40], "token_hash": _h(token),
                   "created": int(time.time()), "last_seen": int(time.time()), "last_ip": ip}
            self.data["devices"][dev_id] = dev
            self.by_hash[dev["token_hash"]] = dev_id
            self._save()
        return token, {"id": dev_id, **{k: v for k, v in dev.items() if k != "token_hash"}}

    # ---------------------------------------------------------------- verificacion
    def check(self, token: str | None, ip: str = "") -> str | None:
        """Id del dispositivo si la llave es valida (busqueda por huella: no hay comparacion de secretos)."""
        if not token:
            return None
        dev_id = self.by_hash.get(_h(token))
        if dev_id:
            dev = self.data["devices"].get(dev_id)
            if dev is None:
                return None
            now = time.time()
            dev["last_seen"], dev["last_ip"] = int(now), ip or dev.get("last_ip", "")
            if now - self._last_write > SEEN_WRITE_EVERY:
                with self.lock:
                    self._save()
        return dev_id

    def list(self) -> list[dict]:
        return [{"id": i, **{k: v for k, v in d.items() if k != "token_hash"}}
                for i, d in sorted(self.data["devices"].items(), key=lambda kv: -kv[1].get("last_seen", 0))]

    def revoke(self, dev_id: str) -> bool:
        with self.lock:
            dev = self.data["devices"].pop(dev_id, None)
            if not dev:
                return False
            self.by_hash.pop(dev["token_hash"], None)
            self._save()
        return True
