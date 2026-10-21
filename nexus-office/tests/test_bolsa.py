"""Oficina de bolsa e inversiones: mesa simulada (horarios, ordenes, costes, FIFO) y modo real con confirmacion."""
import datetime as dt

import pytest
from fastapi.testclient import TestClient

from nexus import office
from nexus.office import bolsa as B

UTC = dt.timezone.utc


class Clock:
    def __init__(self, *a):
        self.t = dt.datetime(*a, tzinfo=UTC)

    def __call__(self):
        return self.t

    def go(self, *a):
        self.t = dt.datetime(*a, tzinfo=UTC)


@pytest.fixture
def mesa(tmp_path):
    clock = Clock(2026, 10, 20, 8, 0)        # martes, 10:00 en Madrid (horario de verano)
    b = B.Bolsa(tmp_path / "bolsa.db", seed=7, clock=clock)
    b.clock_ctl = clock
    yield b
    b.close()


def set_price(b, sym, p):
    b.precio(sym)
    b.con.execute("UPDATE precios SET precio=?, cierre_ant=?, ts=? WHERE simbolo=?", (p, p, b.now().timestamp(), sym))
    b.con.commit()


def test_calendars_and_hours():
    assert B.easter(2026) == dt.date(2026, 4, 5) and B.easter(2027) == dt.date(2027, 3, 28)
    eu = B.holidays("eu", 2026)
    assert {dt.date(2026, 4, 3), dt.date(2026, 4, 6), dt.date(2026, 12, 24), dt.date(2026, 12, 31)} <= eu
    us = B.holidays("us", 2026)
    assert dt.date(2026, 11, 26) in us and dt.date(2026, 7, 3) in us and dt.date(2026, 1, 19) in us   # 4 de julio en sabado
    assert dt.date(2026, 12, 28) in B.holidays("uk", 2026)                                          # San Esteban en sabado
    # Madrid: 9:00 local es 7:00 UTC en verano y 8:00 UTC en invierno
    assert B.session("BME", dt.date(2026, 7, 1))[0] == dt.datetime(2026, 7, 1, 7, 0, tzinfo=UTC)
    assert B.session("BME", dt.date(2026, 12, 1))[0] == dt.datetime(2026, 12, 1, 8, 0, tzinfo=UTC)
    # Nueva York abre a las 13:30 UTC en verano; entre el cambio de hora europeo y el de EE. UU. a las 14:30 de Madrid
    assert B.session("NYSE", dt.date(2026, 7, 1))[0] == dt.datetime(2026, 7, 1, 13, 30, tzinfo=UTC)
    assert B.session("NYSE", dt.date(2026, 10, 27))[0] == dt.datetime(2026, 10, 27, 13, 30, tzinfo=UTC)
    assert B.session("NYSE", dt.date(2026, 11, 3))[0] == dt.datetime(2026, 11, 3, 14, 30, tzinfo=UTC)
    assert B.session("BME", dt.date(2026, 10, 24)) is None                                          # sabado
    assert B.is_open("CRIPTO", dt.datetime(2026, 10, 24, 3, 0, tzinfo=UTC))
    assert not B.is_open("BME", dt.datetime(2026, 12, 24, 10, 0, tzinfo=UTC))


def test_market_order_costs_and_fifo(mesa):
    b = mesa
    set_price(b, "SAN.MC", 6.0)
    o = b.ordenar("SAN.MC", "compra", 1000)
    op = o["operacion"]
    assert o["estado"] == "ejecutada" and op["precio"] >= 6.0          # se compra al precio de venta (horquilla)
    assert op["impuestos_eur"] == pytest.approx(op["efectivo_eur"] * 0.002, abs=0.01)   # impuesto de transacciones
    assert op["comision_eur"] > 2 and op["total_eur"] == pytest.approx(
        op["efectivo_eur"] + op["comision_eur"] + op["impuestos_eur"], abs=0.02)
    cash = b.resumen()["efectivo_eur"]
    assert cash == pytest.approx(10000 - op["total_eur"], abs=0.01)
    b.clock_ctl.go(2026, 10, 20, 9, 0)
    set_price(b, "SAN.MC", 6.5)
    b.ordenar("SAN.MC", "compra", 500)
    b.clock_ctl.go(2027, 1, 12, 10, 0)                                 # mas de dos meses despues
    set_price(b, "SAN.MC", 7.0)
    v = b.ordenar("SAN.MC", "venta", 1000)["operacion"]               # FIFO: sale el primer lote (a ~6 €)
    assert v["impuestos_eur"] == 0 and v["ganancia_eur"] > 800 and not v["aviso"]
    pos = b.posiciones()
    assert pos[0]["cantidad"] == 500 and pos[0]["coste_medio_eur"] > 6.5
    with pytest.raises(ValueError, match="corto"):
        b.ordenar("SAN.MC", "venta", 501)
    assert b.informe_fiscal(anio=2027)["neto_eur"] == v["ganancia_eur"]


