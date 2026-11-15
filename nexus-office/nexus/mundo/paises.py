"""Paises: a que pais pertenece un punto del mapa y como se llama cada uno (codigo ISO, espanol e ingles).

Los contornos vienen de Natural Earth (dominio publico) y van dentro del programa (``paises.json``): la consulta
no necesita red. Las fuentes de informacion usan esto para ordenar lo que reciben por paises.
"""
from __future__ import annotations

import json
import math
import threading
import unicodedata
from pathlib import Path

_FICHERO = Path(__file__).with_name("paises.json")
_mu = threading.Lock()
_datos: list[dict] | None = None
_por_codigo: dict[str, dict] = {}
_por_nombre: dict[str, str] = {}

# nombres que usan las fuentes y que no coinciden con los de Natural Earth
_ALIAS = {
    "usa": "US", "u.s.": "US", "u.s.a.": "US", "united states": "US", "estados unidos": "US", "ee. uu.": "US",
    "eeuu": "US", "uk": "GB", "united kingdom": "GB", "great britain": "GB", "gran bretana": "GB", "inglaterra": "GB",
    "russia": "RU", "russian federation": "RU", "rusia": "RU", "south korea": "KR", "korea, republic of": "KR",
    "republic of korea": "KR", "corea del sur": "KR", "north korea": "KP", "corea del norte": "KP",
    "iran": "IR", "iran, islamic republic of": "IR", "syria": "SY", "siria": "SY", "vietnam": "VN", "viet nam": "VN",
    "czech republic": "CZ", "czechia": "CZ", "chequia": "CZ", "republica checa": "CZ", "turkey": "TR", "turkiye": "TR",
    "turquia": "TR", "ivory coast": "CI", "cote d'ivoire": "CI", "costa de marfil": "CI", "laos": "LA",
    "moldova": "MD", "moldavia": "MD", "tanzania": "TZ", "bolivia": "BO", "venezuela": "VE", "taiwan": "TW",
    "hong kong": "HK", "macau": "MO", "macao": "MO", "palestine": "PS", "palestina": "PS", "holland": "NL",
    "the netherlands": "NL", "holanda": "NL", "paises bajos": "NL", "uae": "AE", "united arab emirates": "AE",
    "emiratos arabes unidos": "AE", "emiratos": "AE", "dr congo": "CD", "democratic republic of the congo": "CD",
    "congo (kinshasa)": "CD", "republic of the congo": "CG", "congo (brazzaville)": "CG", "burma": "MM",
    "myanmar": "MM", "birmania": "MM", "cape verde": "CV", "cabo verde": "CV", "eswatini": "SZ", "swaziland": "SZ",
    "north macedonia": "MK", "macedonia": "MK", "macedonia del norte": "MK", "bosnia": "BA", "kosovo": "XK",
    "european union": "EU", "union europea": "EU", "eurozona": "EU", "euro area": "EU", "euro zone": "EU",
}
# moneda -> pais o zona (calendario economico y divisas)
MONEDAS = {"USD": "US", "EUR": "EU", "GBP": "GB", "JPY": "JP", "CHF": "CH", "CAD": "CA", "AUD": "AU", "NZD": "NZ",
           "CNY": "CN", "CNH": "CN", "MXN": "MX", "BRL": "BR", "INR": "IN", "KRW": "KR", "SEK": "SE", "NOK": "NO",
           "DKK": "DK", "PLN": "PL", "TRY": "TR", "ZAR": "ZA", "RUB": "RU", "HKD": "HK", "SGD": "SG", "ARS": "AR",
           "CLP": "CL", "COP": "CO", "PEN": "PE", "CZK": "CZ", "HUF": "HU", "ILS": "IL", "THB": "TH", "IDR": "ID"}
_EXTRA = {"EU": {"a2": "EU", "a3": "EUU", "es": "Unión Europea", "en": "European Union", "cont": "Europa"},
          "XZ": {"a2": "XZ", "a3": "", "es": "Aguas internacionales", "en": "International waters", "cont": "Océanos"},
          "": {"a2": "", "a3": "", "es": "Sin país", "en": "No country", "cont": ""}}


def _plano(texto: str) -> str:
    t = unicodedata.normalize("NFKD", str(texto or "").strip().lower())
    return "".join(c for c in t if not unicodedata.combining(c))


def _cargar() -> list[dict]:
    global _datos
    if _datos is None:
        with _mu:
            if _datos is None:
                datos = json.loads(_FICHERO.read_text(encoding="utf-8"))["paises"]
                for p in datos:
                    _por_codigo[p["a2"]] = p
                    for nombre in (p["es"], p["en"], p.get("largo"), p["a2"], p.get("a3")):
                        if nombre:
                            _por_nombre.setdefault(_plano(nombre), p["a2"])
                for alias, codigo in _ALIAS.items():
                    _por_nombre[_plano(alias)] = codigo
                _datos = datos
    return _datos


