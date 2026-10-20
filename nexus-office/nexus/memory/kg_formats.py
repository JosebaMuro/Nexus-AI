"""Lectores de grafos de conocimiento en cualquier formato habitual, sin red y sin dependencias nuevas.

El formato se detecta por el contenido (la extension solo orienta) y todo se normaliza al esquema interno
``{"format", "nodes": [{id, name, type, category, note, scope?, weight?}], "edges": [{src, dst, rel, weight}]}``.
Los atributos de cada nodo (propiedades de Neo4j, literales RDF, columnas de una tabla, ``<data>`` de GraphML...)
se guardan en su nota como ``clave: valor``; las relaciones solo unen entidades.

Formatos (ver ``FORMATS``): JSON propio de Nexus-Office, node-link (D3, vis.js, NetworkX, sigma), Cytoscape (.cyjs y
CX), JSON Graph Format, JSON-LD, GraphSON (TinkerPop/JanusGraph), Neo4j (Browser, HTTP, APOC y CSV de neo4j-admin),
Arrows.app, Obsidian Canvas, XMind, listas de tripletas, listas de adyacencia, arboles (``children``) y, como ultimo
recurso, cualquier JSON/YAML/TOML con objetos con nombre; JSON Lines; CSV/TSV y hojas Excel/ODS (aristas, nodos,
matriz de adyacencia, ConceptNet); GraphML (tambien yEd), GEXF, XGMML, DGML, GXL, RDF/XML y OWL, FreeMind/Freeplane,
OPML y cualquier XML con nodos y enlaces; Graphviz DOT, GML, Pajek, GDF, UCINET DL, TGF, SIF, LGL, NCOL y listas de
aristas; RDF N-Triples, N-Quads, Turtle, TriG y N3; Cypher (CREATE/MERGE/MATCH, exportaciones de APOC); Mermaid
(flowchart, graph, erDiagram, classDiagram, mindmap) y PlantUML; texto con flechas ``A -> B``, ``A: B, C`` y
esquemas con sangria; notas Markdown con ``[[enlaces]]``; bases SQLite (tambien otro Nexus-Office) y cualquiera de
ellos comprimido (gz, bz2, xz) o varios dentro de un ZIP o TAR (p. ej. nodes.csv + edges.csv).
"""
from __future__ import annotations

import bz2
import csv
import gzip
import io
import json
import lzma
import re
import sqlite3
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath

MAX_NODES = 200_000
MAX_BYTES = 512 * 1024 * 1024   # descomprimido: protege de bombas de compresion
RDF_NS = "http://www.w3.org/1999/02/22-rdf-syntax-ns#"


class GraphFormatError(ValueError):
    pass


# ------------------------------------------------------------------ nombres y atributos
def _label(value) -> str:
    """Nombre legible de un id: el final de una IRI, sin guiones bajos."""
    s = str(value if value is not None else "").strip()
    if s.startswith("<") and s.endswith(">"):
        s = s[1:-1]
    if s.startswith("#") and len(s) > 1:   # rdf:about="#Perro"
        s = s[1:]
    if re.match(r"^[a-z][\w+.-]*://", s, re.I) or s.startswith("urn:"):
        s = re.split(r"[#/:]", s.rstrip("/#"))[-1] or s
    return re.sub(r"[_]+", " ", s).strip()[:60]


def _rel(value) -> str:
    s = _label(value) or "relacionado_con"
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", s)   # camelCase -> snake
    return re.sub(r"[^\w]+", "_", s.lower()).strip("_")[:30] or "relacionado_con"


def _local(key) -> str:
    """Parte final de una clave: rdfs:label -> label, http://schema.org/name -> name."""
    return re.split(r"[#/:]", str(key).rstrip("/#"))[-1].lower()


NAME_KEYS = ("name", "nombre", "title", "titulo", "título", "label", "etiqueta", "caption", "displayname",
             "display_name", "fullname", "full_name", "preflabel", "headline", "text", "texto", "value", "username")
NOTE_KEYS = ("description", "descripcion", "descripción", "note", "nota", "comment", "comentario", "summary",
             "resumen", "definition", "definicion", "abstract")
TYPE_KEYS = ("type", "tipo", "group", "grupo", "kind", "class", "clase")
SKIP_PROPS = {"unique import id", "id", "_id", "@id", "identity", "elementid", "x", "y", "z", "size", "color",
              "colour", "graphics", "shape", "style", "fill", "viz", "position", "pos", "width", "height"}


def _val(v):
    """Desenvuelve valores tipados: JSON-LD ``{"@value"}``, GraphSON ``[{"id", "value"}]``, listas de uno."""
    if isinstance(v, dict):
        if "@value" in v:
            return _val(v["@value"])
        if "value" in v and set(v) <= {"id", "value", "label", "properties", "@type", "key"}:
            return _val(v["value"])
    if isinstance(v, list) and v and all(isinstance(x, dict) and ("value" in x or "@value" in x) for x in v):
        vals = [_val(x) for x in v]
        return vals[0] if len(vals) == 1 else vals
    return v


def _text(v) -> str:
    v = _val(v)
    if isinstance(v, (list, tuple)):
        return ", ".join(_text(x) for x in v if x not in (None, ""))
    if isinstance(v, dict):
        return json.dumps(v, ensure_ascii=False)[:200]
    if isinstance(v, float) and v.is_integer():
        v = int(v)
    return "" if v is None else str(v).strip()


def _name_from(props: dict) -> tuple[str | None, str | None]:
    """(nombre, clave usada) entre las propiedades de un nodo."""
    if not isinstance(props, dict):
        return None, None
    loc = {_local(k): k for k in props}
    for k in NAME_KEYS:
        if k in loc and _text(props[loc[k]]):
            return _text(props[loc[k]]), loc[k]
    return None, None


def _props_note(props: dict, skip=()) -> str:
    skip = {str(s).lower() for s in skip} | SKIP_PROPS
    out = []
    for k, v in (props or {}).items():
        if str(k).lower() in skip or _local(k) in skip or str(k).startswith("@"):
            continue
        t = _text(v)
        if t and len(t) < 300:
            out.append(f"{_label(k) or k}: {t}")
    return "; ".join(out)


def _first(d: dict, *keys, default=None):
    for k in keys:
        if isinstance(d, dict) and d.get(k) not in (None, "", [], {}):
            return d[k]
    return default


def _ref(x):
    """Id de un extremo: un id suelto o un objeto (d3 tras la simulacion, Neo4j, GraphSON)."""
    x = _val(x)
    if isinstance(x, dict):
        return _val(_first(x, "id", "@id", "identity", "elementId", "_id", "key", "name"))
    return x


class _Builder:
    def __init__(self) -> None:
        self.nodes: dict[str, dict] = {}
        self.edges: list[dict] = []

    def node(self, nid, name=None, props: dict | None = None, **extra) -> str:
        nid = _text(nid)
        if not nid:
            nid = _text(name)
        if not nid:
            return ""
        name = _text(name)
        if nid not in self.nodes:
            if len(self.nodes) >= MAX_NODES:
                raise GraphFormatError(f"el grafo tiene mas de {MAX_NODES} nodos")
            self.nodes[nid] = {"id": nid, "name": _label(name or nid), "type": "concepto", "category": "", "note": ""}
        n = self.nodes[nid]
        if name and n["name"] == _label(nid):
            n["name"] = _label(name)
            if len(name) > 60 and name not in n["note"]:   # textos largos (Canvas, mapas mentales): completo en la nota
                extra["note"] = (name + (". " + extra["note"] if extra.get("note") else ""))
        for k, v in extra.items():
            if v in (None, "", [], {}):
                continue
            if k == "weight":
                n[k] = v
            elif k == "type":
                if n["type"] == "concepto":
                    n["type"] = (_text(v[0] if isinstance(v, (list, tuple)) else v).lower()[:30] or "concepto")
            elif k == "note":
                t = _text(v)[:500]
                if t and t not in n["note"]:
                    n["note"] = (n["note"] + ". " + t if n["note"] else t)[:500]
            else:
                n[k] = _text(v)[:500]
        if props:
            t = _props_note(props)
            if t and t not in n["note"]:
                n["note"] = (n["note"] + ". " + t if n["note"] else t)[:500]
        return nid

    def entity(self, nid, props: dict | None = None, name=None, **extra) -> str:
        """Nodo con propiedades: el nombre sale de ellas si no se da (name, title, label...)."""
        props = props or {}
        pname, used = _name_from(props)
        note = _first(props, *[k for k in props if _local(k) in NOTE_KEYS]) if props else None
        skip = {used} | {k for k in props if _local(k) in NOTE_KEYS}
        nid = self.node(nid, name or pname or (f"{_text(extra.get('type'))} {_text(nid)}".strip()
                                                if extra.get("type") and _text(nid).isdigit() else None),
                        **extra)
        if nid:
            self.node(nid, note=note)
            t = _props_note(props, skip)
            n = self.nodes[nid]
            if t and t not in n["note"]:
                n["note"] = (n["note"] + ". " + t if n["note"] else t)[:500]
        return nid

    def edge(self, src, dst, rel=None, weight=1.0) -> None:
        s, d = self.node(src), self.node(dst)
        if not s or not d or s == d:
            return
        try:
            w = float(_val(weight))
        except (TypeError, ValueError):
            w = 1.0
        self.edges.append({"src": s, "dst": d, "rel": _rel(rel), "weight": w})

    def result(self, fmt: str, generic: bool = False) -> dict:
        nodes = [n for n in self.nodes.values() if n["name"]]
        if not nodes:
            raise GraphFormatError("no se ha encontrado ningun nodo")
        out = {"format": fmt, "nodes": nodes, "edges": self.edges}
        if generic:
            out["generic"] = True   # interpretacion de ultimo recurso: dentro de un ZIP no cuenta como grafo
        return out


# ------------------------------------------------------------------ JSON (y YAML/TOML, que dan lo mismo)
NODE_LISTS = ("nodes", "vertices", "entities", "entidades", "nodos", "concepts")
EDGE_LISTS = ("edges", "links", "relationships", "relations", "arcs", "aristas", "relaciones", "enlaces")
SRC_KEYS = ("source", "src", "from", "fromId", "fromNode", "startNode", "start", "outV", "_outV", "head", "u",
            "subject", "origen", "sujeto", "s", "node1", "sourceId")
DST_KEYS = ("target", "dst", "to", "toId", "toNode", "endNode", "end", "inV", "_inV", "tail", "v", "object",
            "destino", "objeto", "o", "node2", "targetId")
REL_KEYS = ("relation", "rel", "label", "_label", "type", "predicate", "relacion", "relación", "interaction", "name",
            "caption", "kind", "i")
NODE_ID = ("id", "@id", "key", "_id", "uid", "identity", "elementId", "name", "label")
NODE_NAME = ("label", "name", "title", "caption", "text", "nombre", "file", "url", "n")


def _edge_rel(e: dict):
    r = _first(e, *REL_KEYS)
    if isinstance(r, str) and r.lower() in ("directed", "undirected", "dirigida", "no dirigida", "edge", "link"):
        r = _first(e, *[k for k in REL_KEYS if k not in ("type", "kind")])
    return r if isinstance(r, (str, int, float)) else None


