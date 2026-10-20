"""Importar y exportar grafos de conocimiento en formatos habituales.

Lectura (se detecta el formato por la extension y el contenido):

* JSON: el propio de Nexus-Office (``nexus-kg``), nodos/enlaces de D3 o vis.js, Cytoscape (``elements``), JSON Graph Format
  (``graph.nodes``/``graph.edges``) y listas de tripletas (``s/r/o``, ``subject/predicate/object`` o ``[s, r, o]``).
* JSON Lines (una tripleta por linea), CSV y TSV (``origen,destino[,relacion][,peso]``, ``s,r,o`` o sin cabecera con
  tres columnas), GraphML, GEXF, Graphviz DOT, RDF N-Triples y Turtle (lo basico: prefijos, ``;``, ``,`` y ``a``) y
  notas Markdown con enlaces ``[[wiki]]`` (Obsidian, Logseq...).

Todo se normaliza a ``{"nodes": [{id, name, type, category, note, scope?}], "edges": [{src, dst, rel, weight}]}`` y
``insert_graph`` lo incorpora al grafo general, al de un agente o reparte cada nodo con el organizador ("auto").
"""
from __future__ import annotations

import csv
import io
import json
import re
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from .graph import CATEGORIES, GENERAL, KnowledgeGraph, _key

GRAPH_EXT = {".json", ".jsonl", ".ndjson", ".csv", ".tsv", ".graphml", ".gexf", ".dot", ".gv", ".nt", ".ttl"}
MAX_NODES = 200_000


class GraphFormatError(ValueError):
    pass


def _label(value) -> str:
    """Nombre legible de un id: el final de una IRI, sin guiones bajos."""
    s = str(value if value is not None else "").strip()
    if s.startswith("<") and s.endswith(">"):
        s = s[1:-1]
    if re.match(r"^[a-z][\w+.-]*://", s, re.I) or s.startswith("urn:"):
        s = re.split(r"[#/:]", s.rstrip("/#"))[-1] or s
    return re.sub(r"[_]+", " ", s).strip()[:60]


def _rel(value) -> str:
    s = _label(value) or "relacionado_con"
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", s)   # camelCase -> snake
    return re.sub(r"[^\w]+", "_", s.lower()).strip("_")[:30] or "relacionado_con"


class _Builder:
    def __init__(self) -> None:
        self.nodes: dict[str, dict] = {}
        self.edges: list[dict] = []

    def node(self, nid, name=None, **extra) -> str:
        nid = str(nid)
        if nid not in self.nodes:
            if len(self.nodes) >= MAX_NODES:
                raise GraphFormatError(f"el grafo tiene mas de {MAX_NODES} nodos")
            self.nodes[nid] = {"id": nid, "name": _label(name if name not in (None, "") else nid), "type": "concepto",
                               "category": "", "note": ""}
        n = self.nodes[nid]
        if name not in (None, "") and n["name"] == _label(nid):
            n["name"] = _label(name)
        for k, v in extra.items():
            if v not in (None, ""):
                n[k] = str(v)[:500] if k != "weight" else v
        return nid

    def edge(self, src, dst, rel=None, weight=1.0) -> None:
        s, d = self.node(src), self.node(dst)
        if s == d:
            return
        try:
            w = float(weight)
        except (TypeError, ValueError):
            w = 1.0
        self.edges.append({"src": s, "dst": d, "rel": _rel(rel), "weight": w})

    def result(self, fmt: str) -> dict:
        nodes = [n for n in self.nodes.values() if n["name"]]
        if not nodes:
            raise GraphFormatError("no se ha encontrado ningun nodo")
        return {"format": fmt, "nodes": nodes, "edges": self.edges}


# ------------------------------------------------------------------ JSON
def _first(d: dict, *keys, default=None):
    for k in keys:
        if isinstance(d, dict) and d.get(k) not in (None, ""):
            return d[k]
    return default