def test_closed_market_waits_for_the_open_and_fx_costs(mesa):
    b = mesa
    o = b.ordenar("AAPL", "compra", 10)                                # Nasdaq cerrado a las 4:00 de Nueva York
    assert o["estado"] == "pendiente" and "cerrado" in o["nota"]
    assert b.procesar() == []
    b.clock_ctl.go(2026, 10, 20, 14, 0)                                 # 10:00 en Nueva York
    assert b.procesar() == [o["id"]]
    op = b.orden(o["id"])["operacion"]
    assert op["fx"] != 1 and op["cambio_eur"] == pytest.approx(op["efectivo_eur"] * 0.0025, abs=0.01)
    assert op["impuestos_eur"] == 0


def test_limit_stop_and_expiry(mesa):
    b = mesa
    set_price(b, "IBE.MC", 14.0)
    lim = b.ordenar("IBE.MC", "compra", 100, "limitada", limite=13.0)
    stop_buy = b.ordenar("IBE.MC", "compra", 10, "stop", stop=15.0, validez="hasta_cancelar")
    day = b.ordenar("IBE.MC", "compra", 10, "limitada", limite=10.0)
    assert {lim["estado"], stop_buy["estado"], day["estado"]} == {"pendiente"}
    set_price(b, "IBE.MC", 12.9)
    b.procesar()
    filled = b.orden(lim["id"])
    assert filled["estado"] == "ejecutada" and filled["precio_ejec"] <= 13.0
    assert b.orden(stop_buy["id"])["estado"] == "pendiente"
    set_price(b, "IBE.MC", 15.2)
    b.procesar()
    assert b.orden(stop_buy["id"])["estado"] == "ejecutada"
    b.clock_ctl.go(2026, 10, 21, 8, 0)                                  # al dia siguiente la orden del dia caduca
    b.procesar()
    assert b.orden(day["id"])["estado"] == "caducada"
    sell = b.ordenar("IBE.MC", "venta", 100, "stop", stop=12.0, validez="hasta_cancelar")
    assert b.cancelar(sell["id"])["estado"] == "cancelada"


def test_cash_reserved_and_two_month_rule(mesa):
    b = mesa
    set_price(b, "ITX.MC", 50.0)
    b.ordenar("ITX.MC", "compra", 150, "limitada", limite=40.0)        # reserva ~6.000 €
    with pytest.raises(ValueError, match="insuficiente"):
        b.ordenar("ITX.MC", "compra", 100)
    b.cancelar(b.ordenes(abiertas=True)[0]["id"])
    b.ordenar("ITX.MC", "compra", 20)
    b.ordenar("ITX.MC", "compra", 20)
    set_price(b, "ITX.MC", 40.0)
    v = b.ordenar("ITX.MC", "venta", 20)["operacion"]                  # perdida y quedan titulos recien comprados
    assert v["ganancia_eur"] < 0 and "dos meses" in v["aviso"]
    c = b.ordenar("ITX.MC", "compra", 5)["operacion"]                  # recompra tras vender con perdidas
    assert "dos meses" in c["aviso"]