def _generic_node(b: _Builder, n) -> str:
    if not isinstance(n, dict):
        return b.node(n, n)
    props = dict(n.get("properties") or n.get("attributes") or n.get("data") or n.get("v") or {}) \
        if isinstance(n.get("properties") or n.get("attributes") or n.get("data") or n.get("v"), dict) else {}
    meta = n.get("metadata") if isinstance(n.get("metadata"), dict) else {}
    props.update({k: v for k, v in meta.items() if k not in ("type", "description")})
    nid = _ref(_first(n, *NODE_ID)) or _name_from(props)[0]
    name = _first(n, *NODE_NAME)
    if isinstance(name, dict):
        name = None
    labels = n.get("labels") if isinstance(n.get("labels"), list) else None
    etype = (labels[0] if labels else None) or _first(n, "type", "group", "kind", "_type") or meta.get("type")
    if isinstance(etype, (list, dict)):
        etype = None
    rest = {k: v for k, v in n.items() if k not in NODE_ID + NODE_NAME + ("properties", "attributes", "data", "v",
                                                                          "metadata", "labels", "type", "group",
                                                                          "kind", "_type", "category", "scope",
                                                                          "weight", "children", "adjacency", "outE",
                                                                          "inE", "edges", "links")
            and not isinstance(v, (dict, list))}
    note = _first(n, "description", "note", default=meta.get("description"))
    if n.get("type") == "text" and isinstance(n.get("text"), str):   # Obsidian Canvas: tarjeta de texto
        name, note = n["text"].strip().splitlines()[0].lstrip("# ").strip(), n["text"].strip()
    elif n.get("type") == "file" and n.get("file"):
        name = PurePosixPath(_text(n["file"])).stem
    return b.entity(nid, {**rest, **props}, name=name if name is not None else None, type=etype,
                    category=n.get("category"), scope=n.get("scope"), weight=n.get("weight"), note=note)


def _generic_edges(b: _Builder, edges) -> int:
    n = 0
    for e in edges or []:
        if isinstance(e, dict):
            s, t = _ref(_first(e, *SRC_KEYS)), _ref(_first(e, *DST_KEYS))
            if s is not None and t is not None:
                props = e.get("properties") or e.get("data") or e.get("v") or {}
                rel = _edge_rel(e) or (_edge_rel(props) if isinstance(props, dict) else None)
                b.edge(s, t, rel, e.get("weight", e.get("value", (props or {}).get("weight", 1)
                                                      if isinstance(props, dict) else 1)))
                n += 1
        elif isinstance(e, (list, tuple)) and len(e) >= 2:
            b.edge(e[0], e[-1], e[1] if len(e) > 2 else None)
            n += 1
    return n


def _neo_obj(b: _Builder, o) -> bool:
    """Objetos de Neo4j (Browser, driver, HTTP, APOC): nodos, relaciones y caminos."""
    if not isinstance(o, dict):
        return False
    if "segments" in o:
        for seg in o["segments"]:
            _neo_obj(b, seg.get("start"))
            _neo_obj(b, seg.get("end"))
            _neo_obj(b, seg.get("relationship"))
        return True
    if ("start" in o and "end" in o or "startNode" in o and "endNode" in o) and ("type" in o or "label" in o):
        rel = o.get("label") if o.get("type") == "relationship" else o.get("type")
        for side in ("start", "end"):
            if isinstance(o.get(side), dict) and o[side].get("labels") is not None:
                _neo_obj(b, o[side])
        b.edge(_ref(o.get("start", o.get("startNode"))), _ref(o.get("end", o.get("endNode"))), rel,
               (o.get("properties") or {}).get("weight", 1))
        return True
    if "labels" in o and ("identity" in o or "elementId" in o or "id" in o):
        labels = o.get("labels") or []
        b.entity(_ref(o), o.get("properties") or {}, type=labels[0] if labels else None)
        return True
    return False


def _untype(o):
    """GraphSON 3 con tipos (``{"@type": "g:Int32", "@value": 1}``) -> valores planos."""
    if isinstance(o, dict):
        if set(o) == {"@type", "@value"} and str(o["@type"]).startswith(("g:", "gx:")):
            return _untype(o["@value"])
        return {k: _untype(v) for k, v in o.items()}
    if isinstance(o, list):
        return [_untype(x) for x in o]
    return o


def _graphson_vertex(b: _Builder, v: dict) -> None:
    props = {k: _val(p) for k, p in (v.get("properties") or {}).items()}
    props.update({k: x for k, x in v.items() if not k.startswith("_") and k not in ("id", "label", "properties",
                                                                                   "outE", "inE", "type")})
    vid = _ref(_first(v, "id", "_id"))
    b.entity(vid, props, type=_first(v, "label", "_label"))
    for e_label, es in (v.get("outE") or {}).items():
        for e in es:
            b.edge(vid, _ref(e.get("inV")), e_label, _val((e.get("properties") or {}).get("weight", 1)))


def _jsonld(b: _Builder, data) -> None:
    counter = [0]

    def node(o) -> str:
        if not isinstance(o, dict):
            return ""
        nid = _text(o.get("@id"))
        if not nid:
            counter[0] += 1
            nid = f"_:b{counter[0]}"
        types = o.get("@type")
        etype = _label((types[0] if isinstance(types, list) and types else types) or "") or None
        literals: dict = {}
        name = None
        for k, vals in o.items():
            if k.startswith("@"):
                continue
            for v in vals if isinstance(vals, list) else [vals]:
                if isinstance(v, dict) and "@list" in v:
                    for x in v["@list"]:
                        ref(nid, k, x, literals)
                    continue
                ref(nid, k, v, literals)
        name, _ = _name_from(literals)
        b.entity(nid, literals, name=name, type=etype)
        return nid

    def ref(nid, k, v, literals):
        if isinstance(v, dict) and ("@id" in v or any(not x.startswith("@") for x in v)) and "@value" not in v:
            b.edge(nid, node(v), k)
        elif _local(k) in ("label", "name", "preflabel", "title") or not isinstance(v, (dict, list)) or "@value" in v:
            literals.setdefault(k, _val(v))
    items = data.get("@graph", data) if isinstance(data, dict) else data
    for o in items if isinstance(items, list) else [items]:
        node(o)
    _collapse_blank(b)


def _tree(b: _Builder, o, parent: str | None, rel: str = "contiene", depth: int = 0) -> int:
    """Arboles: d3.hierarchy (``children``), XMind (``rootTopic``), cualquier anidamiento con nombre."""
    if depth > 60 or not isinstance(o, dict):
        return 0
    name, used = _name_from(o)
    if name is None:
        name = _text(_first(o, "id", "key"))
    if not name:
        return 0
    nid = _text(_first(o, "id", "@id", "key")) or f"{parent or ''}/{name}"
    scal = {k: v for k, v in o.items() if not isinstance(v, (dict, list))}
    b.entity(nid, scal, name=name, type=_first(o, "type", "kind") if isinstance(_first(o, "type", "kind"), str) else None)
    made = 0
    if parent:
        b.edge(parent, nid, rel)
        made += 1
    for k, v in o.items():
        kids = v.get("attached") if isinstance(v, dict) and isinstance(v.get("attached"), list) else v
        if isinstance(kids, dict):
            kids = [kids]
        if not isinstance(kids, list):
            continue
        r = "contiene" if k in ("children", "hijos", "topics", "attached", "items", "subtopics", "rootTopic") else k
        for c in kids:
            if isinstance(c, dict):
                made += _tree(b, c, nid, r, depth + 1)
            elif isinstance(c, (str, int, float)) and len(_text(c)) <= 60 and k not in ("labels", "markers"):
                b.edge(nid, b.node(f"{k}:{c}", c), k)
                made += 1
    return made


def parse_json(data) -> dict:
    b = _Builder()
    if isinstance(data, (dict, list)) and '"@type": "g:' in json.dumps(data, ensure_ascii=False)[:20000]:
        data = _untype(data)
    if isinstance(data, dict) and data.get("format") == "nexus-kg":
        for n in data.get("nodes", []):
            b.node(n["id"], n.get("name"), type=n.get("type"), category=n.get("category"), note=n.get("note"),
                   scope=n.get("scope", ""), weight=n.get("weight"))
            if n.get("scope", "") == "":
                b.nodes[_text(n["id"])]["scope"] = ""
        for e in data.get("edges", []):
            b.edge(e["src"], e["dst"], e.get("rel"), e.get("weight", 1.0))
        return b.result("nexus-kg")
    if isinstance(data, dict) and "elements" in data:   # Cytoscape (.cyjs)
        el = data["elements"]
        items = el if isinstance(el, list) else list(el.get("nodes", [])) + list(el.get("edges", []))
        for it in items:
            d = it.get("data", it)
            if "source" in d and "target" in d:
                b.edge(d["source"], d["target"], _first(d, "label", "interaction", "relation", "name"), d.get("weight", 1))
            elif "id" in d:
                props = {k: v for k, v in d.items() if k not in ("id", "label", "name", "type", "description")}
                b.entity(d["id"], props, name=_first(d, "label", "name"), type=d.get("type"), note=d.get("description"))
        return b.result("cytoscape")
    if isinstance(data, list) and data and all(isinstance(x, dict) and len(x) <= 3 for x in data) and \
            any("nodes" in x or "edges" in x for x in data):   # Cytoscape CX / CX2: lista de aspectos
        for asp in data:
            for n in asp.get("nodes", []):
                b.entity(_first(n, "@id", "id"), n.get("v") or {}, name=n.get("n"))
            for e in asp.get("edges", []):
                b.edge(_first(e, "s", "source"), _first(e, "t", "target"), _first(e, "i") or (e.get("v") or {}).get(
                    "interaction"))
        return b.result("cytoscape-cx")
    if isinstance(data, dict) and ("@graph" in data or "@context" in data) or \
            isinstance(data, list) and data and all(isinstance(x, dict) and "@id" in x for x in data[:20]):
        _jsonld(b, data)
        return b.result("json-ld")
    if isinstance(data, dict) and isinstance(data.get("results"), list):   # Neo4j HTTP API (resultDataContents graph)
        for res in data["results"]:
            for row in res.get("data", []):
                g = row.get("graph") or {}
                for n in g.get("nodes", []):
                    _neo_obj(b, n)
                for r in g.get("relationships", []):
                    _neo_obj(b, r)
                for v in row.get("row", []) if not g else []:
                    _neo_obj(b, v)
        if b.nodes:
            return b.result("neo4j")
    rows = data if isinstance(data, list) else None
    if rows and all(isinstance(x, dict) for x in rows[:50]):
        if any(x.get("type") in ("node", "relationship") for x in rows[:50]):   # APOC export json
            for x in rows:
                if x.get("type") == "node":
                    labels = x.get("labels") or []
                    b.entity(x.get("id"), x.get("properties") or {}, type=labels[0] if labels else None)
                elif x.get("type") == "relationship":
                    _neo_obj(b, x)
            return b.result("neo4j-apoc")
        if any("outE" in x or "inE" in x or ("label" in x and isinstance(x.get("properties"), dict) and
                                             all(isinstance(v, list) for v in x["properties"].values()))
               for x in rows[:50]):   # GraphSON por vertice
            for x in rows:
                _graphson_vertex(b, x)
            return b.result("graphson")
        neo = 0
        for rec in rows:   # Neo4j Browser: [{"n": {...}, "r": {...}, "m": {...}}]
            for v in rec.values():
                for o in v if isinstance(v, list) else [v]:
                    neo += _neo_obj(b, o)
        if neo:
            return b.result("neo4j")
    if isinstance(data, dict) and isinstance(data.get("vertices"), list) and not data.get("nodes"):   # GraphSON
        for v in data["vertices"]:
            _graphson_vertex(b, v)
        _generic_edges(b, data.get("edges"))
        return b.result("graphson")
    if isinstance(data, list) and data and isinstance(data[0], dict) and "rootTopic" in data[0]:   # XMind
        for sheet in data:
            _tree(b, sheet["rootTopic"], None)
        return b.result("xmind")
    g = data.get("graph") if isinstance(data, dict) and isinstance(data.get("graph"), dict) and \
        any(k in data["graph"] for k in NODE_LISTS + EDGE_LISTS + ("graphs",)) else data
    if isinstance(g, dict) and ("graphs" in g and isinstance(g["graphs"], list)):   # JGF con varios grafos
        g = {"nodes": [n for x in g["graphs"] for n in _as_list(x.get("nodes"))],
             "edges": [e for x in g["graphs"] for e in x.get("edges", [])]}
    if isinstance(g, dict) and any(k in g for k in NODE_LISTS + EDGE_LISTS):
        nodes = next((g[k] for k in NODE_LISTS if k in g), [])
        nodes = _as_list(nodes)
        ids = [_generic_node(b, n) for n in nodes]
        for k in EDGE_LISTS:
            if k in g:
                _generic_edges(b, g[k])
        if isinstance(g.get("adjacency"), list):   # NetworkX adjacency_data
            seen = set()
            for i, nbrs in enumerate(g["adjacency"]):
                for nb in nbrs if i < len(ids) else []:
                    if isinstance(nb, dict):
                        pair = (_text(_ref(nb)), ids[i])
                        if g.get("directed") is False and pair in seen:   # no dirigido: cada arista sale dos veces
                            continue
                        seen.add(pair[::-1])
                        b.edge(ids[i], _ref(nb), _edge_rel(nb), nb.get("weight", 1))
        return b.result("json")
    rows = data if isinstance(data, list) else (data.get("triples") or data.get("facts") or data.get("tripletas")) \
        if isinstance(data, dict) else None
    if isinstance(rows, list) and sum(1 for t in rows if _triple(b, t)):
        return b.result("triples")
    if isinstance(data, dict) and data and all(isinstance(v, list) and v and all(isinstance(x, (str, int, float))
                                                                                    for x in v) for v in data.values()):
        for k, vs in data.items():   # {"A": ["B", "C"]}: lista de adyacencia
            for v in vs:
                b.edge(k, v)
        return b.result("adyacencia")
    if isinstance(data, dict) and data and all(isinstance(v, dict) and v and all(isinstance(x, dict) for x in v.values())
                                               for v in data.values()):
        for k, nbrs in data.items():   # NetworkX to_dict_of_dicts: {"A": {"B": {"weight": 2}}}
            for nb, attrs in nbrs.items():
                b.edge(k, nb, _edge_rel(attrs), attrs.get("weight", 1))
        return b.result("adyacencia")
    if isinstance(data, dict) and (isinstance(data.get("children"), list) or "rootTopic" in data):
        _tree(b, data.get("rootTopic", data), None)
        return b.result("arbol")
    made = 0   # ultimo recurso: cualquier JSON con objetos con nombre
    for o in data if isinstance(data, list) else [data]:
        made += _tree(b, o, None) if isinstance(o, dict) else 0
    if not b.nodes:
        raise GraphFormatError("el JSON no tiene nodos/enlaces, tripletas ni objetos con nombre reconocibles")
    return b.result("json-generico", generic=True)


