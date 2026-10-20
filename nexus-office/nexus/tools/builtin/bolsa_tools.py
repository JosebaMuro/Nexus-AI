"""Herramientas de la mesa de bolsa (nexus/office/bolsa.py): cotizaciones, ordenes y carteras simuladas.

Funcionan sin internet. Un agente puede preparar ordenes, pero en una cartera real nunca las confirma ni las
ejecuta: eso solo lo hace una persona desde la app o la terminal.
"""
from __future__ import annotations

from ...office import bolsa


def _b():
    return bolsa.get()


def bolsa_mercados(args, ctx):
    return {"mercados": _b().mercados(), "aviso": bolsa.AVISO}


def bolsa_cotizacion(args, ctx):
    b = _b()
    if args.get("simbolo"):
        return {**b.precio(args["simbolo"]), "aviso": bolsa.AVISO}
    found = b.instrumentos(args.get("buscar", ""))
    return {"instrumentos": [{k: i[k] for k in ("simbolo", "nombre", "mercado", "divisa", "tipo")} for i in found[:40]],
            "aviso": bolsa.AVISO}


def bolsa_cartera(args, ctx):
    b = _b()
    cid = args.get("cartera") or None
    out = b.resumen(cid)
    if args.get("operaciones"):
        out["operaciones"] = b.operaciones(cid, 20)
    out["ordenes_abiertas"] = b.ordenes(cid, abiertas=True)
    out["carteras"] = [{k: c[k] for k in ("id", "nombre", "modo", "tarifa")} for c in b.carteras()]
    return out


def bolsa_costes(args, ctx):
    b = _b()
    q = b.precio(args["simbolo"])
    px = float(args.get("precio") or (q["venta"] if args.get("lado", "compra") == "compra" else q["compra"]))
    g = b.costes(args.get("cartera") or None, args["simbolo"], args.get("lado", "compra"), float(args["cantidad"]), px)
    g["total_gastos_eur"] = round(g["comision_eur"] + g["impuestos_eur"] + g["cambio_eur"], 2)
    g["precio"] = px
    return g


def bolsa_orden(args, ctx):
    return _b().ordenar(args["simbolo"], args["lado"], args["cantidad"], args.get("tipo", "mercado"),
                        args.get("limite"), args.get("stop"), args.get("validez", "dia"), args.get("cartera") or None,
                        origen=f"agente:{ctx.agent}" if ctx and ctx.agent else "agente")


def bolsa_cancelar(args, ctx):
    return _b().cancelar(int(args["orden"]))


def bolsa_informe_fiscal(args, ctx):
    return _b().informe_fiscal(args.get("cartera") or None, args.get("anio") or None)


_SYM = {"type": "string", "description": "símbolo del valor, p. ej. SAN.MC, AAPL, VWCE.DE, BTC-EUR"}
_CART = {"type": "integer", "description": "id de la cartera (vacío = la primera)"}
TOOLS = [
    ({"name": "bolsa_mercados", "description": "Estado de las bolsas (abiertas o cerradas, hora local, próxima apertura "
      "o cierre) en la mesa de bolsa simulada", "parameters": {"type": "object", "properties": {}},
      "risk": "low", "permissions": []}, bolsa_mercados),
    ({"name": "bolsa_cotizacion", "description": "Cotización simulada de un valor (precio, compra/venta, variación) o "
      "búsqueda de instrumentos disponibles. Precios ficticios, no reales",
      "parameters": {"type": "object", "properties": {"simbolo": _SYM, "buscar": {"type": "string"}}},
      "risk": "low", "permissions": []}, bolsa_cotizacion),
    ({"name": "bolsa_cartera", "description": "Resumen de una cartera: efectivo, posiciones, plusvalías, rentabilidad, "
      "gastos y órdenes abiertas", "parameters": {"type": "object", "properties": {
          "cartera": _CART, "operaciones": {"type": "boolean", "default": False}}},
      "risk": "low", "permissions": []}, bolsa_cartera),
    ({"name": "bolsa_costes", "description": "Calcula antes de operar la comisión, el canon, el impuesto sobre "
      "transacciones financieras y el coste del cambio de divisa de una compra o venta",
      "parameters": {"type": "object", "properties": {
          "simbolo": _SYM, "lado": {"type": "string", "enum": ["compra", "venta"], "default": "compra"},
          "cantidad": {"type": "number"}, "precio": {"type": "number"}, "cartera": _CART},
          "required": ["simbolo", "cantidad"]}, "risk": "low", "permissions": []}, bolsa_costes),
    ({"name": "bolsa_orden", "description": "Da una orden en la mesa de bolsa. En una cartera simulada se ejecuta como "
      "en un mercado real (horario, horquilla, deslizamiento, comisiones); en una cartera real solo queda preparada y "
      "la tiene que confirmar el usuario",
      "parameters": {"type": "object", "properties": {
          "simbolo": _SYM, "lado": {"type": "string", "enum": ["compra", "venta"]},
          "cantidad": {"type": "number"},
          "tipo": {"type": "string", "enum": list(bolsa.ORDER_TYPES), "default": "mercado"},
          "limite": {"type": "number", "description": "precio límite (limitada y stop_limitada)"},
          "stop": {"type": "number", "description": "precio de activación (stop y stop_limitada)"},
          "validez": {"type": "string", "enum": ["dia", "hasta_cancelar"], "default": "dia"},
          "cartera": _CART}, "required": ["simbolo", "lado", "cantidad"]},
      "risk": "low", "permissions": []}, bolsa_orden),
    ({"name": "bolsa_cancelar", "description": "Cancela una orden abierta de la mesa de bolsa",
      "parameters": {"type": "object", "properties": {"orden": {"type": "integer"}}, "required": ["orden"]},
      "risk": "low", "permissions": []}, bolsa_cancelar),
    ({"name": "bolsa_informe_fiscal", "description": "Ganancias y pérdidas realizadas en un año por valor (FIFO), con "
      "los avisos de la regla de los dos meses, para orientar la declaración de la Renta",
      "parameters": {"type": "object", "properties": {"cartera": _CART, "anio": {"type": "integer"}}},
      "risk": "low", "permissions": []}, bolsa_informe_fiscal),
]
