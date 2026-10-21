"""Resolucion DNS para la app Android autonoma.

El Python de la app usa musl, que resuelve nombres leyendo /etc/resolv.conf. Android no tiene ese fichero
(sus apps preguntan al servicio DNS del sistema), asi que sin esto ningun nombre de internet se resolveria:
ni descargas de modelos ni rastreo web. ``install()`` sustituye ``socket.getaddrinfo`` por una version que
consulta por UDP los servidores DNS de la red activa (los pasa la app en NEXUS_DNS) y, si no responden,
unos publicos. Las IP literales y localhost no salen del movil.
"""
from __future__ import annotations

import ipaddress
import os
import secrets
import socket
import struct
import threading
import time

FALLBACK = ("1.1.1.1", "8.8.8.8", "9.9.9.9")
TIMEOUT = 2.0
_A, _AAAA, _CNAME = 1, 28, 5
_cache: dict[tuple[str, int], tuple[float, list[str]]] = {}
_lock = threading.Lock()
_orig_getaddrinfo = socket.getaddrinfo


def servers() -> list[str]:
    own = [s.strip() for s in os.environ.get("NEXUS_DNS", "").split(",") if s.strip()]
    return own + [s for s in FALLBACK if s not in own]


def _query(name: str, qtype: int) -> bytes:
    qid = secrets.randbits(16)
    head = struct.pack("!HHHHHH", qid, 0x0100, 1, 0, 0, 0)   # consulta recursiva, una pregunta
    qname = b"".join(bytes([len(p)]) + p.encode("idna") for p in name.rstrip(".").split(".")) + b"\0"
    return head + qname + struct.pack("!HH", qtype, 1)


def _skip_name(msg: bytes, i: int) -> int:
    while True:
        n = msg[i]
        if n == 0:
            return i + 1
        if n & 0xC0 == 0xC0:   # puntero de compresion
            return i + 2
        i += n + 1


def _parse(msg: bytes, qid: int, qtype: int) -> tuple[list[str], int]:
    """(direcciones, ttl) de la respuesta; sigue los CNAME porque toma cualquier A/AAAA de la seccion de respuestas."""
    rid, flags, qd, an = struct.unpack("!HHHH", msg[:8])
    if rid != qid or not flags & 0x8000:
        raise ValueError("respuesta DNS ajena")
    if flags & 0x000F not in (0, 3):    # 3 = el nombre no existe: respuesta valida y vacia
        raise ValueError(f"error DNS {flags & 0x000F}")
    i = 12
    for _ in range(qd):
        i = _skip_name(msg, i) + 4
    out, ttl = [], 300
    for _ in range(an):
        i = _skip_name(msg, i)
        rtype, _cls, rttl, rlen = struct.unpack("!HHIH", msg[i:i + 10])
        data = msg[i + 10:i + 10 + rlen]
        i += 10 + rlen
        if rtype == qtype == _A and rlen == 4:
            out.append(socket.inet_ntop(socket.AF_INET, data))
        elif rtype == qtype == _AAAA and rlen == 16:
            out.append(socket.inet_ntop(socket.AF_INET6, data))
        else:
            continue
        ttl = min(ttl, max(30, rttl))
    return out, ttl


def _ask(server: str, name: str, qtype: int, port: int = 53) -> tuple[list[str], int]:
    q = _query(name, qtype)
    qid = struct.unpack("!H", q[:2])[0]
    fam = socket.AF_INET6 if ":" in server else socket.AF_INET
    with socket.socket(fam, socket.SOCK_DGRAM) as s:
        s.settimeout(TIMEOUT)
        s.sendto(q, (server, port))
        while True:
            data, _ = s.recvfrom(4096)
            try:
                return _parse(data, qid, qtype)
            except ValueError as e:
                if "ajena" not in str(e):
                    raise


def resolve(name: str, family: int = socket.AF_UNSPEC, port: int = 53) -> list[str]:
    """Direcciones IP de ``name`` (IPv4 primero), con cache segun el TTL."""
    name = name.rstrip(".").lower()
    types = [_A] if family == socket.AF_INET else [_AAAA] if family == socket.AF_INET6 else [_A, _AAAA]
    out: list[str] = []
    for qtype in types:
        key = (name, qtype)
        with _lock:
            hit = _cache.get(key)
        if hit and hit[0] > time.monotonic():
            out += hit[1]
            continue
        for srv in servers():   # el primero que conteste; si ninguno, este tipo queda sin direcciones
            try:
                ips, ttl = _ask(srv, name, qtype, port)
            except (OSError, ValueError):
                continue
            with _lock:
                _cache[key] = (time.monotonic() + ttl, ips)
            out += ips
            break
    return out


def _literal(host: str) -> bool:
    try:
        ipaddress.ip_address(host.split("%")[0])
        return True
    except ValueError:
        return False


def getaddrinfo(host, port, family=0, type=0, proto=0, flags=0):
    if isinstance(host, bytes):
        host = host.decode("idna")
    if host is None or _literal(host) or flags & socket.AI_NUMERICHOST:
        return _orig_getaddrinfo(host, port, family, type, proto, flags)
    if host.lower() in ("localhost", "localhost.localdomain", "ip6-localhost"):
        local = ["127.0.0.1"] if family != socket.AF_INET6 else []
        local += ["::1"] if family in (0, socket.AF_INET6) else []
        return [r for ip in local for r in _orig_getaddrinfo(ip, port, family, type, proto, flags | socket.AI_NUMERICHOST)]
    ips = resolve(host, family)
    if not ips:
        raise socket.gaierror(socket.EAI_NONAME, f"No se pudo resolver {host}")
    return [r for ip in ips for r in _orig_getaddrinfo(ip, port, family, type, proto, flags | socket.AI_NUMERICHOST)]


def gethostbyname(host: str) -> str:
    return getaddrinfo(host, None, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]


def needed() -> bool:
    return not os.path.exists("/etc/resolv.conf")


def install(force: bool = False) -> bool:
    """Activa el resolvedor propio si el sistema no tiene /etc/resolv.conf (Android). Devuelve si se activo."""
    if not (force or needed()):
        return False
    socket.getaddrinfo = getaddrinfo
    socket.gethostbyname = gethostbyname
    return True