def _as_list(nodes):
    if isinstance(nodes, dict):   # JSON Graph Format: {id: {label...}}
        return [{"id": k, **(v if isinstance(v, dict) else {"label": v})} for k, v in nodes.items()]
    return nodes or []


def _triple(b: _Builder, t) -> bool:
    if isinstance(t, (list, tuple)) and len(t) >= 3 and all(isinstance(x, (str, int, float)) for x in t[:3]):
        b.edge(b.node(t[0], t[0]), b.node(t[2], t[2]), t[1], t[3] if len(t) > 3 else 1.0)
        return True
    if isinstance(t, dict):
        s, r, o = _first(t, "s", "subject", "sujeto", "head", "from"), _first(t, "r", "p", "predicate", "relation",
                                                                             "relacion", "rel"), \
            _first(t, "o", "object", "objeto", "tail", "to")
        if s is not None and o is not None and not isinstance(s, (dict, list)) and not isinstance(o, (dict, list)):
            b.node(s, s)
            b.node(o, o, type=t.get("type"), category=t.get("category") or t.get("categoria"))
            b.edge(s, o, r, t.get("weight", 1.0))
            return True
    return False


def parse_jsonl(text: str) -> dict:
    b = _Builder()
    objs, n = [], 0
    for line in text.splitlines():
        line = line.strip().rstrip(",")
        if line:
            try:
                o = json.loads(line)
            except json.JSONDecodeError:
                continue
            if _triple(b, o):
                n += 1
            else:
                objs.append(o)
    if objs:   # APOC (una linea por nodo/relacion), GraphSON por vertice...
        try:
            g = parse_json(objs)
        except GraphFormatError:
            g = None
        if g and not (n and g.get("generic")):
            if not n:
                return g
            return merge([("tripletas", b.result("jsonl")), ("objetos", g)])
    if not n:
        raise GraphFormatError("ninguna linea es una tripleta o un objeto de grafo JSON")
    return b.result("jsonl")


def parse_yaml(text: str) -> dict:
    try:
        import yaml
    except ImportError as e:   # pragma: no cover - pyyaml va en requirements
        raise GraphFormatError("falta pyyaml para leer YAML") from e
    docs = [d for d in yaml.safe_load_all(text) if d is not None]
    g = parse_json(docs[0] if len(docs) == 1 else docs)
    g["format"] = "yaml" if not g.get("generic") else "yaml-generico"
    return g


def parse_toml(text: str) -> dict:
    import tomllib
    g = parse_json(tomllib.loads(text))
    g["format"] = "toml" if not g.get("generic") else "toml-generico"
    return g


# ------------------------------------------------------------------ tablas (CSV, TSV, hojas, SQLite)
SRC = ("source", "src", "from", "origen", "s", "subject", "sujeto", "head", "node1", "source_id", "sourceid",
       "source id", "from_id", "nodo1", "start", "inicio", "parent", "padre")
DST = ("target", "dst", "to", "destino", "o", "object", "objeto", "tail", "node2", "target_id", "targetid",
       "target id", "to_id", "nodo2", "end", "fin", "child", "hijo")
REL = ("relation", "rel", "label", "predicate", "relacion", "relación", "interaction", "r", "p", "edge", "type",
       "tipo", "kind", "name")
ID_COLS = ("id", "node", "nodo", "key", "node_id", "nodeid", "identifier", "uid", "entity_id", "vertex")
NAME_COLS = ("label", "name", "nombre", "title", "titulo", "título", "entity", "entidad", "concepto", "concept")
TYPE_COLS = ("type", "tipo", "group", "grupo", "class", "clase", "labels", "kind")
NOTE_COLS = ("description", "descripcion", "descripción", "note", "nota", "comment", "comentario", "definition")
WEIGHT_COLS = ("weight", "peso", "value", "valor", "strength")


def _num(s) -> float | None:
    try:
        return float(str(s).strip().replace(",", ".")) if str(s).strip() else 0.0
    except ValueError:
        return None


def _neo_col(h: str) -> tuple[str, str]:
    """Cabecera de neo4j-admin: ``name:ID(Person)`` -> ("name", "ID"); ``age:int`` -> ("age", "")."""
    m = re.match(r"^\s*([^:]*?)\s*:\s*(ID|START_ID|END_ID|TYPE|LABEL)\b", h, re.I)
    if m:
        return m.group(1), m.group(2).upper()
    return re.sub(r":\w+(\[\])?$", "", h).strip(), ""


def _rows_to_graph(b: _Builder, rows: list[list[str]]) -> str:
    """Una tabla -> nodos y aristas en ``b``. Devuelve el tipo de tabla (aristas, nodos, matriz, tripletas...)."""
    rows = [[c.strip() for c in r] for r in rows if any(str(c).strip() for c in r)]
    if not rows:
        raise GraphFormatError("tabla vacia")
    raw = rows[0]
    head = [c.lower().strip().strip('"') for c in raw]
    body = rows[1:]
    neo = [_neo_col(h) for h in raw]
    if any(k for _, k in neo):   # neo4j-admin import
        kinds = [k for _, k in neo]
        cols = [n for n, _ in neo]

        def props(r, skip):
            return {cols[i]: r[i] for i in range(min(len(r), len(cols))) if i not in skip and cols[i] and r[i]}
        if "START_ID" in kinds and "END_ID" in kinds:
            si, di = kinds.index("START_ID"), kinds.index("END_ID")
            ti = kinds.index("TYPE") if "TYPE" in kinds else None
            for r in body:
                if len(r) > max(si, di):
                    b.edge(r[si], r[di], r[ti] if ti is not None and ti < len(r) else None,
                           props(r, {si, di, ti}).get("weight", 1))
            return "neo4j-aristas"
        ii = kinds.index("ID") if "ID" in kinds else 0
        li = kinds.index("LABEL") if "LABEL" in kinds else None
        for r in body:
            if len(r) > ii and r[ii]:
                p = props(r, {ii, li})
                if cols[ii]:
                    p[cols[ii]] = r[ii]
                b.entity(r[ii], p, type=r[li].split(";")[0] if li is not None and li < len(r) and r[li] else None)
        return "neo4j-nodos"
    if len(raw) >= 4 and raw[0].startswith("/a/") and raw[1].startswith("/r/"):   # ConceptNet
        for r in rows:
            if len(r) >= 4:
                b.edge(r[2], r[3], r[1].rsplit("/", 1)[-1])
                b.node(r[2], r[2].split("/")[3] if r[2].count("/") >= 3 else r[2])
                b.node(r[3], r[3].split("/")[3] if r[3].count("/") >= 3 else r[3])
        return "conceptnet"

    def col(names, exclude=()):
        return next((i for n in names for i, h in enumerate(head) if h == n and i not in exclude), None)
    si, di = col(SRC), col(DST)
    if si is not None and di is not None and si != di:
        ri = col(REL, {si, di})
        if ri is not None and all(r[ri].lower() in ("directed", "undirected", "") for r in body[:50] if ri < len(r)):
            ri = col([x for x in REL if x not in ("type", "tipo", "kind")], {si, di})   # Gephi: Type=Directed
        wi = col(WEIGHT_COLS, {si, di})
        for r in body:
            if len(r) > max(si, di) and r[si] and r[di]:
                b.edge(r[si], r[di], r[ri] if ri is not None and ri < len(r) else None,
                       r[wi] if wi is not None and wi < len(r) else 1.0)
        return "aristas"
    ii, ni = col(ID_COLS), col(NAME_COLS)
    if (ii is not None or ni is not None) and body and len(head) >= 2 and all(_num(h) is None for h in head if h):
        ti, oi = col(TYPE_COLS, {ii, ni}), col(NOTE_COLS, {ii, ni})
        key = ii if ii is not None else ni
        for r in body:
            if len(r) > key and r[key]:
                p = {raw[i]: r[i] for i in range(min(len(r), len(raw))) if i not in (ii, ni, ti, oi) and r[i]}
                b.entity(r[key], p, name=r[ni] if ni is not None and ni < len(r) and r[ni] else None,
                         type=r[ti] if ti is not None and ti < len(r) else None,
                         note=r[oi] if oi is not None and oi < len(r) else None)
        return "nodos"
    n = len(raw)
    if n >= 3 and len(body) >= 2 and all(len(r) == n for r in body) and \
            all(_num(c) is not None for r in body for c in r[1:]):   # matriz de adyacencia con nombres
        names = raw[1:]
        for r in body:
            for j, c in enumerate(r[1:]):
                w = _num(c)
                if w:
                    b.edge(r[0], names[j], None, w)
        for nm in names:
            b.node(nm, nm)
        return "matriz"
    if n >= 3:   # sin cabecera: sujeto, relacion, objeto[, peso]
        for r in rows:
            if len(r) >= 3 and r[0] and r[2]:
                w = _num(r[3]) if len(r) > 3 else None
                b.edge(r[0], r[2], r[1], w if w is not None else 1.0)
        return "tripletas"
    if n == 2:   # sin cabecera: origen, destino
        for r in rows:
            if len(r) == 2 and r[0] and r[1]:
                b.edge(r[0], r[1])
        return "pares"
    raise GraphFormatError("la tabla necesita columnas origen/destino, una columna id/nombre o tres columnas "
                           "sujeto,relacion,objeto")


def _sniff_delimiter(sample: str) -> str:
    counts = {d: sample.count(d) for d in ("\t", ",", ";", "|")}
    return max(counts, key=lambda d: (counts[d], d == ","))


def parse_table(text: str, delimiter: str | None = None) -> dict:
    delimiter = delimiter or _sniff_delimiter(text[:4096])
    rows = list(csv.reader(io.StringIO(text), delimiter=delimiter))
    b = _Builder()
    kind = _rows_to_graph(b, rows)
    g = b.result("csv" if delimiter == "," else "tsv" if delimiter == "\t" else "tabla")
    g["table"] = kind
    return g


