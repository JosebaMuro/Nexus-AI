"""Cotizaciones reales (con fuentes simuladas por httpx.MockTransport) y agente de bolsa autonomo."""
import datetime as dt
import json
import math

import httpx
import pytest

from nexus.office import bolsa as B
from nexus.office import bolsa_agente as A
from nexus.office import bolsa_datos as D

UTC = dt.timezone.utc


class Clock:
    def __init__(self, *a):
        self.t = dt.datetime(*a, tzinfo=UTC)

    def __call__(self):
        return self.t


def yahoo_chart(sym, price, prev, n=0, step=0.0, currency="EUR", t0=1_790_000_000):
    ts = [t0 + i * 86400 for i in range(n)]
    closes = [price - step * (n - 1 - i) for i in range(n)]
    return {"chart": {"result": [{"meta": {"regularMarketPrice": price, "previousClose": prev, "currency": currency,
                                           "regularMarketTime": t0 + n * 86400},
                                  "timestamp": ts,
                                  "indicators": {"quote": [{"close": closes, "high": [c * 1.01 for c in closes],
                                                            "low": [c * 0.99 for c in closes]}]}}]}}


def test_yahoo_quote_series_and_fallbacks():
    calls = []

    def handler(req: httpx.Request):
        calls.append(str(req.url))
        if "yahoo" in req.url.host:
            sym = req.url.path.rsplit("/", 1)[-1]
            if sym == "SAN.MC":
                return httpx.Response(200, json=yahoo_chart(sym, 7.25, 7.0, n=3, step=0.1))
            if sym == "SHEL.L":
                return httpx.Response(200, json=yahoo_chart(sym, 2700.0, 2650.0, currency="GBp"))
            if sym == "EURUSD=X":
                return httpx.Response(200, json=yahoo_chart(sym, 1.08, 1.07))
            return httpx.Response(503)
        if req.url.host == "stooq.com":
            return httpx.Response(200, text="Symbol,Date,Time,Open,High,Low,Close,Volume,Prev\nAAPL.US,2026-10-26,"
                                             "22:00:00,230,233,229,231.5,1000,228.0\n")
        if req.url.host == "api.coinbase.com":
            return httpx.Response(200, json={"data": {"amount": "61234.5", "currency": "EUR"}})
        return httpx.Response(404)

    d = D.DatosMercado(httpx.Client(transport=httpx.MockTransport(handler)))
    q = d.quote("SAN.MC")
    assert q["precio"] == 7.25 and q["cierre_anterior"] == 7.0 and q["fuente"] == "Yahoo Finance"
    assert len(q["puntos"]) == 3
    assert d.quote("san.mc") is q                       # cache
    assert d.fx("USD") == 1.08
    # Yahoo cae para AAPL (503) -> Stooq; la fuente Yahoo queda apartada un rato y se salta
    a = d.quote("AAPL")
    assert a["fuente"] == "Stooq" and a["precio"] == 231.5 and a["cierre_anterior"] == 228.0
    assert not d.fuentes["yahoo"].disponible()
    btc = d.quote("BTC-EUR")
    assert btc["fuente"] == "Coinbase" and btc["precio"] == 61234.5
    assert d.estado()["fuentes"]["yahoo"]["fallos"] >= 1


def test_mesa_usa_precio_real_y_peniques(tmp_path):
    def handler(req):
        sym = req.url.path.rsplit("/", 1)[-1]
        if sym == "SHEL.L":
            return httpx.Response(200, json=yahoo_chart(sym, 2700.0, 2650.0, currency="GBp"))
        return httpx.Response(200, json=yahoo_chart(sym, 7.25, 7.0, n=40, step=0.01))

    b = B.Bolsa(tmp_path / "b.db", seed=1, clock=Clock(2026, 10, 20, 8, 0))
    b.datos = D.DatosMercado(httpx.Client(transport=httpx.MockTransport(handler)))
    q = b.precio("SAN.MC")
    assert q["real"] and q["precio"] == 7.25 and q["fuente"] == "Yahoo Finance" and q["variacion_pct"] == 3.57
    assert b.precio("SHEL.L")["precio"] == 27.0             # 2700 peniques = 27 libras
    s = b.serie("SAN.MC", "6mo")
    assert s["real"] and len(s["puntos"]) == 40
    assert b._one("SELECT COUNT(*) n FROM historico WHERE simbolo='SAN.MC'")["n"] == 40   # guardado para offline
    o = b.ordenar("SAN.MC", "compra", 100)
    assert o["estado"] == "ejecutada" and abs(o["precio_ejec"] - 7.25) < 0.05
    b.close()