def test_prices_move_only_while_open_and_csv_import(mesa):
    b = mesa
    p0 = b.precio("SAP.DE")["precio"]
    b.clock_ctl.go(2026, 10, 20, 12, 0)
    p1 = b.precio("SAP.DE")["precio"]
    assert p1 != p0
    b.clock_ctl.go(2026, 10, 24, 12, 0)                                 # sabado: no hay sesion... pero si la del viernes
    p2 = b.precio("SAP.DE")["precio"]
    b.clock_ctl.go(2026, 10, 25, 12, 0)                                 # domingo: el precio no se mueve
    assert b.precio("SAP.DE")["precio"] == p2
    csv = "Date,Open,High,Low,Close\n" + "\n".join(f"2026-09-{d:02d},1,1,1,{200 + d * (-1) ** d}" for d in range(1, 25))
    r = b.importar_csv("SAP.DE", csv)
    assert r["filas"] == 24 and r["ultimo_cierre"] == 224 and r["volatilidad_anual"] > 0.05
    assert b.precio("SAP.DE")["precio"] == 224
    b.anadir_instrumento("ACME.MC", "Acme", "BME", "3,5")
    assert b.precio("ACME.MC")["precio"] == 3.5
    with pytest.raises(ValueError):
        b.precio("NOEXISTE")


def test_real_portfolio_never_executes_without_confirmation(mesa):
    b = mesa
    real = b.crear_cartera("Mi cartera", 5000, "banco", "real")
    o = b.ordenar("TEF.MC", "compra", 100, cartera=real["id"])
    assert o["estado"] == "por_confirmar" and "NO se ha enviado" in o["nota"]
    b.clock_ctl.go(2026, 10, 20, 12, 0)
    b.procesar()
    assert b.orden(o["id"])["estado"] == "por_confirmar" and not b.operaciones(real["id"])
    with pytest.raises(PermissionError):
        b.confirmar(o["id"], "si")
    with pytest.raises(ValueError):
        b.registrar_ejecucion(o["id"], 4.2)
    assert b.confirmar(o["id"], "confirmo")["estado"] == "confirmada"
    b.procesar()
    assert not b.operaciones(real["id"])                               # confirmada tampoco se ejecuta sola
    done = b.registrar_ejecucion(o["id"], "4,25", comision_eur=8)
    assert done["estado"] == "ejecutada" and b.resumen(real["id"])["efectivo_eur"] == pytest.approx(5000 - 425 - 8 - 0.85,
                                                                                                    abs=0.01)
    with pytest.raises(ValueError):
        b.reiniciar(real["id"])


def test_office_profile_and_tools(services):
    s = services
    p = office.Office(s.config).apply("bolsa-inversiones")
    assert {"analista-mercados", "gestor-carteras", "operador-bolsa", "riesgos-inversion",
            "fiscalidad-inversiones"} <= {a["id"] for a in p["agents"]}
    s.agents.reload()
    assert "bolsa_orden" in s.agents.get("operador-bolsa").tools
    assert office.sector_models("bolsa-inversiones")
    from nexus.tools.base import ToolContext
    ctx = ToolContext(services=s, agent="operador-bolsa")
    r = s.supervisor.execute("bolsa_cartera", {}, ctx)
    assert r["modo"] == "simulado" and r["total_eur"] == 10000 and "asesoramiento" in r["aviso"]
    r = s.supervisor.execute("bolsa_orden", {"simbolo": "BTC-EUR", "lado": "compra", "cantidad": 0.01}, ctx)
    assert r["estado"] == "ejecutada" and r["origen"] == "agente:operador-bolsa"
    assert "error" in s.supervisor.execute("bolsa_orden", {"simbolo": "AAPL", "lado": "venta", "cantidad": 1}, ctx)
    assert not any("confirm" in t.name for t in s.tools.for_agent(["*"]))   # ningun agente puede confirmar ordenes


def test_api(services):
    from nexus.server.app import create_app
    c = TestClient(create_app(services))
    d = c.get("/api/bolsa").json()
    assert d["resumen"]["total_eur"] == 10000 and len(d["mercados"]) == 7 and d["confirmacion"] == "CONFIRMO"
    assert c.get("/api/bolsa/cotizacion/BTC-EUR").json()["simbolo"] == "BTC-EUR"
    o = c.post("/api/bolsa/orden", json={"simbolo": "BTC-EUR", "lado": "compra", "cantidad": 0.002}).json()
    assert o["estado"] == "ejecutada"
    assert c.post("/api/bolsa/orden", json={"simbolo": "BTC-EUR", "lado": "compra", "cantidad": -1}).status_code == 400
    assert c.post("/api/bolsa/costes", json={"simbolo": "AAPL", "cantidad": 10}).json()["cambio_eur"] > 0
    assert c.get("/api/bolsa/fiscal").json()["neto_eur"] == 0
    assert any(i["simbolo"] == "SAN.MC" for i in c.get("/api/bolsa/instrumentos?q=santander").json())