def parse_spreadsheet(data: bytes, name: str) -> dict:
    """Excel (.xlsx) u OpenDocument (.ods): cada hoja es una tabla (p. ej. una de nodos y otra de aristas)."""
    from ..rag.readers import read_document
    ext = ".ods" if name.lower().endswith(".ods") else ".xlsx"
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / f"hoja{ext}"
        p.write_bytes(data)
        _, pages = read_document(p)
    b = _Builder()
    kinds = []
    for page in pages:
        try:
            kinds.append(_rows_to_graph(b, [line.split("\t") for line in page.splitlines()]))
        except GraphFormatError:
            continue
    if not kinds:
        raise GraphFormatError("ninguna hoja tiene forma de grafo (origen/destino, id/nombre o tripletas)")
    g = b.result("excel" if ext == ".xlsx" else "ods")
    g["table"] = "+".join(kinds)
    return g


def parse_sqlite(data: bytes) -> dict:
    """Base SQLite: tablas de nodos y aristas (o la base de otro Nexus-Office, con sus grafos y ambitos)."""
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "grafo.db"
        p.write_bytes(data)
        con = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
        try:
            tables = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                                                "'sqlite_%' ORDER BY name")]
            cols = {t: [r[1] for r in con.execute(f'PRAGMA table_info("{t}")')] for t in tables}
            b = _Builder()
            if {"scope", "key", "name"} <= set(cols.get("entities", [])) and {"src", "dst", "rel"} <= set(
                    cols.get("relations", [])):   # otro Nexus-Office: mismo esquema
                for r in con.execute(f"SELECT id, name, type, category, note, scope, weight FROM entities LIMIT {MAX_NODES}"):
                    b.node(r[0], r[1], type=r[2], category=r[3], note=r[4], scope=r[5], weight=r[6])
                    b.nodes[_text(r[0])]["scope"] = r[5] or ""
                for r in con.execute("SELECT src, dst, rel, weight FROM relations"):
                    b.edge(r[0], r[1], r[2], r[3])
                return b.result("nexus-kg")
            kinds = []
            for t in sorted(tables, key=lambda t: any(c.lower() in SRC for c in cols[t])):   # nodos antes que aristas
                rows = [cols[t]] + [["" if v is None else str(v) for v in r]
                                    for r in con.execute(f'SELECT * FROM "{t}" LIMIT {MAX_NODES * 2}')]
                try:
                    kinds.append(f"{t}:{_rows_to_graph(b, rows)}")
                except GraphFormatError:
                    continue
        finally:
            con.close()
    if not kinds:
        raise GraphFormatError("ninguna tabla de la base SQLite tiene forma de grafo")
    g = b.result("sqlite")
    g["table"] = ", ".join(kinds)
    return g


# ------------------------------------------------------------------ XML
def _strip(tag) -> str:
    return str(tag).rsplit("}", 1)[-1]


def _xml(text_or_root):
    return text_or_root if isinstance(text_or_root, ET.Element) else ET.fromstring(text_or_root)


def parse_graphml(text) -> dict:
    root = _xml(text)
    keys = {k.get("id"): (k.get("attr.name") or k.get("id") or "").lower() for k in root.iter() if _strip(k.tag) == "key"}
    b = _Builder()

    def data(el) -> dict:
        return {keys.get(d.get("key"), d.get("key")): "".join(d.itertext()).strip() for d in el
                if _strip(d.tag) == "data"}

    def yed_label(el, tag):
        lab = next((x for x in el.iter() if _strip(x.tag) == tag), None)
        return "".join(lab.itertext()).strip() if lab is not None else None
    for el in root.iter():
        t = _strip(el.tag)
        if t == "node":
            d = data(el)
            name = _first(d, "label", "name", "title", "nombre") or yed_label(el, "NodeLabel")
            props = {k: v for k, v in d.items() if k not in ("label", "name", "title", "nombre", "type", "category",
                                                            "description", "note", "scope") and v
                     and not re.match(r"^d\d+$", str(k))}
            b.entity(el.get("id"), props, name=name, type=_first(d, "type", "labels"), category=d.get("category"),
                     note=_first(d, "description", "note"))
        elif t == "edge":
            d = data(el)
            b.edge(el.get("source"), el.get("target"),
                   _first(d, "label", "relation", "rel", "type", "predicate") or el.get("label") or
                   yed_label(el, "EdgeLabel"), d.get("weight", 1.0))
    return b.result("graphml")


def parse_gexf(text) -> dict:
    root = _xml(text)
    b = _Builder()
    attrs = {}
    for a in root.iter():
        if _strip(a.tag) == "attributes":
            for x in a:
                attrs[(a.get("class", "node"), x.get("id"))] = x.get("title") or x.get("id")
    for el in root.iter():
        t = _strip(el.tag)
        if t == "node":
            props = {attrs.get(("node", v.get("for") or v.get("id")), v.get("for")): v.get("value")
                     for v in el.iter() if _strip(v.tag) == "attvalue"}
            name = el.get("label")
            b.entity(el.get("id"), props, name=name, type=props.pop("type", None) if "type" in props else None)
        elif t == "edge":
            props = {attrs.get(("edge", v.get("for") or v.get("id")), v.get("for")): v.get("value")
                     for v in el.iter() if _strip(v.tag) == "attvalue"}
            b.edge(el.get("source"), el.get("target"), el.get("label") or props.get("label") or el.get("kind"),
                   el.get("weight", 1.0))
    return b.result("gexf")


def _rdf_triple(b: _Builder, s: str, p: str, o: str, literal: bool) -> None:
    lp = _local(p)
    if p in ("type", RDF_NS + "type") or lp == "type" and not literal:
        b.node(s, None, type=_label(o).lower()[:30] or "concepto")
    elif literal and lp in ("label", "name", "preflabel", "title", "altlabel", "nombre"):
        b.node(s, o)
    elif literal and lp in ("comment", "description", "definition", "abstract", "note", "scopenote"):
        b.node(s, None, note=o)
    elif literal:
        b.entity(s, {_label(p) or lp: o})
    else:
        b.edge(s, o, p)


def parse_rdfxml(text) -> dict:
    """RDF/XML y OWL (rdf:Description, nodos tipados, rdf:resource, anidados y parseType="Resource")."""
    root = _xml(text)
    b = _Builder()
    n = [0, 0]   # contador de nodos anonimos, tripletas
    RES, ABOUT, ID, NID = (f"{{{RDF_NS}}}{x}" for x in ("resource", "about", "ID", "nodeID"))

    def tag_iri(tag) -> str:
        return str(tag)[1:].replace("}", "") if str(tag).startswith("{") else str(tag)

    def subject(el, sid=None) -> str:
        if sid is None:
            sid = el.get(ABOUT) or (("#" + el.get(ID)) if el.get(ID) else None) or \
                (("_:" + el.get(NID)) if el.get(NID) else None)
            if sid is None:
                n[0] += 1
                sid = f"_:b{n[0]}"
            b.node(sid)
            if tag_iri(el.tag) != RDF_NS + "Description":
                b.node(sid, None, type=_label(tag_iri(el.tag)).lower()[:30])
        for k, v in el.attrib.items():
            if not k.startswith(f"{{{RDF_NS}}}") and not k.startswith("{http://www.w3.org/XML/1998/namespace}"):
                _rdf_triple(b, sid, tag_iri(k), v, True)
                n[1] += 1
        for prop in el:
            p = tag_iri(prop.tag)
            res = prop.get(RES) or (("_:" + prop.get(NID)) if prop.get(NID) else None)
            if res:
                _rdf_triple(b, sid, p, res, False)
            elif prop.get(f"{{{RDF_NS}}}parseType") == "Resource":
                n[0] += 1
                child = f"_:b{n[0]}"
                b.node(child)
                subject(prop, child)
                _rdf_triple(b, sid, p, child, False)
            elif len(prop):
                for c in prop:
                    _rdf_triple(b, sid, p, subject(c), False)
            elif (prop.text or "").strip():
                _rdf_triple(b, sid, p, prop.text.strip(), True)
            else:
                continue
            n[1] += 1
        return sid
    for el in root:
        subject(el)
    if not n[1] and len(b.nodes) < 2:
        raise GraphFormatError("no se han encontrado tripletas RDF/XML")
    _collapse_blank(b)
    return b.result("rdf-xml")


NODE_TAGS = {"node", "vertex", "nodo", "entity", "entidad", "concept", "concepto", "topic", "outline", "item"}
EDGE_TAGS = {"edge", "link", "arc", "relationship", "relation", "arista", "association", "enlace", "arrowlink"}
XML_ID = ("id", "key", "uid", "name")
XML_NAME = ("label", "name", "text", "title", "nombre", "caption")
XML_SRC = ("source", "from", "src", "start", "origin", "origen")
XML_DST = ("target", "to", "dst", "end", "destination", "destino")
XML_REL = ("label", "type", "category", "relation", "name", "kind", "rel")


def parse_xml_generic(root: ET.Element) -> dict:
    """XGMML, DGML, GXL, FreeMind/Freeplane, OPML, XMind (content.xml) y cualquier XML con nodos y enlaces.
    Un nodo dentro de otro (mapas mentales, esquemas) queda unido a el con ``contiene``."""
    b = _Builder()
    count = [0, 0]
    root_tag = _strip(root.tag).lower()

    def low(el) -> dict:
        return {_strip(k).lower(): v for k, v in el.attrib.items()}

    def child_props(el) -> dict:
        out = {}
        for c in el:
            t = _strip(c.tag).lower()
            if t in ("att", "attr", "attribute", "property", "data", "attvalue"):
                a = low(c)
                k = a.get("name") or a.get("key") or a.get("for")
                v = a.get("value") if "value" in a else "".join(c.itertext()).strip()
                if k and v:
                    out[k] = v
            elif t in XML_NAME and not len(c) and (c.text or "").strip():
                out.setdefault(t, c.text.strip())
        return out

    def walk(el, parent):
        t = _strip(el.tag).lower()
        a = low(el)
        if t in NODE_TAGS and not (t == "item" and root_tag in ("rss", "channel")):
            props = child_props(el)
            name = _first(a, *XML_NAME) or _first(props, *XML_NAME)
            count[0] += 1
            nid = _first(a, *XML_ID) or (f"{parent}/{name}" if name else f"_n{count[0]}")
            rest = {k: v for k, v in {**a, **props}.items() if k not in XML_ID + XML_NAME + ("type", "category")
                    and len(str(v)) < 300}
            b.entity(nid, rest, name=name, type=a.get("type") or a.get("category") or props.get("type"))
            if parent:
                b.edge(parent, nid, "contiene")
                count[1] += 1
            for c in el:
                walk(c, nid)
            return
        if t in EDGE_TAGS:
            s, d = _first(a, *XML_SRC) or parent, _first(a, *XML_DST)
            if s and d:
                props = child_props(el)
                b.edge(s, d, _first(a, *XML_REL) or _first(props, "label", "interaction", "type"),
                       a.get("weight") or props.get("weight") or 1)
                count[1] += 1
            return
        for c in el:
            walk(c, parent)
    walk(root, None)
    if not b.nodes:
        raise GraphFormatError("el XML no tiene elementos de nodo o enlace reconocibles")
    known = root_tag in ("graph", "directedgraph", "gxl", "map", "opml", "xmap-content")
    fmt = {"graph": "xgmml", "directedgraph": "dgml", "gxl": "gxl", "map": "mapa-mental", "opml": "opml",
           "xmap-content": "xmind"}.get(root_tag, "xml")
    return b.result(fmt, generic=not known or not count[1])


def parse_xml(text: str) -> dict:
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        raise GraphFormatError(f"XML mal formado: {e}") from e
    tag = _strip(root.tag).lower()
    if tag == "graphml":
        return parse_graphml(root)
    if tag == "gexf":
        return parse_gexf(root)
    if tag == "rdf" or root.tag == f"{{{RDF_NS}}}RDF":
        return parse_rdfxml(root)
    if tag == "html":
        raise GraphFormatError("es una pagina HTML, no un grafo: importala en la Biblioteca")
    return parse_xml_generic(root)


