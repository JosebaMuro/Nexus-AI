"""Asistente del router: detecta la puerta de enlace, su pagina de configuracion y la marca, y prepara
los datos exactos para crear la regla de redireccion de puertos que necesita el acceso remoto.

Solo lee (titulo de la pagina de inicio y descripcion UPnP); nunca inicia sesion ni cambia nada en el router.
"""
from __future__ import annotations

import ipaddress
import re
import socket
import ssl
import subprocess
import sys
from pathlib import Path

import psutil

from . import upnp

# marca -> (nombre, ruta orientativa del menu, consejo). El nombre exacto varia segun modelo y firmware.
GUIDES: dict[str, tuple[str, str, str]] = {
    "movistar": ("Movistar (HGU / Askey / Mitrastar)", "Configuración avanzada → NAT / Puertos (Port Mapping)",
                 "También desde la app Smart WiFi de Movistar → Puertos."),
    "vodafone": ("Vodafone (Station / Sercomm / Hitron)", "Ajustes → Internet / Firewall → Mapeo de puertos (Port Forwarding)",
                 "En routers de cable antiguos (ONO) la opción está en Básico/Avanzado → Reenvío de puertos."),
    "orange": ("Orange / Jazztel (Livebox)", "Configuración avanzada → NAT/PAT", "Elige este equipo en la lista de dispositivos."),
    "digi": ("Digi", "Avanzado / Red → NAT → Reenvío de puertos", "Los routers de Digi suelen ser ZTE: mira también la guía ZTE."),
    "fritz": ("FRITZ!Box (AVM)", "Internet → Permitir acceso → Uso compartido de puertos → Añadir dispositivo",
              "Puedes elegir este equipo por su nombre y añadir el puerto."),
    "tplink": ("TP-Link", "Avanzado → NAT Forwarding → Servidores virtuales (Virtual Servers)", ""),
    "asus": ("ASUS", "WAN → Servidor virtual / Reenvío de puertos", ""),
    "netgear": ("Netgear", "Avanzado → Configuración avanzada → Reenvío de puertos / activación de puertos", ""),
    "huawei": ("Huawei", "Avanzado → NAT → Mapeo de puertos (Port Mapping)", ""),
    "zte": ("ZTE", "Internet / Red → Seguridad → Reenvío de puertos (Port Forwarding)", ""),
    "xiaomi": ("Xiaomi (MiWiFi)", "Ajustes avanzados → Reenvío de puertos", ""),
    "mikrotik": ("MikroTik (RouterOS)", "IP → Firewall → NAT → regla dst-nat (TCP, puerto, to-addresses)", ""),
    "dlink": ("D-Link", "Avanzado → Reenvío de puertos / Servidor virtual", ""),
    "linksys": ("Linksys", "Seguridad → Aplicaciones y juegos → Reenvío de un solo puerto", ""),
    "sagemcom": ("Sagemcom", "Acceso avanzado → Red → NAT / Reenvío de puertos", ""),
    "generic": ("Router", "Busca un menú llamado «NAT», «Reenvío de puertos», «Port forwarding», «Port mapping» o «Servidores virtuales»",
                "Suele estar en la configuración avanzada o en Seguridad / Firewall."),
}
BRAND_RE = [
    ("movistar", r"movistar|askey|mitrastar|\bhgu\b|telef[oó]nica"), ("vodafone", r"vodafone|sercomm|hitron|compal|\bono\b"),
    ("orange", r"livebox|orange|jazztel|arcadyan"), ("digi", r"\bdigi\b"), ("fritz", r"fritz|\bavm\b"),
    ("tplink", r"tp-?link|archer|\bdeco\b"), ("asus", r"\basus\b|\brt-[a-z]{1,2}\d"), ("netgear", r"netgear|nighthawk|orbi"),
    ("huawei", r"huawei|\bhg8\d"), ("zte", r"\bzte\b|zxhn|\bf6\d\d"), ("xiaomi", r"xiaomi|miwifi|redmi"),
    ("mikrotik", r"mikrotik|routeros"), ("dlink", r"d-link|dlink"), ("linksys", r"linksys"), ("sagemcom", r"sagemcom"),
]


