"""Noticias de la mesa de analistas por RSS (paridad con bolsa_mesa.cpp de la edicion C++ 3.x).

Titulares de Yahoo Finance (por simbolo) y Google Noticias (por nombre de la empresa), con su sentimiento segun el
lexico financiero de bolsa_lab. Se guardan en data/bolsa/noticias.json para usarlos despues sin conexion; se
refrescan cada 30 minutos y se olvidan a los 120 dias. Sin conexion la mesa sigue funcionando con lo guardado.
"""
from __future__ import annotations

import email.utils
import html
import json
import os
import re
import threading
import time
import urllib.parse

import httpx

from .. import log, paths

L = log.get("bolsa")
TTL = 1800            # segundos entre consultas a las fuentes
OLVIDO = 120 * 86400  # titulares mas viejos se borran
UA = "Mozilla/5.0 (Agente IA)"
_LOCK = threading.Lock()


def _fichero():
    return paths.sub("bolsa") / "noticias.json"


def _cargar() -> dict:
    try:
        return json.loads(_fichero().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _guardar(d: dict) -> None:
    f = _fichero()
    tmp = f.with_suffix(".tmp")
    tmp.write_text(json.dumps(d, ensure_ascii=False), encoding="utf-8")
    tmp.replace(f)


def _etiqueta(item: str, nombre: str) -> str:
    m = re.search(rf"<{nombre}(?:\s[^>]*)?>(.*?)</{nombre}>", item, re.S | re.I)
    if not m:
        return ""
    v = m.group(1).strip()
    if v.startswith("<![CDATA["):
        v = v[9:].rsplit("]]>", 1)[0]
    return html.unescape(re.sub(r"<[^>]+>", "", v)).strip()


def fecha_rss(fecha: str) -> float:
    try:
        return email.utils.parsedate_to_datetime(fecha).timestamp()
    except (TypeError, ValueError, IndexError):
        return 0.0


def leer_rss(xml: str) -> list[dict]:
    """[{titulo, enlace, fecha, ts, fuente}] de un RSS 2.0 (tolerante: sin parser XML estricto)."""
    out = []
    for m in re.finditer(r"<item[\s>](.*?)</item>", xml or "", re.S | re.I):
        it = m.group(1)
        t = _etiqueta(it, "title")
        if not t:
            continue
        fecha = _etiqueta(it, "pubDate")
        out.append({"titulo": t, "enlace": _etiqueta(it, "link"), "fecha": fecha, "ts": fecha_rss(fecha),
                    "fuente": _etiqueta(it, "source")})
    return out


def fuentes(simbolo: str, nombre: str = "") -> list[tuple[str, str]]:
    base = simbolo.split(".")[0]
    q = (nombre or base) + " bolsa"
    return [("Yahoo Finance", "https://feeds.finance.yahoo.com/rss/2.0/headline?" +
             urllib.parse.urlencode({"s": simbolo, "region": "US", "lang": "en-US"})),
            ("Google Noticias", "https://news.google.com/rss/search?" +
             urllib.parse.urlencode({"q": q, "hl": "es", "gl": "ES", "ceid": "ES:es"}))]


def noticias(simbolo: str, nombre: str = "", refrescar: bool = False, client: httpx.Client | None = None,
             limite: int = 30) -> dict:
    """Titulares recientes de un valor: {simbolo, noticias: [...], sentimiento, errores, actualizado}."""
    from .bolsa_lab import sentimiento
    sym = (simbolo or "").strip().upper()
    if not sym:
        raise ValueError("indica un valor")
    ahora = time.time()
    with _LOCK:
        d = _cargar()
    ent = d.get(sym) or {"items": {}, "ok": 0}
    errores = []
    sin_red = client is None and os.environ.get("NEXUS_BOLSA_OFFLINE") == "1"   # pruebas o modo sin conexion
    if not sin_red and (refrescar or ahora - ent.get("ok", 0) > TTL):
        cerrar = client is None
        client = client or httpx.Client(timeout=8, headers={"User-Agent": UA}, follow_redirects=True)
        try:
            for fuente, url in fuentes(sym, nombre):
                try:
                    r = client.get(url)
                    r.raise_for_status()
                except (httpx.HTTPError, OSError) as e:
                    code = getattr(getattr(e, "response", None), "status_code", 0)
                    errores.append(f"{fuente}: " + (f"HTTP {code}" if code else "sin conexión"))
                    continue
                for n in leer_rss(r.text):
                    clave = n["enlace"] or n["titulo"]
                    ent["items"].setdefault(clave, {"titulo": n["titulo"][:300], "enlace": n["enlace"],
                                                    "ts": n["ts"] or ahora, "fuente": n["fuente"] or fuente,
                                                    "sentimiento": sentimiento(n["titulo"])})
        finally:
            if cerrar:
                client.close()
        if len(errores) < 2:   # alguna fuente respondio
            ent["ok"] = ahora
            ent["items"] = {k: v for k, v in ent["items"].items() if v["ts"] > ahora - OLVIDO}
            with _LOCK:
                d = _cargar()
                d[sym] = ent
                _guardar(d)
    items = sorted(ent["items"].values(), key=lambda v: v["ts"], reverse=True)[:limite]
    recientes = [v["sentimiento"] for v in items if v["ts"] > ahora - 14 * 86400] or [v["sentimiento"] for v in items]
    return {"simbolo": sym, "noticias": items, "errores": errores, "actualizado": ent.get("ok", 0),
            "sentimiento": round(sum(recientes) / len(recientes), 2) if recientes else 0.0,
            "sin_conexion": bool(errores) and len(errores) >= 2}


def titulares(simbolo: str, nombre: str = "", dias: int = 14, maximo: int = 15) -> list[str]:
    """Titulares de los ultimos dias para la mesa (vacio si no hay ni conexion ni guardados)."""
    try:
        res = noticias(simbolo, nombre)
    except Exception as e:  # noqa: BLE001
        L.info("Noticias de %s no disponibles: %s", simbolo, e)
        return []
    corte = time.time() - dias * 86400
    return [n["titulo"] for n in res["noticias"] if n["ts"] > corte][:maximo]
