"""VPN cifrada (WireGuard) entre la app Android «Nexus-Office VPN» y la instalacion de Nexus-Office en un PC.

Nexus-Office hace de servidor de configuracion: genera las claves, el fichero del tunel del PC
(data/vpn/nexusvpn.conf, formato wg-quick, el mismo en Windows, Linux y macOS) y un codigo
NEXUSV1:... por dispositivo que la app importa. El tunel lo levanta WireGuard oficial en el PC
(Windows: servicio de WireGuard; Linux: wg-quick/systemd; macOS: wg-quick o la app WireGuard) y la
app Android lleva su propio WireGuard (VpnService), sin instalar nada mas en el movil.

Cifrado: Noise IK con Curve25519, ChaCha20-Poly1305 y BLAKE2s (WireGuard), mas una clave
precompartida de 256 bits por dispositivo (capa simetrica adicional, resistente a ordenadores
cuanticos). La app solo enruta por el tunel el trafico de la propia app hacia el PC (10.78.0.1),
el resto del movil sale por su red normal.
"""
from __future__ import annotations

import base64
import ipaddress
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .. import log, paths

L = log.get("vpn")
IFACE = "nexusvpn"                   # nombre del tunel (y del fichero .conf) en el PC
CODE_PREFIX = "NEXUSV1:"
KEEPALIVE = 25