def identify(*texts: str) -> str:
    blob = " ".join(t for t in texts if t).lower()
    return next((key for key, rx in BRAND_RE if re.search(rx, blob)), "generic")


# ---------------------------------------------------------------- red local
def parse_route_print(text: str) -> str:
    """Windows `route print -4 0.0.0.0`: puerta de enlace de la ruta por defecto con menor metrica."""
    best = None
    for gw, _iface, metric in re.findall(r"^\s*0\.0\.0\.0\s+0\.0\.0\.0\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+)", text, re.M):
        if best is None or int(metric) < best[1]:
            best = (gw, int(metric))
    return best[0] if best else ""


def parse_proc_route(text: str) -> str:
    """Linux / Android `/proc/net/route`: destino 00000000 con la puerta en hexadecimal little-endian."""
    for line in text.splitlines()[1:]:
        f = line.split()
        if len(f) > 2 and f[1] == "00000000" and f[2] != "00000000":
            return socket.inet_ntoa(bytes.fromhex(f[2])[::-1])
    return ""


def default_gateway() -> str:
    try:
        if sys.platform == "win32":
            out = subprocess.run(["route", "print", "-4", "0.0.0.0"], capture_output=True, text=True, timeout=8,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
            return parse_route_print(out)
        if Path("/proc/net/route").exists():
            return parse_proc_route(Path("/proc/net/route").read_text())
        out = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True, timeout=8).stdout
        m = re.search(r"gateway:\s*(\S+)", out)
        return m.group(1) if m else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def interface_for(ip: str) -> dict:
    """Nombre del adaptador y direccion MAC del equipo con esa IP (para la reserva DHCP)."""
    for name, addrs in psutil.net_if_addrs().items():
        if any(getattr(a.family, "name", "") == "AF_INET" and a.address == ip for a in addrs):
            mac = next((a.address for a in addrs if a.family == psutil.AF_LINK), "")
            return {"name": name, "mac": mac.replace("-", ":").upper()}
    return {"name": "", "mac": ""}


# ---------------------------------------------------------------- sondeo de la pagina del router
def probe_page(host: str, port: int, tls: bool, timeout: float = 3.0) -> dict | None:
    """GET tolerante (muchos routers responden con cabeceras no estandar o TLS antiguo). Solo lee."""
    try:
        sock = socket.create_connection((host, port), timeout)
    except OSError:
        return None
    try:
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
            ctx.minimum_version = ssl.TLSVersion.MINIMUM_SUPPORTED      # solo para leer el titulo de routers viejos
            try:
                ctx.set_ciphers("ALL:@SECLEVEL=0")
            except ssl.SSLError:
                pass
            sock = ctx.wrap_socket(sock, server_hostname=host)
        sock.settimeout(timeout)
        sock.sendall(f"GET / HTTP/1.1\r\nHost: {host}\r\nUser-Agent: Mozilla/5.0 Nexus-Office-router-assistant\r\n"
                     "Accept: text/html\r\nConnection: close\r\n\r\n".encode())
        buf = b""
        while len(buf) < 65536:
            chunk = sock.recv(8192)
            if not chunk:
                break
            buf += chunk
    except (OSError, ssl.SSLError):
        return {"url": f"{'https' if tls else 'http'}://{host}{'' if port in (80, 443) else f':{port}'}/", "responds": True, "title": "", "text": ""}
    finally:
        try:
            sock.close()
        except OSError:
            pass
    text = buf.decode("latin-1", "replace")
    head, _, body = text.partition("\r\n\r\n")
    title = re.search(r"<title[^>]*>(.*?)</title>", body, re.I | re.S)
    t = re.sub(r"\s+", " ", title.group(1)).strip()[:80] if title else ""
    if re.fullmatch(r"\d{3}\b.*", t):                 # "400 Bad Request", "401 Unauthorized"... no identifica nada
        t = ""
    realm = re.search(r'realm="([^"]+)"', head, re.I)
    server = re.search(r"(?im)^server:\s*(.+)$", head)
    return {"url": f"{'https' if tls else 'http'}://{host}{'' if port in (80, 443) else f':{port}'}/", "responds": True,
            "title": t or (realm.group(1) if realm else ""), "text": " ".join(filter(None, [t, realm and realm.group(1),
                                                                                          server and server.group(1), body[:4000]]))}