# ------------------------------------------------------------------ Graphviz DOT
_DOT_ID = r'(?:\x01\d+\x01|<[^<>]*>|-?[\w.]+)'


def _protect(text: str, pattern=r'"(?:[^"\\]|\\.)*"') -> tuple[str, list[str]]:
    """Cambia las cadenas por marcas \\x01N\\x01 para que las expresiones no se lien con su contenido."""
    found: list[str] = []

    def sub(m):
        found.append(m.group(0))
        return f"\x01{len(found) - 1}\x01"
    return re.sub(pattern, sub, text), found


def parse_dot(text: str) -> dict:
    text, strings = _protect(text)
    text = re.sub(r"//[^\n]*|/\*.*?\*/|^\s*#[^\n]*", "", text, flags=re.S | re.M)
    b = _Builder()

    def unq(s: str) -> str:
        m = re.fullmatch(r"\x01(\d+)\x01", s.strip())
        if m:
            s = strings[int(m.group(1))][1:-1].replace('\\"', '"').replace("\\n", " ")
        elif s.startswith("<") and s.endswith(">"):
            s = re.sub(r"<[^>]+>", " ", s[1:-1]).strip()
        return s

    def attr(attrs: str, key: str):
        m = re.search(rf'\b{key}\s*=\s*({_DOT_ID})', attrs or "")
        return unq(m.group(1)) if m else None
    for m in re.finditer(rf"({_DOT_ID})\s*\[([^\]]*)\]", text):   # atributos de nodo (label, tooltip)
        if m.group(1) in ("graph", "node", "edge") or re.search(r"-[->]\s*$", text[:m.start()].rstrip()[-3:]):
            continue
        b.node(unq(m.group(1)), attr(m.group(2), "label"), note=attr(m.group(2), "tooltip"))
    found = 0
    for m in re.finditer(rf"({_DOT_ID}(?:\s*-[->]\s*{_DOT_ID})+)\s*(\[([^\]]*)\])?", text):
        ids = [unq(x) for x in re.split(r"\s*-[->]\s*", m.group(1))]
        for a, c in zip(ids, ids[1:]):
            b.edge(a, c, attr(m.group(3), "label") or attr(m.group(3), "xlabel"), attr(m.group(3), "weight") or 1)
            found += 1
    if not found:
        raise GraphFormatError("no hay aristas a -> b en el fichero DOT")
    return b.result("dot")


# ------------------------------------------------------------------ GML
def parse_gml(text: str) -> dict:
    text = re.sub(r"(?m)^\s*#.*$", "", text)
    toks = re.findall(r'"(?:[^"\\]|\\.)*"|\[|\]|[^\s\[\]"]+', text)

    def block(i):
        items = []
        while i < len(toks):
            t = toks[i]
            if t == "]":
                return items, i + 1
            if i + 1 >= len(toks):
                break
            v = toks[i + 1]
            if v == "[":
                val, i = block(i + 2)
            else:
                val, i = (v[1:-1] if v.startswith('"') else v), i + 2
            items.append((t.lower(), val))
        return items, i
    top, _ = block(0)
    b = _Builder()
    graphs = [v for k, v in top if k == "graph" and isinstance(v, list)]
    if not graphs:
        raise GraphFormatError("no hay bloque graph [ ... ] en el GML")
    for g in graphs:
        for k, v in g:
            if not isinstance(v, list):
                continue
            d = {}
            for kk, vv in v:
                d.setdefault(kk, vv)
            if k == "node":
                props = {kk: vv for kk, vv in d.items() if not isinstance(vv, list) and kk not in ("id", "label")}
                b.entity(d.get("id"), props, name=d.get("label") or d.get("name"), type=d.get("type"))
            elif k == "edge":
                b.edge(d.get("source"), d.get("target"), _first(d, "label", "relation", "type", "name"),
                       _first(d, "weight", "value", default=1))
    return b.result("gml")


# ------------------------------------------------------------------ formatos de texto de analisis de redes
def _unq(s: str) -> str:
    s = s.strip()
    return s[1:-1] if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'" else s


def _split(line: str) -> list[str]:
    return [_unq(x) for x in re.findall(r'"[^"]*"|\'[^\']*\'|\S+', line)]


def parse_pajek(text: str) -> dict:
    b = _Builder()
    section, found = "", 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("%"):
            continue
        if line.startswith("*"):
            section = line.split()[0].lower()
            continue
        parts = _split(line)
        if section == "*vertices" and parts:
            b.node(parts[0], parts[1] if len(parts) > 1 else parts[0])
        elif section in ("*arcs", "*edges") and len(parts) >= 2:
            b.edge(parts[0], parts[1], parts[3] if len(parts) > 3 and _num(parts[3]) is None else None,
                   parts[2] if len(parts) > 2 and _num(parts[2]) is not None else 1)
            found += 1
        elif section in ("*arcslist", "*edgeslist") and len(parts) >= 2:
            for d in parts[1:]:
                b.edge(parts[0], d)
                found += 1
    if not b.nodes:
        raise GraphFormatError("no hay *Vertices ni aristas Pajek")
    return b.result("pajek")


def parse_tgf(text: str) -> dict:
    b = _Builder()
    edges = False
    for line in text.splitlines():
        if line.strip() == "#":
            edges = True
            continue
        parts = line.strip().split(None, 2 if edges else 1)
        if not parts:
            continue
        if not edges:
            b.node(parts[0], parts[1] if len(parts) > 1 else parts[0])
        elif len(parts) >= 2:
            b.edge(parts[0], parts[1], parts[2] if len(parts) > 2 else None)
    return b.result("tgf")


def parse_gdf(text: str) -> dict:
    """GUESS/Gephi: ``nodedef>name VARCHAR,label VARCHAR`` y ``edgedef>node1 VARCHAR,node2 VARCHAR``."""
    b = _Builder()
    mode, cols = "", []
    q = "'" if text.count("'") > text.count('"') else '"'
    for line in text.splitlines():
        if not line.strip():
            continue
        m = re.match(r"^\s*(nodedef|edgedef)\s*>\s*(.*)$", line, re.I)
        if m:
            mode = m.group(1).lower()
            cols = [c.strip().split()[0].lower() if c.strip() else "" for c in m.group(2).split(",")]
            continue
        r = next(csv.reader([line], quotechar=q, skipinitialspace=True))
        if mode == "nodedef" and r:
            p = {cols[i]: r[i] for i in range(1, min(len(r), len(cols))) if r[i] and cols[i] not in ("label",)}
            li = cols.index("label") if "label" in cols else None
            b.entity(r[0], p, name=r[li] if li is not None and li < len(r) and r[li] else None)
        elif mode == "edgedef" and len(r) >= 2:
            d = {cols[i]: r[i] for i in range(min(len(r), len(cols)))}
            b.edge(r[0], r[1], _first(d, "label", "relation", "type"), _first(d, "weight", default=1))
    return b.result("gdf")


def parse_ucinet(text: str) -> dict:
    """UCINET DL: formatos fullmatrix, edgelist1 y nodelist1, con etiquetas aparte o incrustadas."""
    low = text.lower()
    head = low.split("data:", 1)[0]
    n = int(re.search(r"\bn\s*=\s*(\d+)", head).group(1)) if re.search(r"\bn\s*=\s*(\d+)", head) else 0
    fmt = re.search(r"format\s*=\s*(\w+)", head)
    fmt = fmt.group(1) if fmt else "fullmatrix"
    labels: list[str] = []
    m = re.search(r"(?:^|\n)\s*(?:row\s+)?labels\s*:\s*\n(.*?)(?=\n\s*(?:col\s+labels|data)\s*:)", text, re.I | re.S)
    if m:
        labels = [_unq(x) for x in re.split(r"[,\n]+", m.group(1)) if x.strip()]
    body = re.split(r"(?im)^\s*data\s*:\s*$", text, maxsplit=1)
    lines = [ln for ln in (body[1] if len(body) > 1 else "").splitlines() if ln.strip()]
    b = _Builder()
    lab = lambda i: labels[i] if i < len(labels) else str(i + 1)  # noqa: E731
    if fmt.startswith("edgelist"):
        for ln in lines:
            p = _split(ln.replace(",", " "))
            if len(p) >= 2:
                b.edge(p[0], p[1], None, p[2] if len(p) > 2 else 1)
    elif fmt.startswith("nodelist"):
        for ln in lines:
            p = _split(ln.replace(",", " "))
            for d in p[1:]:
                b.edge(p[0], d)
    else:
        rows = [_split(ln.replace(",", " ")) for ln in lines]
        if "labels embedded" in head and rows:
            labels = rows[0]
            rows = [r[1:] for r in rows[1:]]
        for i, r in enumerate(rows[:n or len(rows)]):
            b.node(lab(i), lab(i))
            for j, c in enumerate(r):
                w = _num(c)
                if w:
                    b.edge(lab(i), lab(j), None, w)
    return b.result("ucinet-dl")


def parse_sif(text: str) -> dict:
    """Cytoscape SIF: ``nodo relacion destino1 destino2 ...`` (tabuladores o espacios)."""
    b = _Builder()
    for line in text.splitlines():
        p = line.split("\t") if "\t" in line else line.split()
        p = [x.strip() for x in p if x.strip()]
        if len(p) >= 3:
            for d in p[2:]:
                b.edge(p[0], d, p[1])
        elif len(p) == 1:
            b.node(p[0], p[0])
    return b.result("sif")


def parse_lgl(text: str) -> dict:
    b = _Builder()
    cur = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#"):
            cur = line[1:].strip()
            b.node(cur, cur)
        elif line and cur:
            p = line.split()
            b.edge(cur, p[0], None, p[1] if len(p) > 1 else 1)
    return b.result("lgl")


def parse_edgelist(text: str, adjlist: bool = False) -> dict:
    """NCOL, .edgelist y listas de adyacencia de NetworkX: ``a b [peso | relacion]`` o ``a b c d`` (adyacencia)."""
    b = _Builder()
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip() if not line.lstrip().startswith("#") else ""
        if not line:
            continue
        p = _split(line.replace(",", " ") if "," in line and " " not in line else line)
        if adjlist:
            b.node(p[0], p[0])
            for d in p[1:]:
                b.edge(p[0], d)
        elif len(p) == 2:
            b.edge(p[0], p[1])
        elif len(p) >= 3:
            w = _num(p[2])
            if w is not None and len(p) == 3:
                b.edge(p[0], p[1], None, w)
            elif p[2].startswith("{"):   # NetworkX: a b {'weight': 3}
                attrs = re.findall(r"""['"](\w+)['"]\s*:\s*['"]?([^,'"}]+)""", line)
                d = dict(attrs)
                b.edge(p[0], p[1], _first(d, "label", "relation", "type"), d.get("weight", 1))
            else:
                b.edge(p[0], p[2] if len(p) == 3 else p[1], p[1] if len(p) == 3 else p[2])
        elif len(p) == 1:
            b.node(p[0], p[0])
    return b.result("lista-de-aristas" if not adjlist else "adjlist")


def _collapse_blank(b: _Builder) -> None:
    """Nodos anonimos de RDF (``ex:Ana ex:vive [ ex:ciudad ex:Bilbao ]``): Ana vive_ciudad Bilbao, y sus literales a
    la nota de quien los usa. Asi el conocimiento queda entre entidades con nombre."""
    for _ in range(8):   # anidados: de fuera hacia dentro
        blank = {k for k, n in b.nodes.items() if k.startswith("_:") and n["name"] == _label(k)}
        inc: dict[str, list] = {}
        for e in b.edges:
            if e["dst"] in blank:
                inc.setdefault(e["dst"], []).append(e)
        ready = {k for k in blank if inc.get(k) and all(e["src"] not in blank for e in inc[k])}
        if not ready:
            break
        keep, outs = [], {}
        for e in b.edges:
            if e["src"] in ready:
                outs.setdefault(e["src"], []).append(e)
            elif e["dst"] not in ready:
                keep.append(e)
        for k in ready:
            for ei in inc[k]:
                for eo in outs.get(k, []):
                    keep.append({"src": ei["src"], "dst": eo["dst"], "rel": f"{ei['rel']}_{eo['rel']}"[:30],
                                 "weight": ei["weight"]})
                note = b.nodes[k]["note"]
                if note:
                    b.node(ei["src"], note=f"{ei['rel'].replace('_', ' ')}: {note}")
            del b.nodes[k]
        b.edges = keep