def _triple(b: _Builder, t) -> bool:
    if isinstance(t, (list, tuple)) and len(t) >= 3:
        b.edge(b.node(t[0], t[0]), b.node(t[2], t[2]), t[1])
        return True
    if isinstance(t, dict):
        s, r, o = _first(t, "s", "subject", "sujeto", "head", "from"), _first(t, "r", "p", "predicate", "relation",
                                                                             "relacion", "rel"), \
            _first(t, "o", "object", "objeto", "tail", "to")
        if s is not None and o is not None:
            b.node(s, s)
            b.node(o, o, type=t.get("type"), category=t.get("category") or t.get("categoria"))
            b.edge(s, o, r, t.get("weight", 1.0))
            return True
    return False


def parse_json(data) -> dict:
    b = _Builder()
    if isinstance(data, dict) and data.get("format") == "nexus-kg":
        for n in data.get("nodes", []):
            b.node(n["id"], n.get("name"), type=n.get("type"), category=n.get("category"), note=n.get("note"),
                   scope=n.get("scope", ""), weight=n.get("weight"))
        for e in data.get("edges", []):
            b.edge(e["src"], e["dst"], e.get("rel"), e.get("weight", 1.0))
        return b.result("nexus-kg")
    if isinstance(data, dict) and "elements" in data:   # Cytoscape
        el = data["elements"]
        items = el if isinstance(el, list) else list(el.get("nodes", [])) + list(el.get("edges", []))
        for it in items:
            d = it.get("data", it)
            if "source" in d and "target" in d:
                b.edge(d["source"], d["target"], _first(d, "label", "interaction", "relation", "name"), d.get("weight", 1))
            elif "id" in d:
                b.node(d["id"], _first(d, "label", "name"), type=d.get("type"), note=d.get("description"))
        return b.result("cytoscape")
    g = data.get("graph") if isinstance(data, dict) and isinstance(data.get("graph"), dict) else data
    if isinstance(g, dict) and ("nodes" in g or "edges" in g or "links" in g):
        nodes = g.get("nodes", [])
        if isinstance(nodes, dict):   # JSON Graph Format: {id: {label...}}
            nodes = [{"id": k, **(v or {})} for k, v in nodes.items()]
        for n in nodes:
            if isinstance(n, dict):
                meta = n.get("metadata") or {}
                b.node(_first(n, "id", "key", "name", "label"), _first(n, "label", "name", "title"),
                       type=_first(n, "type", "group", default=meta.get("type")), category=n.get("category"),
                       note=_first(n, "description", "note", "title", default=meta.get("description")))
            else:
                b.node(n, n)
        for e in list(g.get("edges", [])) + list(g.get("links", [])):
            if isinstance(e, dict):
                s, t = _first(e, "source", "src", "from"), _first(e, "target", "dst", "to")
                if isinstance(s, dict):   # d3 tras la simulacion: objetos en vez de ids
                    s = _first(s, "id", "name")
                if isinstance(t, dict):
                    t = _first(t, "id", "name")
                if s is not None and t is not None:
                    b.edge(s, t, _first(e, "relation", "rel", "label", "type", "predicate"), e.get("weight", e.get("value", 1)))
            elif isinstance(e, (list, tuple)) and len(e) >= 2:
                b.edge(e[0], e[-1], e[1] if len(e) > 2 else None)
        return b.result("json")
    rows = data if isinstance(data, list) else (data.get("triples") or data.get("facts") or []) if isinstance(data, dict) else []
    n = sum(1 for t in rows if _triple(b, t))
    if not n:
        raise GraphFormatError("el JSON no tiene nodos/enlaces ni tripletas reconocibles")
    return b.result("triples")


# ------------------------------------------------------------------ tablas
SRC = ("source", "src", "from", "origen", "s", "subject", "sujeto", "head", "node1")
DST = ("target", "dst", "to", "destino", "o", "object", "objeto", "tail", "node2")
REL = ("relation", "rel", "label", "type", "r", "p", "predicate", "relacion", "relación", "edge", "interaction")


