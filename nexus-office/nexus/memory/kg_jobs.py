"""Trabajos del gestor de conocimiento (Ajustes > Conocimiento), en segundo plano y con progreso:

* importar un grafo (JSON, CSV, GraphML, GEXF, DOT, RDF...) al general, a un agente o repartido automaticamente;
* leer un ZIP (o RAR, 7z, tar), razonarlo y procesarlo: los grafos que traiga se insertan, los documentos van a
  la biblioteca RAG, se agrupan por temas con el modelo de embeddings y el LLM razona los mas importantes para
  extraer resumen, hechos y procedimientos, que se colocan en el grafo general o en el del especialista;
* reorganizar lo ya aprendido (que es comun y que es de cada agente) y buscar duplicados.
"""
from __future__ import annotations

import json
import re
import shutil
import tempfile
import threading
import time
import uuid
from pathlib import Path, PurePosixPath

import numpy as np

from .. import log, paths
from . import kg_io
from .graph import AGENT_CATEGORIES, CATEGORIES, GENERAL_CATEGORIES, KnowledgeGraph, _key

L = log.get("kg")
DOC_PRIORITY = re.compile(r"(^|/)(readme|index|overview|introduc|getting[-_ ]started|guia|guide|manual|tutorial|"
                          r"arquitectura|architecture|design|docs?/)", re.I)
REASON_PROMPT = """Eres el bibliotecario de conocimiento de un asistente con varios agentes especialistas.
Lee el documento y devuelve SOLO un objeto JSON (sin texto alrededor) con esta forma:
{{"resumen": "2-3 frases", "tipo": "manual|referencia|tutorial|codigo|notas|datos|otro",
 "ambito": "general" o el id del especialista al que corresponde este saber hacer,
 "hechos": [{{"s": "sujeto", "r": "relacion_corta", "o": "objeto", "categoria": "comun|especialidad|procedimiento|comportamiento|relacion|preferencia"}}],
 "pasos": ["paso 1", "paso 2"]}}
- "general": conocimiento comun, pautas de comportamiento, relaciones o preferencias. Un especialista: su oficio.
- Como mucho 8 hechos concretos y utiles; "pasos" solo si explica como hacer algo (si no, lista vacia).
Especialistas: {agents}
Archivo: {path}
---
{text}"""
SYNTH_PROMPT = """Estos son los resumenes de los documentos de un archivo comprimido llamado «{name}».
Escribe en 3-4 frases de que trata el conjunto y para que sirve. Solo el texto.
{items}"""


def _json_obj(raw: str) -> dict | None:
    if not raw:
        return None
    raw = re.sub(r"^```(?:json)?|```$", "", raw.strip(), flags=re.M)
    a, b = raw.find("{"), raw.rfind("}")
    if a < 0 or b <= a:
        return None
    try:
        data = json.loads(raw[a:b + 1])
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, dict) else None


def _first_sentences(text: str, n: int = 2, limit: int = 320) -> str:
    text = re.sub(r"\s+", " ", re.sub(r"[#>*`_\-]{2,}", " ", text)).strip()
    parts = [p for p in re.split(r"(?<=[.!?])\s+", text) if len(p) > 30]
    return (" ".join(parts[:n]) or text)[:limit]


