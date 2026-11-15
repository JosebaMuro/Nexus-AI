"""Enlace con un broker para pasar de la mesa simulada a ordenes de verdad (paridad con bolsa_real.cpp de la C++).

Reglas que no cambian:
  - el agente autonomo opera SIEMPRE en su cartera simulada; aqui nunca envia nada por su cuenta;
  - ninguna orden sale sin que una persona escriba CONFIRMO en la interfaz (y sea administrador);
  - Alpaca empieza en su entorno de papel (paper-api.alpaca.markets): dinero ficticio con precios reales. El entorno
    real solo se usa si el administrador pone broker.entorno = real Y broker.permitir_real = true.
  - tipo «manual» no conecta con nada: la orden se apunta como pendiente de ejecutar a mano en tu banco o broker.

Configuracion (Ajustes avanzados o PUT /api/config): broker.tipo (manual | alpaca), broker.entorno (papel | real),
broker.clave y broker.secreto (claves de la API de Alpaca, se guardan enmascaradas), broker.permitir_real.
"""
from __future__ import annotations

import json
import threading
import time
import uuid

import httpx

from .. import log, paths

L = log.get("bolsa")
URLS = {"papel": "https://paper-api.alpaca.markets", "real": "https://api.alpaca.markets"}
CONFIRMACION = "CONFIRMO"
_LOCK = threading.Lock()


def _cfg(cfg: dict | None = None) -> dict:
    if cfg is None:
        from ..config import get_config
        cfg = get_config().get("broker", {}) or {}
    return {"tipo": str(cfg.get("tipo") or "manual").lower(), "entorno": str(cfg.get("entorno") or "papel").lower(),
            "clave": str(cfg.get("clave") or ""), "secreto": str(cfg.get("secreto") or ""),
            "permitir_real": bool(cfg.get("permitir_real", False))}


def entorno_efectivo(c: dict) -> str:
    """El real solo si se pide expresamente con las dos opciones; si no, papel."""
    return "real" if c["entorno"] == "real" and c["permitir_real"] else "papel"


def _registro():
    return paths.sub("bolsa") / "broker_ordenes.json"


