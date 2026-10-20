"""Acceso remoto por internet: TLS 1.3, certificado anclado, emparejamiento, puerta del puerto remoto."""
import socket
import ssl
import threading

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from nexus.remote import COOKIE, RemoteGate, read_invite
from nexus.remote.devices import DeviceStore, RateLimiter, normalize_code
from nexus.remote.tlscert import ensure_cert, fingerprint, server_context
from nexus.remote.upnp import is_cgnat_or_private, parse_description, parse_soap_value, parse_ssdp, soap_body


def test_certificate_is_persistent_and_pinnable(tmp_path):
    cert, key, fp = ensure_cert(tmp_path, ["192.168.1.20", "nexus.example"])
    assert len(fp) == 64 and fingerprint(cert.read_bytes()) == fp
    assert ensure_cert(tmp_path)[2] == fp                      # se conserva: la huella anclada no cambia
    assert ensure_cert(tmp_path, regenerate=True)[2] != fp     # regenerar si cambia
    assert b"PRIVATE KEY" in key.read_bytes()


def test_tls13_only_handshake_with_pinned_fingerprint(tmp_path):
    cert, key, fp = ensure_cert(tmp_path)
    srv_ctx = server_context(cert, key)
    lsock = socket.socket()
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(2)
    port = lsock.getsockname()[1]

    def serve():
        for _ in range(2):
            conn, _ = lsock.accept()
            try:
                with srv_ctx.wrap_socket(conn, server_side=True) as tls:
                    tls.sendall(b"ok")
            except (ssl.SSLError, OSError):
                pass
    t = threading.Thread(target=serve, daemon=True)
    t.start()
    # cliente moderno: TLS 1.3 + comprobacion de la huella (como hace la app Android)
    cli = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    cli.check_hostname, cli.verify_mode = False, ssl.CERT_NONE
    with cli.wrap_socket(socket.create_connection(("127.0.0.1", port)), server_hostname="nexus") as tls:
        import hashlib
        assert hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest().upper() == fp
        assert tls.version() == "TLSv1.3"
        assert tls.cipher()[0] in {"TLS_AES_256_GCM_SHA384", "TLS_AES_128_GCM_SHA256", "TLS_CHACHA20_POLY1305_SHA256"}
        assert tls.recv(2) == b"ok"
    # cliente limitado a TLS 1.2: rechazado
    old = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    old.check_hostname, old.verify_mode = False, ssl.CERT_NONE
    old.maximum_version = ssl.TLSVersion.TLSv1_2
    with pytest.raises((ssl.SSLError, ConnectionError, OSError)):
        with old.wrap_socket(socket.create_connection(("127.0.0.1", port)), server_hostname="nexus") as tls:
            tls.recv(2)
    t.join(5)
    lsock.close()


def test_pairing_codes_single_use_and_tokens_hashed(tmp_path):
    store = DeviceStore(tmp_path)
    p = store.new_pairing("Movil")
    assert len(normalize_code(p["code"])) == 12
    token, dev = store.pair(p["code"].lower(), "", "1.2.3.4")
    assert dev["name"] == "Movil" and store.check(token) == dev["id"]
    assert store.pair(p["code"], "", "1.2.3.4") is None        # un solo uso
    assert token not in store.path.read_text(encoding="utf-8")  # solo se guarda la huella de la llave
    assert DeviceStore(tmp_path).check(token) == dev["id"]      # persiste
    assert store.revoke(dev["id"]) and store.check(token) is None
    lim = RateLimiter(max_fails=3)
    for _ in range(3):
        lim.fail("ip")
    assert lim.blocked("ip") and not lim.blocked("otra")


@pytest.fixture
def remote_clients(services):
    from nexus.server import create_app
    app = create_app(services)
    state = app.state.nexus_state
    gate = RemoteGate(app, lambda: state["s"].remote if "s" in state else None)
    with TestClient(app) as local, TestClient(gate, base_url="https://nexus.example") as remote:
        token = local.post("/api/admin/setup", json={"pin": "4321"}).json()["token"]
        yield local, remote, {"X-Admin-Token": token}


def test_remote_gate_requires_paired_device(remote_clients):
    local, remote, admin = remote_clients
    r = remote.get("/")
    assert r.status_code == 401 and "Emparejar" in r.text and ":" in r.text      # pagina de emparejamiento con la huella
    assert r.headers["x-frame-options"] == "DENY" and r.headers["referrer-policy"] == "no-referrer"
    assert remote.get("/api/status").status_code == 401
    assert remote.get("/api/memory").status_code == 401
    assert remote.get("/outputs/cualquier.png").status_code == 401
    assert remote.get("/api/ping").status_code == 200
    assert remote.get("/static/js/app.js").status_code == 200                       # la interfaz es publica
    with pytest.raises(WebSocketDisconnect):
        with remote.websocket_connect("/ws") as ws:
            ws.receive_text()
    # una llave inventada tampoco sirve
    assert remote.get("/api/status", headers={"X-Nexus-Device": "inventada"}).status_code == 401


