"""VPN WireGuard: claves, fichero del tunel del PC, codigo para la app y API."""
import base64

import pytest

from nexus.remote import vpn


def test_keys_are_valid_curve25519():
    priv, pub = vpn.keypair()
    assert len(base64.b64decode(priv)) == 32 and len(base64.b64decode(pub)) == 32
    assert vpn.public_from_private(priv) == pub
    assert len(base64.b64decode(vpn.preshared())) == 32


def test_code_roundtrip():
    data = {"v": 1, "e": ["1.2.3.4:51820"], "k": "x"}
    code = vpn.encode_code(data)
    assert code.startswith("NEXUSV1:") and "=" not in code and vpn.decode_code(code) == data


def test_server_conf_and_peers(services):
    v = services.vpn
    services.config.set("vpn.endpoints", ["casa.example.org"])
    r1 = v.add_peer("Móvil de Ana", "clave-red", public_ip="203.0.113.7")
    r2 = v.add_peer("Tablet", "clave-red")
    assert r1["peer"]["ip"] == "10.78.0.2" and r2["peer"]["ip"] == "10.78.0.3"
    conf = v.conf_file.read_text()
    assert "[Interface]" in conf and "ListenPort = 51820" in conf and "Address = 10.78.0.1/24" in conf
    assert conf.count("[Peer]") == 2 and f"PublicKey = {r1['peer']['public']}" in conf and "AllowedIPs = 10.78.0.2/32" in conf
    code = vpn.decode_code(r1["code"])
    assert code["a"] == "10.78.0.2/32" and code["s"] == v.state["server_public"] and code["key"] == "clave-red"
    assert code["e"][0] == "casa.example.org:51820" and code["e"][1] == "203.0.113.7:51820"
    assert code["u"] == "http://10.78.0.1:8775" and code["r"] == "10.78.0.1/32"
    assert vpn.public_from_private(code["k"]) == r1["peer"]["public"]          # la privada corresponde a la publica
    assert "Endpoint = casa.example.org:51820" in r1["client_conf"] and "PersistentKeepalive = 25" in r1["client_conf"]
    assert code["k"] not in str(v.status())                                    # la privada del movil no se guarda
    assert code["k"] not in v.state_file.read_text()
    assert v.remove_peer(r1["peer"]["id"]) and not v.remove_peer("nada")
    assert v.conf_file.read_text().count("[Peer]") == 1 and "10.78.0.2/32" not in v.conf_file.read_text()
    assert v.add_peer("Otro", "k")["peer"]["ip"] == "10.78.0.2"                # reutiliza la direccion libre


def test_tunnel_address_is_not_an_endpoint(services, monkeypatch):
    from nexus.coop import node
    monkeypatch.setattr(node, "overlay_addresses", lambda: [{"iface": "nexusvpn", "ip": "10.78.0.1", "kind": "lan"},
                                                          {"iface": "eth0", "ip": "192.168.1.20", "kind": "lan"}])
    assert services.vpn.endpoints() == ["192.168.1.20:51820"]


def test_bad_subnet(services):
    services.config.set("vpn.subnet", "10.0.0.0/30")
    with pytest.raises(ValueError):
        services.vpn.network()


def test_instructions_cover_all_platforms(services):
    ins = services.vpn.instructions()
    assert set(ins) == {"windows", "linux", "macos", "wsl"}
    assert any("installtunnelservice" in c for c in ins["windows"]["up"])
    assert any("wg-quick@nexusvpn" in c for c in ins["linux"]["up"])
    assert any("wg-quick up" in c for c in ins["macos"]["up"])


def test_vpn_api_requires_admin_and_works(services):
    from fastapi.testclient import TestClient
    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        assert c.get("/api/vpn").status_code == 403
        tok = c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]
        h = {"X-Admin-Token": tok}
        assert c.post("/api/vpn/peers", json={"name": "x"}, headers=h).status_code == 400   # sin activar
        r = c.post("/api/vpn/enable", json={"enabled": True, "port": 51999}, headers=h).json()
        assert r["enabled"] and r["port"] == 51999 and r["restart_required"] and services.config.get("server.host") == "0.0.0.0"
        services.config.set("vpn.endpoints", ["198.51.100.4"])
        p = c.post("/api/vpn/peers", json={"name": "Móvil"}, headers=h).json()
        assert p["code"].startswith("NEXUSV1:") and p["endpoints"][0] == "198.51.100.4:51999"
        conf = c.get("/api/vpn/server.conf", headers=h)
        assert conf.status_code == 200 and "ListenPort = 51999" in conf.text and "attachment" in conf.headers["content-disposition"]
        st = c.get("/api/vpn", headers=h).json()
        assert len(st["peers"]) == 1 and st["platform"] in ("linux", "wsl", "windows", "macos")
        ap = c.post("/api/vpn/apply", json={}, headers=h).json()
        assert "applied" in ap and (ap["applied"] or ap["commands"])
        assert c.delete(f"/api/vpn/peers/{p['peer']['id']}", headers=h).status_code == 200
