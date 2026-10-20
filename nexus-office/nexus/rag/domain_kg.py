"""Grafo de conocimiento multicapa de un dominio rastreado entero.

Mientras el rastreo de «todo el dominio» avanza por lotes, cada pagina y documento leido alimenta un grafo propio
del dominio, guardado en disco (data/rag/crawls/<trabajo>.kg.sqlite), asi que no hay limite de paginas: la memoria
no crece con el tamano del sitio. Al terminar (o cuando se pida) se razona y se empaqueta para darselo a un agente.

Capas:
  1. Fuentes      paginas y documentos con su URL (cada afirmacion apunta a las suyas).
  2. Estructura   dominio -> subdominios y secciones -> paginas, y los enlaces entre paginas.
  3. Conceptos    entidades y terminos clave de cada pagina, con su frecuencia en el dominio.
  4. Afirmaciones frases con contenido factual, agrupadas cuando varias paginas dicen lo mismo: «contrastada» si lo
                  respaldan fuentes distintas, «fuente unica» si no, «discrepante» si coinciden en todo menos en las
                  cifras. Las mas importantes se comprueban ademas contra el resto de la biblioteca (otras fuentes).
  5. Relaciones   conceptos que aparecen juntos (PMI) y, con un embedder semantico, conceptos de significado parecido.
  6. Sintesis     temas (comunidades de conceptos) con una sintesis razonada por el LLM (o extractiva sin el), sus
                  conclusiones y las afirmaciones en que se apoyan; y un resumen del dominio entero.

El paquete (.zip) lleva graph.json (formato nexus-kg), conocimiento.md (todo lo anterior legible y citado) y
manifest.json. Importarlo en un agente (aqui o en el importador de ZIP / grafos) le da el grafo en su subgrafo y el
conocimiento en su biblioteca.
"""
from __future__ import annotations

import json
import math
import re
import sqlite3
import threading
import time
import zipfile
from collections import Counter, defaultdict
from pathlib import Path
from urllib.parse import urlparse

from .. import log
from ..memory.embeddings import normalize, tokens
from ..memory.graph import STOP, KnowledgeGraph

L = log.get("domain_kg")
CLAIMS_PER_PAGE = 14
CONCEPTS_PER_PAGE = 12
SENT = re.compile(r"(?<=[.!?])\s+(?=[A-ZÁÉÍÓÚÑ¿¡0-9\"«(])|\n{1,}")
FACTUAL = re.compile(r"\b(es|son|era|fue|fueron|tiene|tienen|permite|permiten|incluye|consiste|se\s+\w+|puede|pueden|"
                     r"debe|deben|significa|define|sirve|usa|utiliza|contiene|ofrece|is|are|was|were|has|have|can|"
                     r"must|allows?|provides?|includes?|means|uses?|contains?|supports?|requires?|returns?)\b", re.I)
NUM = re.compile(r"\d+(?:[.,]\d+)*")
_locks: dict[str, threading.Lock] = defaultdict(threading.Lock)


def _stems(text: str) -> set[str]:
    return {w[:7] for w in tokens(text) if len(w) >= 4 and w not in STOP and not w.isdigit()}


def _short(text: str, n: int = 58) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= n else text[: n - 1].rstrip() + "…"


def section_of(url: str) -> str:
    """host + primer nivel de ruta: «docs.x.org/api»."""
    p = urlparse(url)
    seg = [s for s in p.path.split("/") if s][:1]
    host = p.netloc.lower()[4:] if p.netloc.lower().startswith("www.") else p.netloc.lower()
    if seg and "." in seg[0]:
        seg = []
    return host + ("/" + seg[0] if seg else "")


def claims_of(text: str, limit: int = CLAIMS_PER_PAGE) -> list[str]:
    """Frases que afirman algo (verbo de estado o accion, o cifras) y no son navegacion ni titulos."""
    out, seen = [], set()
    for raw in SENT.split(text):
        s = re.sub(r"\s+", " ", raw).strip(" -•*#>|")
        if not (45 <= len(s) <= 360) or s.count(" ") < 6 or s.endswith(":"):
            continue
        if not (FACTUAL.search(s) or NUM.search(s)):
            continue
        key = normalize(s)[:120]
        if key in seen:
            continue
        seen.add(key)
        score = len(_stems(s)) + 2 * bool(NUM.search(s)) + (1.5 if FACTUAL.search(s) else 0)
        out.append((score, len(out), s))
    out.sort(key=lambda x: (-x[0], x[1]))
    return [s for _, _, s in sorted(out[:limit], key=lambda x: x[1])]