def test_sin_conexion_sigue_simulado(tmp_path):
    b = B.Bolsa(tmp_path / "b.db", seed=1, clock=Clock(2026, 10, 20, 8, 0))
    b.datos = D.DatosMercado(httpx.Client(transport=httpx.MockTransport(lambda r: (_ for _ in ()).throw(
        httpx.ConnectError("sin red")))))
    q = b.precio("SAN.MC")
    assert not q["real"] and q["fuente"] == "simulado"
    assert b.datos.estado()["en_linea"] is False
    b.close()


def test_indicadores():
    x = [100 + i for i in range(60)]
    assert A.sma(x, 20) == sum(x[-20:]) / 20
    assert A.rsi(x) == 100.0
    an = A.analizar([{"precio": p} for p in x])
    assert an["factores"]["tendencia"] == 1.0 and an["factores"]["momento"] > 0
    assert A.analizar([{"precio": 1}] * 5) is None


class FakeDatos:
    """Proveedor determinista: SAN.MC sube con fuerza, TEF.MC cae, el resto plano."""

    def __init__(self):
        self.px = {"SAN.MC": 8.0, "TEF.MC": 3.0, "ITX.MC": 50.0}

    def series_for(self, sym):
        n = 120
        if sym == "SAN.MC":
            return [{"ts": 1.7e9 + i * 86400, "precio": 5.0 + 3.0 * i / (n - 1) + 0.15 * math.sin(i * 1.3)} for i in range(n)]
        if sym == "TEF.MC":
            return [{"ts": 1.7e9 + i * 86400, "precio": 5.0 - 2.0 * i / (n - 1)} for i in range(n)]
        return [{"ts": 1.7e9 + i * 86400, "precio": 50.0 + 0.2 * math.sin(i / 3)} for i in range(n)]

    def quote(self, sym, max_age=None):
        if sym not in self.px:
            return None
        return {"precio": self.px[sym], "cierre_anterior": self.px[sym], "fuente": "prueba", "retraso": "", "puntos": []}

    def serie(self, sym, rango="1d"):
        return self.series_for(sym) if sym in self.px else []

    def fx(self, d):
        return None

    def estado(self):
        return {"en_linea": True, "fuentes": {}}


def test_agente_autonomo_compra_pone_stop_y_vende_en_objetivo(tmp_path):
    clock = Clock(2026, 10, 20, 8, 0)          # martes 10:00 en Madrid: BME abierta
    b = B.Bolsa(tmp_path / "b.db", seed=3, clock=clock)
    b.datos = FakeDatos()
    ag = A.AgenteBolsa(b, lambda: {"universo": ["SAN.MC", "TEF.MC", "ITX.MC"], "capital": 10000})
    r = ag.ciclo()
    assert "compra SAN.MC" in r["acciones"] and not any("TEF" in a for a in r["acciones"])
    cid = r["cartera"]
    pos = {p["simbolo"]: p for p in b.posiciones(cid)}
    assert "SAN.MC" in pos
    g = b._one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'")
    stop = b.orden(g["orden_stop"])
    assert stop["tipo"] == "stop" and stop["estado"] == "pendiente" and stop["stop"] < g["entrada"] < g["objetivo"]
    # riesgo: perder hasta el stop cuesta como mucho ~1 % del patrimonio (+ gastos)
    assert pos["SAN.MC"]["cantidad"] * (g["entrada"] - g["stop"]) <= 10000 * 0.011
    est = ag.estado()
    assert est["diario"][0]["tipo"] in ("compra", "stop") and est["curva"]
    assert any(p["simbolo"] == "SAN.MC" and p["stop"] for p in est["resumen"]["posiciones"])
    # el precio llega al objetivo: vende y aprende (sube el peso de los factores que acertaron)
    b.datos.px["SAN.MC"] = g["objetivo"] * 1.01
    pesos0 = ag.pesos()
    r2 = ag.ciclo()
    assert "venta SAN.MC" in r2["acciones"]
    assert not b._one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'")
    assert b.orden(g["orden_stop"])["estado"] == "cancelada"
    assert ag.pesos()["tendencia"] > pesos0["tendencia"]
    assert ag.estado()["estadisticas"]["ganadoras"] == 1
    b.close()


