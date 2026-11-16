"""Oficina Herrero: de un encargo en lenguaje corriente («porche de 6 x 4 m galvanizado») a un proyecto de taller.

La oficina trabaja por puestos, como un taller de cerrajeria con oficina tecnica, y todos son codigo determinista
(el resultado no depende del modelo de lenguaje y funciona igual en el movil con un modelo pequeno o sin modelo):

    recepcion   interpreta el encargo (tipo, medidas, acero, acabado, uso, cubierta...) y anota los supuestos
    calculista  cargas del CTE y perfiles elegidos con las comprobaciones del Eurocodigo 3 (herrero_modelo)
    despiece    piezas con marca, lista de corte en barras comerciales, chapas y pesos
    delineante  plano de montaje A3 (alzados, planta, cotas, marcas), isometrica y PDF de 3 hojas
    soldador    uniones con su garganta y simbolo ISO 2553, metros de cordon, hilo y requisitos EN 1090
    montaje     secuencia de montaje en obra
    fotografia  imagen realista: render propio y, si hay modelo de imagen, version fotografica a partir de el

El agente «herrero» (agents/defaults/herrero.yaml) usa esta oficina con la herramienta herrero_proyecto y explica
el resultado; la pestana HERRERO de la interfaz la usa directamente.
"""
from __future__ import annotations

import csv
import io
import json
import math
import re
import time
import unicodedata
from pathlib import Path

from .. import log, paths
from . import herrero_calculo as C
from . import herrero_dibujo as D
from . import herrero_modelo as M

L = log.get("herrero")
AVISO = ("Predimensionado orientativo según el CTE y el Eurocódigo 3 para presupuestar y preparar el taller. Una "
         "estructura real necesita el proyecto y la firma de un técnico competente y su ejecución según EN 1090.")