def parse_table(text: str, delimiter: str | None = None) -> dict:
    sample = text[:4096]
    if delimiter is None:
        delimiter = "\t" if sample.count("\t") > sample.count(",") else ";" if sample.count(";") > sample.count(",") else ","
    rows = [r for r in csv.reader(io.StringIO(text), delimiter=delimiter) if any(c.strip() for c in r)]
    if not rows:
        raise GraphFormatError("tabla vacia")
    head = [c.strip().lower() for c in rows[0]]
    b = _Builder()

    def col(names):
        return next((i for i, h in enumerate(head) if h in names), None)
    si, di, ri = col(SRC), col(DST), col(REL)
    wi = col(("weight", "peso", "value"))
    if si is not None and di is not None:
        body = rows[1:]
    elif len(head) >= 3:
        si, ri, di, body = 0, 1, 2, rows   # sin cabecera: sujeto, relacion, objeto
    else:
        raise GraphFormatError("la tabla necesita columnas origen/destino o tres columnas sujeto,relacion,objeto")
    for r in body:
        if len(r) > max(si, di) and r[si].strip() and r[di].strip():
            b.node(r[si].strip(), r[si].strip())
            b.node(r[di].strip(), r[di].strip())
            b.edge(r[si].strip(), r[di].strip(), r[ri] if ri is not None and ri < len(r) else None,
                   r[wi] if wi is not None and wi < len(r) else 1.0)
    return b.result("csv" if delimiter == "," else "tsv")


def parse_jsonl(text: str) -> dict:
    b = _Builder()
    n = 0
    for line in text.splitlines():
        line = line.strip()
        if line:
            try:
                n += _triple(b, json.loads(line))
            except json.JSONDecodeError:
                continue
    if not n:
        raise GraphFormatError("ninguna linea es una tripleta JSON")
    return b.result("jsonl")