def test_remote_pairing_flow_and_revocation(remote_clients):
    local, remote, admin = remote_clients
    pairing = local.post("/api/remote/pairing", json={"name": "Movil de prueba"}, headers=admin).json()
    inv = read_invite(pairing["invite"])
    assert inv["c"] == pairing["code"] and len(inv["f"]) == 64
    assert any(link.endswith("/?pair=" + pairing["code"]) for link in pairing["links"]) or not pairing["links"]
    r = remote.post("/api/remote/pair", json={"code": pairing["code"], "name": ""})
    assert r.status_code == 200 and r.json()["device"]["name"] == "Movil de prueba"
    sc = r.headers["set-cookie"]
    assert COOKIE in sc and "HttpOnly" in sc and "Secure" in sc and "samesite=strict" in sc.lower()
    assert remote.get("/api/status").status_code == 200                              # la cookie ya autentica
    token = r.json()["token"]
    with remote.websocket_connect("/ws", headers={"X-Nexus-Device": token}) as ws:    # y el websocket tambien
        ws.send_text('{"type": "noop"}')
    assert remote.post("/api/remote/pair", json={"code": pairing["code"]}).status_code == 401   # codigo gastado
    devs = local.get("/api/remote", headers=admin).json()["devices"]
    assert len(devs) == 1
    assert local.delete(f"/api/remote/devices/{devs[0]['id']}", headers=admin).status_code == 200
    assert remote.get("/api/status").status_code == 401                              # revocado al instante


def test_remote_brute_force_is_throttled(remote_clients):
    local, remote, admin = remote_clients
    # dispositivo emparejado antes de los ataques (sirve luego para probar el limite del PIN)
    code = local.post("/api/remote/pairing", json={}, headers=admin).json()["code"]
    dev = {"X-Nexus-Device": remote.post("/api/remote/pair", json={"code": code}).json()["token"]}
    remote.cookies.clear()                       # a partir de aqui, un dispositivo desconocido
    # sin emparejar, por el puerto remoto ni siquiera se puede intentar el PIN
    r = remote.post("/api/admin/login", json={"pin": "0000"})
    assert r.status_code == 401 and "emparejado" in r.json()["detail"]
    # codigos inventados: 8 fallos y la IP queda bloqueada, incluso para un codigo valido
    codes = [remote.post("/api/remote/pair", json={"code": "AAAA-BBBB-CCCC"}).status_code for _ in range(9)]
    assert codes[:8] == [401] * 8 and codes[8] == 429
    valid = local.post("/api/remote/pairing", json={}, headers=admin).json()["code"]
    assert remote.post("/api/remote/pair", json={"code": valid}).status_code == 429
    # PIN desde un dispositivo emparejado: limite por IP; en el propio equipo no se bloquea
    pins = [remote.post("/api/admin/login", json={"pin": "0000"}, headers=dev).status_code for _ in range(9)]
    assert pins[:8] == [401] * 8 and pins[8] == 429
    assert remote.post("/api/admin/login", json={"pin": "4321"}, headers=dev).status_code == 429   # bloqueado aunque acierte
    assert local.post("/api/admin/login", json={"pin": "4321"}).status_code == 200


def test_remote_admin_endpoints_need_admin(remote_clients):
    local, remote, admin = remote_clients
    assert local.get("/api/remote").status_code == 403
    st = local.post("/api/remote/enable", json={"enabled": True, "port": 8443}, headers=admin).json()
    assert st["enabled"] and st["restart_required"] and "TLS 1.3" in st["crypto"]
    assert local.post("/api/remote/enable", json={"enabled": True, "port": 8775}, headers=admin).status_code == 400


def test_upnp_parsers():
    assert parse_ssdp(b"HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1:5000/desc.xml\r\nST: x\r\n\r\n") == "http://192.168.1.1:5000/desc.xml"
    desc = """<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><device><deviceList><device><serviceList>
      <service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL></service>
      </serviceList></device></deviceList></device></root>"""
    assert parse_description(desc, "http://192.168.1.1:5000/desc.xml") == (
        "http://192.168.1.1:5000/ctl/IPConn", "urn:schemas-upnp-org:service:WANIPConnection:1")
    body = soap_body("AddPortMapping", "urn:x", {"NewExternalPort": 8443, "NewProtocol": "TCP"})
    assert "<NewExternalPort>8443</NewExternalPort>" in body and 'xmlns:u="urn:x"' in body
    resp = ('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetExternalIPAddressResponse xmlns:u="urn:x">'
            "<NewExternalIPAddress>81.40.1.2</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>")
    assert parse_soap_value(resp, "NewExternalIPAddress") == "81.40.1.2"
    assert is_cgnat_or_private("100.72.1.9") and is_cgnat_or_private("192.168.1.1") and not is_cgnat_or_private("81.40.1.2")