def lista() -> list[dict]:
    """Todos los paises: codigo, nombres, continente y poblacion (sin los contornos)."""
    return [{k: p.get(k) for k in ("a2", "a3", "es", "en", "cont", "pob")} for p in _cargar()]


def nombre(codigo: str, idioma: str = "es") -> str:
    _cargar()
    codigo = (codigo or "").upper()
    p = _por_codigo.get(codigo) or _EXTRA.get(codigo)
    return (p.get(idioma) or p.get("es")) if p else codigo


def continente(codigo: str) -> str:
    _cargar()
    p = _por_codigo.get((codigo or "").upper()) or _EXTRA.get((codigo or "").upper())
    return p.get("cont", "") if p else ""


def codigo(texto: str) -> str:
    """Codigo ISO de dos letras a partir de un nombre en espanol o ingles, un codigo de dos o tres letras o una
    moneda. Cadena vacia si no se reconoce."""
    _cargar()
    t = str(texto or "").strip()
    if not t:
        return ""
    if len(t) == 2 and (t.upper() in _por_codigo or t.upper() in _EXTRA):
        return t.upper()
    return _por_nombre.get(_plano(t), "")


def en_texto(texto: str) -> list[str]:
    """Paises nombrados en una frase («terremotos en Japon y Chile» -> ['JP', 'CL']), en el orden en que salen."""
    _cargar()
    plano = " " + "".join(c if c.isalnum() else " " for c in _plano(texto)) + " "
    hallados: list[tuple[int, str]] = []
    for nombre_plano, cod in _por_nombre.items():
        if len(nombre_plano) < 4:
            continue
        clave = " " + "".join(c if c.isalnum() else " " for c in nombre_plano) + " "
        i = plano.find(clave)
        if i >= 0:
            hallados.append((i, cod))
    out: list[str] = []
    for _, cod in sorted(hallados):
        if cod not in out:
            out.append(cod)
    return out


def _dentro(lon: float, lat: float, anillo: list) -> bool:
    dentro, j = False, len(anillo) - 1
    for i in range(len(anillo)):
        xi, yi = anillo[i]
        xj, yj = anillo[j]
        if (yi > lat) != (yj > lat) and lon < (xj - xi) * (lat - yi) / (yj - yi) + xi:
            dentro = not dentro
        j = i
    return dentro


def _dist_anillo(lon: float, lat: float, anillo: list) -> float:
    """Distancia aproximada en grados al contorno (basta para decidir «en la costa» o «en el mar»)."""
    mejor = 1e9
    k = math.cos(math.radians(lat)) or 1e-6
    for i in range(len(anillo) - 1):
        x1, y1 = anillo[i]
        x2, y2 = anillo[i + 1]
        dx, dy = (x2 - x1) * k, y2 - y1
        px, py = (lon - x1) * k, lat - y1
        largo = dx * dx + dy * dy
        t = 0.0 if largo == 0 else max(0.0, min(1.0, (px * dx + py * dy) / largo))
        d = math.hypot(px - t * dx, py - t * dy)
        if d < mejor:
            mejor = d
    return mejor


def pais_de(lat, lon, margen: float = 0.45) -> str:
    """Codigo del pais donde cae el punto. En el mar devuelve '' salvo que este a menos de ``margen`` grados de
    una costa (unos 50 km): los contornos son de escala pequena y los puertos caen a veces fuera."""
    try:
        lat, lon = float(lat), float(lon)
    except (TypeError, ValueError):
        return ""
    if not (-90 <= lat <= 90) or not (-180 <= lon <= 360):
        return ""
    if lon > 180:
        lon -= 360
    cerca, cerca_d = "", margen
    for p in _cargar():
        if "pol" not in p:
            cx, cy = p["centro"]
            d = math.hypot((lon - cx) * math.cos(math.radians(lat)), lat - cy)
            if d <= p["radio"]:
                return p["a2"]
            continue
        x0, y0, x1, y1 = p["caja"]
        if not (x0 - margen <= lon <= x1 + margen and y0 - margen <= lat <= y1 + margen):
            continue
        for anillo in p["pol"]:
            if _dentro(lon, lat, anillo):
                # un pais pequeno dentro de otro (San Marino, Vaticano, Lesoto dibujado) tiene preferencia
                for q in _datos or []:
                    if "radio" in q and math.hypot((lon - q["centro"][0]) * math.cos(math.radians(lat)),
                                                   lat - q["centro"][1]) <= q["radio"] * 0.6:
                        return q["a2"]
                return p["a2"]
            d = _dist_anillo(lon, lat, anillo)
            if d < cerca_d:
                cerca, cerca_d = p["a2"], d
    return cerca
