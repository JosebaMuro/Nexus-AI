"""Cliente UPnP-IGD minimo: abrir/cerrar un puerto TCP en el router y leer la IP publica.

Solo se usa cuando el administrador lo pide desde el panel (nunca automaticamente). Sin dependencias:
SSDP (UDP multicast) + SOAP sobre HTTP con httpx.
"""
from __future__ import annotations

import ipaddress
import socket
import xml.etree.ElementTree as ET
from urllib.parse import urljoin, urlparse

import httpx

SSDP = ("239.255.255.250", 1900)
TARGETS = ("urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2")
SERVICES = ("urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
            "urn:schemas-upnp-org:service:WANPPPConnection:1")
DESC = "Nexus-Office acceso remoto"


def parse_ssdp(reply: bytes) -> str | None:
    for line in reply.decode(errors="ignore").splitlines():
        k, _, v = line.partition(":")
        if k.strip().lower() == "location":
            return v.strip()
    return None


def parse_description(xml_text: str, location: str) -> tuple[str, str] | None:
    """(url de control, tipo de servicio) del servicio WAN que permite mapear puertos."""
    root = ET.fromstring(xml_text)
    ns = ""
    if root.tag.startswith("{"):
        ns = root.tag.split("}")[0] + "}"
    base = (root.findtext(f"{ns}URLBase") or location).strip()
    found = {}
    for svc in root.iter(f"{ns}service"):
        st, ctl = (svc.findtext(f"{ns}serviceType") or "").strip(), (svc.findtext(f"{ns}controlURL") or "").strip()
        if st in SERVICES and ctl:
            found[st] = urljoin(base, ctl)
    for st in SERVICES:
        if st in found:
            return found[st], st
    return None


def soap_body(action: str, service: str, args: dict) -> str:
    inner = "".join(f"<{k}>{v}</{k}>" for k, v in args.items())
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            f'<u:{action} xmlns:u="{service}">{inner}</u:{action}></s:Body></s:Envelope>')


def parse_soap_value(xml_text: str, field: str) -> str:
    for el in ET.fromstring(xml_text).iter():
        if el.tag.split("}")[-1] == field:
            return (el.text or "").strip()
    return ""


def is_cgnat_or_private(ip: str) -> bool:
    """IP 'publica' que en realidad no lo es: doble NAT del operador (CGNAT 100.64/10) o privada."""
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return True
    return a.is_private or a in ipaddress.ip_network("100.64.0.0/10") or a.is_loopback or a.is_link_local


def local_ip_towards(host: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 80))
        return s.getsockname()[0]
    finally:
        s.close()


class Gateway:
    def __init__(self, control_url: str, service: str) -> None:
        self.control_url, self.service = control_url, service

    @classmethod
    def discover(cls, timeout: float = 2.5) -> "Gateway":
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.settimeout(timeout)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        locations: list[str] = []
        try:
            for st in TARGETS:
                msg = (f"M-SEARCH * HTTP/1.1\r\nHOST: {SSDP[0]}:{SSDP[1]}\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: {st}\r\n\r\n")
                sock.sendto(msg.encode(), SSDP)
            while True:
                try:
                    data, _ = sock.recvfrom(4096)
                except socket.timeout:
                    break
                loc = parse_ssdp(data)
                if loc and loc not in locations:
                    locations.append(loc)
        finally:
            sock.close()
        for loc in locations:
            try:
                svc = parse_description(httpx.get(loc, timeout=4).text, loc)
            except Exception:  # noqa: BLE001
                continue
            if svc:
                return cls(*svc)
        raise RuntimeError("No se encontro un router con UPnP activo (activalo en el router o abre el puerto a mano)")

    def _call(self, action: str, args: dict) -> str:
        r = httpx.post(self.control_url, content=soap_body(action, self.service, args), timeout=6,
                       headers={"Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": f'"{self.service}#{action}"'})
        if r.status_code != 200:
            err = parse_soap_value(r.text, "errorDescription") if r.text else ""
            raise RuntimeError(f"El router rechazo {action}: {err or r.status_code}")
        return r.text

    def external_ip(self) -> str:
        return parse_soap_value(self._call("GetExternalIPAddress", {}), "NewExternalIPAddress")

    def add_mapping(self, port: int, lease_s: int = 0, protocol: str = "TCP") -> str:
        internal = local_ip_towards(urlparse(self.control_url).hostname or "8.8.8.8")
        self._call("AddPortMapping", {"NewRemoteHost": "", "NewExternalPort": port, "NewProtocol": protocol,
                                      "NewInternalPort": port, "NewInternalClient": internal, "NewEnabled": 1,
                                      "NewPortMappingDescription": DESC, "NewLeaseDuration": lease_s})
        return internal

    def delete_mapping(self, port: int, protocol: str = "TCP") -> None:
        self._call("DeletePortMapping", {"NewRemoteHost": "", "NewExternalPort": port, "NewProtocol": protocol})