class DomainGraph:
    """Acumula el dominio en un SQLite propio (por lotes) y lo razona y empaqueta al final."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.lock = _locks[str(self.path)]
        with self._db() as db:
            db.executescript("""
            CREATE TABLE IF NOT EXISTS pages(id INTEGER PRIMARY KEY, url TEXT UNIQUE, title TEXT, section TEXT,
                words INT, document TEXT, summary TEXT);
            CREATE TABLE IF NOT EXISTS concepts(id INTEGER PRIMARY KEY, key TEXT UNIQUE, name TEXT, df INT DEFAULT 0,
                tf INT DEFAULT 0);
            CREATE TABLE IF NOT EXISTS page_concepts(page INT, concept INT, n INT, PRIMARY KEY(page, concept));
            CREATE TABLE IF NOT EXISTS cooc(a INT, b INT, n INT, PRIMARY KEY(a, b));
            CREATE TABLE IF NOT EXISTS links(src INT, dst TEXT, PRIMARY KEY(src, dst));
            CREATE TABLE IF NOT EXISTS claims(id INTEGER PRIMARY KEY, page INT, text TEXT, stems TEXT, nums TEXT,
                concepts TEXT);
            CREATE INDEX IF NOT EXISTS claims_page ON claims(page);
            CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
            """)

    def _db(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=60)
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=NORMAL")
        return db

    # ------------------------------------------------------------------ capas 1-4: por lotes, en disco
    def add_pages(self, pages: list[dict]) -> int:
        """Incorpora las paginas de un lote. Devuelve cuantas eran nuevas."""
        added = 0
        with self.lock, self._db() as db:
            for p in pages:
                text = p.get("text") or ""
                cur = db.execute("INSERT OR IGNORE INTO pages(url, title, section, words, document, summary) "
                                 "VALUES(?,?,?,?,?,?)", (p["url"], (p.get("title") or p["url"])[:300],
                                                         section_of(p["url"]), int(p.get("words") or 0),
                                                         p.get("document") or "", (p.get("description") or "")[:400]))
                if not cur.rowcount:
                    continue
                added += 1
                pid = cur.lastrowid
                found = KnowledgeGraph.keywords(" ".join(p.get("headings") or [])[:2000] + "\n" + text[:60_000],
                                                CONCEPTS_PER_PAGE)
                low = normalize(text)
                cids = []
                for name in found:
                    key = normalize(name).strip()
                    if len(key) < 3 or key in STOP:
                        continue
                    n = max(1, low.count(key))
                    db.execute("INSERT INTO concepts(key, name, df, tf) VALUES(?,?,1,?) ON CONFLICT(key) DO UPDATE SET "
                               "df=df+1, tf=tf+excluded.tf", (key, name[:60], n))
                    cid = db.execute("SELECT id FROM concepts WHERE key=?", (key,)).fetchone()[0]
                    db.execute("INSERT OR IGNORE INTO page_concepts(page, concept, n) VALUES(?,?,?)", (pid, cid, n))
                    cids.append((cid, key))
                ids = sorted({c for c, _ in cids})
                for i, a in enumerate(ids[:10]):
                    for b in ids[i + 1:10]:
                        db.execute("INSERT INTO cooc(a, b, n) VALUES(?,?,1) ON CONFLICT(a, b) DO UPDATE SET n=n+1", (a, b))
                for dst in (p.get("links") or [])[:200]:
                    db.execute("INSERT OR IGNORE INTO links(src, dst) VALUES(?,?)", (pid, dst))
                for c in claims_of(text):
                    lc = normalize(c)
                    about = [str(cid) for cid, key in cids if key in lc]
                    db.execute("INSERT INTO claims(page, text, stems, nums, concepts) VALUES(?,?,?,?,?)",
                               (pid, c, " ".join(sorted(_stems(c))), " ".join(NUM.findall(c)), " ".join(about)))
        return added

    def stats(self) -> dict:
        with self._db() as db:
            q = lambda sql: db.execute(sql).fetchone()[0]   # noqa: E731
            return {"pages": q("SELECT COUNT(*) FROM pages"), "documents": q("SELECT COUNT(*) FROM pages WHERE document!=''"),
                    "concepts": q("SELECT COUNT(*) FROM concepts"), "claims": q("SELECT COUNT(*) FROM claims"),
                    "links": q("SELECT COUNT(*) FROM links"), "sections": q("SELECT COUNT(DISTINCT section) FROM pages")}

    # ------------------------------------------------------------------ capas 4-6: razonar
    def _verify(self, db, claims: dict, by_concept: dict) -> tuple[dict, dict]:
        """Agrupa afirmaciones equivalentes de paginas distintas (union-find por concepto compartido)."""
        parent = {cid: cid for cid in claims}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        compared = 0
        for con, members in sorted(by_concept.items(), key=lambda kv: -len(kv[1]))[:4000]:
            block = members[-120:]
            if len({claims[c]["page"] for c in block}) < 2:
                continue
            for i, a in enumerate(block):
                sa, pa = claims[a]["stems"], claims[a]["page"]
                for b in block[i + 1:]:
                    if claims[b]["page"] == pa:
                        continue
                    sb = claims[b]["stems"]
                    inter = len(sa & sb)
                    compared += 1
                    if inter >= 3 and inter / max(1, len(sa | sb)) >= 0.5:
                        parent[find(a)] = find(b)
        groups: dict[int, list[int]] = defaultdict(list)
        for cid in claims:
            groups[find(cid)].append(cid)
        return groups, {"compared": compared}

    def _communities(self, db, top: list[int], names: dict[int, str]) -> dict[int, int]:
        """Comunidades de conceptos por propagacion de etiquetas sobre la co-ocurrencia (determinista)."""
        if not top:
            return {}
        ok = set(top)
        neigh: dict[int, list[tuple[int, int]]] = defaultdict(list)
        for a, b, n in db.execute(f"SELECT a, b, n FROM cooc WHERE n >= 2 AND a IN ({','.join(map(str, top))})"):
            if b in ok:
                neigh[a].append((b, n))
                neigh[b].append((a, n))
        label = {c: c for c in top}
        for _ in range(12):
            changed = 0
            for c in top:
                if not neigh[c]:
                    continue
                score: Counter = Counter()
                for o, n in neigh[c]:
                    score[label[o]] += n
                best = max(score.items(), key=lambda kv: (kv[1], -kv[0]))[0]
                if best != label[c] and score[best] > score.get(label[c], 0):
                    label[c] = best
                    changed += 1
            if not changed:
                break
        return label

    def build(self, title: str = "", site: str = "", llm=None, embedder=None, rag=None, exclude_docs=(),
              progress=None, max_llm_topics: int = 40, max_claims: int = 300_000) -> dict:
        """Razona el dominio y devuelve {"graph": nexus-kg, "markdown": str, "manifest": dict}."""
        step = progress or (lambda *_: None)
        t0 = time.time()
        with self.lock, self._db() as db:
            pages = {r[0]: {"url": r[1], "title": r[2], "section": r[3], "words": r[4], "document": r[5]}
                     for r in db.execute("SELECT id, url, title, section, words, document FROM pages")}
            if not pages:
                raise ValueError("el dominio aun no tiene paginas leidas")
            ndocs = len(pages)
            concepts = {r[0]: {"key": r[1], "name": r[2], "df": r[3], "tf": r[4]}
                        for r in db.execute("SELECT id, key, name, df, tf FROM concepts WHERE df >= ?",
                                            (1 if ndocs < 30 else 2,))}
            step("Comprobando afirmaciones entre fuentes")
            claims, by_concept = {}, defaultdict(list)
            # dominios enormes: se razona sobre las afirmaciones mas conectadas (todo el texto sigue en la biblioteca)
            rows = db.execute("SELECT id, page, text, stems, nums, concepts FROM claims ORDER BY "
                              "length(concepts) DESC, id LIMIT ?", (max_claims,))
            for cid, page, text, stems, nums, about in rows:
                about_ids = [int(x) for x in about.split() if int(x) in concepts]
                claims[cid] = {"page": page, "text": text, "stems": set(stems.split()), "nums": nums.split(),
                               "about": about_ids}
                for a in about_ids:
                    by_concept[a].append(cid)
            groups, vstats = self._verify(db, claims, by_concept)
            # una afirmacion por grupo: la mas completa, con todas sus fuentes
            facts = []
            for members in groups.values():
                srcs = sorted({claims[m]["page"] for m in members})
                main = max(members, key=lambda m: (len(claims[m]["stems"]), -m))
                nums = {tuple(claims[m]["nums"]) for m in members if claims[m]["nums"]}
                if len(nums) >= 3 and len(nums) >= len(srcs) - 1:
                    continue      # «Capítulo 1/2/3 de...»: plantilla repetida con numeracion, no una afirmacion
                sections = {pages[s]["section"] for s in srcs}
                status = "discrepante" if len(nums) > 1 else "contrastada" if len(srcs) >= 2 else "fuente única"
                about = sorted({a for m in members for a in claims[m]["about"]})
                weight = len(srcs) * 2 + len(sections) + sum(math.log(1 + concepts[a]["df"]) for a in about[:5]) / 3
                facts.append({"id": main, "text": claims[main]["text"], "sources": srcs, "status": status,
                              "independent": len(sections), "about": about, "weight": weight,
                              "variants": [claims[m]["text"] for m in members if m != main][:3]})
            facts.sort(key=lambda f: -f["weight"])
            # comprobacion con otras fuentes de la biblioteca (lo que no es este dominio)
            external = 0
            if rag is not None:
                step("Contrastando con el resto de la biblioteca")
                skip = set(exclude_docs)
                for f in facts[:300]:
                    try:
                        hits = [h for h in rag.search(f["text"], k=4, min_score=0.62) if h["doc_id"] not in skip]
                    except Exception:  # noqa: BLE001
                        break
                    if hits:
                        f["external"] = [{"title": h.get("title", ""), "score": round(h["score"], 2)} for h in hits[:2]]
                        external += 1
                        if f["status"] == "fuente única":
                            f["status"] = "contrastada"
            # relaciones entre conceptos: co-ocurrencia (PMI) y significado
            step("Relacionando conceptos")
            top = sorted(concepts, key=lambda c: -concepts[c]["df"])[:5000]
            names = {c: concepts[c]["name"] for c in top}
            rel_edges = []
            topset = set(top)
            for a, b, n in db.execute("SELECT a, b, n FROM cooc WHERE n >= 2 ORDER BY n DESC LIMIT 200000"):
                if a in topset and b in topset:
                    pmi = math.log((n * ndocs) / (concepts[a]["df"] * concepts[b]["df"]))
                    if pmi > 0.3:
                        rel_edges.append((a, b, "aparece_con", round(min(5.0, n * pmi / 2), 3)))
            semantic = 0
            if embedder is not None and not str(getattr(embedder, "id", "")).startswith("hash") and len(top) > 3:
                try:
                    import numpy as np
                    sel = top[:3000]
                    vecs = np.asarray(embedder.embed([names[c] for c in sel]), dtype=np.float32)
                    vecs /= np.linalg.norm(vecs, axis=1, keepdims=True) + 1e-9
                    for i in range(0, len(sel), 512):
                        sims = vecs[i:i + 512] @ vecs.T
                        for r, row in enumerate(sims):
                            ci = i + r
                            row[ci] = -1
                            for j in np.argsort(-row)[:3]:
                                if row[j] >= 0.62 and ci < j:
                                    rel_edges.append((sel[ci], sel[int(j)], "relacionado_con", round(float(row[j]), 3)))
                                    semantic += 1
                except Exception as e:  # noqa: BLE001
                    L.info("Relaciones semanticas omitidas: %s", e)
            # temas
            step("Agrupando por temas")
            label = self._communities(db, top[:2500], names)
            comm: dict[int, list[int]] = defaultdict(list)
            for c, lab in label.items():
                comm[lab].append(c)
            topics = sorted((sorted(m, key=lambda c: -concepts[c]["df"]) for m in comm.values() if len(m) >= 3),
                            key=lambda m: -sum(concepts[c]["df"] for c in m))[:80]
            topic_of = {c: i for i, m in enumerate(topics) for c in m}
            page_concepts = defaultdict(list)
            for page, concept, n in db.execute("SELECT page, concept, n FROM page_concepts"):
                if concept in concepts:
                    page_concepts[page].append((n, concept))
            links = db.execute("SELECT src, dst FROM links").fetchall()
        url_id = {p["url"]: pid for pid, p in pages.items()}

        # sintesis razonada por tema
        tdata = []
        for i, members in enumerate(topics):
            mset = set(members)
            tf = [f for f in facts if mset & set(f["about"])][:12]
            name = "Tema: " + ", ".join(concepts[c]["name"] for c in members[:3])
            synth, concl = "", []
            if llm is not None and i < max_llm_topics and tf:
                step(f"Razonando el tema {i + 1}/{min(len(topics), max_llm_topics)}")
                lines = "\n".join(f"- ({f['status']}, {len(f['sources'])} fuentes) {f['text']}" for f in tf[:10])
                prompt = (f"Eres un analista que asimila el conocimiento del sitio {site or title}. Tema: "
                          f"{', '.join(concepts[c]['name'] for c in members[:8])}.\nAfirmaciones extraidas del sitio:\n"
                          f"{lines}\n\nRazona sobre ellas: relacionalas, deduce lo que implican juntas y senala lo dudoso "
                          "(fuente unica o discrepante). Devuelve SOLO JSON {\"sintesis\": \"3-5 frases\", "
                          "\"conclusiones\": [\"...\", \"...\", \"...\"]} en espanol.")
                try:
                    raw = llm(prompt)
                    data = json.loads(raw[raw.find("{"): raw.rfind("}") + 1])
                    synth = str(data.get("sintesis", "")).strip()[:1500]
                    concl = [str(x).strip()[:300] for x in data.get("conclusiones", []) if str(x).strip()][:5]
                except Exception:  # noqa: BLE001
                    synth = ""
            if not synth:
                ok = [f for f in tf if f["status"] == "contrastada"] or tf
                synth = " ".join(f["text"] for f in ok[:3])
                concl = [f["text"] for f in tf[3:6]]
            tdata.append({"name": name, "concepts": members, "facts": tf, "synthesis": synth, "conclusions": concl,
                          "reasoned": bool(llm is not None and i < max_llm_topics and synth and tf and concl)})
        overview = (f"{title or site}: {len(pages)} páginas y documentos en {len({p['section'] for p in pages.values()})} "
                    f"secciones. Temas principales: " + "; ".join(t["name"][6:] for t in tdata[:8]) + ".")
        if llm is not None and tdata:
            try:
                raw = llm("Resume en 4-6 frases, en espanol, de que trata y que ensena este sitio a partir de las "
                          "sintesis de sus temas:\n" + "\n".join(f"- {t['name']}: {t['synthesis'][:400]}" for t in tdata[:15]))
                overview = raw.strip()[:2000] or overview
            except Exception:  # noqa: BLE001
                pass

        # ------------------------------------------------------------ grafo nexus-kg
        step("Construyendo el grafo")
        nodes, edges = [], []
        dom_id = "d0"
        domain_name = site or title or "dominio"
        nodes.append({"id": dom_id, "name": _short(domain_name), "type": "dominio", "category": "", "note": overview[:500],
                      "weight": 5})
        sec_ids = {}
        for pid, p in pages.items():
            sec = p["section"]
            if sec not in sec_ids:
                sec_ids[sec] = f"s{len(sec_ids)}"
                nodes.append({"id": sec_ids[sec], "name": _short(sec), "type": "seccion", "category": "", "note": "",
                              "weight": 1})
                edges.append({"src": dom_id, "dst": sec_ids[sec], "rel": "contiene", "weight": 1})
        page_cap = 50_000
        shown = set(sorted(pages, key=lambda pid: -pages[pid]["words"])[:page_cap])
        for pid in shown:
            p = pages[pid]
            nodes.append({"id": f"p{pid}", "name": _short(p["title"]), "type": "documento" if p["document"] else "pagina",
                          "category": "", "note": p["url"], "weight": 1})
            edges.append({"src": sec_ids[p["section"]], "dst": f"p{pid}", "rel": "contiene", "weight": 1})
            for n, c in sorted(page_concepts.get(pid, []), reverse=True)[:5]:
                edges.append({"src": f"p{pid}", "dst": f"c{c}", "rel": "menciona", "weight": 1})
        for src, dst in links[:300_000]:
            d = url_id.get(dst)
            if d and d != src and src in shown and d in shown:
                edges.append({"src": f"p{src}", "dst": f"p{d}", "rel": "enlaza", "weight": 0.5})
        used_concepts = set(top) | {c for t in topics for c in t}
        best_fact = {}
        for f in facts:
            for a in f["about"]:
                best_fact.setdefault(a, f)
        for c in used_concepts:
            f = best_fact.get(c)
            nodes.append({"id": f"c{c}", "name": concepts[c]["name"], "type": "concepto", "category": "",
                          "note": (f["text"][:480] if f else ""), "weight": round(1 + math.log(1 + concepts[c]["df"]), 3)})
        for a, b, rel, w in rel_edges:
            edges.append({"src": f"c{a}", "dst": f"c{b}", "rel": rel, "weight": w})
        fact_cap = 60_000
        for f in facts[:fact_cap]:
            srcs = f["sources"]
            note = f"{f['text']} [{f['status']}; {len(srcs)} fuente{'s' if len(srcs) != 1 else ''}: " + \
                   ", ".join(pages[s]["url"] for s in srcs[:3]) + "]"
            nodes.append({"id": f"f{f['id']}", "name": _short(f["text"]), "type": "afirmacion", "category": "",
                          "note": note[:500], "weight": round(1 + len(srcs) * 0.5, 3)})
            for a in f["about"][:6]:
                if a in used_concepts:
                    edges.append({"src": f"f{f['id']}", "dst": f"c{a}", "rel": "sobre", "weight": 1})
            for s in srcs[:10]:
                if s in shown:
                    edges.append({"src": f"f{f['id']}", "dst": f"p{s}", "rel": "fuente", "weight": 1})
        kept_facts = {f["id"] for f in facts[:fact_cap]}
        for i, t in enumerate(tdata):
            tid = f"t{i}"
            nodes.append({"id": tid, "name": _short(t["name"]), "type": "tema", "category": "",
                          "note": t["synthesis"][:500], "weight": 3})
            edges.append({"src": dom_id, "dst": tid, "rel": "trata_de", "weight": 2})
            for c in t["concepts"][:40]:
                edges.append({"src": tid, "dst": f"c{c}", "rel": "incluye", "weight": 1})
            for f in t["facts"][:8]:
                if f["id"] in kept_facts:
                    edges.append({"src": tid, "dst": f"f{f['id']}", "rel": "se_apoya_en", "weight": 1})
            for j, cl in enumerate(t["conclusions"]):
                cid = f"t{i}k{j}"
                nodes.append({"id": cid, "name": _short(cl), "type": "conclusion", "category": "", "note": cl[:500],
                              "weight": 2})
                edges.append({"src": tid, "dst": cid, "rel": "concluye", "weight": 1.5})
        graph = {"format": "nexus-kg", "version": 1, "layers": ["fuentes", "estructura", "conceptos", "afirmaciones",
                                                                  "relaciones", "sintesis"],
                 "nodes": nodes, "edges": edges}

        # ------------------------------------------------------------ conocimiento legible y citado
        counts = Counter(f["status"] for f in facts)
        md = [f"# Conocimiento del dominio: {domain_name}", "", overview, "",
              f"_{len(pages)} páginas y documentos · {len(sec_ids)} secciones · {len(used_concepts)} conceptos · "
              f"{len(facts)} afirmaciones ({counts.get('contrastada', 0)} contrastadas, {counts.get('fuente única', 0)} "
              f"de fuente única, {counts.get('discrepante', 0)} discrepantes) · {len(tdata)} temas._", ""]
        for t in tdata:
            md += [f"## {t['name'][6:]}", "", t["synthesis"], ""]
            if t["conclusions"]:
                md += ["**Conclusiones**", ""] + [f"- {c}" for c in t["conclusions"]] + [""]
            md += [f"**Conceptos:** {', '.join(concepts[c]['name'] for c in t['concepts'][:20])}", ""]
            if t["facts"]:
                md += ["**Afirmaciones**", ""]
                for f in t["facts"][:12]:
                    md.append(f"- {f['text']} _({f['status']}; " + ", ".join(pages[s]["url"] for s in f["sources"][:3]) + ")_")
                md.append("")
        rest = [f for f in facts if not any(set(f["about"]) & set(t["concepts"]) for t in tdata)][:400]
        if rest:
            md += ["## Otras afirmaciones", ""] + [f"- {f['text']} _({f['status']}; {pages[f['sources'][0]]['url']})_"
                                                   for f in rest] + [""]
        md += ["## Mapa del sitio", ""]
        by_sec = defaultdict(list)
        for pid, p in pages.items():
            by_sec[p["section"]].append(p)
        for sec in sorted(by_sec):
            md += [f"### {sec}", ""] + [f"- [{p['title']}]({p['url']})" for p in by_sec[sec][:500]] + [""]
        manifest = {"format": "nexus-domain-knowledge", "version": 1, "domain": domain_name, "title": title,
                    "created": time.strftime("%Y-%m-%d %H:%M"), "pages": len(pages),
                    "documents": sum(1 for p in pages.values() if p["document"]), "sections": len(sec_ids),
                    "concepts": len(used_concepts), "facts": len(facts), "facts_by_status": dict(counts),
                    "external_checked": external, "topics": len(tdata),
                    "reasoned_topics": sum(1 for t in tdata if t["reasoned"]), "semantic_relations": semantic,
                    "relations": len(edges), "nodes": len(nodes), "layers": graph["layers"],
                    "seconds": round(time.time() - t0, 1), **vstats}
        return {"graph": graph, "markdown": "\n".join(md) + "\n", "manifest": manifest}

    def package(self, out: Path, built: dict) -> Path:
        out = Path(out)
        tmp = out.with_suffix(".tmp")
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("graph.json", json.dumps(built["graph"], ensure_ascii=False))
            z.writestr("conocimiento.md", built["markdown"])
            z.writestr("manifest.json", json.dumps(built["manifest"], ensure_ascii=False, indent=1))
        tmp.replace(out)
        return out


def read_package(path: Path) -> dict:
    with zipfile.ZipFile(path) as z:
        return {"graph": json.loads(z.read("graph.json")), "markdown": z.read("conocimiento.md").decode("utf-8"),
                "manifest": json.loads(z.read("manifest.json"))}


def import_package(services, data: dict, agent: str = "", source: str = "") -> dict:
    """Da el conocimiento del dominio a un agente (o al grafo general): grafo en su subgrafo y conocimiento en su
    biblioteca, para que lo use en sus respuestas."""
    from ..memory import kg_io
    from ..memory.kg_formats import parse_json
    m = data["manifest"]
    parsed = parse_json(data["graph"])
    res = kg_io.insert_graph(services.graph, parsed, agent or "", None, services.agents.ids(),
                             source or f"dominio:{m.get('domain', '')}")
    title = f"Conocimiento del dominio {m.get('domain', '')}"[:200]
    md = data["markdown"]
    parts = [p for p in re.split(r"\n(?=## )", md) if p.strip()]
    doc = services.rag.ingest_pages(title, parts, source=source or m.get("domain", ""), kind="text",
                                    meta={"origin": "domain-knowledge", **{k: m[k] for k in ("domain", "pages", "facts",
                                                                                             "topics") if k in m}},
                                    agent=agent or "")
    return {"graph": res, "rag_doc_id": doc["id"], "rag_chunks": doc["chunks"], "agent": agent or "", "manifest": m}
