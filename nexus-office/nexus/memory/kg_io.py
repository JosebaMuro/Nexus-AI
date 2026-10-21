"""Importar y exportar grafos de conocimiento.

La lectura de cualquier formato (detectado por el contenido) vive en ``kg_formats``: JSON en todas sus variantes
(node-link, Cytoscape, JSON-LD, GraphSON, Neo4j...), tablas y hojas de calculo, GraphML, GEXF, GML, DOT, RDF, Cypher,
Mermaid, formatos de analisis de redes, mapas mentales, notas [[enlazadas]], SQLite y comprimidos. Todo se normaliza a
``{"nodes": [{id, name, type, category, note, scope?}], "edges": [{src, dst, rel, weight}]}`` e ``insert_graph`` lo
fusiona con el grafo general, con el de un agente o reparte cada nodo con el organizador ("auto").
"""
from __future__ import annotations

import csv
import io
import json
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from .graph import CATEGORIES, GENERAL, KnowledgeGraph
from .kg_formats import (FORMATS, IMPORT_EXT, MAX_NODES, GraphFormatError, detect, merge, parse_bytes,  # noqa: F401
                         parse_dot, parse_gexf, parse_graphml, parse_json, parse_jsonl, parse_many, parse_rdf,
                         parse_table, parse_wiki_notes)

# extensiones que, dentro de un ZIP de documentos, se tratan como grafo (lo demas va a la biblioteca RAG)
GRAPH_EXT = {".json", ".jsonl", ".ndjson", ".jsonld", ".cyjs", ".graphson", ".canvas", ".csv", ".tsv", ".graphml",
             ".gexf", ".xgmml", ".dgml", ".gxl", ".gml", ".dot", ".gv", ".net", ".paj", ".gdf", ".dl", ".tgf", ".sif",
             ".lgl", ".ncol", ".edgelist", ".adjlist", ".nt", ".nq", ".ttl", ".trig", ".n3", ".rdf", ".owl",
             ".cypher", ".cql", ".mmd", ".mermaid", ".puml", ".mm", ".opml"}
EXPLICIT_EXT = GRAPH_EXT - {".json", ".jsonl", ".ndjson", ".csv", ".tsv", ".canvas"}


def looks_like_graph(name: str, data: bytes) -> bool:
    """Para ficheros dentro de un ZIP: ¿es un grafo (y no un documento o un JSON cualquiera)?"""
    ext = Path(name).suffix.lower()
    if ext not in GRAPH_EXT:
        return False
    try:
        g = parse_bytes(data[:20_000_000], name)
    except (GraphFormatError, ValueError, ET.ParseError, KeyError, IndexError):
        return False
    if g.get("generic"):
        return False
    if ext in EXPLICIT_EXT:
        return True
    if ext in (".csv", ".tsv"):   # solo tablas con columnas de grafo explicitas
        return g.get("table") in ("aristas", "neo4j-aristas", "neo4j-nodos")
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
    last = graph.db.query("SELECT COALESCE(MAX(id), 0) AS m FROM entities")[0]["m"]   # lo que pase de aqui es nuevo
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
    uniq = set(ids.values())
    new = sum(1 for i in uniq if i > last)
    out = {"format": parsed.get("format"), "nodes": len(uniq), "new": new, "merged": len(uniq) - new,
           "relations": made, "skipped": skipped, "by_scope": by_scope}
    for k in ("files", "unread", "table"):
        if parsed.get(k):
            out[k] = parsed[k]
    return out


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