# ------------------------------------------------------------------ XML
def _strip(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_graphml(text: str) -> dict:
    root = ET.fromstring(text)
    keys = {k.get("id"): (k.get("attr.name") or k.get("id") or "").lower() for k in root.iter() if _strip(k.tag) == "key"}
    b = _Builder()

    def data(el) -> dict:
        return {keys.get(d.get("key"), d.get("key")): (d.text or "").strip() for d in el if _strip(d.tag) == "data"}
    for el in root.iter():
        t = _strip(el.tag)
        if t == "node":
            d = data(el)
            b.node(el.get("id"), _first(d, "label", "name", "title"), type=d.get("type"), category=d.get("category"),
                   note=_first(d, "description", "note"))
        elif t == "edge":
            d = data(el)
            b.edge(el.get("source"), el.get("target"), _first(d, "label", "relation", "rel", "type", "predicate"),
                   d.get("weight", 1.0))
    return b.result("graphml")


def parse_gexf(text: str) -> dict:
    root = ET.fromstring(text)
    b = _Builder()
    for el in root.iter():
        t = _strip(el.tag)
        if t == "node":
            b.node(el.get("id"), el.get("label"))
        elif t == "edge":
            b.edge(el.get("source"), el.get("target"), el.get("label") or el.get("kind"), el.get("weight", 1.0))
    return b.result("gexf")


# ------------------------------------------------------------------ DOT
_DOT_ID = r'(?:"(?:[^"\\]|\\.)*"|[\w.]+)'


def parse_dot(text: str) -> dict:
    text = re.sub(r"//[^\n]*|/\*.*?\*/|#[^\n]*", "", text, flags=re.S)
    b = _Builder()
    unq = lambda s: s[1:-1].replace('\\"', '"') if s.startswith('"') else s  # noqa: E731
    for m in re.finditer(rf"({_DOT_ID})\s*\[([^\]]*)\]", text):   # atributos de nodo (label)
        if m.group(1) in ("graph", "node", "edge"):
            continue
        lab = re.search(rf'label\s*=\s*({_DOT_ID})', m.group(2))
        b.node(unq(m.group(1)), unq(lab.group(1)) if lab else None)
    found = 0
    for m in re.finditer(rf"({_DOT_ID}(?:\s*-[->]\s*{_DOT_ID})+)\s*(\[([^\]]*)\])?", text):
        ids = [unq(x) for x in re.split(r"\s*-[->]\s*", m.group(1))]
        lab = re.search(rf'label\s*=\s*({_DOT_ID})', m.group(3) or "")
        for a, c in zip(ids, ids[1:]):
            b.edge(a, c, unq(lab.group(1)) if lab else None)
            found += 1
    if not found:
        raise GraphFormatError("no hay aristas a -> b en el fichero DOT")
    return b.result("dot")


# ------------------------------------------------------------------ RDF (N-Triples / Turtle basico)
_TTL_TOKEN = re.compile(r'"""(?:.|\n)*?"""(?:@[\w-]+|\^\^\S+)?|"(?:[^"\\]|\\.)*"(?:@[\w-]+|\^\^\S+)?|<[^>]*>|'
                        r'@prefix|@base|PREFIX|BASE|[;,.]|_:\w+|[\w-]*:[\w.\-%]*|\ba\b|[^\s;,.]+')


def parse_rdf(text: str) -> dict:
    text = re.sub(r'(?m)^\s*#.*$', "", text)
    toks = _TTL_TOKEN.findall(text)
    prefixes: dict[str, str] = {}
    b = _Builder()
    i, stmt = 0, []
    triples = 0

    def iri(t: str):
        if t.startswith("<"):
            return t[1:-1]
        if t == "a":
            return "type"
        if ":" in t and not t.startswith('"') and not t.startswith("_:"):
            p, local = t.split(":", 1)
            return prefixes.get(p, p + ":") + local if p in prefixes else local or p
        return t

    def lit(t: str):
        m = re.match(r'^(?:"""|")(.*?)(?:"""|")(?:@[\w-]+|\^\^\S+)?$', t, re.S)
        return m.group(1) if m else t
    subj = pred = None
    while i < len(toks):
        t = toks[i]
        if t in ("@prefix", "PREFIX"):
            if i + 2 < len(toks):
                prefixes[toks[i + 1].rstrip(":")] = toks[i + 2].strip("<>")
            i += 4 if t == "@prefix" else 3
            continue
        if t in ("@base", "BASE"):
            i += 3 if t == "@base" else 2
            continue
        if t == ".":
            subj = pred = None
            stmt = []
        elif t == ";":
            pred = None
        elif t == ",":
            pass
        elif subj is None:
            subj = iri(t)
        elif pred is None:
            pred = iri(t)
        else:
            obj_is_lit = t.startswith('"')
            o = lit(t) if obj_is_lit else iri(t)
            if pred in ("type", "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"):
                b.node(subj, None, type=_label(o).lower()[:30] or "concepto")
            elif pred.rsplit("#", 1)[-1].rsplit("/", 1)[-1] in ("label", "name", "prefLabel", "title") and obj_is_lit:
                b.node(subj, o)
            elif pred.rsplit("#", 1)[-1].rsplit("/", 1)[-1] in ("comment", "description", "definition") and obj_is_lit:
                b.node(subj, None, note=o)
            else:
                b.edge(subj, o, pred)
            triples += 1
        stmt.append(t)
        i += 1
    if not triples:
        raise GraphFormatError("no se han encontrado tripletas RDF")
    return b.result("rdf")


# ------------------------------------------------------------------ Markdown con [[enlaces]]
WIKILINK = re.compile(r"\[\[([^\]|#]+)(?:#[^\]|]*)?(?:\|[^\]]*)?\]\]")


def parse_wiki_notes(notes: dict[str, str]) -> dict | None:
    """{nombre_de_nota: texto} -> grafo de notas enlazadas (None si no hay enlaces)."""
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


# ------------------------------------------------------------------ deteccion
def parse_bytes(data: bytes, name: str = "") -> dict:
    ext = Path(name).suffix.lower()
    text = data.decode("utf-8-sig", errors="replace")
    head = text.lstrip()[:200].lower()
    if ext in (".graphml",) or "<graphml" in head:
        return parse_graphml(text)
    if ext == ".gexf" or "<gexf" in head:
        return parse_gexf(text)
    if ext in (".dot", ".gv") or re.match(r"^(strict\s+)?(di)?graph\b", head):
        return parse_dot(text)
    if ext in (".nt", ".ttl") or head.startswith(("@prefix", "prefix ")):
        return parse_rdf(text)
    if ext in (".jsonl", ".ndjson"):
        return parse_jsonl(text)
    if ext == ".json" or head.startswith(("{", "[")):
        try:
            return parse_json(json.loads(text))
        except json.JSONDecodeError:
            return parse_jsonl(text)
    if ext in (".csv", ".tsv", ".txt"):
        return parse_table(text, "\t" if ext == ".tsv" else None)
    raise GraphFormatError(f"formato de grafo no reconocido: {name or 'sin nombre'}")


def looks_like_graph(name: str, data: bytes) -> bool:
    """Para ficheros dentro de un ZIP: ¿es un grafo (y no un documento o un JSON cualquiera)?"""
    ext = Path(name).suffix.lower()
    if ext in (".graphml", ".gexf", ".dot", ".gv", ".nt", ".ttl"):
        return True
    if ext not in (".json", ".jsonl", ".ndjson", ".csv", ".tsv"):
        return False
    try:
        g = parse_bytes(data[:20_000_000], name)
    except (GraphFormatError, ValueError, ET.ParseError):
        return False
    if ext in (".csv", ".tsv"):   # solo tablas con columnas de grafo explicitas
        head = data[:400].decode("utf-8", errors="replace").splitlines()[0].lower() if data else ""
        return any(k in re.split(r"[,;\t]", head) for k in ("source", "target", "origen", "destino", "subject", "object"))
    return len(g["edges"]) > 0


# ------------------------------------------------------------------ insercion y exportacion
def _num(value, default: float) -> float:
    try:
        return max(0.0, float(value))
    except (TypeError, ValueError):
        return default


def insert_graph(graph: KnowledgeGraph, parsed: dict, target: str = "", organizer=None, known_agents=(),
                 source: str = "", progress=None) -> dict:
    """target: "" grafo general, id de agente, o "auto" (el organizador reparte: lo comun al general, lo de cada
    especialidad a su agente). Los ambitos de un export de Nexus-Office se respetan si el agente existe."""
    nodes, edges = parsed["nodes"], parsed["edges"]
    scope_of: dict[str, str] = {}
    known = set(known_agents)
    auto = target == "auto"
    if auto and organizer is not None:
        neigh: dict[str, list[str]] = {}
        names = {n["id"]: n["name"] for n in nodes}
        for e in edges[:200_000]:
            neigh.setdefault(e["src"], []).append(names.get(e["dst"], ""))
            neigh.setdefault(e["dst"], []).append(names.get(e["src"], ""))
        pending = [n for n in nodes if not (n.get("scope") in known or n.get("scope") == "")]
        for b in range(0, len(pending), 64):
            block = pending[b:b + 64]
            texts = [f"{n['name']}. {n.get('note', '')} " + ", ".join(neigh.get(n["id"], [])[:8]) for n in block]
            for n, aff in zip(block, organizer.affinities(texts)):
                scope_of[n["id"]] = aff["scope"]
                if not n.get("category"):
                    n["category"] = aff["category"]
            if progress:
                progress(min(b + 64, len(pending)), len(pending))
    ids: dict[str, int] = {}
    skipped = 0
    with graph.db.transaction():
        for n in nodes:
            sc = n.get("scope")
            if sc is not None and (sc == "" or sc in known) and parsed.get("format") == "nexus-kg" and auto:
                scope = sc
            else:
                scope = scope_of.get(n["id"], "" if auto else (target or GENERAL))
            if scope and scope not in known:
                scope = GENERAL
            cat = n.get("category") if n.get("category") in CATEGORIES else ""
            view = graph.for_agent(scope)
            etype = (n.get("type") or "concepto")[:30]
            if etype in ("self", "agente"):
                etype = "concepto"
            eid = view.upsert_entity(n["name"], etype, _num(n.get("weight"), 1.0), cat, n.get("note", ""), source)
            if eid:
                ids[n["id"]] = eid
        scopes = {r["id"]: r["scope"] for r in graph.db.query(
            f"SELECT id, scope FROM entities WHERE id IN ({','.join(map(str, set(ids.values()))) or '0'})")}
        made = 0
        for e in edges:
            a, b = ids.get(e["src"]), ids.get(e["dst"])
            if not a or not b or a == b:
                skipped += 1
                continue
            try:
                rs = KnowledgeGraph.relation_scope(scopes[a], scopes[b])
            except ValueError:
                skipped += 1
                continue
            graph.db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                             "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight",
                             (a, b, e["rel"], _num(e.get("weight"), 1.0), time.time(), rs))
            made += 1
    by_scope: dict[str, int] = {}
    for eid in set(ids.values()):
        sc = scopes.get(eid, "") or "general"
        by_scope[sc] = by_scope.get(sc, 0) + 1
    return {"format": parsed.get("format"), "nodes": len(set(ids.values())), "relations": made, "skipped": skipped,
            "by_scope": by_scope}


