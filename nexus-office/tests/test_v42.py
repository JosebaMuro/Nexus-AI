"""Agente IA 4.2: noticias RSS de la mesa, broker Alpaca en papel y editor de video (paridad con la C++ 3.x)."""
import json

import httpx
import pytest
from fastapi.testclient import TestClient

RSS = """<?xml version="1.0"?><rss version="2.0"><channel><title>x</title>
<item><title><![CDATA[Iberdrola dispara sus beneficios &amp; mejora previsiones]]></title>
<link>https://ejemplo.test/a</link><pubDate>{fecha}</pubDate><source url="https://ejemplo.test">Expansion</source></item>
<item><title>Caida y perdidas para el sector</title><link>https://ejemplo.test/b</link><pubDate>{fecha}</pubDate></item>
<item><description>sin titulo</description></item>
</channel></rss>"""


def create(services):
    from nexus.server.app import create_app
    return create_app(services)


def _fecha():
    import email.utils
    return email.utils.formatdate(usegmt=True)


def test_leer_rss():
    from nexus.office import bolsa_noticias as N
    items = N.leer_rss(RSS.format(fecha=_fecha()))
    assert len(items) == 2
    assert items[0]["titulo"] == "Iberdrola dispara sus beneficios & mejora previsiones"
    assert items[0]["enlace"] == "https://ejemplo.test/a" and items[0]["fuente"] == "Expansion" and items[0]["ts"] > 0
    assert N.leer_rss("") == [] and N.fecha_rss("no es fecha") == 0.0
    nombres = [f for f, _ in N.fuentes("IBE.MC", "Iberdrola")]
    assert nombres == ["Yahoo Finance", "Google Noticias"]


def test_noticias_cache_y_sin_conexion(monkeypatch):
    from nexus.office import bolsa_noticias as N
    pedidas = []

    def ok(req):
        pedidas.append(str(req.url))
        return httpx.Response(200, text=RSS.format(fecha=_fecha()))

    with httpx.Client(transport=httpx.MockTransport(ok)) as c:
        r = N.noticias("ibe.mc", "Iberdrola", client=c)
    assert r["simbolo"] == "IBE.MC" and len(r["noticias"]) == 2 and not r["errores"] and len(pedidas) == 2
    assert r["noticias"][0]["sentimiento"] is not None
    # sin conexion: se sirve lo guardado y se avisa
    with httpx.Client(transport=httpx.MockTransport(lambda req: httpx.Response(503))) as c:
        r2 = N.noticias("IBE.MC", refrescar=True, client=c)
    assert len(r2["noticias"]) == 2 and r2["sin_conexion"] and len(r2["errores"]) == 2
    # dentro del TTL no se consulta
    with httpx.Client(transport=httpx.MockTransport(ok)) as c:
        N.noticias("IBE.MC", client=c)
    assert len(pedidas) == 2
    assert N.titulares("IBE.MC")[:1] == ["Iberdrola dispara sus beneficios & mejora previsiones"]
    with pytest.raises(ValueError):
        N.noticias("  ")


def _alpaca(pedidas):
    def h(req):
        pedidas.append((req.method, req.url.path, req.headers.get("APCA-API-KEY-ID"),
                        json.loads(req.content) if req.content else None))
        if req.url.path == "/v2/account":
            return httpx.Response(200, json={"status": "ACTIVE", "cash": "1000", "currency": "USD"})
        if req.url.path == "/v2/orders" and req.method == "POST":
            return httpx.Response(200, json={"id": "abc", "status": "accepted"})
        if req.url.path == "/v2/orders":
            return httpx.Response(200, json=[{"id": "abc", "symbol": "AAPL", "status": "filled"}])
        if req.url.path == "/v2/positions":
            return httpx.Response(200, json=[{"symbol": "AAPL", "qty": "2"}])
        if req.method == "DELETE":
            return httpx.Response(204)
        return httpx.Response(404, json={"message": "no"})
    return h