# ------------------------------------------------------------------ RDF de texto (N-Triples, N-Quads, Turtle, TriG, N3)
_TTL_TOKEN = re.compile(r'"""(?:.|\n)*?"""(?:@[\w-]+|\^\^\S+)?|\'\'\'(?:.|\n)*?\'\'\'(?:@[\w-]+|\^\^\S+)?|'
                        r'"(?:[^"\\]|\\.)*"(?:@[\w-]+|\^\^\S+)?|\'(?:[^\'\\]|\\.)*\'(?:@[\w-]+|\^\^\S+)?|<[^>\s]*>|'
                        r'@prefix|@base|PREFIX|BASE|GRAPH|[+-]?\d+\.\d+(?:[eE][+-]?\d+)?|[\[\](){};,.]|_:[\w.-]*\w|'
                        r'[\w-]*:[\w.\-%]*[\w\-%]|[\w-]*:|\ba\b|[^\s;,.\[\](){}]+')


_ESC = {"n": "\n", "t": "\t", "r": "", "b": "", "f": ""}


def _unescape(s: str) -> str:
    return re.sub(r"\\(u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|.)",
                  lambda m: chr(int(m.group(1)[1:], 16)) if m.group(1)[0] in "uU" and len(m.group(1)) > 1
                  else _ESC.get(m.group(1), m.group(1)), s)


def parse_rdf(text: str) -> dict:
    text = re.sub(r'(?m)^\s*#.*$', "", text)
    toks = _TTL_TOKEN.findall(text)
    prefixes: dict[str, str] = {}
    b = _Builder()
    triples = 0
    bnodes = [0]
    stack: list[tuple] = []

    def iri(t: str):
        if t.startswith("<"):
            return t[1:-1]
        if t == "a":
            return "type"
        if ":" in t and not t.startswith(("'", '"', "_:")):
            p, local = t.split(":", 1)
            return prefixes[p] + local if p in prefixes else local or p
        return t

    def lit(t: str):
        m = re.match(r'^("""|\'\'\'|"|\')(.*?)\1(?:@[\w-]+|\^\^\S+)?$', t, re.S)
        return _unescape(m.group(2)) if m else t

    def bnode():
        bnodes[0] += 1
        return f"_:b{bnodes[0]}"
    subj = pred = None
    last_obj = False   # N-Quads: un cuarto termino tras el objeto es el nombre del grafo
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in ("@prefix", "PREFIX"):
            if i + 2 < len(toks):
                prefixes[toks[i + 1].rstrip(":")] = toks[i + 2].strip("<>")
            i += 4 if t == "@prefix" and i + 3 < len(toks) and toks[i + 3] == "." else 3
            continue
        if t in ("@base", "BASE"):
            i += 3 if t == "@base" else 2
            continue
        if t == "GRAPH":   # TriG: GRAPH <g> { ... }
            i += 2
            continue
        if t in ("{", "}", "(", ")"):
            if t == "{" and subj is not None and pred is None and not stack:   # TriG: <g> { ... }
                subj = None
            i += 1
            continue
        if t == ".":
            subj = pred = None
            last_obj = False
        elif t == ";":
            pred = None
            last_obj = False
        elif t == ",":
            last_obj = False
        elif t == "[":
            nb = bnode()
            b.node(nb)
            if subj is not None and pred is not None:
                _rdf_triple(b, subj, pred, nb, False)
                triples += 1
            stack.append((subj, pred))
            subj, pred, last_obj = nb, None, False
        elif t == "]":
            nb = subj
            subj, pred = stack.pop() if stack else (None, None)
            if subj is None:   # [ ... ] como sujeto
                subj = nb
            last_obj = True
        elif subj is None:
            subj = iri(t)
        elif pred is None:
            pred = iri(t)
        elif last_obj:   # N-Quads: se ignora el grafo
            pass
        else:
            is_lit = t.startswith(('"', "'")) or bool(re.fullmatch(r"[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false", t))
            _rdf_triple(b, subj, pred, lit(t) if is_lit else iri(t), is_lit)
            triples += 1
            last_obj = True
        i += 1
    if not triples:
        raise GraphFormatError("no se han encontrado tripletas RDF")
    _collapse_blank(b)
    return b.result("rdf")


# ------------------------------------------------------------------ Cypher (Neo4j)
_CY_NODE = re.compile(r"\(\s*(`[^`]+`|\w+)?\s*((?::\s*(?:`[^`]+`|\w+)\s*)*)(\{[^{}]*\})?\s*\)")
_CY_REL = re.compile(r"^\s*(<)?-\s*(?:\[\s*(`[^`]+`|\w+)?\s*(?::\s*(`[^`]+`|\w+)(?:\s*\|\s*:?\s*(?:`[^`]+`|\w+))*)?"
                     r"\s*(?:\*[\d.]*)?\s*(\{[^{}]*\})?\s*\])?\s*-\s*(>)?\s*$")


def _cy_map(s: str | None, strings: list[str]) -> dict:
    out = {}
    if not s:
        return out
    for m in re.finditer(r"(`[^`]+`|\w+)\s*:\s*(\x01\d+\x01|\[[^\]]*\]|[^,}]+)", s[1:-1]):
        k, v = m.group(1).strip("`"), m.group(2).strip()
        v = re.sub(r"\x01(\d+)\x01", lambda x: strings[int(x.group(1))][1:-1], v)
        if v.startswith("["):
            v = ", ".join(_unq(x) for x in v[1:-1].split(",") if x.strip())
        out[k] = _unq(v)
    return out


def parse_cypher(text: str) -> dict:
    """CREATE / MERGE / MATCH ... CREATE de Neo4j, a mano o exportado con APOC (UNIQUE IMPORT ID)."""
    text, strings = _protect(text, r"'(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\"")
    text = re.sub(r"//[^\n]*", "", text)
    b = _Builder()
    var: dict[str, str] = {}
    by_key: dict[str, str] = {}
    clause_rx = re.compile(r"\b(OPTIONAL\s+MATCH|MATCH|CREATE|MERGE|WITH|RETURN|WHERE|SET|DETACH\s+DELETE|DELETE|"
                           r"REMOVE|UNWIND|CALL|FOREACH|ON\s+CREATE|ON\s+MATCH|ORDER\s+BY|LIMIT)\b", re.I)
    for stmt in re.split(r";\s*(?:\n|$)|^\s*:(?:begin|commit)\s*$", text, flags=re.M):
        local: dict[str, str] = {}

        def resolve(m, create: bool) -> str:
            v = (m.group(1) or "").strip("`")
            labels = [x.strip(" `") for x in (m.group(2) or "").split(":") if x.strip(" `")]
            props = _cy_map(m.group(3), strings)
            uid = props.pop("UNIQUE IMPORT ID", None)
            name, _ = _name_from(props)
            key = f"uid:{uid}" if uid is not None else (f"{labels[0] if labels else ''}:{name}" if name else None)
            nid = by_key.get(key) if key else None
            if nid is None and v:
                nid = local.get(v) or var.get(v)
            if nid is None:
                if not (key or v and create):   # MATCH (p:Persona) sin nombre: es una consulta, no un dato
                    return ""
                nid = key or f"var:{v}"
                if key:
                    by_key[key] = nid
            if create or key:
                if labels or props:
                    b.entity(nid, props, type=next((x for x in labels if x != "UNIQUE IMPORT LABEL"), None))
                elif nid not in b.nodes:
                    b.node(nid, v)
            if v:
                local[v] = var[v] = nid
            return nid
        parts = clause_rx.split(stmt)
        for kw, body in zip(parts[1::2], parts[2::2]):
            create = kw.upper() in ("CREATE", "MERGE")
            if not create and not kw.upper().endswith("MATCH"):
                continue
            nodes = list(_CY_NODE.finditer(body))
            ids = [resolve(n, create) for n in nodes]
            if not create:
                continue
            for k, (a_, c_) in enumerate(zip(nodes, nodes[1:])):
                m = _CY_REL.match(body[a_.end():c_.start()])
                if m and ids[k] and ids[k + 1]:
                    rel = (m.group(3) or "relacionado_con").strip("`")
                    src, dst = (ids[k + 1], ids[k]) if m.group(1) and not m.group(5) else (ids[k], ids[k + 1])
                    b.edge(src, dst, rel, _cy_map(m.group(4), strings).get("weight", 1))
    if not b.nodes:
        raise GraphFormatError("no hay patrones (n)-[:REL]->(m) de Cypher")
    for n in b.nodes.values():   # las variables sin propiedades se quedan con su nombre
        if n["id"].startswith("var:") and n["name"] == _label(n["id"]):
            n["name"] = _label(n["id"][4:])
    return b.result("cypher")


# ------------------------------------------------------------------ diagramas de texto (Mermaid, PlantUML, flechas)
_MM_SHAPE = re.compile(r"(?<![-=.\w])([\w]+)\s*(\[\[.*?\]\]|\[\(.*?\)\]|\(\(\(.*?\)\)\)|\(\(.*?\)\)|\(\[.*?\]\)|"
                       r"\{\{.*?\}\}|\[/.*?/\]|\[\\.*?\\\]|\[.*?\]|\(.*?\)|\{.*?\}|>.*?\])")
_ARROW_NORM = [
    (re.compile(r"\s*(?:-{2,}|={2,}|-\.+-?)\s*>?\s*\|([^|]*)\|\s*"), r" -[\1]-> "),                # A -->|x| B
    (re.compile(r"\s+(?:--|==|-\.)\s+([^->=.|][^>|]*?)\s+(?:-->|==>|\.->|---|===)\s*"), r" -[\1]-> "),  # A -- x --> B
    (re.compile(r"\s*-{2,}\s*([^\s\-<>\[\]|][^<>\[\]|]*?)\s*-{2,}>\s*"), r" -[\1]-> "),             # A --x--> B
    (re.compile(r"\s*-+(?:up|down|left|right|u|d|l|r)-+>\s*", re.I), " -> "),                         # PlantUML -down->
]
_ARROW = re.compile(r"\s*(<-\[([^\]]*)\]-+|-+\[([^\]]*)\]->|<-+|<=+|<\.+-|-+>|=+>|-\.+->|\.+>|→|⇒|⟶|←|"
                    r"(?<=\s)-{2,}(?=\s)|(?<=\s)={2,}(?=\s)|\s—\s|↔|<->)\s*")


def _clean_end(s: str) -> str:
    s = s.strip().strip(";").strip()
    s = re.sub(r"^\[(.*)\]$|^\((.*)\)$|^:(.*):$", lambda m: next(g for g in m.groups() if g is not None), s)
    s = re.sub(r"^(?:class|interface|actor|participant|component|node|usecase|entity|database|object)\s+", "", s,
               flags=re.I)
    return _unq(s.strip())