def export_graph(graph: KnowledgeGraph, scope: str | None = None, fmt: str = "json") -> tuple[bytes, str, str]:
    """(contenido, tipo MIME, extension). scope=None: todo (general y agentes)."""
    where, params = ("WHERE scope=?", (scope,)) if scope is not None else ("", ())
    nodes = graph.db.query(f"SELECT id, name, type, category, note, scope, weight FROM entities {where} ORDER BY id", params)
    ids = {n["id"] for n in nodes}
    rels = [r for r in graph.db.query(f"SELECT src, dst, rel, weight, scope FROM relations {where} ORDER BY id", params)
            if r["src"] in ids and r["dst"] in ids]
    if fmt == "csv":
        names = {n["id"]: n["name"] for n in nodes}
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["source", "relation", "target", "weight", "scope"])
        for r in rels:
            w.writerow([names[r["src"]], r["rel"], names[r["dst"]], round(r["weight"], 3), r["scope"]])
        return buf.getvalue().encode("utf-8"), "text/csv", "csv"
    if fmt == "graphml":
        esc = lambda s: (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")  # noqa: E731
                         .replace('"', "&quot;"))
        out = ['<?xml version="1.0" encoding="UTF-8"?>',
               '<graphml xmlns="http://graphml.graphdrawing.org/xmlns">']
        for k in ("label", "type", "category", "note", "scope"):
            out.append(f'<key id="{k}" for="node" attr.name="{k}" attr.type="string"/>')
        out += ['<key id="rel" for="edge" attr.name="label" attr.type="string"/>',
                '<key id="w" for="edge" attr.name="weight" attr.type="double"/>', '<graph edgedefault="directed">']
        for n in nodes:
            out.append(f'<node id="n{n["id"]}"><data key="label">{esc(n["name"])}</data><data key="type">{esc(n["type"])}'
                       f'</data><data key="category">{esc(n["category"])}</data><data key="note">{esc(n["note"])}</data>'
                       f'<data key="scope">{esc(n["scope"])}</data></node>')
        for r in rels:
            out.append(f'<edge source="n{r["src"]}" target="n{r["dst"]}"><data key="rel">{esc(r["rel"])}</data>'
                       f'<data key="w">{r["weight"]}</data></edge>')
        out += ["</graph>", "</graphml>"]
        return "\n".join(out).encode("utf-8"), "application/xml", "graphml"
    data = {"format": "nexus-kg", "version": 1,
            "nodes": [{"id": n["id"], "name": n["name"], "type": n["type"], "category": n["category"], "note": n["note"],
                       "scope": n["scope"], "weight": round(n["weight"] or 1, 3)} for n in nodes],
            "edges": [{"src": r["src"], "dst": r["dst"], "rel": r["rel"], "weight": round(r["weight"] or 1, 3),
                       "scope": r["scope"]} for r in rels]}
    return json.dumps(data, ensure_ascii=False, indent=1).encode("utf-8"), "application/json", "json"