def _leer_registro() -> list[dict]:
    try:
        return json.loads(_registro().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []


def _apuntar(o: dict) -> None:
    with _LOCK:
        lista = _leer_registro()
        lista.insert(0, o)
        f = _registro()
        tmp = f.with_suffix(".tmp")
        tmp.write_text(json.dumps(lista[:500], ensure_ascii=False, indent=1), encoding="utf-8")
        tmp.replace(f)


def _cliente(c: dict, client: httpx.Client | None) -> httpx.Client:
    if client is not None:
        return client
    return httpx.Client(base_url=URLS[entorno_efectivo(c)], timeout=10,
                        headers={"APCA-API-KEY-ID": c["clave"], "APCA-API-SECRET-KEY": c["secreto"]})


def _pedir(c: dict, metodo: str, ruta: str, client: httpx.Client | None = None, **kw):
    if c["tipo"] != "alpaca":
        raise ValueError("el broker es «manual»: no hay ninguna conexión que consultar")
    if not c["clave"] or not c["secreto"]:
        raise ValueError("faltan la clave y el secreto de la API de Alpaca (Ajustes avanzados → broker)")
    cl = _cliente(c, client)
    try:
        r = cl.request(metodo, ruta, **kw)
    except (httpx.HTTPError, OSError) as e:
        raise ConnectionError(f"Alpaca no responde: {e}") from e
    finally:
        if client is None:
            cl.close()
    if r.status_code in (401, 403):
        raise PermissionError("Alpaca rechaza las claves (revisa clave, secreto y entorno papel/real)")
    if r.status_code >= 400:
        try:
            msg = r.json().get("message") or r.text
        except ValueError:
            msg = r.text
        raise ValueError(f"Alpaca: {msg[:300]}")
    return r.json() if r.content else {}


def estado(consultar: bool = False, cfg: dict | None = None, client: httpx.Client | None = None) -> dict:
    c = _cfg(cfg)
    out = {"tipo": c["tipo"], "entorno": entorno_efectivo(c), "entorno_pedido": c["entorno"],
           "configurado": c["tipo"] == "manual" or bool(c["clave"] and c["secreto"]),
           "confirmacion": CONFIRMACION, "ordenes": len(_leer_registro())}
    if c["tipo"] == "alpaca" and c["entorno"] == "real" and not c["permitir_real"]:
        out["aviso"] = "Pediste el entorno real pero broker.permitir_real está desactivado: se usa el de papel."
    if consultar and c["tipo"] == "alpaca":
        try:
            a = _pedir(c, "GET", "/v2/account", client)
            out["cuenta"] = {k: a.get(k) for k in ("status", "currency", "cash", "buying_power", "equity",
                                                    "portfolio_value", "pattern_day_trader")}
        except (ValueError, PermissionError, ConnectionError) as e:
            out["error"] = str(e)
    return out


def _validar(orden: dict) -> dict:
    sym = str(orden.get("simbolo") or "").strip().upper()
    lado = {"compra": "buy", "buy": "buy", "venta": "sell", "sell": "sell"}.get(str(orden.get("lado", "")).lower())
    tipo = {"mercado": "market", "market": "market", "limitada": "limit", "limit": "limit"}.get(
        str(orden.get("tipo") or "mercado").lower())
    try:
        qty = float(orden.get("cantidad") or 0)
    except (TypeError, ValueError):
        qty = 0
    if not sym:
        raise ValueError("falta el valor")
    if not lado:
        raise ValueError("el lado tiene que ser compra o venta")
    if not tipo:
        raise ValueError("tipo de orden admitido: mercado o limitada")
    if qty <= 0:
        raise ValueError("la cantidad tiene que ser mayor que cero")
    o = {"simbolo": sym, "lado": lado, "tipo": tipo, "cantidad": qty,
         "validez": "gtc" if str(orden.get("validez", "")).lower() in ("gtc", "hasta_cancelar") else "day"}
    if tipo == "limit":
        try:
            o["limite"] = float(orden["limite"])
        except (KeyError, TypeError, ValueError) as e:
            raise ValueError("una orden limitada necesita precio límite") from e
        if o["limite"] <= 0:
            raise ValueError("el precio límite tiene que ser mayor que cero")
    return o


def enviar(orden: dict, confirmacion: str, quien: str = "", cfg: dict | None = None,
           client: httpx.Client | None = None) -> dict:
    """Envia (Alpaca) o apunta (manual) una orden. Exige la palabra CONFIRMO escrita por una persona."""
    if (confirmacion or "").strip().upper() != CONFIRMACION:
        raise PermissionError(f"Para enviar una orden de verdad escribe {CONFIRMACION}")
    c = _cfg(cfg)
    o = _validar(orden)
    reg = {"id": uuid.uuid4().hex[:12], "ts": time.time(), "broker": c["tipo"], "entorno": entorno_efectivo(c),
           "quien": quien or "administrador", **o}
    if c["tipo"] != "alpaca":
        reg["estado"] = "pendiente_manual"
        reg["nota"] = "Ejecútala tú en tu banco o broker; queda apuntada aquí."
        _apuntar(reg)
        return reg
    cuerpo = {"symbol": o["simbolo"],   # Alpaca opera valores de EE. UU. y cripto (AAPL, BTC/USD)
              "qty": str(o["cantidad"]), "side": o["lado"], "type": o["tipo"], "time_in_force": o["validez"],
              "client_order_id": "agente-ia-" + reg["id"]}
    if o["tipo"] == "limit":
        cuerpo["limit_price"] = str(o["limite"])
    try:
        r = _pedir(c, "POST", "/v2/orders", client, json=cuerpo)
    except (ValueError, PermissionError, ConnectionError) as e:
        reg.update(estado="rechazada", error=str(e))
        _apuntar(reg)
        raise
    reg.update(estado=r.get("status", "enviada"), broker_id=r.get("id", ""))
    _apuntar(reg)
    L.info("Orden enviada a Alpaca (%s): %s %s %s", reg["entorno"], o["lado"], o["cantidad"], o["simbolo"])
    return reg


def ordenes(sincronizar: bool = False, cfg: dict | None = None, client: httpx.Client | None = None) -> dict:
    """Ordenes apuntadas en este equipo y, con Alpaca, las del broker y sus posiciones."""
    c = _cfg(cfg)
    out = {"registro": _leer_registro()[:100]}
    if sincronizar and c["tipo"] == "alpaca":
        try:
            out["broker"] = [{k: x.get(k) for k in ("id", "client_order_id", "symbol", "side", "type", "qty",
                                                     "filled_qty", "filled_avg_price", "status", "submitted_at")}
                             for x in _pedir(c, "GET", "/v2/orders", client, params={"status": "all", "limit": 50})]
            out["posiciones"] = [{k: x.get(k) for k in ("symbol", "qty", "avg_entry_price", "current_price",
                                                         "market_value", "unrealized_pl")}
                                 for x in _pedir(c, "GET", "/v2/positions", client)]
        except (ValueError, PermissionError, ConnectionError) as e:
            out["error"] = str(e)
    return out


def cancelar(broker_id: str, cfg: dict | None = None, client: httpx.Client | None = None) -> dict:
    c = _cfg(cfg)
    _pedir(c, "DELETE", f"/v2/orders/{broker_id}", client)
    return {"cancelada": broker_id}
