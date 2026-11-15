"""Genera nexus/mundo/paises.json a partir de Natural Earth (dominio publico): contornos a 110 m de los paises
grandes y un circulo para los pequenos que esa escala no dibuja (Singapur, Malta, islas...).

    python packaging/build_paises.py ne_110m_admin_0_countries.geojson ne_50m_admin_0_countries.geojson
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

DESTINO = Path(__file__).resolve().parent.parent / "nexus" / "mundo" / "paises.json"
CONT = {"Europe": "Europa", "Asia": "Asia", "Africa": "África", "North America": "América del Norte",
        "South America": "América del Sur", "Oceania": "Oceanía", "Antarctica": "Antártida",
        "Seven seas (open ocean)": "Océanos"}
SIN_ISO = {"CYN": "CY", "SOL": "SO", "KOS": "XK"}   # sin codigo propio: van con el pais que los administra o XK
NOMBRES = {"TW": "Taiwán", "US": "Estados Unidos", "GB": "Reino Unido", "CD": "República Democrática del Congo"}


def _codigo(p: dict) -> str:
    for k in ("ISO_A2_EH", "ISO_A2"):
        if p.get(k) and p[k] != "-99":
            return p[k]
    return SIN_ISO.get(p.get("ADM0_A3", ""), "")


def _dentro(lon: float, lat: float, anillo: list) -> bool:
    dentro, j = False, len(anillo) - 1
    for i in range(len(anillo)):
        xi, yi = anillo[i]
        xj, yj = anillo[j]
        if (yi > lat) != (yj > lat) and lon < (xj - xi) * (lat - yi) / (yj - yi) + xi:
            dentro = not dentro
        j = i
    return dentro


def _anillos(geom: dict, decimales: int = 2) -> list[list[list[float]]]:
    polis = geom["coordinates"] if geom["type"] == "MultiPolygon" else [geom["coordinates"]]
    out = []
    for poli in polis:
        anillo, previo = [], None
        for lon, lat in poli[0]:   # solo el contorno exterior: los huecos (Lesoto) tienen su propia entrada
            punto = [round(lon, decimales), round(lat, decimales)]
            if punto != previo:
                anillo.append(punto)
            previo = punto
        if len(anillo) >= 4:
            out.append(anillo)
    return out


def _caja(anillos) -> list[float]:
    xs = [p[0] for a in anillos for p in a]
    ys = [p[1] for a in anillos for p in a]
    return [min(xs), min(ys), max(xs), max(ys)]


def main(f110: str, f50: str) -> None:
    paises, vistos = [], set()
    for f in json.loads(Path(f110).read_text(encoding="utf-8"))["features"]:
        p = f["properties"]
        a2 = _codigo(p)
        if not a2:
            continue
        anillos = _anillos(f["geometry"])
        if a2 in vistos:   # N. Chipre y Somalilandia: se suman a su pais
            previo = next(x for x in paises if x["a2"] == a2)
            previo["pol"] += anillos
            previo["caja"] = _caja(previo["pol"])
            continue
        vistos.add(a2)
        paises.append({"a2": a2, "a3": p.get("ISO_A3_EH") if p.get("ISO_A3_EH") != "-99" else p.get("ADM0_A3", ""),
                       "es": NOMBRES.get(a2) or p.get("NAME_ES") or p["NAME"], "en": p["NAME"], "largo": p.get("NAME_LONG") or "",
                       "cont": CONT.get(p.get("CONTINENT", ""), p.get("CONTINENT", "")),
                       "pob": int(p.get("POP_EST") or 0), "centro": [round(p["LABEL_X"], 2), round(p["LABEL_Y"], 2)],
                       "caja": _caja(anillos), "pol": anillos})
    grandes = {x["a2"]: x for x in paises}
    for f in json.loads(Path(f50).read_text(encoding="utf-8"))["features"]:
        p = f["properties"]
        a2 = _codigo(p)
        if a2 in grandes:   # islas que la escala de 110 m no dibuja (Canarias, Baleares, Azores, Hawai...)
            pais = grandes[a2]
            base = list(pais["pol"])
            for anillo in _anillos(f["geometry"], 1):
                cx = sum(q[0] for q in anillo) / len(anillo)
                cy = sum(q[1] for q in anillo) / len(anillo)
                if not any(_dentro(cx, cy, b) for b in base):
                    pais["pol"].append(anillo)
            pais["caja"] = _caja(pais["pol"])
            continue
        if not a2 or a2 in vistos:
            continue
        vistos.add(a2)
        caja = _caja(_anillos(f["geometry"]) or [[[p["LABEL_X"], p["LABEL_Y"]]] * 4])
        radio = max(0.15, min(3.0, max(caja[2] - caja[0], caja[3] - caja[1]) / 2))
        paises.append({"a2": a2, "a3": p.get("ISO_A3_EH") if p.get("ISO_A3_EH") != "-99" else p.get("ADM0_A3", ""),
                       "es": NOMBRES.get(a2) or p.get("NAME_ES") or p["NAME"], "en": p["NAME"], "largo": p.get("NAME_LONG") or "",
                       "cont": CONT.get(p.get("CONTINENT", ""), p.get("CONTINENT", "")),
                       "pob": int(p.get("POP_EST") or 0), "centro": [round(p["LABEL_X"], 2), round(p["LABEL_Y"], 2)],
                       "radio": round(radio, 2)})
    paises.sort(key=lambda x: x["a2"])
    DESTINO.write_text(json.dumps({"fuente": "Natural Earth (dominio publico), 110 m y 50 m", "paises": paises},
                                  ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"{len(paises)} paises, {DESTINO.stat().st_size // 1024} KB -> {DESTINO}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