def test_android_app_pins_certificate():
    from pathlib import Path
    root = Path(__file__).resolve().parent.parent
    src = root / "android" / "src" / "ai" / "nexus" / "office" / "client" / "MainActivity.java"
    if not src.exists():
        pytest.skip("el codigo de la app Android solo va en el proyecto completo y el paquete python")
    java = src.read_text(encoding="utf-8")
    assert "onReceivedSslError" in java and 'pin.equalsIgnoreCase(certSha256(error.getCertificate()))' in java
    assert "handler.cancel()" in java and java.index("handler.proceed()") < java.index("handler.cancel()")
    assert "public String openRemote(String code)" in java and "NEXUSR1:" in java
    setup = (root / "android" / "assets" / "setup.html").read_text(encoding="utf-8")
    assert "App.openRemote(code)" in setup


def test_router_assistant_parsers_and_brands():
    from nexus.remote import router
    route = """===========================================================================
Rutas activas:
Destino de red        Máscara de red   Puerta de enlace   Interfaz  Métrica
          0.0.0.0          0.0.0.0      192.168.0.1     192.168.0.41     40
          0.0.0.0          0.0.0.0       10.0.0.1         10.0.0.7     55
"""
    assert router.parse_route_print(route) == "192.168.0.1"                       # la de menor metrica
    proc = "Iface\tDestination\tGateway \tFlags\nwlan0\t00000000\t0100A8C0\t0003\nwlan0\t0000A8C0\t00000000\t0001\n"
    assert router.parse_proc_route(proc) == "192.168.0.1"
    assert router.identify("<title>Livebox 6</title>") == "orange"
    assert router.identify("FRITZ!Box 7590") == "fritz"
    assert router.identify("Archer AX55", "TP-Link") == "tplink"
    assert router.identify("Movistar HGU Askey") == "movistar"
    assert router.identify("Broadcom Corporation WPS X1") == "generic"
    assert all(len(g) == 3 and g[1] for g in router.GUIDES.values())


def test_router_page_probe_tolerates_odd_servers():
    """Muchos routers responden con lineas de estado no estandar (como 'UNKNOWN 400 Bad Request')."""
    from nexus.remote import router
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(2)
    port = srv.getsockname()[1]
    replies = [b"UNKNOWN 400 Bad Request\r\nDate: x\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD></HTML>",
               b"HTTP/1.0 200 OK\r\nServer: httpd\r\n\r\n<html><head><title> Livebox  Orange </title></head></html>"]

    def serve():
        for body in replies:
            c, _ = srv.accept()
            c.recv(4096)
            c.sendall(body)
            c.close()
    t = threading.Thread(target=serve, daemon=True)
    t.start()
    odd = router.probe_page("127.0.0.1", port, False)
    assert odd["responds"] and odd["title"] == ""                                  # "400 Bad Request" no identifica nada
    ok = router.probe_page("127.0.0.1", port, False)
    assert ok["title"] == "Livebox Orange" and router.identify(ok["text"]) == "orange"
    t.join(5)
    srv.close()
    assert router.probe_page("127.0.0.1", 1, False) is None                       # nada escuchando


def test_router_endpoint(remote_clients, monkeypatch):
    from nexus.remote import router
    local, remote, admin = remote_clients
    fake = {"gateway": "192.168.0.1", "gateway_private": True, "local_ip": "192.168.0.41", "interface": {"name": "Wi-Fi", "mac": "CC:15:31:00:00:01"},
            "admin_urls": ["http://192.168.0.1/"], "page_title": "", "brand": "generic", "brand_name": "Router no identificado",
            "menu_path": router.GUIDES["generic"][1], "tip": "", "upnp_igd": False, "upnp_devices": [],
            "rule": {"name": "Nexus-Office", "protocol": "TCP", "external_port": 8443, "internal_ip": "192.168.0.41", "internal_port": 8443}}
    monkeypatch.setattr(router, "detect", lambda port: fake)
    assert local.get("/api/remote/router").status_code == 403                    # solo administrador
    d = local.get("/api/remote/router", headers=admin).json()
    assert d["rule"]["internal_ip"] == "192.168.0.41" and d["port"] == 8443 and d["enabled"] is False
    assert remote.get("/api/remote/router", headers=admin).status_code == 401    # por internet, primero emparejar