class KnowledgeJobs:
    def __init__(self, services) -> None:
        self.s = services
        self.jobs: dict[str, dict] = {}
        self.listeners: list = []
        self._lock = threading.Lock()

    # ------------------------------------------------------------ registro
    def _emit(self, job: dict) -> None:
        for fn in list(self.listeners):
            try:
                fn({"type": "kg_job", "job": self.public(job)})
            except Exception:  # noqa: BLE001
                pass

    @staticmethod
    def public(job: dict) -> dict:
        return {k: v for k, v in job.items() if not k.startswith("_")}

    def list(self) -> list[dict]:
        return [self.public(j) for j in sorted(self.jobs.values(), key=lambda j: -j["started"])][:30]

    def get(self, jid: str) -> dict | None:
        j = self.jobs.get(jid)
        return self.public(j) if j else None

    def _start(self, kind: str, title: str, fn, *args) -> dict:
        jid = uuid.uuid4().hex[:10]
        job = {"id": jid, "kind": kind, "title": title, "status": "en curso", "done": 0, "total": 0, "step": "",
               "log": [], "result": None, "error": "", "started": time.time(), "finished": None}
        self.jobs[jid] = job

        def run():
            try:
                job["result"] = fn(job, *args)
                job["status"] = "completado"
            except Exception as e:  # noqa: BLE001
                L.exception("Trabajo %s fallido", title)
                job.update(status="error", error=str(e)[:500])
            finally:
                job["finished"] = time.time()
                self._emit(job)
                for fn_ in list(self.listeners):
                    try:
                        fn_({"type": "graph_update"})
                    except Exception:  # noqa: BLE001
                        pass
        threading.Thread(target=run, name=f"kg-{kind}-{jid}", daemon=True).start()
        self._emit(job)
        return self.public(job)

    def _step(self, job: dict, text: str, done: int | None = None, total: int | None = None) -> None:
        job["step"] = text
        if done is not None:
            job["done"] = done
        if total is not None:
            job["total"] = total
        job["log"] = (job["log"] + [text])[-40:]
        self._emit(job)

    def _target(self, target: str) -> str:
        target = (target or "").strip()
        if target in ("", "general"):
            return ""
        if target == "auto":
            return "auto"
        if not self.s.agents.get(target):
            raise ValueError(f"agente desconocido: {target}")
        return target

    # ------------------------------------------------------------ importar un grafo
    def import_graph(self, data: bytes, name: str, target: str = "") -> dict:
        target = self._target(target)
        parsed = kg_io.parse_bytes(data, name)   # el formato se valida ya: el error llega al usuario al momento
        return self._start("import", f"Importar grafo «{name}»", self._run_import, parsed, name, target)

    def _run_import(self, job: dict, parsed: dict, name: str, target: str) -> dict:
        self._step(job, f"{len(parsed['nodes'])} nodos y {len(parsed['edges'])} relaciones ({parsed['format']})")
        res = kg_io.insert_graph(self.s.graph, parsed, target, self.s.organizer, self.s.agents.ids(), f"grafo:{name}",
                                 progress=lambda d, t: self._step(job, f"Clasificando nodos {d}/{t}", d, t))
        self._step(job, f"Insertados {res['nodes']} nodos y {res['relations']} relaciones")
        return res

    # ------------------------------------------------------------ ZIP: leer, razonar y procesar
    def process_archive(self, path: Path, name: str, target: str = "auto", reason: bool = True, rag: bool = True,
                        max_reason: int | None = None) -> dict:
        target = self._target(target)
        n = max_reason if max_reason is not None else int(self.s.config.get("memory.zip_reason_docs", 12) or 0)
        return self._start("zip", f"Procesar «{name}»", self._run_archive, Path(path), name, target, reason, rag, n)

    def _run_archive(self, job: dict, path: Path, name: str, target: str, reason: bool, rag: bool,
                     max_reason: int) -> dict:
        from ..rag.archive import RICH_EXT, _indexable, _read_for_index, detect_kind, extract_any
        s = self.s
        work = Path(tempfile.mkdtemp(prefix="kgzip_", dir=paths.sub("cache")))
        report: dict = {"archive": name, "graphs": [], "themes": [], "reasoned": 0, "facts": 0, "skills": 0}
        try:
            kind = detect_kind(path, name)
            if not kind:
                raise ValueError("el fichero no es un ZIP, RAR, 7z o tar valido")
            self._step(job, "Descomprimiendo de forma segura")
            res = extract_any(path, work, kind)
            files: list[PurePosixPath] = res["files"]
            report["files"] = len(files)
            self._step(job, f"{len(files)} ficheros")

            # 1) grafos de conocimiento que vengan dentro
            graph_files = []
            for rel in files:
                p = work / Path(*rel.parts)
                if p.suffix.lower() in kg_io.GRAPH_EXT and p.stat().st_size < 50 * 1024 * 1024:
                    data = p.read_bytes()
                    if kg_io.looks_like_graph(rel.name, data):
                        graph_files.append((rel, data))
            # 2) documentos legibles (lo que no es grafo)
            gset = {g[0] for g in graph_files}
            docs = [r for r in files if _indexable(r) and r not in gset]
            docs.sort(key=lambda r: (0 if DOC_PRIORITY.search(str(r)) else 1, 0 if r.suffix.lower() in
                                     {".md", ".rst", ".txt", ".adoc"} | RICH_EXT else 1, len(r.parts), str(r)))
            texts: dict[PurePosixPath, str] = {}
            for i, rel in enumerate(docs[:600]):
                pages, err = _read_for_index(str(work / Path(*rel.parts)))
                if not err and pages:
                    texts[rel] = "\n".join(pages)[:20000]
                if i % 40 == 0:
                    self._step(job, f"Leyendo documentos {i + 1}/{min(len(docs), 600)}", i + 1, min(len(docs), 600))
            # notas Markdown con [[enlaces]] (Obsidian, Logseq): son un grafo
            wiki = kg_io.parse_wiki_notes({r.stem: t for r, t in texts.items() if r.suffix.lower() in (".md", ".markdown")})

            # 3) destino: el que se pidio o el que decide el organizador por el contenido
            scope = target
            if target == "auto":
                sample = name + "\n" + "\n".join(f"{r}: {t[:400]}" for r, t in list(texts.items())[:12])
                scope = s.organizer.route(sample)[0] if texts else ""
            report["scope"] = scope or "general"
            label = s.agents.get(scope).name if scope and s.agents.get(scope) else "general"
            self._step(job, f"Destino: grafo {label}")
            graph_target = "auto" if target == "auto" else scope
            for rel, data in graph_files:
                try:
                    parsed = kg_io.parse_bytes(data, rel.name)
                    r = kg_io.insert_graph(s.graph, parsed, graph_target, s.organizer, s.agents.ids(), f"zip:{name}")
                    report["graphs"].append({"file": str(rel), **r})
                    self._step(job, f"Grafo {rel}: {r['nodes']} nodos, {r['relations']} relaciones")
                except Exception as e:  # noqa: BLE001
                    report["graphs"].append({"file": str(rel), "error": str(e)[:200]})
            if wiki:
                r = kg_io.insert_graph(s.graph, wiki, graph_target, s.organizer, s.agents.ids(), f"zip:{name}")
                report["graphs"].append({"file": "notas [[enlazadas]]", **r})
                self._step(job, f"Notas enlazadas: {r['nodes']} notas, {r['relations']} enlaces")

            # 4) biblioteca RAG (el archivo entero, con su arbol, en el sandbox de los agentes)
            if rag and docs:
                self._step(job, "Indexando en la biblioteca RAG")
                doc = s.rag.ingest_file(path, "", name, agent=scope)
                report["rag_doc"] = {"id": doc["id"], "title": doc["title"], "chunks": doc["chunks"]}

            view = s.graph.for_agent(scope)
            arch_title = re.sub(r"\.(zip|rar|7z|tar|tgz|tar\.gz|tar\.bz2|tar\.xz)$", "", name, flags=re.I)[:60]
            view.upsert_entity(arch_title, "proyecto", 3, "documento", f"Archivo {name}: {len(files)} ficheros",
                               f"zip:{name}")
            view.relate(view.self_name, "ha_estudiado", arch_title, view.self_type, "proyecto", 1.0, category="documento")

            # 5) organizacion por temas con el modelo de embeddings
            items = list(texts.items())[:400]
            themes = self._themes(job, items, view, arch_title) if items else []
            report["themes"] = themes

            # 6) razonamiento con el LLM sobre los documentos principales
            doc_cat = "documento"
            reasoned: list[dict] = []
            llm_ok = reason and s.llm.has_real_llm() and max_reason > 0
            chosen = self._representatives(items, themes, max_reason if llm_ok else 0)
            agents_txt = "; ".join(f"{a.id} ({a.description})" for a in s.organizer.specialists()) or "ninguno"
            for i, (rel, text) in enumerate(items):
                title = str(rel)[-60:]
                if (rel, text) in chosen:
                    self._step(job, f"Razonando {len(reasoned) + 1}/{len(chosen)}: {rel}", len(reasoned), len(chosen))
                    out = None
                    try:
                        out = _json_obj(s.llm.complete(REASON_PROMPT.format(agents=agents_txt, path=rel, text=text[:3500]),
                                                       max_tokens=700, temperature=0.1))
                    except Exception as e:  # noqa: BLE001
                        L.warning("Razonamiento de %s fallido: %s", rel, e)
                    if out:
                        reasoned.append({"file": str(rel), **self._apply_reasoning(out, rel, title, scope, name, report)})
                        continue
                summary = _first_sentences(text)   # sin LLM: resumen extractivo y definiciones por reglas
                view.upsert_entity(title, "documento", 1, doc_cat, summary, f"zip:{name}")
                if i < 120:
                    report["facts"] += len(view.extract_topics(text[:3000], "user", ""))
            report["reasoned"] = len(reasoned)
            report["reasoning"] = reasoned[:50]
            if reasoned and llm_ok:
                self._step(job, "Sintetizando el conjunto")
                try:
                    synth = s.llm.complete(SYNTH_PROMPT.format(name=name, items="\n".join(
                        f"- {r['file']}: {r.get('resumen', '')}" for r in reasoned[:20])), max_tokens=300, temperature=0.2)
                except Exception:  # noqa: BLE001
                    synth = ""
                if synth.strip():
                    report["summary"] = synth.strip()[:1200]
                    s.graph.db.execute("UPDATE entities SET note=? WHERE scope=? AND key=?",
                                       (report["summary"][:500], scope, _key(arch_title)))
                    s.memory.remember(f"{arch_title}: {report['summary']}", "skill" if scope else "note", 0.7,
                                      source=f"zip:{name}", agent=scope)
            report["llm"] = bool(llm_ok)
            self._step(job, "Terminado")
            return report
        finally:
            shutil.rmtree(work, ignore_errors=True)
            if str(path).startswith(str(paths.sub("cache"))):
                Path(path).unlink(missing_ok=True)

    def _apply_reasoning(self, out: dict, rel, title: str, scope: str, name: str, report: dict) -> dict:
        s = self.s
        resumen = str(out.get("resumen") or "")[:500]
        amb = str(out.get("ambito") or "").strip().lower()
        dscope = scope
        if amb and amb not in ("general", "comun", "común") and s.agents.get(amb) and not scope:
            dscope = amb   # el archivo es general pero este documento es del oficio de un especialista
        view = s.graph.for_agent(dscope)
        view.upsert_entity(title, "documento", 1.5, "documento", resumen, f"zip:{name}")
        n = 0
        for h in (out.get("hechos") or [])[:8]:
            if not isinstance(h, dict) or not h.get("s") or not h.get("o"):
                continue
            cat = str(h.get("categoria") or "")
            cat = cat if cat in CATEGORIES else ""
            hv = s.graph.general() if cat in GENERAL_CATEGORIES and cat != "comun" else view
            if cat == "comun" and dscope:
                hv = s.graph.general()
            if cat in AGENT_CATEGORIES and hv.scope == "":
                cat = "comun"
            if hv.relate(str(h["s"])[:60], str(h.get("r") or "relacionado_con")[:30], str(h["o"])[:60],
                         category=cat, src_category=cat):
                if hv is view:   # el documento explica el hecho (en el mismo ambito)
                    hv.relate(title, "explica", str(h["s"])[:60], "documento", "concepto", 0.5, src_category="documento")
                n += 1
        report["facts"] += n
        steps = [str(p)[:200] for p in (out.get("pasos") or []) if str(p).strip()][:12]
        if steps:   # procedimientos: memoria propia del agente (o comun si el archivo es general)
            s.memory.remember(f"Procedimiento ({rel}): " + " → ".join(steps), "skill", 0.75, source=f"zip:{name}",
                              agent=dscope)
            view.upsert_entity(f"Cómo: {title}"[:60], "procedimiento", 1.2,
                               "procedimiento" if dscope else "comun", " → ".join(steps)[:500], f"zip:{name}")
            report["skills"] += 1
        if resumen:
            s.memory.remember(f"{title}: {resumen}", "skill" if dscope else "note", 0.6, source=f"zip:{name}", agent=dscope)
        return {"resumen": resumen, "ambito": dscope or "general", "hechos": n, "pasos": len(steps)}

    def _themes(self, job: dict, items: list, view: KnowledgeGraph, arch_title: str) -> list[dict]:
        """Agrupa los documentos por tema (embeddings con la instruccion «organizar documentacion») y lo refleja en
        el grafo: archivo -contiene-> tema -incluye-> documento."""
        emb = self.s.embedder
        semantic = not str(emb.id).startswith("hash")
        thr = 0.62 if semantic else 0.3
        texts = [f"{rel}\n{t[:600]}" for rel, t in items]
        vecs = []
        for b in range(0, len(texts), 32):
            vecs.append(emb.embed_query(texts[b:b + 32], "organize") if hasattr(emb, "embed_query") else emb.embed(texts[b:b + 32]))
            self._step(job, f"Organizando por temas {min(b + 32, len(texts))}/{len(texts)}", min(b + 32, len(texts)), len(texts))
        mat = np.vstack(vecs)
        clusters: list[dict] = []
        for i, v in enumerate(mat):
            best, sim = None, -1.0
            for c in clusters:
                cs = float(c["centroid"] @ v / max(np.linalg.norm(c["centroid"]), 1e-9))
                if cs > sim:
                    best, sim = c, cs
            if best is not None and (sim >= thr or len(clusters) >= 12):
                best["members"].append(i)
                best["centroid"] = best["centroid"] + v
            else:
                clusters.append({"members": [i], "centroid": v.copy()})
        out = []
        for c in sorted(clusters, key=lambda c: -len(c["members"])):
            members = [items[i] for i in c["members"]]
            kws = KnowledgeGraph.keywords(" ".join(f"{r.stem.replace('_', ' ')}. {t[:800]}" for r, t in members[:20]), 3)
            label = re.sub(r"\s+", " ", "Tema: " + ", ".join(kws))[:60] if kws else f"Tema {len(out) + 1}"
            view.upsert_entity(label, "tema", 1 + 0.2 * len(members), "", f"{len(members)} documentos de {arch_title}",
                               "organizador")
            view.relate(arch_title, "contiene", label, "proyecto", "tema", 1.0, src_category="documento")
            for rel, _ in members[:60]:
                view.relate(label, "incluye", str(rel)[-60:], "tema", "documento", 0.6, category="documento")
            out.append({"theme": label, "documents": len(members), "examples": [str(r) for r, _ in members[:5]],
                        "_members": c["members"]})
        return out

    @staticmethod
    def _representatives(items: list, themes: list, n: int) -> list:
        """Documentos que razona el LLM: los principales (README, guias...) y uno por tema, hasta n."""
        if n <= 0 or not items:
            for t in themes:
                t.pop("_members", None)
            return []
        chosen: list = []
        for t in themes:   # primero el mas representativo de cada tema (el primero en orden de prioridad)
            if len(chosen) >= n:
                break
            first = min(t["_members"])
            if items[first] not in chosen:
                chosen.append(items[first])
        for it in items:
            if len(chosen) >= n:
                break
            if it not in chosen:
                chosen.append(it)
        for t in themes:
            t.pop("_members", None)
        return chosen

    # ------------------------------------------------------------ reorganizar y duplicados
    def reorganize(self, apply: bool = False) -> dict:
        return self._start("reorganize", "Reorganizar la memoria" if apply else "Revisar la organizacion",
                           self._run_reorganize, apply)

    def _run_reorganize(self, job: dict, apply: bool) -> dict:
        self._step(job, "Clasificando lo aprendido (comun o de cada especialidad)")
        moves = self.s.organizer.plan(progress=lambda d, t: self._step(job, f"Clasificando {d}/{t}", d, t))
        res = {"moves": [{k: m.get(k) for k in ("id", "name", "type", "scope", "category", "to", "to_category", "reason",
                                                 "score")} for m in moves[:500]], "total": len(moves)}
        if apply:
            self._step(job, f"Moviendo {len(moves)} nodos")
            res.update(self.s.organizer.apply(moves, store=self.s.memory))
        return res

    def duplicates(self, scope: str | None = None) -> dict:
        return self._start("duplicates", "Buscar duplicados", self._run_duplicates, scope)

    def _run_duplicates(self, job: dict, scope: str | None) -> dict:
        self._step(job, "Comparando nodos con el modelo de embeddings")
        pairs = self.s.organizer.duplicates(scope)
        return {"pairs": pairs, "total": len(pairs)}
