#!/usr/bin/env python3
"""Grafo multicapa de conocimiento de la Wikipedia en español, como ejemplo para importar en Nexus-Office.

    python3 tools/wikipedia_grafo.py [--salida ejemplos/grafo-wikipedia-es] [--articulos 1500]

Solo usa la biblioteca estándar y las API públicas de es.wikipedia.org y wikidata.org (hace falta internet; se lanza
desde la GitHub Action wikipedia-grafo.yml). Capas:
  1-temas      categorías de la Wikipedia, desde unas raíces temáticas (ciencia, historia, arte...), con su jerarquía
  2-articulos  artículos de esas categorías, con el resumen de su introducción como nota
  3-enlaces    enlaces entre esos artículos (solo los que quedan dentro del grafo)
  4-wikidata   hechos de Wikidata de cada artículo: qué es, país, lugar, autor, fechas, población...
Cada nodo y cada arista lleva su capa (`capa`), de modo que se puede filtrar en Gephi o al preguntar a los agentes.
Salida: grafo.graphml (Gephi, yEd, Nexus-Office), grafo.json (node-link de D3/NetworkX) y LEEME.md con las cifras.
Licencia de los textos: CC BY-SA 4.0 (Wikipedia); datos de Wikidata: CC0.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from collections import deque
from pathlib import Path
from xml.sax.saxutils import escape

WIKI = "https://es.wikipedia.org/w/api.php"
WIKIDATA = "https://www.wikidata.org/w/api.php"
UA = "Nexus-Office-grafo/2.1 (https://github.com/JosebaMuro/Nexus-AI; ejemplo de grafo de conocimiento)"

RAICES = ["Ciencia", "Historia", "Geografía", "Arte", "Tecnología", "Economía", "Filosofía", "Deporte", "Música",
          "Literatura", "Medicina", "Matemáticas", "Biología", "Física", "Química", "Informática", "Derecho",
          "Política", "Gastronomía", "Cine", "Arquitectura", "Astronomía", "Educación", "Religión", "Idiomas",
          "España", "América Latina", "Empresas", "Agricultura", "Medio ambiente"]

# propiedades de Wikidata: entidad -> relación entre nodos; valor -> atributo del artículo
RELACIONES = {"P31": "es_un", "P279": "subclase_de", "P17": "pais", "P131": "situado_en", "P30": "continente",
              "P36": "capital", "P106": "ocupacion", "P27": "nacionalidad", "P19": "nacio_en", "P20": "murio_en",
              "P361": "parte_de", "P50": "autor", "P170": "creador", "P136": "genero", "P495": "pais_de_origen",
              "P69": "estudio_en", "P108": "trabajo_para", "P101": "campo", "P135": "movimiento", "P138": "nombre_por",
              "P61": "descubridor", "P176": "fabricante", "P127": "propietario", "P159": "sede", "P452": "sector",
              "P37": "idioma_oficial", "P38": "moneda", "P1376": "capital_de", "P737": "influido_por"}
ATRIBUTOS = {"P1082": "poblacion", "P2046": "superficie_km2", "P2044": "altitud_m", "P569": "nacimiento",
             "P570": "fallecimiento", "P571": "fundacion", "P577": "publicacion", "P575": "descubrimiento",
             "P585": "fecha", "P2048": "altura_m", "P2067": "masa_kg", "P1128": "empleados", "P856": "web"}


def get(url: str, params: dict, intentos: int = 8) -> dict:
    # maxlag solo con la Wikipedia: Wikidata va con retraso a menudo y para leer no hace falta
    extra = {"maxlag": "5"} if url == WIKI else {}
    q = urllib.parse.urlencode({**params, "format": "json", "formatversion": "2", **extra})
    for n in range(intentos):
        try:
            req = urllib.request.Request(f"{url}?{q}", headers={"User-Agent": UA, "Accept-Encoding": "identity"})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.loads(r.read().decode("utf-8"))
            if data.get("error", {}).get("code") == "maxlag":
                raise RuntimeError("maxlag")
            return data
        except Exception as e:  # noqa: BLE001  red o servidor ocupado: se reintenta con espera creciente
            if n == intentos - 1:
                raise
            print(f"  reintento ({e})", file=sys.stderr)
            time.sleep(min(60, 2 ** n))
    return {}


def query_all(params: dict, limite: int = 100000):
    """Recorre una consulta de la API de MediaWiki siguiendo «continue»."""
    cont: dict = {}
    vistos = 0
    while True:
        data = get(WIKI, {"action": "query", **params, **cont})
        yield data.get("query", {})
        vistos += 1
        if "continue" not in data or vistos >= limite:
            return
        cont = data["continue"]
        time.sleep(0.1)


def lotes(items: list, n: int):
    for i in range(0, len(items), n):
        yield items[i:i + n]


class Grafo:
    def __init__(self):
        self.nodos: dict[str, dict] = {}
        self.aristas: dict[tuple[str, str, str], dict] = {}

    def nodo(self, nid: str, nombre: str, tipo: str, capa: str, **attrs) -> str:
        n = self.nodos.setdefault(nid, {"id": nid, "label": nombre, "type": tipo, "capa": capa})
        n.update({k: v for k, v in attrs.items() if v not in (None, "")})
        return nid

    def arista(self, a: str, b: str, rel: str, capa: str, peso: float = 1.0):
        if a != b and a in self.nodos and b in self.nodos:
            self.aristas.setdefault((a, b, rel), {"source": a, "target": b, "relation": rel, "capa": capa,
                                                  "weight": peso})


def categorias(g: Grafo, raices: list[str], por_categoria: int, maximo: int, profundidad: int) -> list[str]:
    """Capas 1 y 2: árbol de categorías desde las raíces y los artículos de cada una."""
    articulos: list[str] = []
    cola = deque((r, 0, None) for r in raices)
    vistas: set[str] = set()
    cupo = max(1, maximo // max(1, len(raices)))
    por_raiz: dict[str, int] = {}
    while cola and len(articulos) < maximo:
        cat, prof, raiz = cola.popleft()
        if cat in vistas:
            continue
        vistas.add(cat)
        raiz = raiz or cat
        cid = g.nodo(f"cat:{cat}", cat, "categoria", "1-temas", url=f"https://es.wikipedia.org/wiki/Categoría:{cat}")
        print(f"[categoría] {cat} (nivel {prof})", file=sys.stderr)
        paginas, subcats = [], []
        for q in query_all({"list": "categorymembers", "cmtitle": f"Categoría:{cat}", "cmtype": "page|subcat",
                            "cmlimit": "500"}, limite=2):
            for m in q.get("categorymembers", []):
                if m["ns"] == 14:
                    subcats.append(m["title"].split(":", 1)[1])
                elif m["ns"] == 0:
                    paginas.append(m["title"])
        for t in paginas[:por_categoria]:
            if por_raiz.get(raiz, 0) >= cupo or len(articulos) >= maximo:
                break
            if f"art:{t}" not in g.nodos:
                articulos.append(t)
                por_raiz[raiz] = por_raiz.get(raiz, 0) + 1
            g.nodo(f"art:{t}", t, "articulo", "2-articulos", url="https://es.wikipedia.org/wiki/" + t.replace(" ", "_"))
            g.arista(f"art:{t}", cid, "pertenece_a", "1-temas")
        if prof < profundidad:
            for s in subcats[:12]:
                g.nodo(f"cat:{s}", s, "categoria", "1-temas")
                g.arista(f"cat:{s}", cid, "subcategoria_de", "1-temas")
                cola.append((s, prof + 1, raiz))
    return articulos


def detalles(g: Grafo, articulos: list[str]) -> dict[str, str]:
    """Capa 2: resumen e identificador de Wikidata de cada artículo. Devuelve {título: Qid}."""
    qids: dict[str, str] = {}
    for n, lote in enumerate(lotes(articulos, 20)):
        print(f"[artículos] {n * 20 + len(lote)}/{len(articulos)}", file=sys.stderr)
        for q in query_all({"prop": "extracts|pageprops|description", "titles": "|".join(lote), "exintro": "1",
                            "explaintext": "1", "exsentences": "3", "exlimit": "20", "ppprop": "wikibase_item",
                            "redirects": "1"}, limite=5):
            alias = {r["from"]: r["to"] for r in q.get("redirects", [])}
            for orig, dest in alias.items():   # una redireccion: el nodo pasa a llamarse como el articulo real
                if f"art:{orig}" in g.nodos and f"art:{dest}" not in g.nodos:
                    g.nodos[f"art:{orig}"]["label"] = dest
            for p in q.get("pages", []):
                if p.get("missing"):
                    continue
                t = p["title"]
                nid = f"art:{t}" if f"art:{t}" in g.nodos else next(
                    (f"art:{o}" for o, d in alias.items() if d == t and f"art:{o}" in g.nodos), None)
                if not nid:
                    continue
                g.nodo(nid, t, "articulo", "2-articulos", resumen=(p.get("extract") or "").strip()[:900],
                       descripcion=p.get("description", ""))
                q_id = p.get("pageprops", {}).get("wikibase_item")
                if q_id:
                    qids[nid] = q_id
                    g.nodos[nid]["wikidata"] = q_id
        time.sleep(0.1)
    return qids


def enlaces(g: Grafo, articulos: list[str]):
    """Capa 3: enlaces entre artículos del grafo."""
    por_titulo = {g.nodos[f"art:{t}"]["label"]: f"art:{t}" for t in articulos if f"art:{t}" in g.nodos}
    titulos = list(por_titulo)
    for n, lote in enumerate(lotes(titulos, 50)):
        print(f"[enlaces] {n * 50 + len(lote)}/{len(titulos)}", file=sys.stderr)
        for q in query_all({"prop": "links", "titles": "|".join(lote), "plnamespace": "0", "pllimit": "max"},
                           limite=40):
            for p in q.get("pages", []):
                a = por_titulo.get(p["title"])
                for l in p.get("links", []):
                    b = por_titulo.get(l["title"])
                    if a and b:
                        g.arista(a, b, "enlaza_a", "3-enlaces", 0.5)
        time.sleep(0.1)


def _valor(dv: dict):
    v = dv.get("value")
    t = dv.get("type")
    if t == "quantity":
        x = float(v["amount"])
        return int(x) if x.is_integer() else round(x, 3)
    if t == "time":
        s = v["time"].lstrip("+")
        prec = v.get("precision", 11)
        return s[:4] if prec <= 9 else s[:7] if prec == 10 else s[:10]
    if t == "string":
        return v
    if t == "monolingualtext":
        return v.get("text")
    return None


def wikidata(g: Grafo, qids: dict[str, str]):
    """Capa 4: hechos de Wikidata (relaciones con otras entidades y atributos)."""
    por_q = {q: nid for nid, q in qids.items()}
    hechos: list[tuple[str, str, str]] = []
    destinos: set[str] = set()
    lista = list(por_q)
    for n, lote in enumerate(lotes(lista, 50)):
        print(f"[wikidata] {n * 50 + len(lote)}/{len(lista)}", file=sys.stderr)
        data = get(WIKIDATA, {"action": "wbgetentities", "ids": "|".join(lote), "props": "claims"})
        for q, ent in data.get("entities", {}).items():
            nid = por_q.get(q)
            claims = ent.get("claims", {})
            if not nid:
                continue
            for p, rel in RELACIONES.items():
                for c in claims.get(p, [])[:6]:
                    dv = c.get("mainsnak", {}).get("datavalue", {})
                    if dv.get("type") == "wikibase-entityid":
                        hechos.append((nid, rel, dv["value"]["id"]))
                        destinos.add(dv["value"]["id"])
            for p, attr in ATRIBUTOS.items():
                cs = [c for c in claims.get(p, []) if c.get("rank") != "deprecated"]
                pref = [c for c in cs if c.get("rank") == "preferred"] or cs
                if pref:
                    val = _valor(pref[-1].get("mainsnak", {}).get("datavalue", {}))
                    if val is not None:
                        g.nodos[nid][attr] = val
        time.sleep(0.2)
    etiquetas: dict[str, tuple[str, str]] = {}
    pendientes = [q for q in destinos if q not in por_q]
    for n, lote in enumerate(lotes(pendientes, 50)):
        print(f"[wikidata etiquetas] {n * 50 + len(lote)}/{len(pendientes)}", file=sys.stderr)
        data = get(WIKIDATA, {"action": "wbgetentities", "ids": "|".join(lote), "props": "labels|descriptions",
                              "languages": "es", "languagefallback": "1"})
        for q, ent in data.get("entities", {}).items():
            lab = ent.get("labels", {}).get("es", {}).get("value")
            if lab:
                etiquetas[q] = (lab, ent.get("descriptions", {}).get("es", {}).get("value", ""))
        time.sleep(0.2)
    for nid, rel, q in hechos:
        if q in por_q:
            g.arista(nid, por_q[q], rel, "4-wikidata")
        elif q in etiquetas:
            lab, desc = etiquetas[q]
            dest = f"art:{lab}" if f"art:{lab}" in g.nodos else g.nodo(
                f"wd:{q}", lab, "concepto", "4-wikidata", wikidata=q, descripcion=desc,
                url=f"https://www.wikidata.org/wiki/{q}")
            g.arista(nid, dest, rel, "4-wikidata")


def podar(g: Grafo):
    """Quita los conceptos de Wikidata que solo toca un artículo y las categorías vacías: el ejemplo queda legible."""
    grado: dict[str, int] = {}
    for a, b, _ in g.aristas:
        grado[a] = grado.get(a, 0) + 1
        grado[b] = grado.get(b, 0) + 1
    fuera = {n for n, d in g.nodos.items() if (d["type"] == "concepto" and grado.get(n, 0) < 2)
             or (d["type"] == "categoria" and grado.get(n, 0) == 0)}
    for n in fuera:
        del g.nodos[n]
    g.aristas = {k: v for k, v in g.aristas.items() if k[0] in g.nodos and k[1] in g.nodos}


def guardar(g: Grafo, salida: Path, meta: dict):
    salida.mkdir(parents=True, exist_ok=True)
    claves = sorted({k for n in g.nodos.values() for k in n} - {"id"})
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<graphml xmlns="http://graphml.graphdrawing.org/xmlns">',
           f'  <!-- {escape(meta["fuente"])} -->']
    tipos = {k: "string" for k in claves}
    for k in claves:
        vals = [n[k] for n in g.nodos.values() if k in n]
        if vals and all(isinstance(v, int) and not isinstance(v, bool) for v in vals):
            tipos[k] = "long"
        elif vals and all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in vals):
            tipos[k] = "double"
    for k in claves:
        out.append(f'  <key id="{k}" for="node" attr.name="{k}" attr.type="{tipos[k]}"/>')
    out += ['  <key id="relation" for="edge" attr.name="relation" attr.type="string"/>',
            '  <key id="ecapa" for="edge" attr.name="capa" attr.type="string"/>',
            '  <key id="weight" for="edge" attr.name="weight" attr.type="double"/>',
            '  <graph id="wikipedia-es" edgedefault="directed">']
    for n in g.nodos.values():
        out.append(f'    <node id="{escape(n["id"], {chr(34): "&quot;"})}">')
        for k in claves:
            if k in n:
                out.append(f'      <data key="{k}">{escape(str(n[k]))}</data>')
        out.append("    </node>")
    for e in g.aristas.values():
        q = {chr(34): "&quot;"}
        out.append(f'    <edge source="{escape(e["source"], q)}" target="{escape(e["target"], q)}">'
                   f'<data key="relation">{e["relation"]}</data><data key="ecapa">{e["capa"]}</data>'
                   f'<data key="weight">{e["weight"]}</data></edge>')
    out += ["  </graph>", "</graphml>"]
    (salida / "grafo.graphml").write_text("\n".join(out) + "\n", encoding="utf-8")
    nl = {"directed": True, "multigraph": False, "graph": meta, "nodes": list(g.nodos.values()),
          "links": list(g.aristas.values())}
    (salida / "grafo.json").write_text(json.dumps(nl, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    capas_n: dict[str, int] = {}
    capas_e: dict[str, int] = {}
    rels: dict[str, int] = {}
    for n in g.nodos.values():
        capas_n[n["capa"]] = capas_n.get(n["capa"], 0) + 1
    for e in g.aristas.values():
        capas_e[e["capa"]] = capas_e.get(e["capa"], 0) + 1
        rels[e["relation"]] = rels.get(e["relation"], 0) + 1
    top = sorted(rels.items(), key=lambda x: -x[1])[:15]
    lee = [f"# Grafo multicapa de la Wikipedia en español", "",
           f"Generado el {meta['fecha']} con `tools/wikipedia_grafo.py` desde la API pública de es.wikipedia.org y "
           "wikidata.org. Es un ejemplo de grafo de conocimiento para importar en Nexus-Office "
           "(**Ajustes > Gestor de conocimiento > Importar / exportar**, o `graph_import` desde el chat).", "",
           f"- **{len(g.nodos)} nodos** y **{len(g.aristas)} relaciones**.",
           f"- Raíces temáticas: {', '.join(meta['raices'])}.", "",
           "| Capa | Qué contiene | Nodos | Relaciones |", "|---|---|---|---|"]
    desc = {"1-temas": "categorías de la Wikipedia y su jerarquía; cada artículo `pertenece_a` sus categorías",
            "2-articulos": "artículos, con el resumen de su introducción y su descripción",
            "3-enlaces": "enlaces `enlaza_a` entre artículos del grafo",
            "4-wikidata": "hechos de Wikidata: `es_un`, `pais`, `situado_en`, `autor`... y atributos como población o fechas"}
    for c in ("1-temas", "2-articulos", "3-enlaces", "4-wikidata"):
        lee.append(f"| `{c}` | {desc[c]} | {capas_n.get(c, 0)} | {capas_e.get(c, 0)} |")
    lee += ["", "Relaciones más frecuentes: " + ", ".join(f"`{r}` ({n})" for r, n in top) + ".", "",
            "## Ficheros", "- `grafo.graphml`: GraphML con los atributos `capa`, `type`, `resumen`, `url`, `wikidata`... "
            "Se abre en Gephi o yEd y se importa en Nexus-Office.",
            "- `grafo.json`: el mismo grafo en JSON node-link (D3, NetworkX, sigma.js).", "",
            "## Cómo importarlo",
            "```", "nexus-office grafo importar grafo.graphml --en auto        # edición C++",
            "python -m nexus grafo importar grafo.graphml --en auto      # edición Python", "```",
            "Con `--en auto`, lo común va al grafo general y lo especializado al agente que corresponde. "
            "Los atributos de cada nodo (resumen, población, fechas) quedan en su nota y los agentes los usan al responder.",
            "", "## Licencia",
            "Textos de la Wikipedia en español bajo [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/deed.es) "
            "(autores: colaboradores de la Wikipedia; cada nodo lleva su `url`). Datos de Wikidata bajo CC0.", ""]
    (salida / "LEEME.md").write_text("\n".join(lee), encoding="utf-8")
    print(f"[ok] {len(g.nodos)} nodos, {len(g.aristas)} relaciones en {salida}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--salida", default="ejemplos/grafo-wikipedia-es")
    ap.add_argument("--articulos", type=int, default=1500, help="número máximo de artículos")
    ap.add_argument("--por-categoria", type=int, default=12)
    ap.add_argument("--profundidad", type=int, default=1, help="niveles de subcategorías bajo cada raíz")
    ap.add_argument("--raices", default=",".join(RAICES))
    a = ap.parse_args(argv)
    raices = [r.strip() for r in a.raices.split(",") if r.strip()]
    g = Grafo()
    arts = categorias(g, raices, a.por_categoria, a.articulos, a.profundidad)
    qids = detalles(g, arts)
    enlaces(g, arts)
    wikidata(g, qids)
    podar(g)
    meta = {"name": "Wikipedia en español (multicapa)", "fecha": time.strftime("%Y-%m-%d"), "raices": raices,
            "fuente": "es.wikipedia.org (CC BY-SA 4.0) y wikidata.org (CC0)",
            "capas": ["1-temas", "2-articulos", "3-enlaces", "4-wikidata"]}
    guardar(g, Path(a.salida), meta)
    return 0


if __name__ == "__main__":
    sys.exit(main())