# ------------------------------------------------------------------------------------------- claves
def keypair() -> tuple[str, str]:
    """(privada, publica) en base64, formato WireGuard (Curve25519)."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    k = X25519PrivateKey.generate()
    raw = serialization.Encoding.Raw
    priv = k.private_bytes(raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    pub = k.public_key().public_bytes(raw, serialization.PublicFormat.Raw)
    return base64.b64encode(priv).decode(), base64.b64encode(pub).decode()


def public_from_private(priv_b64: str) -> str:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    k = X25519PrivateKey.from_private_bytes(base64.b64decode(priv_b64))
    return base64.b64encode(k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)).decode()


def preshared() -> str:
    return base64.b64encode(secrets.token_bytes(32)).decode()


def encode_code(data: dict) -> str:
    raw = json.dumps(data, separators=(",", ":")).encode()
    return CODE_PREFIX + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def decode_code(code: str) -> dict:
    body = code.strip()[len(CODE_PREFIX):]
    return json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))


def platform_name() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    try:
        if "microsoft" in Path("/proc/version").read_text().lower():
            return "wsl"
    except OSError:
        pass
    return "linux"


# ------------------------------------------------------------------------------------------- servidor
class VPN:
    def __init__(self, config) -> None:
        self.cfg = config
        self.folder = paths.sub("vpn")
        self.state_file = self.folder / "vpn.json"
        self.conf_file = self.folder / f"{IFACE}.conf"
        self.state = self._load()

    # ---------------------------------------------------------------- estado
    def _load(self) -> dict:
        if self.state_file.exists():
            try:
                return json.loads(self.state_file.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                L.warning("vpn.json ilegible: se regenera")
        return {"server_private": "", "server_public": "", "peers": []}

    def _save(self) -> None:
        self.state_file.write_text(json.dumps(self.state, indent=1), encoding="utf-8")
        _private(self.state_file)

    def network(self) -> ipaddress.IPv4Network:
        net = ipaddress.ip_network(self.cfg.get("vpn.subnet", "10.78.0.0/24"), strict=False)
        if net.version != 4 or net.prefixlen > 29:
            raise ValueError("vpn.subnet debe ser una red IPv4 privada de /29 o mayor (por ejemplo 10.78.0.0/24)")
        return net

    @property
    def server_ip(self) -> str:
        return str(next(self.network().hosts()))

    def port(self) -> int:
        return int(self.cfg.get("vpn.port", 51820))

    def http_port(self) -> int:
        return int(self.cfg.get("server.port", 8775))

    def ensure_server_keys(self) -> None:
        if not self.state.get("server_private"):
            self.state["server_private"], self.state["server_public"] = keypair()
            self._save()

    # ---------------------------------------------------------------- ficheros
    def server_conf(self) -> str:
        self.ensure_server_keys()
        net = self.network()
        lines = ["# Nexus-Office VPN (WireGuard) - tunel de este PC. Lo genera Nexus-Office: no lo edites a mano.",
                 f"# Importalo en WireGuard (Windows/macOS) o usalo con wg-quick (Linux/macOS). Red {net}.",
                 "[Interface]", f"PrivateKey = {self.state['server_private']}",
                 f"Address = {self.server_ip}/{net.prefixlen}", f"ListenPort = {self.port()}", ""]
        for p in self.state["peers"]:
            lines += [f"# {p['name']} ({time.strftime('%Y-%m-%d', time.localtime(p['created']))})", "[Peer]",
                      f"PublicKey = {p['public']}", f"PresharedKey = {p['psk']}", f"AllowedIPs = {p['ip']}/32", ""]
        return "\n".join(lines)

    def write_conf(self) -> Path:
        self.conf_file.write_text(self.server_conf(), encoding="utf-8")
        _private(self.conf_file)
        return self.conf_file

    def client_conf(self, peer: dict, private: str, endpoint: str) -> str:
        return "\n".join(["# Nexus-Office VPN - dispositivo «%s» (para la app WireGuard oficial)" % peer["name"], "[Interface]",
                          f"PrivateKey = {private}", f"Address = {peer['ip']}/32", "", "[Peer]",
                          f"PublicKey = {self.state['server_public']}", f"PresharedKey = {peer['psk']}",
                          f"Endpoint = {endpoint}", f"AllowedIPs = {self.server_ip}/32",
                          f"PersistentKeepalive = {KEEPALIVE}", ""])

    # ---------------------------------------------------------------- dispositivos
    def _next_ip(self) -> str:
        used = {p["ip"] for p in self.state["peers"]} | {self.server_ip}
        for h in self.network().hosts():
            if str(h) not in used:
                return str(h)
        raise ValueError("no quedan direcciones libres en la red de la VPN (amplia vpn.subnet)")

    def endpoints(self, public_ip: str = "") -> list[str]:
        """host:puerto por los que el movil puede llegar al PC: los configurados, la IP publica y las de casa."""
        from ..coop.node import overlay_addresses
        port = self.port()
        out = [e if ":" in e.rsplit("]", 1)[-1] else f"{e}:{port}" for e in self.cfg.get("vpn.endpoints", []) or []]
        if public_ip:
            out.append(f"{public_ip}:{port}")
        net = self.network()
        for a in sorted(overlay_addresses(), key=lambda a: a["kind"] != "lan"):
            ip = a["ip"]
            if ip.startswith("169.254.") or ipaddress.ip_address(ip) in net:
                continue                                     # la propia direccion del tunel no sirve para llegar a el
            out.append(f"{ip}:{port}")
        return list(dict.fromkeys(out))

    def add_peer(self, name: str, lan_key: str, public_ip: str = "") -> dict:
        self.ensure_server_keys()
        name = (name or "Android").strip()[:40] or "Android"
        priv, pub = keypair()
        peer = {"id": secrets.token_hex(4), "name": name, "ip": self._next_ip(), "public": pub, "psk": preshared(),
                "created": time.time()}
        self.state["peers"].append(peer)
        self._save()
        self.write_conf()
        eps = self.endpoints(public_ip)
        if not eps:
            raise ValueError("no se ha encontrado ninguna direccion de este PC; anade una en vpn.endpoints")
        code = encode_code({"v": 1, "n": name, "a": f"{peer['ip']}/32", "k": priv, "s": self.state["server_public"],
                            "p": peer["psk"], "e": eps, "r": f"{self.server_ip}/32",
                            "u": f"http://{self.server_ip}:{self.http_port()}", "key": lan_key})
        L.info("VPN: dispositivo '%s' anadido (%s)", name, peer["ip"])
        return {"peer": self._public(peer), "code": code, "endpoints": eps, "client_conf": self.client_conf(peer, priv, eps[0]),
                "restart_tunnel": True}

    def remove_peer(self, peer_id: str) -> bool:
        before = len(self.state["peers"])
        self.state["peers"] = [p for p in self.state["peers"] if p["id"] != peer_id]
        if len(self.state["peers"]) == before:
            return False
        self._save()
        self.write_conf()
        return True

    @staticmethod
    def _public(p: dict) -> dict:
        return {k: p[k] for k in ("id", "name", "ip", "public", "created")}

    # ---------------------------------------------------------------- estado del tunel en el PC
    def tunnel_up(self) -> bool:
        try:
            import psutil
            return any(a.address == self.server_ip for addrs in psutil.net_if_addrs().values() for a in addrs)
        except Exception:  # noqa: BLE001
            return False

    def wireguard_tool(self) -> str:
        if platform_name() == "windows":
            for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),):
                exe = Path(base) / "WireGuard" / "wireguard.exe"
                if exe.exists():
                    return str(exe)
            return shutil.which("wireguard") or ""
        return shutil.which("wg-quick") or ""

    def instructions(self) -> dict:
        conf, port, http, net = str(self.conf_file), self.port(), self.http_port(), str(self.network())
        win_exe = r"& 'C:\Program Files\WireGuard\wireguard.exe'"
        return {
            "windows": {
                "title": "Windows 10/11 (PowerShell como administrador)",
                "install": ["winget install WireGuard.WireGuard"],
                "up": [f"{win_exe} /installtunnelservice '{conf}'",
                       f"New-NetFirewallRule -DisplayName 'Nexus-Office VPN (WireGuard)' -Direction Inbound -Protocol UDP -LocalPort {port} -Action Allow",
                       f"New-NetFirewallRule -DisplayName 'Nexus-Office por la VPN' -Direction Inbound -Protocol TCP -LocalPort {http} -RemoteAddress {net} -Action Allow"],
                "reload": [f"{win_exe} /uninstalltunnelservice {IFACE}", "Start-Sleep 2", f"{win_exe} /installtunnelservice '{conf}'"],
                "down": [f"{win_exe} /uninstalltunnelservice {IFACE}"],
            },
            "linux": {
                "title": "Linux (Ubuntu, Debian, Fedora, Arch, Raspberry Pi OS...)",
                "install": ["sudo apt install wireguard-tools    # Fedora: sudo dnf install wireguard-tools · Arch: sudo pacman -S wireguard-tools"],
                "up": [f"sudo install -m 600 '{conf}' /etc/wireguard/{IFACE}.conf", f"sudo systemctl enable --now wg-quick@{IFACE}",
                       f"sudo ufw allow {port}/udp    # solo si usas ufw"],
                "reload": [f"sudo install -m 600 '{conf}' /etc/wireguard/{IFACE}.conf",
                           f"sudo bash -c 'wg syncconf {IFACE} <(wg-quick strip {IFACE})'"],
                "down": [f"sudo systemctl disable --now wg-quick@{IFACE}"],
            },
            "macos": {
                "title": "macOS (Terminal) — o la app WireGuard de la App Store: «Importar túnel desde archivo»",
                "install": ["brew install wireguard-tools"],
                "up": [f"sudo wg-quick up '{conf}'"],
                "reload": [f"sudo wg-quick down '{conf}'", f"sudo wg-quick up '{conf}'"],
                "down": [f"sudo wg-quick down '{conf}'"],
            },
            "wsl": {
                "title": "WSL (Nexus-Office dentro de Ubuntu en Windows)",
                "install": ["En Windows, %UserProfile%\\.wslconfig →  [wsl2]  networkingMode=mirrored   (y luego: wsl --shutdown)",
                            "sudo apt install wireguard-tools"],
                "up": [f"sudo install -m 600 '{conf}' /etc/wireguard/{IFACE}.conf", f"sudo wg-quick up {IFACE}",
                       f"(PowerShell admin) New-NetFirewallHyperVRule -Name NexusVPN -DisplayName 'Nexus-Office VPN' -Direction Inbound "
                       f"-VMCreatorId '{{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}}' -Protocol UDP -LocalPorts {port}"],
                "reload": [f"sudo install -m 600 '{conf}' /etc/wireguard/{IFACE}.conf",
                           f"sudo bash -c 'wg syncconf {IFACE} <(wg-quick strip {IFACE})'"],
                "down": [f"sudo wg-quick down {IFACE}"],
            },
        }

    def apply(self) -> dict:
        """Levanta o recarga el tunel en este PC si Nexus-Office tiene permisos; si no, devuelve los comandos."""
        self.write_conf()
        plat = platform_name()
        steps = self.instructions()[plat]
        cmds = steps["reload"] if self.tunnel_up() else steps["up"]
        tool = self.wireguard_tool()
        if not tool:
            return {"applied": False, "platform": plat, "reason": "WireGuard no está instalado en este PC",
                    "commands": steps["install"] + steps["up"]}
        try:
            if plat in ("linux", "wsl", "macos") and hasattr(os, "geteuid") and os.geteuid() == 0:
                if plat == "macos":
                    if self.tunnel_up():
                        subprocess.run([tool, "down", str(self.conf_file)], capture_output=True, timeout=30)
                    out = subprocess.run([tool, "up", str(self.conf_file)], capture_output=True, text=True, timeout=60)
                else:
                    target = Path("/etc/wireguard") / f"{IFACE}.conf"
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(self.conf_file, target)
                    os.chmod(target, 0o600)
                    if self.tunnel_up():
                        stripped = subprocess.run([tool, "strip", IFACE], capture_output=True, text=True, timeout=30).stdout
                        out = subprocess.run(["wg", "syncconf", IFACE, "/dev/stdin"], input=stripped, capture_output=True,
                                             text=True, timeout=30)
                    else:
                        out = subprocess.run([tool, "up", IFACE], capture_output=True, text=True, timeout=60)
            elif plat == "windows" and _is_windows_admin():
                if self.tunnel_up():
                    subprocess.run([tool, "/uninstalltunnelservice", IFACE], capture_output=True, timeout=60)
                    time.sleep(2)
                out = subprocess.run([tool, "/installtunnelservice", str(self.conf_file)], capture_output=True, text=True,
                                     timeout=60)
            else:
                return {"applied": False, "platform": plat, "reason": "Nexus-Office no se ejecuta con permisos de administrador",
                        "commands": cmds}
        except Exception as e:  # noqa: BLE001
            return {"applied": False, "platform": plat, "reason": str(e), "commands": cmds}
        ok = out.returncode == 0
        return {"applied": ok, "platform": plat, "reason": "" if ok else (out.stderr or out.stdout or "").strip()[-300:],
                "commands": [] if ok else cmds}

    def status(self) -> dict:
        plat = platform_name()
        return {"enabled": bool(self.cfg.get("vpn.enabled", False)), "port": self.port(), "subnet": str(self.network()),
                "server_ip": self.server_ip, "public_key": self.state.get("server_public", ""),
                "peers": [self._public(p) for p in self.state["peers"]], "conf_path": str(self.conf_file),
                "platform": plat, "wireguard": bool(self.wireguard_tool()), "tunnel_up": self.tunnel_up(),
                "listening_lan": self.cfg.get("server.host", "127.0.0.1") not in ("127.0.0.1", "localhost"),
                "instructions": self.instructions()}


def _private(p: Path) -> None:
    try:
        os.chmod(p, 0o600)
    except OSError:
        pass


def _is_windows_admin() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())   # type: ignore[attr-defined]
    except Exception:  # noqa: BLE001
        return False