def test_broker():
    from nexus.office import bolsa_broker as B
    manual = {"tipo": "manual"}
    with pytest.raises(PermissionError):
        B.enviar({"simbolo": "AAPL", "lado": "compra", "cantidad": 1}, "si", cfg=manual)
    for mala in ({"lado": "compra", "cantidad": 1}, {"simbolo": "X", "lado": "regalo", "cantidad": 1},
                 {"simbolo": "X", "lado": "compra", "cantidad": 0},
                 {"simbolo": "X", "lado": "venta", "cantidad": 1, "tipo": "limitada"}):
        with pytest.raises(ValueError):
            B.enviar(mala, "CONFIRMO", cfg=manual)
    r = B.enviar({"simbolo": "aapl", "lado": "compra", "cantidad": 2}, "confirmo", cfg=manual)
    assert r["estado"] == "pendiente_manual" and r["simbolo"] == "AAPL" and r["lado"] == "buy"
    assert B.ordenes()["registro"][0]["id"] == r["id"]
    # el real solo con las dos opciones
    assert B.entorno_efectivo(B._cfg({"tipo": "alpaca", "entorno": "real"})) == "papel"
    assert B.entorno_efectivo(B._cfg({"tipo": "alpaca", "entorno": "real", "permitir_real": True})) == "real"
    assert "aviso" in B.estado(cfg={"tipo": "alpaca", "entorno": "real"})
    alp = {"tipo": "alpaca", "clave": "k", "secreto": "s"}
    assert B.estado(cfg={"tipo": "alpaca"})["configurado"] is False
    pedidas = []
    with httpx.Client(base_url="https://paper-api.alpaca.markets", transport=httpx.MockTransport(_alpaca(pedidas)),
                      headers={"APCA-API-KEY-ID": "k"}) as c:
        assert B.estado(True, cfg=alp, client=c)["cuenta"]["status"] == "ACTIVE"
        o = B.enviar({"simbolo": "AAPL", "lado": "venta", "cantidad": 1, "tipo": "limitada", "limite": 190},
                     "CONFIRMO", cfg=alp, client=c)
        assert o["estado"] == "accepted" and o["broker_id"] == "abc"
        cuerpo = [p for p in pedidas if p[0] == "POST"][0][3]
        assert cuerpo["side"] == "sell" and cuerpo["type"] == "limit" and cuerpo["limit_price"] == "190.0"
        assert cuerpo["client_order_id"] == "agente-ia-" + o["id"]
        s = B.ordenes(True, cfg=alp, client=c)
        assert s["broker"][0]["status"] == "filled" and s["posiciones"][0]["qty"] == "2"
        assert B.cancelar("abc", cfg=alp, client=c) == {"cancelada": "abc"}
    with httpx.Client(base_url="https://x.test", transport=httpx.MockTransport(lambda r: httpx.Response(401))) as c:
        with pytest.raises(PermissionError):
            B.enviar({"simbolo": "AAPL", "lado": "compra", "cantidad": 1}, "CONFIRMO", cfg=alp, client=c)
    assert B.ordenes()["registro"][0]["estado"] == "rechazada"


def test_rutas_noticias_broker_y_editor(services):
    from nexus.office import bolsa
    sym = bolsa.get().instrumentos("")[0]["simbolo"]
    with TestClient(create(services)) as c:
        n = c.get(f"/api/bolsa/noticias/{sym}").json()   # sin red en las pruebas: vacio, sin error
        assert n["simbolo"] == sym and n["noticias"] == []
        assert c.get("/api/bolsa/broker").status_code == 403   # solo administrador
        h = {"X-Admin-Token": c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]}
        e = c.get("/api/bolsa/broker", headers=h).json()
        assert e["tipo"] == "manual" and e["entorno"] == "papel" and e["confirmacion"] == "CONFIRMO"
        orden = {"simbolo": "AAPL", "lado": "compra", "cantidad": 1}
        assert c.post("/api/bolsa/broker/orden", json={"orden": orden, "confirmacion": "no"}, headers=h).status_code == 403
        assert c.post("/api/bolsa/broker/orden", json={"orden": {}, "confirmacion": "CONFIRMO"},
                      headers=h).status_code == 400
        r = c.post("/api/bolsa/broker/orden", json={"orden": orden, "confirmacion": "CONFIRMO"}, headers=h).json()
        assert r["estado"] == "pendiente_manual"
        assert c.get("/api/bolsa/broker/ordenes", headers=h).json()["registro"][0]["id"] == r["id"]
        c.put("/api/config", json={"broker.clave": "secreta123"}, headers=h)
        assert "secreta123" not in c.get("/api/config", headers=h).text
        assert c.get("/static/video.html").status_code == 200
        assert c.get("/static/js/video-imagen.js").status_code == 200


def test_video_deja_proyecto_para_el_editor(services):
    from PIL import Image
    from nexus import paths
    carpeta = paths.sub("outputs", "images")
    for i in range(2):
        Image.new("RGB", (64, 64), "red").save(carpeta / f"p{i}.png")
    with TestClient(create(services)) as c:
        r = c.post("/api/video/rapido", json={"ultimas": 2, "segundos": 0.3, "titulo": "Editor"}).json()
        assert r["editor"].startswith("/static/video.html?proyecto=/outputs/videos/")
        proyecto = c.get(r["editor"].split("proyecto=")[1]).json()
        assert len(proyecto["imagenes"]) == 2 and proyecto["imagenes"][0].startswith("/outputs/")
        assert all(u.endswith(".png") for u in c.get("/api/outputs").json()["images"])
