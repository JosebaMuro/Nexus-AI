"""Fuentes de informacion publica del mundo, ordenadas por categoria y por pais.

Cada fuente es una funcion que pide datos a un servicio publico y devuelve registros normalizados:

    {"uid", "titulo", "detalle", "pais" (ISO de dos letras o ""), "lat", "lon", "ts" (segundos), "url",
     "valor" (numero principal: magnitud, recuento, precio...), "datos" {extra}}

Las capas siguen las de «God's Eye View» (bilawalsidhu/gods-eye-view, MIT): movimiento, sucesos, clima, espacio,
noticias, camaras, audio y naturaleza. Aqui no hay globo: lo que se guarda es lo que un agente puede consultar.
Las de mercados salen de ``nexus.office.mercados`` (OpenTerminal). Ninguna fuente pide datos de personas.

Los analizadores (``_leer_*``) son funciones puras para poder probarlos sin red.
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import json
import math
import re
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Callable

from . import paises

CATEGORIAS = {
    "movimiento": "Movimiento (aviones, barcos, bicicletas)",
    "sucesos": "Sucesos (terremotos, incendios, desastres)",
    "clima": "Clima (tiempo, ciclones, aire)",
    "espacio": "Espacio (satélites y lanzamientos)",
    "noticias": "Noticias",
    "camaras": "Cámaras públicas",
    "audio": "Radio",
    "naturaleza": "Naturaleza",
    "mercados": "Mercados y economía",
}


@dataclass
class Fuente:
    id: str
    nombre: str
    categoria: str
    obtener: Callable            # obtener(ctx) -> list[dict]
    intervalo_s: int = 900       # cada cuanto se actualiza por defecto
    minimo_s: int = 60           # no bajar de aqui (limites del servicio)
    origen: str = ""             # quien da los datos (para citarlo)
    descripcion: str = ""
    clave: str = ""              # nombre de la clave gratuita que necesita (mundo.claves.<clave>); "" = sin clave
    peso: str = "ligera"         # ligera | media | pesada (las pesadas no se activan solas en el movil)
    acumula: bool = False        # True: guarda lo nuevo y conserva lo anterior (noticias); False: foto del momento
    activa: bool = True          # activa por defecto
    extra: dict = field(default_factory=dict)

    def publico(self) -> dict:
        return {k: getattr(self, k) for k in ("id", "nombre", "categoria", "intervalo_s", "minimo_s", "origen",
                                              "descripcion", "clave", "peso", "acumula", "activa")}


# ------------------------------------------------------------------ utilidades de lectura
def _num(v, defecto=None):
    try:
        if v is None or v == "":
            return defecto
        x = float(str(v).replace(",", "").replace("%", "").replace("$", "").strip())
        return defecto if math.isnan(x) or math.isinf(x) else x
    except (TypeError, ValueError):
        return defecto


def _fecha(v) -> float | None:
    """Segundos desde 1970 a partir de lo que mandan las fuentes (ISO 8601, RFC 822, milisegundos, AAAAMMDD...)."""
    if v is None or v == "":
        return None
    if isinstance(v, (int, float)):
        return float(v) / 1000.0 if v > 1e11 else float(v)
    s = str(v).strip()
    if re.fullmatch(r"\d{10,13}", s):
        return _fecha(int(s))
    m = re.fullmatch(r"(\d{4})(\d{2})(\d{2})T?(\d{2})(\d{2})(\d{2})Z?", s)   # GDELT: 20261114T213000Z
    if m:
        return dt.datetime(*map(int, m.groups()), tzinfo=dt.timezone.utc).timestamp()
    try:
        d = dt.datetime.fromisoformat(s.replace("Z", "+00:00"))
        return (d if d.tzinfo else d.replace(tzinfo=dt.timezone.utc)).timestamp()
    except ValueError:
        pass
    try:
        from email.utils import parsedate_to_datetime
        return parsedate_to_datetime(s).timestamp()
    except (TypeError, ValueError):
        return None


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].lower()


def leer_rss(texto: str) -> list[dict]:
    """Entradas de un RSS o Atom como diccionarios planos (etiqueta sin prefijo -> texto)."""
    try:
        raiz = ET.fromstring(texto.encode("utf-8") if isinstance(texto, str) else texto)
    except ET.ParseError:
        return []
    out = []
    for item in raiz.iter():
        if _local(item.tag) not in ("item", "entry"):
            continue
        d: dict = {}
        for hijo in item.iter():
            if hijo is item:
                continue
            k = _local(hijo.tag)
            if k == "link" and not (hijo.text or "").strip() and hijo.get("href"):
                d.setdefault("link", hijo.get("href"))
            elif (hijo.text or "").strip():
                d.setdefault(k, hijo.text.strip())
        out.append(d)
    return out


def _sin_html(s: str, n: int = 400) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", s or "")).strip()[:n]


def _campo(d: dict, *sufijos: str):
    """Valor del primer campo cuyo nombre acaba como alguno de los sufijos (los servicios cambian los prefijos)."""
    bajo = {k.lower(): v for k, v in d.items()}
    for s in sufijos:
        s = s.lower()
        for k, v in bajo.items():
            if k == s or k.endswith(s):
                if v not in (None, ""):
                    return v
    return None


# ------------------------------------------------------------------ sucesos
def _leer_sismos(datos: dict) -> list[dict]:
    out = []
    for f in datos.get("features") or []:
        p = f.get("properties") or {}
        c = (f.get("geometry") or {}).get("coordinates") or [None, None, None]
        mag = _num(p.get("mag"))
        lugar = p.get("place") or ""
        pais = paises.pais_de(c[1], c[0], margen=1.5) or (paises.codigo(lugar.rsplit(",", 1)[-1]) if "," in lugar else "")
        out.append({"uid": str(f.get("id") or p.get("code") or p.get("time")),
                    "titulo": f"Terremoto M{mag:.1f} · {lugar}" if mag is not None else f"Terremoto · {lugar}",
                    "detalle": f"Profundidad {c[2]:.0f} km" + (" · aviso de tsunami" if p.get("tsunami") else "")
                    if isinstance(c[2], (int, float)) else "",
                    "pais": pais, "lat": c[1], "lon": c[0], "ts": _fecha(p.get("time")), "url": p.get("url") or "",
                    "valor": mag, "datos": {"magnitud": mag, "profundidad_km": c[2], "alerta": p.get("alert") or "",
                                            "tsunami": bool(p.get("tsunami")), "sentido": p.get("felt")}})
    return out


def _sismos(ctx) -> list[dict]:
    umbral = "2.5" if float(ctx.cfg("sismos_magnitud_min", 2.5)) < 4.5 else "4.5"
    return _leer_sismos(ctx.json(f"https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/{umbral}_day.geojson"))


_EONET_ES = {"wildfires": "Incendio forestal", "volcanoes": "Volcán", "severeStorms": "Tormenta severa",
             "seaLakeIce": "Hielo marino", "earthquakes": "Terremoto", "floods": "Inundación",
             "landslides": "Deslizamiento", "drought": "Sequía", "dustHaze": "Polvo o calima",
             "snow": "Nevada", "tempExtremes": "Temperatura extrema", "waterColor": "Color del agua",
             "manmade": "Origen humano"}


def _leer_eonet(datos: dict) -> list[dict]:
    out = []
    for e in datos.get("events") or []:
        geo = (e.get("geometry") or [{}])[-1]
        c = geo.get("coordinates") or [None, None]
        if c and isinstance(c[0], list):   # poligono: su primer punto
            c = c[0][0] if c[0] and isinstance(c[0][0], list) else c[0]
        cat = (e.get("categories") or [{}])[0]
        tipo = _EONET_ES.get(cat.get("id", ""), cat.get("title", "Evento"))
        mag = _num(geo.get("magnitudeValue"))
        out.append({"uid": str(e.get("id")), "titulo": f"{tipo}: {e.get('title', '')}",
                    "detalle": (f"{mag:g} {geo.get('magnitudeUnit', '')}".strip() if mag is not None else ""),
                    "pais": paises.pais_de(c[1], c[0], margen=1.0) if len(c) >= 2 else "",
                    "lat": c[1] if len(c) >= 2 else None, "lon": c[0] if len(c) >= 2 else None,
                    "ts": _fecha(geo.get("date")), "url": e.get("link") or "", "valor": mag,
                    "datos": {"tipo": tipo, "fuentes": [s.get("id") for s in e.get("sources") or []][:4]}})
    return out


def _eonet(ctx) -> list[dict]:
    return _leer_eonet(ctx.json("https://eonet.gsfc.nasa.gov/api/v3/events?status=open&days=20&limit=300"))


_GDACS_TIPO = {"EQ": "Terremoto", "TC": "Ciclón tropical", "FL": "Inundación", "VO": "Volcán", "DR": "Sequía",
               "WF": "Incendio forestal", "TS": "Tsunami"}
_GDACS_NIVEL = {"green": 1, "orange": 2, "red": 3}


def _leer_gdacs(texto: str) -> list[dict]:
    out = []
    for it in leer_rss(texto):
        lat, lon = _num(it.get("lat")), _num(it.get("long"))
        if lat is None and it.get("point"):
            try:
                lat, lon = (float(x) for x in it["point"].split()[:2])
            except ValueError:
                lat = lon = None
        pais = paises.codigo(it.get("iso3") or "") or paises.codigo((it.get("country") or "").split(",")[0]) or \
            (paises.pais_de(lat, lon, margen=1.5) if lat is not None else "")
        nivel = (it.get("alertlevel") or "").lower()
        tipo = _GDACS_TIPO.get((it.get("eventtype") or "").upper(), it.get("eventtype") or "Desastre")
        out.append({"uid": it.get("guid") or it.get("link") or it.get("title", ""),
                    "titulo": f"{tipo} · alerta {({'green': 'verde', 'orange': 'naranja', 'red': 'roja'}).get(nivel, nivel or '?')}: "
                              f"{it.get('title', '')}"[:240],
                    "detalle": _sin_html(it.get("description", ""), 300), "pais": pais, "lat": lat, "lon": lon,
                    "ts": _fecha(it.get("pubdate") or it.get("fromdate")), "url": it.get("link") or "",
                    "valor": _GDACS_NIVEL.get(nivel), "datos": {"tipo": tipo, "nivel": nivel,
                                                                "paises": it.get("country") or ""}})
    return out


def _gdacs(ctx) -> list[dict]:
    return _leer_gdacs(ctx.texto("https://www.gdacs.org/xml/rss.xml"))


def _leer_perimetros(datos: dict) -> list[dict]:
    out = []
    for f in datos.get("features") or []:
        a = f.get("attributes") or f.get("properties") or {}
        nombre = _campo(a, "IncidentName") or "Incendio"
        acres = _num(_campo(a, "GISAcres", "IncidentSize", "DailyAcres"))
        cont = _num(_campo(a, "PercentContained"))
        lat, lon = _num(_campo(a, "InitialLatitude")), _num(_campo(a, "InitialLongitude"))
        ha = round(acres * 0.404686) if acres is not None else None
        estado = str(_campo(a, "POOState") or "").replace("US-", "")
        out.append({"uid": str(_campo(a, "IrwinID", "GlobalID", "OBJECTID") or nombre),
                    "titulo": f"Incendio {str(nombre).title()}" + (f" ({estado})" if estado else ""),
                    "detalle": ", ".join(x for x in (f"{ha:,} ha".replace(",", ".") if ha is not None else "",
                                                     f"contenido al {cont:.0f} %" if cont is not None else "") if x),
                    "pais": "US", "lat": lat, "lon": lon,
                    "ts": _fecha(_campo(a, "FireDiscoveryDateTime", "DateCurrent", "CreateDate")),
                    "url": "", "valor": ha, "datos": {"hectareas": ha, "contenido_pct": cont, "estado": estado}})
    out.sort(key=lambda r: -(r["valor"] or 0))
    return out[:250]


def _perimetros(ctx) -> list[dict]:
    return _leer_perimetros(ctx.json(
        "https://services3.arcgis.com/T4QMspbfLg3qTGWY/arcgis/rest/services/WFIGS_Interagency_Perimeters_Current/"
        "FeatureServer/0/query", params={"where": "1=1", "outFields": "*", "returnGeometry": "false",
                                         "resultRecordCount": "400", "f": "json"}))


def _leer_firms(texto: str) -> list[dict]:
    """Detecciones de fuego de NASA FIRMS (CSV): se agrupan por pais (suelen ser decenas de miles)."""
    grupos: dict[str, dict] = {}
    for fila in csv.DictReader(io.StringIO(texto)):
        lat, lon = _num(fila.get("latitude")), _num(fila.get("longitude"))
        if lat is None or lon is None:
            continue
        pais = paises.pais_de(lat, lon, margen=0.3)
        g = grupos.setdefault(pais, {"n": 0, "frp": 0.0, "max": None})
        frp = _num(fila.get("frp"), 0.0) or 0.0
        g["n"] += 1
        g["frp"] += frp
        if g["max"] is None or frp > g["max"][0]:
            g["max"] = (frp, lat, lon, f"{fila.get('acq_date', '')} {fila.get('acq_time', '')}".strip())
    out = []
    for pais, g in grupos.items():
        frp, lat, lon, cuando = g["max"]
        out.append({"uid": f"firms-{pais or 'mar'}", "pais": pais, "lat": lat, "lon": lon, "ts": time.time(),
                    "titulo": f"Focos de fuego en {paises.nombre(pais)}: {g['n']} detecciones en 24 h",
                    "detalle": f"Potencia radiativa total {g['frp']:.0f} MW; el mayor, {frp:.0f} MW ({cuando} UTC)",
                    "url": "https://firms.modaps.eosdis.nasa.gov/map/", "valor": g["n"],
                    "datos": {"detecciones": g["n"], "frp_total_mw": round(g["frp"], 1), "frp_max_mw": frp}})
    out.sort(key=lambda r: -r["valor"])
    return out


def _firms(ctx) -> list[dict]:
    return _leer_firms(ctx.texto(
        f"https://firms.modaps.eosdis.nasa.gov/api/area/csv/{ctx.clave('firms')}/VIIRS_SNPP_NRT/world/1"))


# ------------------------------------------------------------------ movimiento
def _leer_vuelos(datos: dict, tope_paises: int = 400) -> list[dict]:
    """Estados de OpenSky: un registro por pais sobrevolado con cuantos aviones hay, mas las emergencias."""
    grupos: dict[str, dict] = {}
    alertas = []
    ahora = _fecha(datos.get("time")) or time.time()
    for e in datos.get("states") or []:
        if len(e) < 15:
            continue
        lon, lat = e[5], e[6]
        if lat is None or lon is None:
            continue
        pais = paises.pais_de(lat, lon, margen=0.2) or "XZ"
        g = grupos.setdefault(pais, {"aire": 0, "tierra": 0, "matriculas": {}, "alt": 0.0})
        if e[8]:
            g["tierra"] += 1
        else:
            g["aire"] += 1
            g["alt"] += float(e[7] or 0)
        g["matriculas"][e[2]] = g["matriculas"].get(e[2], 0) + 1
        if str(e[14] or "") in ("7500", "7600", "7700"):
            alertas.append((e, pais))
    out = []
    for pais, g in sorted(grupos.items(), key=lambda kv: -kv[1]["aire"])[:tope_paises]:
        top = sorted(g["matriculas"].items(), key=lambda kv: -kv[1])[:4]
        out.append({"uid": f"vuelos-{pais}", "pais": pais, "ts": ahora, "valor": g["aire"],
                    "titulo": f"Aviones sobre {paises.nombre(pais)}: {g['aire']} en vuelo y {g['tierra']} en tierra",
                    "detalle": "Por país de matrícula: " + ", ".join(f"{n} ({c})" for n, c in top),
                    "url": "https://opensky-network.org/", "lat": None, "lon": None,
                    "datos": {"en_vuelo": g["aire"], "en_tierra": g["tierra"],
                              "altitud_media_m": round(g["alt"] / g["aire"]) if g["aire"] else None}})
    motivo = {"7500": "interferencia ilícita", "7600": "fallo de radio", "7700": "emergencia general"}
    for e, pais in alertas[:40]:
        out.append({"uid": f"alerta-{e[0]}", "pais": pais, "lat": e[6], "lon": e[5], "ts": _fecha(e[4]) or ahora,
                    "titulo": f"Avión {str(e[1] or e[0]).strip()} con código {e[14]} ({motivo[str(e[14])]})",
                    "detalle": f"Matrícula de {e[2]}; altitud {float(e[7] or 0):.0f} m", "valor": 1e6,
                    "url": f"https://opensky-network.org/aircraft-profile?icao24={e[0]}",
                    "datos": {"icao24": e[0], "codigo": e[14], "velocidad_ms": e[9]}})
    return out


def _vuelos(ctx) -> list[dict]:
    return _leer_vuelos(ctx.json("https://opensky-network.org/api/states/all", tope_mb=24, espera=40))


def _leer_militares(datos: dict) -> list[dict]:
    out = []
    ahora = (_num(datos.get("now")) or time.time() * 1000) / 1000.0
    for a in datos.get("ac") or []:
        lat, lon = _num(a.get("lat")), _num(a.get("lon"))
        if lat is None or lon is None:
            continue
        nombre = (a.get("flight") or a.get("r") or a.get("hex") or "").strip()
        alt = a.get("alt_baro")
        emergencia = a.get("emergency") not in (None, "", "none")
        out.append({"uid": str(a.get("hex")), "pais": paises.pais_de(lat, lon, margen=0.2) or "XZ",
                    "lat": lat, "lon": lon, "ts": ahora - float(a.get("seen") or 0),
                    "titulo": f"Aeronave militar {nombre}" + (f" ({a.get('t')})" if a.get("t") else "") +
                              (" · EMERGENCIA" if emergencia else ""),
                    "detalle": ", ".join(x for x in (
                        a.get("desc") or "", f"altitud {alt} ft" if isinstance(alt, (int, float)) else str(alt or ""),
                        f"{a['gs']:.0f} nudos" if isinstance(a.get("gs"), (int, float)) else "") if x),
                    "url": f"https://globe.adsb.lol/?icao={a.get('hex')}", "valor": 1e6 if emergencia else 1,
                    "datos": {"tipo": a.get("t"), "matricula": a.get("r"), "rumbo": a.get("track"),
                              "codigo": a.get("squawk")}})
    return out[:600]


def _militares(ctx) -> list[dict]:
    return _leer_militares(ctx.json("https://api.adsb.lol/v2/mil"))


def _leer_gbfs_sistemas(texto: str) -> list[dict]:
    grupos: dict[str, list] = {}
    for fila in csv.DictReader(io.StringIO(texto)):
        pais = paises.codigo(fila.get("Country Code") or "")
        grupos.setdefault(pais, []).append((fila.get("Name") or "", fila.get("Location") or "",
                                            fila.get("System ID") or "", fila.get("Auto-Discovery URL") or ""))
    out = []
    for pais, sistemas in grupos.items():
        out.append({"uid": f"bicis-{pais}", "pais": pais, "ts": time.time(), "valor": len(sistemas), "lat": None,
                    "lon": None, "url": "https://github.com/MobilityData/gbfs/blob/master/systems.csv",
                    "titulo": f"Bicicleta y patinete compartidos en {paises.nombre(pais)}: {len(sistemas)} sistemas",
                    "detalle": "; ".join(f"{n} ({lugar})" for n, lugar, _, _ in sistemas[:12]),
                    "datos": {"sistemas": [{"nombre": n, "lugar": lugar, "id": sid, "gbfs": u}
                                           for n, lugar, sid, u in sistemas[:60]]}})
    out.sort(key=lambda r: -r["valor"])
    return out


def _leer_gbfs_estado(nombre: str, pais: str, datos: dict, sid: str) -> dict | None:
    estaciones = ((datos.get("data") or {}).get("stations")) or []
    if not estaciones:
        return None
    bicis = sum(int(_num(s.get("num_bikes_available"), 0) or 0) for s in estaciones)
    anclajes = sum(int(_num(s.get("num_docks_available"), 0) or 0) for s in estaciones)
    return {"uid": f"gbfs-{sid}", "pais": pais, "ts": _fecha(datos.get("last_updated")) or time.time(),
            "titulo": f"{nombre}: {bicis} bicicletas libres en {len(estaciones)} estaciones",
            "detalle": f"{anclajes} anclajes libres", "valor": bicis, "lat": None, "lon": None, "url": "",
            "datos": {"bicicletas": bicis, "anclajes": anclajes, "estaciones": len(estaciones)}}


def _bicis(ctx) -> list[dict]:
    texto = ctx.texto("https://raw.githubusercontent.com/MobilityData/gbfs/master/systems.csv")
    out = _leer_gbfs_sistemas(texto)
    quiero = {str(x).lower() for x in ctx.cfg("gbfs_sistemas", []) or []}
    if quiero:   # disponibilidad en vivo de los sistemas elegidos por el usuario
        for fila in csv.DictReader(io.StringIO(texto)):
            sid = (fila.get("System ID") or "").lower()
            if sid not in quiero or not fila.get("Auto-Discovery URL"):
                continue
            try:
                indice = ctx.json(fila["Auto-Discovery URL"])
                feeds = (indice.get("data") or {})
                feeds = next(iter(feeds.values()))["feeds"] if "feeds" not in feeds else feeds["feeds"]
                url = next(f["url"] for f in feeds if f.get("name") == "station_status")
                r = _leer_gbfs_estado(fila.get("Name") or sid, paises.codigo(fila.get("Country Code") or ""),
                                      ctx.json(url), sid)
                if r:
                    out.insert(0, r)
            except Exception as e:  # noqa: BLE001  (un sistema caido no tira la fuente)
                ctx.aviso(f"GBFS {sid}: {e}")
    return out


def _buques(ctx) -> list[dict]:
    """Muestra de unos segundos del flujo mundial de AISStream (necesita clave gratuita)."""
    from websockets.sync.client import connect
    grupos: dict[str, dict] = {}
    fin = time.time() + float(ctx.cfg("buques_muestra_s", 12))
    with connect("wss://stream.aisstream.io/v0/stream", open_timeout=15, close_timeout=3) as ws:
        ws.send(json.dumps({"APIKey": ctx.clave("aisstream"), "BoundingBoxes": [[[-90, -180], [90, 180]]],
                            "FilterMessageTypes": ["PositionReport"]}))
        while time.time() < fin:
            try:
                m = json.loads(ws.recv(timeout=max(0.5, fin - time.time())))
            except TimeoutError:
                break
            meta = m.get("MetaData") or {}
            lat, lon = _num(meta.get("latitude")), _num(meta.get("longitude"))
            if lat is None or lon is None:
                continue
            pais = paises.pais_de(lat, lon, margen=0.3) or "XZ"
            g = grupos.setdefault(pais, {"n": set(), "nombres": []})
            if meta.get("MMSI") not in g["n"]:
                g["n"].add(meta.get("MMSI"))
                if len(g["nombres"]) < 8 and (meta.get("ShipName") or "").strip():
                    g["nombres"].append(meta["ShipName"].strip())
    return _resumen_buques(grupos)


def _resumen_buques(grupos: dict) -> list[dict]:
    out = [{"uid": f"buques-{pais}", "pais": pais, "ts": time.time(), "valor": len(g["n"]), "lat": None, "lon": None,
            "titulo": f"Barcos emitiendo posición en {paises.nombre(pais)}: {len(g['n'])} en la muestra",
            "detalle": "Entre ellos: " + ", ".join(g["nombres"]) if g["nombres"] else "", "url": "https://aisstream.io/",
            "datos": {"barcos": len(g["n"])}} for pais, g in grupos.items()]
    out.sort(key=lambda r: -r["valor"])
    return out


# ------------------------------------------------------------------ clima
def _leer_ciclones(datos: dict) -> list[dict]:
    out = []
    for t in datos.get("activeStorms") or []:
        lat = _num(t.get("latitudeNumeric"), _num(t.get("latitude")))
        lon = _num(t.get("longitudeNumeric"), _num(t.get("longitude")))
        viento = _num(t.get("intensity"))
        clase = {"HU": "Huracán", "TS": "Tormenta tropical", "TD": "Depresión tropical", "STS": "Tormenta subtropical",
                 "PTC": "Ciclón postropical", "MH": "Huracán mayor"}.get(str(t.get("classification", "")).upper(),
                                                                         t.get("classification") or "Ciclón")
        out.append({"uid": str(t.get("id") or t.get("binNumber") or t.get("name")),
                    "titulo": f"{clase} {t.get('name', '')}".strip(),
                    "detalle": ", ".join(x for x in (
                        f"vientos de {viento * 1.852:.0f} km/h" if viento is not None else "",
                        f"presión {t.get('pressure')} hPa" if t.get("pressure") else "",
                        f"se mueve hacia {t.get('movementDir')}° a {t.get('movementSpeed')} nudos"
                        if t.get("movementDir") is not None else "") if x),
                    "pais": paises.pais_de(lat, lon, margen=2.0) if lat is not None else "", "lat": lat, "lon": lon,
                    "ts": _fecha(t.get("lastUpdate")), "valor": viento,
                    "url": (t.get("publicAdvisory") or {}).get("url") or "https://www.nhc.noaa.gov/",
                    "datos": {"clase": clase, "viento_nudos": viento, "presion_hpa": t.get("pressure")}})
    return out


def _ciclones(ctx) -> list[dict]:
    return _leer_ciclones(ctx.json("https://www.nhc.noaa.gov/CurrentStorms.json"))


_WMO = {0: "despejado", 1: "poco nuboso", 2: "intervalos nubosos", 3: "cubierto", 45: "niebla", 48: "niebla con escarcha",
        51: "llovizna débil", 53: "llovizna", 55: "llovizna intensa", 61: "lluvia débil", 63: "lluvia",
        65: "lluvia intensa", 66: "lluvia helada", 67: "lluvia helada intensa", 71: "nieve débil", 73: "nieve",
        75: "nieve intensa", 77: "granizo fino", 80: "chubascos débiles", 81: "chubascos", 82: "chubascos fuertes",
        85: "chubascos de nieve", 86: "chubascos de nieve fuertes", 95: "tormenta", 96: "tormenta con granizo",
        99: "tormenta con granizo fuerte"}


def _leer_tiempo(lugares: list[dict], datos, aire=None) -> list[dict]:
    """``datos``: respuesta de Open-Meteo (un objeto por punto; con un solo punto no viene en lista)."""
    datos = datos if isinstance(datos, list) else [datos]
    aire = aire if isinstance(aire, list) else ([aire] if aire else [])
    out = []
    for i, (lugar, d) in enumerate(zip(lugares, datos)):
        c = (d or {}).get("current") or {}
        if not c:
            continue
        a = (aire[i].get("current") or {}) if i < len(aire) and aire[i] else {}
        estado = _WMO.get(int(c.get("weather_code") or 0), "")
        partes = [f"{c.get('temperature_2m')} °C", estado, f"viento {c.get('wind_speed_10m')} km/h",
                  f"humedad {c.get('relative_humidity_2m')} %" if c.get("relative_humidity_2m") is not None else "",
                  f"lluvia {c.get('precipitation')} mm" if c.get("precipitation") else "",
                  f"índice europeo de calidad del aire {a.get('european_aqi')}" if a.get("european_aqi") is not None else ""]
        out.append({"uid": f"tiempo-{lugar['nombre']}", "pais": lugar.get("pais", ""), "lat": lugar["lat"],
                    "lon": lugar["lon"], "ts": _fecha(c.get("time")) or time.time(),
                    "titulo": f"Tiempo en {lugar['nombre']}: " + ", ".join(p for p in partes[:2] if p),
                    "detalle": ", ".join(p for p in partes[2:] if p), "valor": _num(c.get("temperature_2m")),
                    "url": "https://open-meteo.com/",
                    "datos": {"temperatura_c": c.get("temperature_2m"), "viento_kmh": c.get("wind_speed_10m"),
                              "humedad_pct": c.get("relative_humidity_2m"), "lluvia_mm": c.get("precipitation"),
                              "estado": estado, "calidad_aire": a.get("european_aqi"), "pm2_5": a.get("pm2_5"),
                              "pm10": a.get("pm10")}})
    return out


def _lugares(ctx) -> list[dict]:
    """La ciudad principal de cada pais de interes, mas los lugares que el usuario anada en mundo.lugares."""
    out = []
    for x in ctx.cfg("lugares", []) or []:
        if isinstance(x, dict) and _num(x.get("lat")) is not None and _num(x.get("lon")) is not None:
            out.append({"nombre": str(x.get("nombre") or f"{x['lat']},{x['lon']}"), "lat": float(x["lat"]),
                        "lon": float(x["lon"]), "pais": paises.pais_de(x["lat"], x["lon"])})
    try:
        from pathlib import Path
        ciudades = json.loads((Path(__file__).parent.parent / "devices" / "ciudades.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        ciudades = []
    interes = ctx.paises_interes()
    for c in ciudades:   # [nombre, provincia, pais, latitud, longitud]; las primeras de cada pais son las mayores
        if not isinstance(c, list) or len(c) < 5:
            continue
        pais = paises.codigo(c[2])
        if pais in interes and sum(1 for o in out if o["pais"] == pais) < 3:
            out.append({"nombre": c[0], "lat": float(c[3]), "lon": float(c[4]), "pais": pais})
    return out[:40]


def _tiempo(ctx) -> list[dict]:
    lugares = _lugares(ctx)
    if not lugares:
        return []
    lat = ",".join(f"{x['lat']:.3f}" for x in lugares)
    lon = ",".join(f"{x['lon']:.3f}" for x in lugares)
    datos = ctx.json("https://api.open-meteo.com/v1/forecast", params={
        "latitude": lat, "longitude": lon, "timezone": "UTC",
        "current": "temperature_2m,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"})
    try:
        aire = ctx.json("https://air-quality-api.open-meteo.com/v1/air-quality", params={
            "latitude": lat, "longitude": lon, "current": "european_aqi,pm10,pm2_5", "timezone": "UTC"})
    except Exception as e:  # noqa: BLE001
        ctx.aviso(f"calidad del aire: {e}")
        aire = None
    return _leer_tiempo(lugares, datos, aire)


# ------------------------------------------------------------------ espacio
_GRUPOS_SAT = {"stations": "Estaciones espaciales", "resource": "Observación de la Tierra", "weather": "Meteorológicos",
               "gnss": "Navegación (GPS, Galileo...)", "science": "Científicos",
               "last-30-days": "Lanzados en los últimos 30 días"}


def _altitud_km(vueltas_dia: float) -> float | None:
    if not vueltas_dia:
        return None
    n = vueltas_dia * 2 * math.pi / 86400.0
    return (398600.4418 / (n * n)) ** (1 / 3) - 6371.0


def _leer_satelites(grupo: str, datos: list) -> list[dict]:
    nombre = _GRUPOS_SAT.get(grupo, grupo)
    objetos = [o for o in datos if isinstance(o, dict)]
    out = [{"uid": f"sat-{grupo}", "pais": "", "lat": None, "lon": None, "ts": time.time(), "valor": len(objetos),
            "titulo": f"Satélites · {nombre}: {len(objetos)} objetos en órbita",
            "detalle": ", ".join(o.get("OBJECT_NAME", "") for o in objetos[:10]),
            "url": "https://celestrak.org/", "datos": {"grupo": grupo, "objetos": len(objetos)}}]
    if grupo in ("stations", "last-30-days"):
        for o in objetos[:60]:
            alt = _altitud_km(_num(o.get("MEAN_MOTION"), 0) or 0)
            out.append({"uid": f"sat-{o.get('NORAD_CAT_ID')}", "pais": "", "lat": None, "lon": None,
                        "ts": _fecha(o.get("EPOCH")), "valor": alt,
                        "titulo": f"{o.get('OBJECT_NAME', '')} (NORAD {o.get('NORAD_CAT_ID')})",
                        "detalle": (f"órbita a unos {alt:.0f} km, " if alt else "") +
                                   f"inclinación {o.get('INCLINATION')}°, {o.get('MEAN_MOTION')} vueltas al día",
                        "url": f"https://celestrak.org/satcat/table-satcat.php?CATNR={o.get('NORAD_CAT_ID')}",
                        "datos": {"grupo": grupo, "norad": o.get("NORAD_CAT_ID"), "inclinacion": o.get("INCLINATION")}})
    return out


def _satelites(ctx) -> list[dict]:
    out = []
    for grupo in ctx.cfg("satelites_grupos", list(_GRUPOS_SAT)) or []:
        try:
            out += _leer_satelites(grupo, ctx.json("https://celestrak.org/NORAD/elements/gp.php",
                                                   params={"GROUP": grupo, "FORMAT": "json"}))
        except Exception as e:  # noqa: BLE001
            ctx.aviso(f"satelites {grupo}: {e}")
    if not out:
        raise RuntimeError("CelesTrak no ha devuelto ningún grupo")
    return out


def _leer_lanzamientos(datos: dict) -> list[dict]:
    out = []
    for x in datos.get("results") or []:
        pad = x.get("pad") or {}
        lugar = pad.get("location") or {}
        pais = paises.codigo(lugar.get("country_code") or (pad.get("country") or {}).get("alpha_2_code") or
                             pad.get("country_code") or "")
        lat, lon = _num(pad.get("latitude")), _num(pad.get("longitude"))
        if not pais and lat is not None:
            pais = paises.pais_de(lat, lon, margen=1.0)
        mision = x.get("mission") or {}
        out.append({"uid": str(x.get("id")), "pais": pais, "lat": lat, "lon": lon, "ts": _fecha(x.get("net")),
                    "titulo": f"Lanzamiento: {x.get('name', '')}",
                    "detalle": ", ".join(p for p in (
                        (x.get("status") or {}).get("name") or "", (x.get("launch_service_provider") or {}).get("name") or "",
                        lugar.get("name") or pad.get("name") or "", ((mision.get("orbit") or {}).get("name") or ""),
                        _sin_html(mision.get("description") or "", 200)) if p),
                    "url": x.get("url") or "", "valor": None,
                    "datos": {"estado": (x.get("status") or {}).get("abbrev"), "en_directo": x.get("webcast_live")}})
    return out


def _lanzamientos(ctx) -> list[dict]:
    return _leer_lanzamientos(ctx.json("https://ll.thespacedevs.com/2.2.0/launch/upcoming/",
                                       params={"limit": "25", "mode": "normal"}))


# ------------------------------------------------------------------ noticias
def _leer_gdelt(datos: dict, tema: str = "") -> list[dict]:
    out = []
    for a in datos.get("articles") or []:
        titulo = (a.get("title") or "").strip()
        if not titulo:
            continue
        nombrados = paises.en_texto(titulo)
        origen = paises.codigo(a.get("sourcecountry") or "")
        out.append({"uid": a.get("url") or titulo, "pais": nombrados[0] if nombrados else origen, "lat": None,
                    "lon": None, "ts": _fecha(a.get("seendate")), "titulo": titulo[:240],
                    "detalle": f"{a.get('domain', '')} ({a.get('language', '')})", "url": a.get("url") or "",
                    "valor": None, "datos": {"medio": a.get("domain"), "pais_del_medio": origen,
                                             "idioma": a.get("language"), "tema": tema,
                                             "paises_nombrados": nombrados[:5]}})
    return out


_TEMA_CONFLICTOS = '(conflict OR airstrike OR "military attack" OR ceasefire OR protest OR coup)'


def _gdelt(ctx, consulta: str, tema: str, n: int = 75, plazo: str = "6h") -> list[dict]:
    return _leer_gdelt(ctx.json("https://api.gdeltproject.org/api/v2/doc/doc", params={
        "query": consulta, "mode": "artlist", "format": "json", "maxrecords": str(n), "timespan": plazo,
        "sort": "datedesc"}), tema)


def _conflictos(ctx) -> list[dict]:
    return _gdelt(ctx, _TEMA_CONFLICTOS, "conflictos")


def _noticias_pais(ctx) -> list[dict]:
    out, fallos = [], 0
    interes = ctx.paises_interes()[:8]
    for i, pais in enumerate(interes):
        if i:
            time.sleep(5.5)   # GDELT pide una consulta cada cinco segundos
        try:
            nombre = paises.nombre(pais, "en").lower().replace(" ", "")
            for r in _gdelt(ctx, f"sourcecountry:{nombre}", "local", n=30, plazo="12h"):
                r["pais"] = pais
                out.append(r)
        except Exception as e:  # noqa: BLE001
            fallos += 1
            ctx.aviso(f"noticias de {pais}: {e}")
    if interes and fallos == len(interes):
        raise RuntimeError("GDELT no ha respondido para ningún país")
    return out


# ------------------------------------------------------------------ camaras, audio, naturaleza
def _leer_camaras_tfl(datos: list) -> list[dict]:
    out = []
    for c in datos if isinstance(datos, list) else []:
        extra = {p.get("key"): p.get("value") for p in c.get("additionalProperties") or []}
        if str(extra.get("available", "true")).lower() == "false" or not extra.get("imageUrl"):
            continue
        out.append({"uid": str(c.get("id")), "pais": "GB", "lat": c.get("lat"), "lon": c.get("lon"),
                    "ts": time.time(), "titulo": f"Cámara de tráfico de Londres: {c.get('commonName', '')}",
                    "detalle": extra.get("view") or "", "url": extra.get("imageUrl"), "valor": None,
                    "datos": {"imagen": extra.get("imageUrl"), "video": extra.get("videoUrl")}})
    return out[:500]


def _leer_camaras_fi(datos: dict) -> list[dict]:
    out = []
    for f in datos.get("features") or []:
        p = f.get("properties") or {}
        c = (f.get("geometry") or {}).get("coordinates") or [None, None]
        presets = [x.get("id") for x in p.get("presets") or [] if x.get("id")]
        if not presets or p.get("collectionStatus") == "REMOVED_PERMANENTLY":
            continue
        out.append({"uid": str(p.get("id") or f.get("id")), "pais": "FI", "lat": c[1], "lon": c[0], "ts": time.time(),
                    "titulo": f"Cámara de carretera de Finlandia: {p.get('name', '')}", "detalle": "",
                    "url": f"https://weathercam.digitraffic.fi/{presets[0]}.jpg", "valor": None,
                    "datos": {"imagenes": [f"https://weathercam.digitraffic.fi/{x}.jpg" for x in presets[:4]]}})
    return out[:500]


def _camaras(ctx) -> list[dict]:
    out, fallos = [], []
    for nombre, url, leer in (("TfL", "https://api.tfl.gov.uk/Place/Type/JamCam", _leer_camaras_tfl),
                              ("Fintraffic", "https://tie.digitraffic.fi/api/weathercam/v1/stations", _leer_camaras_fi)):
        try:
            out += leer(ctx.json(url, tope_mb=20, cabeceras={"Digitraffic-User": "AgenteIA"}))
        except Exception as e:  # noqa: BLE001
            fallos.append(f"{nombre}: {e}")
            ctx.aviso(f"cámaras {nombre}: {e}")
    if not out and fallos:
        raise RuntimeError("; ".join(fallos))
    return out


def _leer_radio(datos: list) -> list[dict]:
    out = []
    for e in datos if isinstance(datos, list) else []:
        if not e.get("url_resolved") and not e.get("url"):
            continue
        out.append({"uid": str(e.get("stationuuid")), "pais": paises.codigo(e.get("countrycode") or ""),
                    "lat": _num(e.get("geo_lat")), "lon": _num(e.get("geo_long")), "ts": time.time(),
                    "titulo": f"Radio {str(e.get('name', '')).strip()}",
                    "detalle": ", ".join(x for x in (e.get("tags") or "", e.get("language") or "",
                                                     f"{e.get('codec', '')} {e.get('bitrate', '')} kbps".strip()) if x)[:200],
                    "url": e.get("url_resolved") or e.get("url"), "valor": _num(e.get("clickcount")),
                    "datos": {"web": e.get("homepage"), "idioma": e.get("language")}})
    return out


def _radio(ctx) -> list[dict]:
    base = "https://de1.api.radio-browser.info/json/stations/search"
    comun = {"order": "clickcount", "reverse": "true", "hidebroken": "true"}
    out = _leer_radio(ctx.json(base, params={**comun, "limit": "150"}))
    vistos = {r["uid"] for r in out}
    for pais in ctx.paises_interes()[:6]:
        try:
            for r in _leer_radio(ctx.json(base, params={**comun, "limit": "40", "countrycode": pais})):
                if r["uid"] not in vistos:
                    vistos.add(r["uid"])
                    out.append(r)
        except Exception as e:  # noqa: BLE001
            ctx.aviso(f"radio {pais}: {e}")
    return out


def _leer_fauna(datos: dict) -> list[dict]:
    out = []
    for o in datos.get("results") or []:
        try:
            lat, lon = (float(x) for x in str(o.get("location") or "").split(",")[:2])
        except ValueError:
            continue
        t = o.get("taxon") or {}
        nombre = t.get("preferred_common_name") or o.get("species_guess") or t.get("name") or "Especie"
        foto = ((o.get("photos") or [{}])[0].get("url") or "").replace("square", "medium")
        out.append({"uid": str(o.get("id")), "pais": paises.pais_de(lat, lon, margen=0.5), "lat": lat, "lon": lon,
                    "ts": _fecha(o.get("time_observed_at") or o.get("observed_on") or o.get("created_at")),
                    "titulo": f"Avistamiento: {nombre}" + (f" ({t.get('name')})" if t.get("name") and t.get("name") != nombre else ""),
                    "detalle": o.get("place_guess") or "", "url": o.get("uri") or "", "valor": None,
                    "datos": {"grupo": t.get("iconic_taxon_name"), "foto": foto}})
    return out


def _fauna(ctx) -> list[dict]:
    return _leer_fauna(ctx.json("https://api.inaturalist.org/v1/observations", params={
        "order": "desc", "order_by": "created_at", "per_page": "100", "quality_grade": "research", "photos": "true",
        "geo": "true", "locale": "es", "geoprivacy": "open"}))


# ------------------------------------------------------------------ mercados (nexus.office.mercados)
def _de_mercados(nombre: str, modulo: str = "mercados"):
    def obtener(ctx):
        import importlib
        return getattr(importlib.import_module(f"nexus.office.{modulo}"), nombre)(ctx)
    return obtener


FUENTES: list[Fuente] = [
    Fuente("sismos", "Terremotos de las últimas 24 horas", "sucesos", _sismos, 300, 60, "USGS",
           "Terremotos de magnitud 2,5 o más en todo el mundo"),
    Fuente("eventos_naturales", "Incendios, volcanes y tormentas en curso", "sucesos", _eonet, 1800, 600, "NASA EONET",
           "Sucesos naturales abiertos que sigue la NASA"),
    Fuente("desastres", "Alertas de desastres", "sucesos", _gdacs, 900, 300, "GDACS (ONU y Comisión Europea)",
           "Alertas verdes, naranjas y rojas de terremotos, ciclones, inundaciones, volcanes y sequías"),
    Fuente("perimetros_incendio", "Perímetros de incendios de Estados Unidos", "sucesos", _perimetros, 3600, 900,
           "WFIGS / NIFC", "Superficie quemada y contención de los incendios activos", activa=False),
    Fuente("focos_fuego", "Focos de fuego detectados por satélite", "sucesos", _firms, 3600, 900, "NASA FIRMS",
           "Detecciones VIIRS de las últimas 24 horas agrupadas por país", clave="firms", peso="pesada"),
    Fuente("vuelos", "Aviones en vuelo por país", "movimiento", _vuelos, 1800, 900, "OpenSky Network",
           "Cuántos aviones hay sobre cada país y los que emiten un código de emergencia", peso="pesada"),
    Fuente("vuelos_militares", "Aeronaves militares con transpondedor", "movimiento", _militares, 600, 120, "adsb.lol",
           "Aeronaves militares que emiten su posición por ADS-B", peso="media"),
    Fuente("buques", "Barcos por país", "movimiento", _buques, 1800, 600, "AISStream",
           "Muestra del tráfico marítimo mundial agrupada por país", clave="aisstream", peso="media"),
    Fuente("bicis", "Bicicleta y patinete compartidos", "movimiento", _bicis, 86400, 3600, "MobilityData GBFS",
           "Sistemas de cada país y, de los que elijas en mundo.gbfs_sistemas, las bicicletas libres"),
    Fuente("ciclones", "Ciclones tropicales activos", "clima", _ciclones, 1800, 600, "NOAA NHC / CPHC",
           "Posición, viento y presión de los ciclones del Atlántico y del Pacífico norte"),
    Fuente("tiempo", "Tiempo y calidad del aire", "clima", _tiempo, 1800, 600, "Open-Meteo",
           "Tiempo actual y calidad del aire de las ciudades de tus países de interés y de tus lugares"),
    Fuente("satelites", "Satélites en órbita", "espacio", _satelites, 43200, 7200, "CelesTrak",
           "Recuento por clase y elementos orbitales de estaciones y lanzamientos recientes", peso="media"),
    Fuente("lanzamientos", "Próximos lanzamientos espaciales", "espacio", _lanzamientos, 3600, 900,
           "Launch Library 2", "Los próximos lanzamientos con su base, cohete y misión"),
    Fuente("conflictos", "Noticias de conflictos", "noticias", _conflictos, 900, 300, "GDELT",
           "Titulares de las últimas horas sobre conflictos y protestas, por país nombrado", acumula=True),
    Fuente("noticias_pais", "Noticias de tus países", "noticias", _noticias_pais, 1800, 600, "GDELT",
           "Titulares recientes de los medios de cada país de interés", acumula=True),
    Fuente("camaras", "Cámaras públicas de tráfico", "camaras", _camaras, 21600, 3600, "TfL y Fintraffic",
           "Cámaras de Londres y de las carreteras de Finlandia con el enlace a su imagen", peso="media", activa=False),
    Fuente("radio", "Emisoras de radio del mundo", "audio", _radio, 86400, 3600, "Radio Browser",
           "Las emisoras más escuchadas del mundo y de tus países, con su enlace", peso="media"),
    Fuente("fauna", "Avistamientos de fauna y flora", "naturaleza", _fauna, 3600, 600, "iNaturalist",
           "Observaciones recientes verificadas, con foto"),
    Fuente("indices", "Índices, materias primas y divisas", "mercados", _de_mercados("fuente_indices"), 300, 60,
           "Yahoo Finance, Stooq y otros", "Cotización de los valores de mundo.mercados.simbolos"),
    Fuente("cripto", "Criptomonedas", "mercados", _de_mercados("fuente_cripto"), 300, 60, "CoinGecko y Binance",
           "Las principales criptomonedas y la dominancia de Bitcoin y Ether"),
    Fuente("macro", "Tipos de interés y volatilidad", "mercados", _de_mercados("fuente_macro"), 21600, 3600,
           "FRED (Reserva Federal)", "Curva de tipos del Tesoro de Estados Unidos y VIX"),
    Fuente("calendario_economico", "Calendario económico", "mercados", _de_mercados("fuente_calendario"), 3600, 900,
           "Forex Factory", "Datos de la semana con previsión, dato anterior e importancia"),
    Fuente("noticias_mercado", "Noticias de mercado", "mercados", _de_mercados("fuente_noticias"), 900, 300,
           "Yahoo Finance y Google Noticias", "Titulares de los valores seguidos y del mercado", acumula=True),
    Fuente("sectores", "Sectores de la bolsa", "mercados", _de_mercados("fuente_sectores"), 900, 300,
           "TradingView", "Cómo va cada sector en los mercados elegidos"),
    Fuente("resultados", "Resultados empresariales del día", "mercados", _de_mercados("fuente_resultados"), 21600, 3600,
           "Nasdaq", "Empresas que publican resultados hoy, con el beneficio por acción esperado"),
    Fuente("alertas_bolsa", "Alertas de bolsa", "mercados", _de_mercados("fuente_alertas", "bolsa_analisis"), 300, 60,
           "Agente IA", "Las alertas de precio e indicadores que has creado, cuando se cumplen", acumula=True),
]


def por_id() -> dict[str, Fuente]:
    return {f.id: f for f in FUENTES}