def test_agente_stop_salta_y_registra(tmp_path):
    clock = Clock(2026, 10, 20, 8, 0)
    b = B.Bolsa(tmp_path / "b.db", seed=3, clock=clock)
    b.datos = FakeDatos()
    ag = A.AgenteBolsa(b, lambda: {"universo": ["SAN.MC"]})
    ag.ciclo()
    g = b._one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'")
    b.datos.px["SAN.MC"] = g["stop"] * 0.97
    clock.t += dt.timedelta(minutes=2)
    ag.ciclo()
    assert b.orden(g["orden_stop"])["estado"] == "ejecutada"
    assert any(d["tipo"] == "stop" and "Saltó" in d["texto"] for d in ag.estado()["diario"])
    assert ag.estado()["estadisticas"]["cerradas"] == 1
    b.close()


def test_agente_no_compra_con_bolsa_cerrada(tmp_path):
    b = B.Bolsa(tmp_path / "b.db", seed=3, clock=Clock(2026, 10, 24, 12, 0))   # sabado
    b.datos = FakeDatos()
    ag = A.AgenteBolsa(b, lambda: {"universo": ["SAN.MC"]})
    assert ag.ciclo()["acciones"] == []
    b.close()


def test_api_agente(services):
    from fastapi.testclient import TestClient

    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        st = c.get("/api/bolsa/agente").json()
        assert "diario" in st and "curva" in st and st["cartera"]
        assert c.post("/api/bolsa/agente", json={"activo": False, "riesgo_pct": 0.5}).json()["ajustes"]["riesgo_pct"] == 0.5
        s = c.get("/api/bolsa/serie/SAN.MC?rango=1d").json()
        assert s["simbolo"] == "SAN.MC" and "puntos" in s
        o = c.post("/api/bolsa/orden", json={"simbolo": "SAN.MC", "lado": "compra", "cantidad": 5, "tipo": "limitada",
                                             "limite": 0.01, "validez": "hasta_cancelar"}).json()
        g = c.get(f"/api/bolsa/orden/{o['id']}/grafico").json()
        assert g["orden"]["id"] == o["id"] and any(n["tipo"] == "limite" for n in g["niveles"])
        assert "puntos" in g


def test_cnbc_respaldo_cuando_yahoo_limita():
    import httpx
    from nexus.office import bolsa_datos as D

    def h(req):
        u = str(req.url)
        if "yahoo" in u:
            return httpx.Response(429)
        if "cnbc" in u:
            assert "symbols=SAN-ES" in u
            return httpx.Response(200, json={"FormattedQuoteResult": {"FormattedQuote": [
                {"symbol": "SAN-ES", "last": "7,512.5", "previous_day_closing": "7,400", "currencyCode": "EUR"}]}})
        return httpx.Response(404)
    d = D.DatosMercado(httpx.Client(transport=httpx.MockTransport(h)))
    q = d.quote("SAN.MC")
    assert q["fuente"] == "CNBC" and q["precio"] == 7512.5 and q["cierre_anterior"] == 7400 and q["divisa"] == "EUR"