def _arrow_text(lines: list[str], b: _Builder, labels: dict[str, str] | None = None) -> int:
    labels = labels or {}
    made = 0
    for line in lines:
        rel3 = None
        if " : " in line or re.search(r"\S\s*:\s*\S", line.split("]")[-1]):   # A -> B : relacion
            m = re.match(r"^(.*?[^:\s])\s*:\s*([^:]+)$", line)
            if m and _ARROW.search(m.group(1)):
                line, rel3 = m.group(1), m.group(2).strip().strip('"')
        for rx, rep in _ARROW_NORM:
            line = rx.sub(rep, line)
        parts = _ARROW.split(line)
        if len(parts) < 4:
            continue
        ends = parts[0::4]
        for k in range(len(ends) - 1):
            op, rev_lab, fwd_lab = parts[4 * k + 1], parts[4 * k + 2], parts[4 * k + 3]
            rel = rev_lab if rev_lab is not None else fwd_lab
            rel = rel if rel else rel3
            reverse = op.startswith("<") and not op.endswith(">")
            for a in ends[k].split("&"):
                for c in ends[k + 1].split("&"):
                    a_, c_ = _clean_end(a), _clean_end(c)
                    if not a_ or not c_:
                        continue
                    for x in (a_, c_):
                        b.node(x, labels.get(x, x))
                    b.edge(c_, a_, rel) if reverse else b.edge(a_, c_, rel)
                    made += 1
    return made


def parse_mermaid(text: str) -> dict:
    lines = [ln.strip() for ln in re.split(r"[\n;]", text) if ln.strip() and not ln.strip().startswith("%%")]
    b = _Builder()
    kind = lines[0].split()[0].lower() if lines else ""
    if kind == "erdiagram":   # CUSTOMER ||--o{ ORDER : places
        for ln in lines[1:]:
            m = re.match(r'^"?([\w-]+)"?\s+[|}o{]{1,2}(?:--|\.\.)[|}o{]{1,2}\s+"?([\w-]+)"?\s*:\s*"?([^"]+)"?', ln)
            if m:
                b.edge(m.group(1), m.group(2), m.group(3))
        return b.result("mermaid")
    if kind == "mindmap":
        return _outline(text.split("\n", 1)[1] if "\n" in text else "", b, "mermaid", shapes=True)
    labels: dict[str, str] = {}
    body = []
    for ln in lines[1:] if kind in ("graph", "flowchart", "classdiagram", "statediagram", "statediagram-v2") else lines:
        if re.match(r"^(subgraph|end|classDef|class |style|linkStyle|click|direction)\b", ln):
            continue
        ln = re.sub(r"^\s*class\s+", "", ln) if kind == "classdiagram" else ln

        def shape(m):
            labels[m.group(1)] = re.sub(r"^[\[({>/\\]+|[\])}/\\]+$", "", m.group(2)).strip().strip('"')
            return m.group(1)
        ln = _MM_SHAPE.sub(shape, ln)
        ln = re.sub(r"\s*(<\|--|--\|>|\*--|--\*|o--|--o|\.\.>|<\.\.|\.\.\|>)\s*", " --> ", ln)   # classDiagram
        body.append(ln)
    made = _arrow_text(body, b, labels)
    for k, v in labels.items():
        b.node(k, v)
    if not made and not b.nodes:
        raise GraphFormatError("el diagrama Mermaid no tiene enlaces")
    return b.result("mermaid")


def parse_plantuml(text: str) -> dict:
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and not re.match(
        r"^(@start|@end|skinparam|title|left to right|top to bottom|hide|show|!|'|note|end note|legend|"
        r"endlegend|package|namespace|}|{)", ln.strip(), re.I)]
    b = _Builder()
    if not _arrow_text(lines, b):
        raise GraphFormatError("el diagrama PlantUML no tiene flechas")
    return b.result("plantuml")