def upnp_devices(timeout: float = 2.5) -> list[dict]:
    """Descripciones UPnP que anuncia el router (marca/modelo/pagina) y si ofrece abrir puertos (IGD)."""
    import httpx
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.settimeout(timeout)
    found: dict[str, str] = {}
    try:
        for st in (*upnp.TARGETS, "upnp:rootdevice"):
            sock.sendto((f"M-SEARCH * HTTP/1.1\r\nHOST: {upnp.SSDP[0]}:{upnp.SSDP[1]}\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                         f"ST: {st}\r\n\r\n").encode(), upnp.SSDP)
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                break
            loc = upnp.parse_ssdp(data)
            if loc:
                found.setdefault(loc, addr[0])
    finally:
        sock.close()
    out = []
    for loc, ip in found.items():
        try:
            xml = httpx.get(loc, timeout=3).text
        except Exception:  # noqa: BLE001
            continue
        tag = lambda n: (re.search(rf"<{n}>(.*?)</{n}>", xml, re.S) or [None, ""])[1].strip()  # noqa: E731
        out.append({"ip": ip, "location": loc, "friendly": tag("friendlyName"), "manufacturer": tag("manufacturer"),
                    "model": " ".join(filter(None, [tag("modelName"), tag("modelNumber")])), "presentation": tag("presentationURL"),
                    "igd": upnp.parse_description(xml, loc) is not None})
    return out


def detect(port: int) -> dict:
    gw = default_gateway()
    local_ip = upnp.local_ip_towards(gw or "8.8.8.8")
    if not gw:                                          # ultimo recurso: la .1 de la subred
        gw = ".".join(local_ip.split(".")[:3] + ["1"]) if local_ip else ""
    devs = [d for d in upnp_devices() if d["ip"] == gw]
    pages = [p for p in (probe_page(gw, 80, False), probe_page(gw, 443, True)) if p] if gw else []
    for d in devs:
        if d["presentation"] and d["presentation"].startswith("http"):
            pages.insert(0, {"url": d["presentation"], "responds": True, "title": d["friendly"], "text": ""})
    brand = identify(*(p["text"] for p in pages), *(f"{d['manufacturer']} {d['model']} {d['friendly']}" for d in devs))
    name, path, tip = GUIDES[brand]
    model = next((f"{d['manufacturer']} {d['model']}".strip() for d in devs if d["manufacturer"] and "broadcom" not in d["manufacturer"].lower()), "")
    title = next((p["title"] for p in pages if p["title"]), "")
    private = bool(gw) and ipaddress.ip_address(gw).is_private
    return {
        "gateway": gw, "gateway_private": private, "local_ip": local_ip, "interface": interface_for(local_ip),
        "admin_urls": [p["url"] for p in pages] or ([f"http://{gw}/"] if gw else []),
        "page_title": title, "brand": brand, "brand_name": name if brand != "generic" else (model or title or "Router no identificado"),
        "menu_path": path, "tip": tip, "upnp_igd": any(d["igd"] for d in devs), "upnp_devices": devs,
        "rule": {"name": "Nexus-Office", "protocol": "TCP", "external_port": port, "internal_ip": local_ip, "internal_port": port},
    }


def reachable_from_outside(public_ip: str, port: int, timeout: float = 4.0) -> bool:
    """Intenta el saludo TLS contra la IP publica desde este equipo. Si falla puede ser solo que el router no
    admite el 'bucle' (hairpin NAT): la prueba definitiva es la app con datos moviles."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    try:
        with ctx.wrap_socket(socket.create_connection((public_ip, port), timeout), server_hostname="nexus") as t:
            return t.version() == "TLSv1.3"
    except (OSError, ssl.SSLError):
        return False