def _sin_tildes(t: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFKD", t.lower()) if not unicodedata.combining(c))


def _num(s: str) -> float:
    return float(s.replace(",", "."))


def _a_m(valor: float, unidad: str | None) -> float:
    u = (unidad or "").strip().lower()
    if u.startswith("mm"):
        return valor / 1000
    if u.startswith("cm"):
        return valor / 100
    if not u and valor >= 100:            # 6000 sin unidad: milimetros
        return valor / 1000
    return valor


_TIPOS_RE = [
    ("nave", r"\bnave|hangar|pabellon|almacen agricola|porticos? a dos aguas|cobertizo"),
    ("entreplanta", r"entreplanta|altillo|mezzanine|forjado metalico|plataforma elevada|planta intermedia"),
    ("pasarela", r"pasarela|puente peatonal|\bpuente\b"),
    ("escalera", r"escalera|escalinata"),
    ("pergola", r"pergola"),
    ("porche", r"porche|cochera|marquesina|carport|tejadillo|cubierta|aparcamiento|toldo|techado|voladizo"),
    ("barandilla", r"barandilla|baranda|antepecho|balaustrada|pasamanos|quitamiedos"),
]
_NUM = r"(\d+(?:[.,]\d+)?)"
_UNI = r"\s*(mm|cm|m|metros?)?"


def interpretar(texto: str) -> tuple[str, dict, list[str]]:
    """(tipo, parametros, supuestos) a partir del encargo en espanol. Solo reglas: rapido y sin modelo."""
    t = _sin_tildes(texto or "")
    P: dict = {}
    supuestos: list[str] = []
    tipo = next((k for k, rx in _TIPOS_RE if re.search(rx, t)), "")
    # medidas con etiqueta: «6 m de largo», «largo 6 m», «altura de 3,5 m», «luz 12 m»
    etiquetas = {"largo": r"largo|longitud|long\.?|de fondo largo", "ancho": r"ancho|anchura|fondo|luz|vuelo|profundidad",
                 "alto": r"alto|altura|alt\.?|salvar|altura libre|gálibo|galibo"}
    for k, rx in etiquetas.items():
        m = re.search(_NUM + _UNI + r"\s*(?:de\s+)?(?:" + rx + r")\b", t) or \
            re.search(r"(?:" + rx + r")\s*(?:de|:|=)?\s*" + _NUM + _UNI, t)
        if m:
            P[k] = round(_a_m(_num(m.group(1)), m.group(2)), 3)
    m = re.search(_NUM + _UNI + r"\s*(?:x|×|\*|por)\s*" + _NUM + _UNI + r"(?:\s*(?:x|×|\*|por)\s*" + _NUM + _UNI + ")?", t)
    if m:
        u = m.group(6) or m.group(4) or m.group(2)
        a = _a_m(_num(m.group(1)), m.group(2) or u)
        b = _a_m(_num(m.group(3)), m.group(4) or u)
        P.setdefault("largo", round(a, 3))
        P.setdefault("ancho", round(b, 3))
        if m.group(5):
            P.setdefault("alto", round(_a_m(_num(m.group(5)), m.group(6) or u), 3))
    if not any(k in P for k in ("largo", "ancho", "alto")):
        m = re.search(_NUM + r"\s*(mm|cm|m|metros?)\b", t)
        if m:
            v = _a_m(_num(m.group(1)), m.group(2))
            P["alto" if tipo == "escalera" else "largo"] = round(v, 3)
    if tipo == "escalera" and "alto" not in P and "largo" in P:
        P["alto"] = P.pop("largo")
    m = re.search(r"desnivel\s*(?:de)?\s*" + _NUM + _UNI, t) or re.search(_NUM + _UNI + r"\s*de\s*desnivel", t)
    if m:
        P["desnivel"] = _a_m(_num(m.group(1)), m.group(2))
    if tipo == "barandilla" and "alto" in P:
        P["alto_dado"] = True
    m = re.search(r"\bs\s?(235|275|355)", t)
    if m:
        P["acero"] = "S" + m.group(1)
    for k, rx in (("galvanizado", r"galvaniz"), ("oxiron", r"oxiron|forja|negro|hierro forjado"),
                  ("blanco", r"blanc"), ("verde", r"verde"), ("corten", r"corten|patinable|oxidad"),
                  ("pintado", r"pintad|lacad|antracita|\bral\b|esmalt")):
        if re.search(rx, t):
            P["acabado"] = k
            break
    for k, rx in (("ninguna", r"sin cubierta|sin techo|abierta arriba"), ("lamas", r"lamas|celosia"),
                  ("policarbonato", r"policarbonato"), ("teja", r"\bteja"), ("sandwich", r"sandwich|panel"),
                  ("chapa", r"chapa (?:grecada|simple|ondulada)|cubierta de chapa|techo de chapa")):
        if re.search(rx, t):
            P["cubierta"] = k
            break
    for k, rx in (("tramex", r"tramex|rejilla|trames"), ("chapa", r"lagrimada|chapa estriada"),
                  ("colaborante", r"colaborante|hormigon|losa"), ("tablero", r"tablero|madera|aglomerado")):
        if re.search(rx, t):
            P["forjado"] = k
            break
    for k, rx in (("almacen", r"almacen|palets?|estanteria|archivo|stock"), ("oficinas", r"oficina|despacho"),
                  ("publico", r"publico|clientes|local abierto|colegio|restaurante"), ("comercial", r"tienda|comercio|comercial"),
                  ("industrial", r"industrial|mantenimiento|fabrica|maquinaria|nave industrial"),
                  ("vivienda", r"vivienda|casa|chalet|piso|particular|domestic|jardin|terraza|balcon")):
        if re.search(rx, t):
            P["uso"] = k
            break
    m = re.search(_NUM + r"\s*(kg/m2|kg/m²|kn/m2|kn/m²|kilos por metro)", t)
    if m:
        v = _num(m.group(1))
        P["uso_kn_m2"] = v / 100 if m.group(2).startswith(("kg", "kilos")) else v
    m = re.search(r"(?:altitud|altura sobre el (?:nivel del )?mar|cota)\s*(?:de)?\s*" + _NUM, t) or \
        re.search(_NUM + r"\s*m\s*(?:de altitud|sobre el nivel del mar|snm)", t)
    if m:
        P["altitud"] = _num(m.group(1))
    m = re.search(r"zona\s*(?:de viento\s*)?([abc])\b", t)
    if m:
        P["zona_viento"] = m.group(1).upper()
    m = re.search(r"nieve\s*(?:de)?\s*" + _NUM + r"\s*kn", t)
    if m:
        P["nieve_kn_m2"] = _num(m.group(1))
    m = re.search(r"pendiente\s*(?:del?)?\s*" + _NUM + r"\s*(%|grados|º|°)?", t) or re.search(_NUM + r"\s*(%)\s*de pendiente", t)
    if m:
        v = _num(m.group(1))
        P["pendiente_pct"] = round(math.tan(math.radians(v)) * 100, 1) if (m.group(2) or "") in ("grados", "º", "°") else v
    m = re.search(r"(?:porticos|pilares|vanos)\s*(?:cada|a)\s*" + _NUM + _UNI, t)
    if m:
        P["separacion_mm"] = _a_m(_num(m.group(1)), m.group(2)) * 1000
    if re.search(r"ambos lados|dos lados|los dos lados|doble barandilla", t):
        P["lados_barandilla"] = 2
    if re.search(r"uso restringido|privad", t):
        P["uso_restringido"] = True
    if not tipo:
        supuestos.append("No he reconocido el tipo de estructura: tomo un porche. Indica nave, entreplanta, escalera, "
                         "barandilla, pasarela, pérgola o porche.")
        tipo = "porche"
    defecto = M.TIPOS[tipo][2]
    nombres = {"largo": "largo", "ancho": "ancho", "alto": "alto"}
    for k in ("largo", "ancho", "alto"):
        if k not in P and k in defecto and not (tipo == "barandilla" and k in ("ancho", "alto")) \
                and not (tipo == "escalera" and k == "largo"):
            supuestos.append(f"Sin {nombres[k]}: tomo {str(defecto[k]).replace('.', ',')} m.")
    if "acero" not in P:
        supuestos.append("Acero S275JR (el más habitual en perfiles laminados).")
    if "acabado" not in P:
        supuestos.append("Acabado galvanizado en caliente.")
    if "altitud" not in P and tipo in ("porche", "pergola", "nave"):
        supuestos.append("Altitud de 200 m y zona de viento B: dímelas si la obra está más alta o en la costa.")
    return tipo, P, supuestos


# ---------------------------------------------------------------------------------------------- despiece
def despiece(e: M.Estructura) -> list[dict]:
    grupos: dict[str, dict] = {}
    for b in e.barras:
        g = grupos.get(b.marca)
        if not g:
            lon = round(b.longitud)
            g = grupos[b.marca] = {"marca": b.marca, "rol": b.rol.replace("jacena", "jácena").replace("peldano", "peldaño")
                                   .replace("travesano", "travesaño").capitalize(), "perfil": b.perfil.nombre,
                                   "longitud_mm": lon, "cantidad": 0, "kg_ud": round(b.perfil.kg_m * lon / 1000, 2),
                                   "pintura_m2_ud": round(b.perfil.perimetro / 1000 * lon / 1000, 3)}
        g["cantidad"] += 1
    orden = list(M.ROLES)
    filas = sorted(grupos.values(), key=lambda g: (orden.index(g["rol"].lower().replace("á", "a").replace("ñ", "n"))
                                                   if g["rol"].lower().replace("á", "a").replace("ñ", "n") in orden else 99,
                                                   g["marca"]))
    for f in filas:
        f["kg_total"] = round(f["kg_ud"] * f["cantidad"], 2)
        f["pintura_m2"] = round(f.pop("pintura_m2_ud") * f["cantidad"], 2)
    return filas


def placas(e: M.Estructura) -> list[dict]:
    g: dict[str, dict] = {}
    for p in e.placas:
        f = g.setdefault(p.marca, {"marca": p.marca, "rol": p.rol, "a": int(p.a), "b": int(p.b), "t": int(p.t),
                                   "cantidad": 0, "kg_ud": round(p.kg, 2), "pernos": p.pernos})
        f["cantidad"] += 1
    for f in g.values():
        f["kg_total"] = round(f["kg_ud"] * f["cantidad"], 2)
    return list(g.values())


def largo_comercial(p: C.Perfil) -> int:
    return 12000 if p.serie in ("IPE", "HEA", "HEB", "UPN") and p.h >= 140 else 6000


def lista_corte(e: M.Estructura, sierra: float = 3.0) -> list[dict]:
    """Optimiza el corte de cada perfil en barras comerciales (primer ajuste decreciente)."""
    por_perfil: dict[str, list] = {}
    perf: dict[str, C.Perfil] = {}
    for b in e.barras:
        por_perfil.setdefault(b.perfil.nombre, []).append((round(b.longitud), b.marca))
        perf[b.perfil.nombre] = b.perfil
    out = []
    for nombre, piezas in por_perfil.items():
        p = perf[nombre]
        stock = largo_comercial(p)
        barras: list[dict] = []
        empalmes = 0
        for lon, marca in sorted(piezas, reverse=True):
            while lon > stock:                               # pieza mas larga que la barra: empalme a tope
                barras.append({"piezas": [{"marca": marca, "mm": stock}], "libre": 0})
                lon -= stock
                empalmes += 1
            sitio = next((x for x in barras if x["libre"] >= lon), None)
            if not sitio:
                sitio = {"piezas": [], "libre": stock}
                barras.append(sitio)
            sitio["piezas"].append({"marca": marca, "mm": lon})
            sitio["libre"] -= lon + sierra
        util = sum(lon for lon, _ in piezas)
        for x in barras:
            x["sobra_mm"] = max(0, int(x.pop("libre")))
        kg = len(barras) * stock / 1000 * p.kg_m
        out.append({"perfil": nombre, "piezas": len(piezas), "barras": len(barras), "largo_barra_mm": stock,
                    "metros_utiles": round(util / 1000, 2), "aprovechamiento_pct": round(100 * util / (len(barras) * stock), 1),
                    "kg_compra": round(kg, 1), "empalmes": empalmes, "plan": barras})
    return sorted(out, key=lambda c: -c["kg_compra"])


def soldaduras(e: M.Estructura) -> tuple[list[dict], dict]:
    filas = []
    total_m, total_kg = 0.0, 0.0
    for u in e.uniones:
        if not u.cantidad:
            continue
        a = u.soldadura["garganta_mm"]
        lon_m = u.cantidad * u.longitud_mm / 1000
        kg = lon_m * a * a * 7.85e-3 * 1.15                  # metal depositado (a^2) + perdidas
        total_m += lon_m
        total_kg += kg
        filas.append({"union": u.nombre, "cantidad": u.cantidad, "garganta_mm": a,
                      "simbolo": f"a{C._fmt(a)} ◺{' ○' if u.todo_alrededor else ''} 135",
                      "todo_alrededor": u.todo_alrededor, "cordon_mm_ud": u.longitud_mm, "metros": round(lon_m, 2),
                      "hilo_kg": round(kg, 2), "nota": u.nota, **({"aviso": u.soldadura["aviso"]} if "aviso" in u.soldadura else {})})
    return filas, {"metros_cordon": round(total_m, 1), "hilo_kg": round(total_kg, 1)}


# ---------------------------------------------------------------------------------------------- proyecto completo
def _carpeta() -> Path:
    return paths.sub("outputs", "herrero")


def _id(tipo: str) -> str:
    return time.strftime("%Y%m%d-%H%M%S") + f"-{tipo}-{int(time.time() * 1000) % 1000:03d}"


def proyecto(encargo: str = "", parametros: dict | None = None, services=None, imagen_ia: bool = True,
             imagen: bool = True) -> dict:
    """Calcula y dibuja una estructura y guarda todo en outputs/herrero/<id>/. ``parametros`` (opcional) manda sobre
    lo que se lee en el encargo (tipo, largo, ancho, alto en metros, acero, acabado, cubierta, uso...)."""
    t0 = time.time()
    tipo, P, supuestos = interpretar(encargo)
    extra = {k: v for k, v in (parametros or {}).items() if v not in (None, "", "auto")}
    if extra.get("tipo"):
        if extra["tipo"] != tipo and encargo:
            supuestos = [s for s in supuestos if not s.startswith("No he reconocido")]
        tipo = extra.pop("tipo")
    if tipo == "barandilla" and "alto" in extra:
        extra["alto_dado"] = True
    P.update(extra)
    e = M.construir(tipo, P)
    pid = _id(tipo)
    carpeta = _carpeta() / pid
    carpeta.mkdir(parents=True, exist_ok=True)
    filas, chapas = despiece(e), placas(e)
    corte = lista_corte(e)
    sold, sold_tot = soldaduras(e)
    kg_perfiles = sum(f["kg_total"] for f in filas)
    kg_chapas = sum(f["kg_total"] for f in chapas)
    pintura = sum(f["pintura_m2"] for f in filas) + sum(2 * f["a"] * f["b"] * f["cantidad"] / 1e6 for f in chapas)
    url = f"/outputs/herrero/{pid}"
    # delineante: plano de montaje (SVG) e isometrica, y el PDF de 3 hojas
    h1 = D.plano_montaje(e, pid)
    (carpeta / "plano-montaje.svg").write_text(h1.svg(f"{e.titulo} - plano de montaje"), encoding="utf-8")
    (carpeta / "isometrica.svg").write_text(D.isometrica_svg(e), encoding="utf-8")
    h2 = D.hoja_isometrica(e, filas, pid)
    h3 = D.hoja_listas(e, filas, corte, chapas, e.comprobaciones, pid)
    (carpeta / "proyecto.pdf").write_bytes(D.pdf([h1, h2, h3], f"{e.titulo} - Oficina Herrero"))
    buf = io.StringIO()
    w = csv.writer(buf, delimiter=";")
    w.writerow(["Marca", "Pieza", "Cantidad", "Perfil", "Longitud (mm)", "kg/ud", "kg total", "Pintura (m2)"])
    for f in filas:
        w.writerow([f["marca"], f["rol"], f["cantidad"], f["perfil"], f["longitud_mm"], f"{f['kg_ud']:.2f}".replace(".", ","),
                    f"{f['kg_total']:.2f}".replace(".", ","), f"{f['pintura_m2']:.2f}".replace(".", ",")])
    for f in chapas:
        w.writerow([f["marca"], f["rol"], f["cantidad"], f"Chapa {f['a']}x{f['b']}x{f['t']}", "", f"{f['kg_ud']:.2f}".replace(".", ","),
                    f"{f['kg_total']:.2f}".replace(".", ","), ""])
    (carpeta / "despiece.csv").write_text(buf.getvalue(), encoding="utf-8-sig")
    fotos = {}
    if imagen:
        fotos = _fotos(e, carpeta, url, services, imagen_ia)
    exc = C.clase_ejecucion(tipo, e.parametros["uso"])
    puestos = {
        "recepcion": {"titulo": "Recepción", "texto": f"Encargo: {e.titulo.lower()} de {D._dims(e)}.",
                      "supuestos": supuestos},
        "calculista": {"titulo": "Calculista", "texto": e.puestos.get("calculista", "") + " " +
                       ("Todas las comprobaciones cumplen." if all(c["cumple"] for c in e.comprobaciones)
                        else "Hay comprobaciones que NO cumplen: revisa los avisos.")},
        "despiece": {"titulo": "Despiece", "texto": f"{sum(f['cantidad'] for f in filas)} piezas en {len(filas)} marcas y "
                     f"{sum(f['cantidad'] for f in chapas)} chapas; {kg_perfiles + kg_chapas:.0f} kg de acero; "
                     f"{sum(c['barras'] for c in corte)} barras comerciales a comprar."},
        "delineante": {"titulo": "Delineante", "texto": "Plano de montaje A3 con alzados, planta, cotas y marcas; "
                       "isométrica; PDF de 3 hojas (montaje, isométrica y despiece, lista de corte y comprobaciones)."},
        "soldador": {"titulo": "Soldador", "texto": f"{len(sold)} tipos de unión, {sold_tot['metros_cordon']} m de cordón "
                     f"y unos {sold_tot['hilo_kg']} kg de hilo G3Si1 Ø1,0 con gas M21 (Ar + 18 % CO₂), proceso 135. "
                     f"Clase {exc}: soldadores cualificados UNE-EN ISO 9606-1, WPS respaldadas por WPQR (ISO 15614-1) "
                     f"e inspección visual del 100 % de los cordones" + (" y END según EN 1090-2 tabla 24." if exc != "EXC1" else ".")},
        "montaje": {"titulo": "Montaje", "texto": _montaje(e)},
        "fotografia": {"titulo": "Fotografía", "texto": fotos.get("nota", "Sin imagen.")},
    }
    resultado = {
        "id": pid, "creado": time.strftime("%Y-%m-%d %H:%M:%S"), "encargo": encargo, "tipo": tipo, "titulo": e.titulo,
        "dimensiones": D._dims(e), "parametros": {k: v for k, v in e.parametros.items() if not k.startswith("_")},
        "cargas": e.cargas.publico() if e.cargas else {}, "comprobaciones": e.comprobaciones, "cumple": all(c["cumple"] for c in e.comprobaciones),
        "despiece": filas, "placas": chapas, "lista_corte": corte, "soldaduras": sold, "soldadura_total": sold_tot,
        "totales": {"kg_perfiles": round(kg_perfiles, 1), "kg_chapas": round(kg_chapas, 1),
                    "kg_total": round(kg_perfiles + kg_chapas, 1), "piezas": sum(f["cantidad"] for f in filas),
                    "pintura_m2": round(pintura, 1), "barras_comerciales": sum(c["barras"] for c in corte)},
        "clase_ejecucion": exc, "normas": e.normas, "avisos": e.avisos, "puestos": puestos, "aviso": AVISO,
        "ficheros": {"plano_svg": f"{url}/plano-montaje.svg", "isometrica_svg": f"{url}/isometrica.svg",
                     "pdf": f"{url}/proyecto.pdf", "csv": f"{url}/despiece.csv", **fotos.get("ficheros", {})},
        "geometria": {"barras": [{"marca": b.marca, "rol": b.rol, "perfil": b.perfil.nombre, "h": b.perfil.h,
                                  "b": b.perfil.b, "p1": [round(v) for v in b.p1], "p2": [round(v) for v in b.p2]}
                                 for b in e.barras],
                      "placas": [{"marca": p.marca, "a": p.a, "b": p.b, "t": p.t, "centro": [round(v) for v in p.centro]}
                                 for p in e.placas]},
        "segundos": round(time.time() - t0, 2),
    }
    (carpeta / "proyecto.json").write_text(json.dumps(resultado, ensure_ascii=False, indent=1), encoding="utf-8")
    return resultado


def _montaje(e: M.Estructura) -> str:
    pasos = {"escalera": ["Replanteo y taladros de los anclajes", "Presentar las zancas y nivelar", "Fijar placas",
                          "Peldaños de abajo arriba", "Barandilla", "Retoques de protección"],
             "barandilla": ["Replanteo de montantes", "Taladros y anclaje químico (respetar el curado)",
                            "Presentar tramos a plomo", "Fijar y retocar la protección"]}.get(e.tipo, [
        "Replanteo y pernos de anclaje (plantilla)", "Pilares a plomo sobre tuercas de nivelación",
        "Vigas o pórticos y apriete provisional", "Arriostramientos y comprobación de escuadra",
        "Correas, viguetas o travesaños", "Apriete definitivo y relleno de mortero sin retracción bajo las placas",
        "Cubierta, suelo y barandillas", "Retoques de galvanizado (pintura rica en zinc) o de pintura"])
    return " → ".join(pasos) + "."


def _fotos(e: M.Estructura, carpeta: Path, url: str, services, imagen_ia: bool) -> dict:
    from . import herrero_render as R
    t0 = time.time()
    img = R.render(e)
    ruta = carpeta / "render.jpg"
    img.save(ruta, quality=90)
    out = {"ficheros": {"render": f"{url}/render.jpg"},
           "nota": f"Render de la geometría calculada con luz solar y sombras ({time.time() - t0:.1f} s)."}
    if imagen_ia and services is not None:
        try:
            r = services.images.refinar(ruta, R.prompt_foto(e), 0.42, "cartoon, drawing, text, watermark, people deformed",
                                        ancho=768, alto=512)
            if r:
                destino = carpeta / "foto-ia.png"
                Path(r["path"]).replace(destino)
                out["ficheros"]["foto_ia"] = f"{url}/foto-ia.png"
                out["nota"] += f" Versión fotográfica con {r['engine']} y {r['model']} a partir del render."
            else:
                out["nota"] += " Para una versión fotográfica con IA descarga un modelo de imagen en Modelos."
        except Exception as ex:  # noqa: BLE001
            L.warning("Imagen con IA no disponible: %s", ex)
            out["nota"] += f" La versión con IA falló ({str(ex)[:120]}); queda el render."
    return out


def proyectos(limite: int = 30) -> list[dict]:
    out = []
    for f in sorted(_carpeta().glob("*/proyecto.json"), reverse=True)[:limite]:
        try:
            d = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        out.append({k: d.get(k) for k in ("id", "creado", "encargo", "tipo", "titulo", "dimensiones", "cumple")} |
                   {"kg_total": d.get("totales", {}).get("kg_total"), "render": d.get("ficheros", {}).get("render")})
    return out


def cargar(pid: str) -> dict:
    if not re.fullmatch(r"[\w.-]+", pid or ""):
        raise KeyError(pid)
    f = _carpeta() / pid / "proyecto.json"
    if not f.exists():
        raise KeyError(pid)
    return json.loads(f.read_text(encoding="utf-8"))


def borrar(pid: str) -> bool:
    import shutil
    if not re.fullmatch(r"[\w.-]+", pid or ""):
        raise KeyError(pid)
    d = _carpeta() / pid
    if not d.is_dir():
        raise KeyError(pid)
    shutil.rmtree(d)
    return True


def resumen(r: dict) -> str:
    """Texto corto para el agente (sin las tablas largas)."""
    lineas = [f"{r['titulo']} · {r['dimensiones']} · acero {r['parametros']['acero']} · {r['clase_ejecucion']}"]
    if r["cargas"].get("elu_kn_m2"):
        lineas.append(f"Cargas ELU {r['cargas']['elu_kn_m2']} kN/m² (ELS {r['cargas']['els_kn_m2']})")
    lineas += [n for n in r["cargas"].get("notas", [])]
    for c in r["comprobaciones"]:
        lineas.append(f"- {c['elemento']}: {c['perfil']} ({round(c['max'] * 100)} % {'cumple' if c['cumple'] else 'NO CUMPLE'})")
    t = r["totales"]
    lineas.append(f"Peso {t['kg_total']} kg ({t['piezas']} piezas), pintura {t['pintura_m2']} m², "
                  f"{t['barras_comerciales']} barras comerciales, {r['soldadura_total']['metros_cordon']} m de cordón.")
    for a in r["avisos"]:
        lineas.append("AVISO: " + a)
    return "\n".join(lineas)