def _outline(text: str, b: _Builder, fmt: str, shapes: bool = False) -> dict:
    """Esquemas con sangria o listas Markdown (y titulos #): cada elemento ``contiene`` a los de debajo."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    tree = sum(1 for ln in lines if re.match(r"^\s*(?:[-*+]|\d+[.)])\s+\S|^#{1,6}\s", ln) or
               re.match(r"^\s+\S", ln) and len(ln) < 100 and not re.search(r"[;{}=<>]|\(.*\)", ln))
    if not shapes and tree < max(2, len(lines) * 0.6):
        raise GraphFormatError("el texto no tiene estructura de grafo (flechas, tablas o esquema con sangria)")
    stack: list[tuple[int, str]] = []
    made = 0
    for raw in text.splitlines():
        if not raw.strip() or raw.strip().startswith(("```", "%%", "<!--")):
            continue
        h = re.match(r"^(#{1,6})\s+(.*)$", raw)
        if h:
            level, label = len(h.group(1)) - 7, h.group(2)   # los titulos siempre por encima de las listas
        else:
            m = re.match(r"^(\s*)(?:[-*+]|\d+[.)])?\s*(.*)$", raw.expandtabs(4))
            level, label = len(m.group(1)), m.group(2)
        label = re.sub(r"\[\[([^\]|]+)(?:\|[^\]]*)?\]\]", r"\1", label)
        label = re.sub(r"\*\*|__|`", "", label).strip()
        if shapes:
            label = re.sub(r"^[\w-]*[\[({]+(.*?)[\])}]+$", r"\1", label)
        if not label or len(label) > 200:
            continue
        while stack and stack[-1][0] >= level:
            stack.pop()
        nid = (stack[-1][1] + "/" if stack else "") + label[:60]
        b.node(nid, label)
        if stack:
            b.edge(stack[-1][1], nid, "contiene")
            made += 1
        stack.append((level, nid))
    if not made:
        raise GraphFormatError("el texto no tiene estructura de grafo (flechas, tablas o esquema con sangria)")
    return b.result(fmt, generic=True)


WIKILINK = re.compile(r"\[\[([^\]|#]+)(?:#[^\]|]*)?(?:\|[^\]]*)?\]\]")


def parse_wiki_notes(notes: dict[str, str]) -> dict | None:
    """{nombre_de_nota: texto} -> grafo de notas enlazadas (None si no hay enlaces)."""
    from .graph import _key
    b = _Builder()
    found = 0
    for name, text in notes.items():
        b.node(_key(name) or name, name, type="nota", note=re.sub(r"\s+", " ", text)[:300])
        for m in WIKILINK.finditer(text):
            target = m.group(1).strip()
            if target:
                b.node(_key(target) or target, target, type="nota")
                b.edge(_key(name) or name, _key(target) or target, "enlaza_con")
                found += 1
    return b.result("markdown-wiki") if found else None


def parse_text(text: str, ext: str = "", name: str = "") -> dict:
    """Texto sin formato declarado: flechas, ``A: B, C``, tabla, lista de aristas o esquema con sangria."""
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith(("#", "//", "%"))]
    if not lines:
        raise GraphFormatError("fichero vacio")
    b = _Builder()
    if ext in (".md", ".markdown") and WIKILINK.search(text):
        return parse_wiki_notes({Path(name).stem or "nota": text})
    arrows = [ln for ln in lines if _ARROW.search(ln) and not re.match(r"^\s*[-*+]\s", ln)]
    if arrows and len(arrows) >= max(1, len(lines) // 3):
        if _arrow_text(arrows, b):
            return b.result("flechas")
    adj = [re.match(r"^\s*([^:]{1,80}?)\s*:\s*(.+)$", ln) for ln in lines]
    if all(adj) and all("," in m.group(2) or len(m.group(2)) < 60 for m in adj) and ext not in (".md", ".markdown"):
        for m in adj:
            for d in re.split(r"\s*[,;]\s*", m.group(2)):
                if d.strip():
                    b.edge(m.group(1).strip(), d.strip())
        return b.result("adyacencia")
    if ext not in (".md", ".markdown"):
        delim = _sniff_delimiter("\n".join(lines[:50]))
        counts = {ln.count(delim) for ln in lines[:200]}
        if delim in "".join(lines[:50]) and len(counts) == 1 and all(len(c) < 120 for ln in lines[:50] for c in ln.split(delim)):
            try:
                return parse_table("\n".join(lines), delim)
            except GraphFormatError:
                pass
        toks = [len(_split(ln)) for ln in lines[:200]]
        if all(2 <= t <= 3 for t in toks) and all(len(ln) < 160 for ln in lines[:200]):
            return parse_edgelist("\n".join(lines))
    return _outline(text, b, "esquema")


# ------------------------------------------------------------------ contenedores (comprimidos y varios ficheros)
TABLE_FORMATS = {"csv", "tsv", "tabla", "excel", "ods", "sqlite"}


def merge(parts: list[tuple[str, dict]]) -> dict:
    """Une varios grafos leidos. Las tablas comparten ids (nodes.csv + edges.csv); el resto, cada uno los suyos."""
    b = _Builder()
    fmts: list[str] = []
    for i, (name, g) in enumerate(parts):
        shared = g.get("format") in TABLE_FORMATS
        pre = "" if shared else f"{i}:"
        fmts.append(g.get("format", "?"))
        for n in g["nodes"]:
            nid = pre + n["id"]
            new = nid not in b.nodes
            b.node(nid, n["name"], type=n.get("type") if n.get("type") != "concepto" else None,
                   category=n.get("category"), note=n.get("note"), weight=n.get("weight"))
            if new and "scope" in n:
                b.nodes[nid]["scope"] = n["scope"]
        for e in g["edges"]:
            b.edge(pre + e["src"], pre + e["dst"], e["rel"], e.get("weight", 1))
    out = b.result("+".join(dict.fromkeys(fmts)) if len(set(fmts)) > 1 else (fmts[0] if fmts else "varios"),
                   generic=all(g.get("generic") for _, g in parts))
    out["files"] = [{"file": n, "format": g.get("format"), "nodes": len(g["nodes"]), "edges": len(g["edges"])}
                    for n, g in parts]
    return out


def _limited(read, limit=MAX_BYTES) -> bytes:
    data = read(limit + 1)
    if len(data) > limit:
        raise GraphFormatError(f"descomprimido supera {limit // (1024 * 1024)} MB")
    return data


def decompress(data: bytes, name: str) -> tuple[bytes, str]:
    """gz, bz2 y xz de un solo fichero: devuelve el contenido y el nombre sin la extension del compresor."""
    for magic, opener, exts in ((b"\x1f\x8b", gzip.open, (".gz", ".tgz")), (b"BZh", bz2.open, (".bz2",)),
                                (b"\xfd7zXZ\x00", lzma.open, (".xz",))):
        if data[:len(magic)] == magic:
            with opener(io.BytesIO(data)) as f:
                data = _limited(f.read)
            low = name.lower()
            for e in exts:
                if low.endswith(e):
                    name = name[:-len(e)] + (".tar" if e == ".tgz" else "")
            return decompress(data, name)
    return data, name


def _members(data: bytes) -> list[tuple[str, bytes]]:
    out, total = [], 0
    if data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist()[:5000]:
                if info.is_dir():
                    continue
                total += info.file_size
                if total > MAX_BYTES:
                    raise GraphFormatError("el ZIP descomprimido es demasiado grande")
                out.append((info.filename, z.read(info)))
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            for m in t.getmembers()[:5000]:
                if not m.isfile():
                    continue
                total += m.size
                if total > MAX_BYTES:
                    raise GraphFormatError("el TAR descomprimido es demasiado grande")
                out.append((m.name, t.extractfile(m).read()))
    return [(n, d) for n, d in out if not any(p.startswith(".") or p == "__MACOSX" for p in PurePosixPath(n).parts)]


def _is_tar(data: bytes) -> bool:
    return len(data) > 262 and data[257:262] == b"ustar"


def parse_archive(data: bytes, name: str) -> dict:
    members = _members(data)
    names = {PurePosixPath(n).as_posix() for n, _ in members}
    if "xl/workbook.xml" in names:
        return parse_spreadsheet(data, "x.xlsx")
    mt = next((d for n, d in members if n == "mimetype"), b"")
    if b"opendocument.spreadsheet" in mt:
        return parse_spreadsheet(data, "x.ods")
    if "content.json" in names and ({"metadata.json", "manifest.json"} & names):   # XMind
        return parse_json(json.loads(next(d for n, d in members if n == "content.json")))
    if "content.xml" in names and b"xmap-content" in next(d for n, d in members if n == "content.xml")[:400]:
        return parse_xml(next(d for n, d in members if n == "content.xml").decode("utf-8", "replace"))
    if "word/document.xml" in names or b"opendocument.text" in mt or "ppt/presentation.xml" in names:
        raise GraphFormatError("es un documento de oficina, no un grafo: importalo en la Biblioteca")
    parts, notes, errors = [], {}, []
    for n, d in sorted(members, key=lambda x: (not re.search(r"node|nodo|vert|entit", x[0], re.I), x[0])):
        p = PurePosixPath(n)
        if p.suffix.lower() in (".md", ".markdown"):
            notes[p.stem] = d.decode("utf-8", "replace")
            continue
        if p.suffix.lower() in IGNORE_IN_ARCHIVE:
            continue
        try:
            g = parse_bytes(d, p.name)
        except (GraphFormatError, ValueError, ET.ParseError, UnicodeError, sqlite3.Error):
            errors.append(n)
            continue
        if not g.get("generic"):
            parts.append((n, g))
    wiki = parse_wiki_notes(notes) if notes else None
    if wiki:
        parts.append(("notas [[enlazadas]]", wiki))
    if not parts:
        raise GraphFormatError("el comprimido no trae ningun grafo reconocible"
                               + (f" ({len(errors)} ficheros no se pudieron leer)" if errors else ""))
    g = merge(parts) if len(parts) > 1 else {**parts[0][1], "files": [{"file": parts[0][0],
                                                                      "format": parts[0][1]["format"]}]}
    if errors:
        g["unread"] = errors[:50]
    return g


IGNORE_IN_ARCHIVE = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".pdf", ".css", ".js", ".html", ".htm", ".py",
                     ".exe", ".dll", ".so", ".bin", ".pyc", ".ico", ".woff", ".woff2", ".ttf", ".mp3", ".mp4"}


# ------------------------------------------------------------------ deteccion
def _decode(data: bytes) -> str:
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16")
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp1252", errors="replace")


EXT_FORMAT = {
    ".graphml": "xml", ".gexf": "xml", ".xgmml": "xml", ".dgml": "xml", ".gxl": "xml", ".rdf": "xml", ".owl": "xml",
    ".xml": "xml", ".mm": "xml", ".opml": "xml",
    ".json": "json", ".jsonld": "json", ".cyjs": "json", ".cx": "json", ".cx2": "json", ".canvas": "json",
    ".graphson": "json", ".jgf": "json",
    ".jsonl": "jsonl", ".ndjson": "jsonl",
    ".yaml": "yaml", ".yml": "yaml", ".toml": "toml",
    ".csv": "table", ".tsv": "table", ".tab": "table",
    ".dot": "dot", ".gv": "dot", ".gml": "gml", ".net": "pajek", ".paj": "pajek", ".gdf": "gdf", ".dl": "ucinet",
    ".tgf": "tgf", ".sif": "sif", ".lgl": "lgl", ".ncol": "edgelist", ".edgelist": "edgelist", ".edges": "edgelist",
    ".el": "edgelist", ".wel": "edgelist", ".adjlist": "adjlist",
    ".nt": "rdf", ".nq": "rdf", ".ttl": "rdf", ".trig": "rdf", ".n3": "rdf",
    ".cypher": "cypher", ".cql": "cypher", ".cyp": "cypher",
    ".mmd": "mermaid", ".mermaid": "mermaid", ".puml": "plantuml", ".plantuml": "plantuml", ".pu": "plantuml",
    ".md": "text", ".markdown": "text", ".txt": "text",
}


def detect(text: str, ext: str = "") -> str:
    """Formato por el contenido; la extension decide solo cuando el contenido no lo deja claro."""
    head = text.lstrip()[:2000]
    low = head.lower()
    if ext in (".toml", ".yaml", ".yml"):
        return "toml" if ext == ".toml" else "yaml"
    if re.match(r"^(?:<[^>\s]+>|_:\S+)\s+<[^>\s]+>\s+\S", head):   # N-Triples / N-Quads
        return "rdf"
    if re.match(r"^<(?:\?xml|!--|!doctype|[a-z_][\w.:-]*[\s>/])", low):
        return "xml"
    if head.startswith(("{", "[")) and ext not in (".ttl", ".n3", ".trig", ".gml"):
        return "json"
    if re.match(r"^(?:strict\s+)?(?:di)?graph\s*(?:\"[^\"]*\"|[\w.]+)?\s*\{", low):
        return "dot"
    if re.match(r"^(?:(?:creator|version|comment)\s+[^\n]*\s+)*graph\s*\[", low):
        return "gml"
    if re.match(r"^(?:flowchart|graph)\s*(?:tb|td|bt|rl|lr)?\s*(?:;|\n|$)|^(?:erdiagram|classdiagram|mindmap|"
                r"statediagram(?:-v2)?)\b", low):
        return "mermaid"
    if low.startswith("@startuml") or low.startswith("@startmindmap"):
        return "plantuml" if low.startswith("@startuml") else "text"
    if re.match(r"^\*(?:network|vertices)\b", low):
        return "pajek"
    if re.match(r"^nodedef\s*>", low):
        return "gdf"
    if re.match(r"^dl\s+(?:n|nr|nm)\s*=", low):
        return "ucinet"
    if re.match(r"^(?:@prefix|@base|prefix\s|base\s)", low):
        return "rdf"
    if re.search(r"(?im)^\s*(?:create|merge|match)\s*\(", head) or re.search(r"\(\s*\w*\s*:\s*\w+[^)]*\)\s*-\s*\[", head):
        return "cypher"
    if ext in EXT_FORMAT and EXT_FORMAT[ext] not in ("xml", "json"):
        return EXT_FORMAT[ext]
    if low.startswith("---"):
        return "yaml"
    if re.match(r"^[^\n#]+\n(?:[^\n#]*\n)*#\s*\n", head + "\n") and ext in ("", ".tgf"):
        return "tgf"
    return "text"


BINARY_MAGIC = (b"%PDF-", b"\x89PNG", b"\xff\xd8\xff", b"GIF8", b"\x7fELF", b"MZ", b"RIFF", b"OggS", b"ID3",
                b"\x00\x00\x00", b"Rar!", b"7z\xbc\xaf", b"\xd0\xcf\x11\xe0", b"II*\x00", b"MM\x00*", b"wOFF", b"BM")
NOT_GRAPH_EXT = {".py", ".js", ".ts", ".css", ".html", ".htm", ".java", ".c", ".h", ".cpp", ".cs", ".go", ".rs",
                 ".sh", ".bat", ".ps1", ".php", ".rb", ".kt", ".swift", ".sql", ".nsi", ".ini", ".cfg", ".log",
                 ".svg", ".pdf", ".doc", ".docx", ".odt", ".rtf", ".epub", ".pptx", ".odp"}


def _binary(data: bytes) -> bool:
    if data.startswith(BINARY_MAGIC) and not data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return True
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return False
    head = data[:4096]
    ctrl = sum(1 for x in head if x < 9 or 13 < x < 32)
    return ctrl > len(head) * 0.05


def parse_bytes(data: bytes, name: str = "") -> dict:
    """Cualquier grafo -> esquema interno. Lanza GraphFormatError si no se reconoce."""
    if not data:
        raise GraphFormatError("fichero vacio")
    data, name = decompress(data, name)
    if data[:4] == b"PK\x03\x04" or _is_tar(data):
        return parse_archive(data, name)
    if data[:16] == b"SQLite format 3\x00":
        return parse_sqlite(data)
    if _binary(data):
        raise GraphFormatError(f"{name or 'el fichero'} es binario o un documento, no un grafo")
    ext = Path(name).suffix.lower()
    text = _decode(data)
    fmt = detect(text, ext)
    if fmt == "text" and ext in NOT_GRAPH_EXT:
        raise GraphFormatError(f"{name} no es un grafo (codigo, pagina web o documento): importalo en la Biblioteca")
    if fmt == "xml":
        return parse_xml(text)
    if fmt == "json":
        try:
            return parse_json(json.loads(text))
        except json.JSONDecodeError:
            return parse_jsonl(text)
    if fmt == "jsonl":
        return parse_jsonl(text)
    if fmt == "table":
        return parse_table(text, "\t" if ext in (".tsv", ".tab") else None)
    if fmt == "edgelist":
        return parse_edgelist(text)
    if fmt == "adjlist":
        return parse_edgelist(text, adjlist=True)
    if fmt == "text":
        return parse_text(text, ext, name)
    return {"dot": parse_dot, "gml": parse_gml, "mermaid": parse_mermaid, "plantuml": parse_plantuml,
            "pajek": parse_pajek, "gdf": parse_gdf, "ucinet": parse_ucinet, "rdf": parse_rdf, "cypher": parse_cypher,
            "yaml": parse_yaml, "toml": parse_toml, "tgf": parse_tgf, "sif": parse_sif, "lgl": parse_lgl}[fmt](text)


def parse_many(files: list[tuple[str, bytes]]) -> dict:
    """Varios ficheros de una vez (p. ej. nodes.csv y edges.csv de Neo4j o Gephi): un solo grafo."""
    parts = [(n, parse_bytes(d, n)) for n, d in files]
    if len(parts) == 1:
        return parts[0][1]
    order = sorted(parts, key=lambda p: (len(p[1]["edges"]) > 0, p[0]))   # primero las tablas de nodos
    return merge(order)


# ------------------------------------------------------------------ lista para la interfaz y la documentacion
FORMATS = [
    ("Nexus-Office", ".json", "exportación propia (respeta el grafo general y el de cada agente) o su base SQLite"),
    ("JSON node-link", ".json", "D3, vis.js, sigma.js, NetworkX (node_link y adjacency), JSON Graph Format"),
    ("Cytoscape", ".cyjs .cx .cx2 .sif .xgmml", "Cytoscape.js, CX/CX2, SIF y XGMML"),
    ("Neo4j", ".json .jsonl .csv .cypher", "Browser, API HTTP, APOC (json, cypher, graphml), CSV de neo4j-admin"),
    ("Cypher", ".cypher .cql", "CREATE / MERGE / MATCH ... CREATE"),
    ("JSON-LD", ".jsonld .json", "schema.org, Wikidata, @graph"),
    ("GraphSON", ".json .graphson", "TinkerPop, JanusGraph, Amazon Neptune"),
    ("RDF", ".ttl .nt .nq .trig .n3 .rdf .owl", "Turtle, N-Triples, N-Quads, TriG, N3, RDF/XML y OWL"),
    ("GraphML", ".graphml", "yEd, Gephi, NetworkX, igraph, APOC"),
    ("GEXF", ".gexf", "Gephi, con atributos"),
    ("GML", ".gml", "Graph Modelling Language: igraph, NetworkX, Cytoscape"),
    ("Graphviz DOT", ".dot .gv", "digraph / graph"),
    ("Pajek, GDF, UCINET, TGF, LGL, NCOL", ".net .gdf .dl .tgf .lgl .ncol", "análisis de redes"),
    ("Tablas", ".csv .tsv .xlsx .ods .db .sqlite", "aristas, nodos, matriz de adyacencia, tripletas, ConceptNet"),
    ("Listas", ".edgelist .adjlist .txt", "a b [peso], A: B, C, una tripleta por línea"),
    ("Diagramas", ".mmd .puml .txt", "Mermaid (flowchart, erDiagram, classDiagram, mindmap), PlantUML, A -> B"),
    ("Mapas mentales y esquemas", ".mm .opml .xmind .md", "FreeMind/Freeplane, OPML, XMind, listas con sangría"),
    ("Notas enlazadas", ".md .zip", "Obsidian, Logseq, Foam ([[enlaces]]) y Obsidian Canvas (.canvas)"),
    ("YAML / TOML", ".yaml .yml .toml", "con nodos y aristas, tripletas o cualquier estructura con nombres"),
    ("Comprimidos", ".zip .tar .gz .bz2 .xz", "uno o varios grafos dentro (p. ej. nodes.csv + edges.csv)"),
]
IMPORT_EXT = sorted({e for e in EXT_FORMAT} | {".xlsx", ".ods", ".db", ".sqlite", ".sqlite3", ".zip", ".tar",
                                               ".tgz", ".gz", ".bz2", ".xz", ".xmind"})
