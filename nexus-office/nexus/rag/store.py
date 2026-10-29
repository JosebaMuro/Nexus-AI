"""Biblioteca RAG: documentos troceados con embeddings y busqueda hibrida (semantica + palabras clave).

Formatos: PDF (por paginas), Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, texto y codigo fuente,
archivos comprimidos ZIP/RAR/7z/tar (proyectos o documentacion) y texto directo. Cada documento se copia a
data/rag/files para que la biblioteca sea autocontenida y portable.

Destino: cada documento es del grafo general (``agent=""``, lo consultan todos los agentes) o de un agente
concreto (solo lo consulta ese agente y se incorpora a su subgrafo de conocimiento).

Indexacion progresiva: con un modelo de embeddings lento en CPU (Qwen3-Embedding en llama-server) los fragmentos
se guardan al momento con hashing y nexus.memory.embed_worker los convierte a vectores semanticos en segundo plano.
La busqueda combina ambos mientras tanto.
"""
from __future__ import annotations

import json
import re
import shutil
import time
from pathlib import Path

import numpy as np

from .. import log, paths
from ..db import DB
from ..memory.embeddings import HashingEmbedder, tokens

L = log.get("rag")
CHUNK, OVERLAP = 900, 150
EMBED_BLOCK = 2048
STOP = {"de", "la", "el", "los", "las", "y", "en", "un", "una", "que", "a", "por", "con", "para", "del", "se", "es",
        "the", "of", "and", "to", "in", "is", "for", "on", "with"}


def chunk_text(text: str, size: int = CHUNK, overlap: int = OVERLAP) -> list[str]:
    paras = [p.strip() for p in re.split(r"\n\s*\n|(?<=[.!?])\s{2,}", text) if p.strip()]
    chunks, cur = [], ""
    for p in paras:
        while len(p) > size:  # parrafos enormes: corte por frases/longitud
            cut = p.rfind(". ", 0, size)
            cut = cut + 1 if cut > size // 2 else size
            if cur:
                chunks.append(cur)
                cur = ""
            chunks.append(p[:cut].strip())
            p = p[cut:].strip()
        if len(cur) + len(p) + 2 > size and cur:
            chunks.append(cur)
            cur = cur[-overlap:] + " " if overlap else ""
        cur += ("\n\n" if cur.strip() else "") + p
    if cur.strip():
        chunks.append(cur.strip())
    return chunks


def read_document(path: Path) -> tuple[str, list[str]]:
    """Devuelve (titulo_sugerido, paginas_de_texto). Formatos en nexus.rag.readers."""
    from .readers import read_document as _read
    return _read(Path(path))


class RAGStore:
    def __init__(self, db: DB, embedder) -> None:
        self.db, self.embedder = db, embedder
        self.files = paths.sub("rag", "files")
        self.graph = None   # KnowledgeGraph (lo inyecta core.Services): cada documento se enlaza en su grafo
        self.fast = HashingEmbedder()
        self.on_pending = None   # aviso al indexador de fondo cuando quedan fragmentos por convertir
        self._cache = None

    def _index_embedder(self):
        """Con que se indexa al importar: el modelo activo o, si es lento, hashing (se completa en segundo plano)."""
        return self.fast if getattr(self.embedder, "slow", False) else self.embedder

    def _link(self, agent: str, title: str, text: str) -> None:
        if self.graph is not None:
            try:
                self.graph.for_agent(agent).link_document(title, text)
            except Exception as e:  # noqa: BLE001
                L.warning("RAG: no se pudo enlazar '%s' en el grafo: %s", title, e)

    # ------------------------------------------------------------ ingesta
    def _unlink(self, d: dict) -> None:
        """Quita el documento de su grafo, salvo que otro documento con el mismo titulo siga usando ese nodo."""
        if self.graph is not None and not self.db.one("SELECT 1 FROM rag_docs WHERE agent=? AND title=? AND id!=?",
                                                      (d["agent"], d["title"], d["id"])):
            self.graph.for_agent(d["agent"]).unlink_document(d["title"])

    def ingest_pages(self, title: str, pages: list[str], source: str = "", kind: str = "text", path: str = "",
                     meta: dict | None = None, labels: list[str] | None = None, agent: str = "",
                     link: bool = True) -> dict:
        """labels (opcional, uno por pagina) se antepone a cada fragmento: p.ej. la ruta del fichero en un ZIP.

        agent: "" = grafo general (todos los agentes); un id = solo ese agente. link=False si quien llama ya
        incorpora el documento al grafo con mas detalle (URL->PDF, rastreo)."""
        pieces = [(i + 1, f"{labels[i]}\n{c}" if labels and labels[i] else c)
                  for i, page in enumerate(pages) for c in chunk_text(page)]
        if not pieces:
            raise ValueError("el documento no contiene texto extraible")
        cur = self.db.execute("INSERT INTO rag_docs(title, source, path, kind, pages, chars, added, meta, agent) "
                              "VALUES(?,?,?,?,?,?,?,?,?)",
                              (title[:200], source, path, kind, len(pages), sum(len(p) for p in pages), time.time(),
                               json.dumps(meta or {}, ensure_ascii=False), agent or ""))
        doc_id = int(cur.lastrowid)
        emb = self._index_embedder()
        for b in range(0, len(pieces), EMBED_BLOCK):   # por bloques: memoria acotada en documentos enormes
            block = pieces[b:b + EMBED_BLOCK]
            vecs = emb.embed([f"{title}\n{c}" for _, c in block])
            self.db.executemany("INSERT INTO rag_chunks(doc_id, idx, page, text, embedding, embedder) VALUES(?,?,?,?,?,?)",
                                ((doc_id, b + i, page, text, v.astype(np.float32).tobytes(), emb.id)
                                 for i, ((page, text), v) in enumerate(zip(block, vecs))))
        self._cache = None
        if emb is not self.embedder and self.on_pending:
            self.on_pending()
        if link:
            self._link(agent, title, "\n".join(c for _, c in pieces[:8]))
        L.info("RAG: '%s' indexado (%d fragmentos) en el grafo %s", title, len(pieces), agent or "general")
        return self.get(doc_id)

    def ingest_file(self, src: Path, title: str = "", source: str = "", meta: dict | None = None, copy: bool = True,
                    agent: str = "", link: bool = True) -> dict:
        from .readers import archive_suffix
        src = Path(src)
        if archive_suffix(source or src.name) or archive_suffix(src.name):
            from .archive import ingest_archive
            return ingest_archive(self, src, title, source, meta, agent=agent)
        dest = src
        if copy:
            dest = self.files / f"{int(time.time())}_{re.sub(r'[^A-Za-z0-9._-]+', '_', src.name)[-80:]}"
            shutil.copy2(src, dest)
        suggested, pages = read_document(dest)
        return self.ingest_pages(title or suggested, pages, source or src.name, dest.suffix.lstrip(".").lower(),
                                 str(dest), meta, agent=agent, link=link)

    def ingest_many(self, items: list[tuple[Path, str]], emit=None, agent: str = "") -> list[dict]:
        """Importa varios ficheros a la vez: [(ruta, nombre_original)] -> [{'name', 'doc'} | {'name', 'error'}].

        Los documentos sueltos se leen en paralelo (un fichero por nucleo); los comprimidos se descomprimen
        uno tras otro (cada uno ya reparte su lectura). Los embeddings y el guardado van en este hilo."""
        from .. import parallel
        from .readers import archive_suffix, read_safe
        step = (lambda s: emit({"type": "progress", "text": s})) if emit else (lambda s: None)
        out: list[dict | None] = [None] * len(items)
        loose = [(i, p, n) for i, (p, n) in enumerate(items) if not archive_suffix(n)]
        stored = []
        for i, p, n in loose:   # copia a la biblioteca antes de leer (la biblioteca es autocontenida)
            dest = self.files / f"{int(time.time())}_{i}_{re.sub(r'[^A-Za-z0-9._-]+', '_', n)[-80:]}"
            shutil.copy2(p, dest)
            stored.append((i, dest, n))
        futs = [(i, dest, n, parallel.submit_cpu(read_safe, str(dest))) for i, dest, n in stored]
        for k, (i, dest, n, fut) in enumerate(futs, 1):
            title, pages, err = fut.result()
            step(f"Indexando {k}/{len(futs)}: {n}")
            try:
                if err:
                    raise ValueError(err)
                out[i] = {"name": n, "doc": self.ingest_pages(title, pages, n, dest.suffix.lstrip(".").lower(), str(dest),
                                                              agent=agent)}
            except Exception as e:  # noqa: BLE001
                dest.unlink(missing_ok=True)
                out[i] = {"name": n, "error": str(e)}
        for i, (p, n) in enumerate(items):
            if out[i] is None:
                step(f"Descomprimiendo {n}")
                try:
                    out[i] = {"name": n, "doc": self.ingest_file(Path(p), source=n, agent=agent)}
                except Exception as e:  # noqa: BLE001
                    out[i] = {"name": n, "error": str(e)}
        return out  # type: ignore[return-value]

    # ------------------------------------------------------------ busqueda
    def _matrix(self) -> dict:
        """{"sem": (matriz, ids, destinos), "pend": (...)}: fragmentos con vector del modelo activo y pendientes de
        convertir (con hashing). Se cachea hasta que cambie la biblioteca."""
        if self._cache is None:
            cur = self.embedder.id
            slow = getattr(self.embedder, "slow", False)
            ok = {cur, self.fast.id} if slow else {cur}
            # sin el texto: solo hace falta para re-indexar fragmentos de otro embedder (mucho menos memoria)
            rows = self.db.query("SELECT c.id, c.embedding, c.embedder, d.agent FROM rag_chunks c "
                                 "JOIN rag_docs d ON d.id=c.doc_id ORDER BY c.id")
            stale = [r for r in rows if r["embedder"] not in ok or r["embedding"] is None]
            if stale:   # otro embedder: rapido -> se recalcula ya; modelo lento -> hashing y el resto en segundo plano
                emb = self.fast if slow else self.embedder
                texts = {r["id"]: r["text"] for r in self.db.query(
                    f"SELECT id, text FROM rag_chunks WHERE embedder IS NULL OR embedder NOT IN ({','.join('?' * len(ok))})",
                    tuple(ok))}
                for b in range(0, len(stale), EMBED_BLOCK):
                    part = stale[b:b + EMBED_BLOCK]
                    vecs = emb.embed([texts.get(r["id"], "") for r in part])
                    for r, v in zip(part, vecs):
                        r["embedding"], r["embedder"] = v.astype(np.float32).tobytes(), emb.id
                    self.db.executemany("UPDATE rag_chunks SET embedding=?, embedder=? WHERE id=?",
                                        ((r["embedding"], emb.id, r["id"]) for r in part))
                if slow and self.on_pending:
                    self.on_pending()

            def build(sel: list[dict]):
                if not sel:
                    return None, [], np.array([], dtype=object)
                return (np.vstack([np.frombuffer(r["embedding"], dtype=np.float32) for r in sel]), [r["id"] for r in sel],
                        np.array([r["agent"] or "" for r in sel], dtype=object))
            self._cache = {"sem": build([r for r in rows if r["embedder"] == cur]),
                           "pend": build([r for r in rows if r["embedder"] != cur])}
        return self._cache

    def search(self, query: str, k: int = 5, min_score: float = 0.15, doc_id: int | None = None,
               agent: str | None = None) -> list[dict]:
        """agent=None: toda la biblioteca. agent="": solo lo general. agent="x": lo general y lo de "x"."""
        cache = self._matrix()
        allowed = None
        if doc_id:
            allowed = {r["id"] for r in self.db.query("SELECT id FROM rag_chunks WHERE doc_id=?", (doc_id,))}
        sims_by_id: dict[int, float] = {}
        for key in ("sem", "pend"):
            mat, ids, owners = cache[key]
            if mat is None or not ids:
                continue
            q = (self.embedder.embed_query([query], "search") if key == "sem" and hasattr(self.embedder, "embed_query")
                 else (self.embedder if key == "sem" else self.fast).embed([query]))[0]
            if mat.shape[1] != q.shape[0]:
                self._cache = None
                continue
            sims = mat @ q
            if agent is not None:   # aislamiento: nunca se ven los documentos de otro agente
                sims = np.where((owners == "") | (owners == agent), sims, -np.inf)
            if allowed is not None:
                sims = np.where(np.fromiter((i in allowed for i in ids), bool, len(ids)), sims, -np.inf)
            n = min(len(ids), max(k * 6, 30))
            top = np.argpartition(-sims, n - 1)[:n] if n < len(ids) else np.arange(len(ids))
            for i in top:
                if np.isfinite(sims[i]):
                    sims_by_id[ids[int(i)]] = float(sims[i])
        if not sims_by_id:
            return []
        qterms = {t for t in tokens(query) if t not in STOP and len(t) > 2}
        rows = {r["id"]: r for r in self.db.query(
            f"SELECT c.id, c.doc_id, c.page, c.text, d.title, d.source, d.agent FROM rag_chunks c "
            f"JOIN rag_docs d ON d.id=c.doc_id WHERE c.id IN ({','.join(str(int(i)) for i in sims_by_id)})")}
        out = []
        for cid, sim in sims_by_id.items():
            r = rows.get(cid)
            if not r:
                continue
            words = set(tokens(r["text"]))
            kw = len(qterms & words) / len(qterms) if qterms else 0.0
            score = 0.7 * sim + 0.3 * kw   # hibrida: semantica + coincidencia de terminos
            if score >= min_score:
                out.append({**r, "score": round(score, 3)})
        out.sort(key=lambda r: r["score"], reverse=True)
        return out[:k]

    # ------------------------------------------------------------ gestion
    def get(self, doc_id: int) -> dict | None:
        d = self.db.one("SELECT * FROM rag_docs WHERE id=?", (doc_id,))
        if d:
            d["meta"] = json.loads(d["meta"] or "{}")
            d["chunks"] = self.db.one("SELECT COUNT(*) n FROM rag_chunks WHERE doc_id=?", (doc_id,))["n"]
        return d

    def list(self, agent: str | None = None) -> list[dict]:
        """agent=None: todos; "" solo los generales; "x": los generales y los de "x"."""
        where, params = "", ()
        if agent is not None:
            where, params = "WHERE d.agent IN ('', ?) ", (agent,)
        rows = self.db.query("SELECT d.*, (SELECT COUNT(*) FROM rag_chunks c WHERE c.doc_id=d.id) AS chunks "
                             f"FROM rag_docs d {where}ORDER BY d.id DESC", params)
        for r in rows:
            r["meta"] = json.loads(r["meta"] or "{}")
        return rows

    def assign(self, doc_id: int, agent: str) -> dict:
        """Mueve un documento al grafo general ("") o al de un agente; su nodo de conocimiento se mueve con el."""
        d = self.get(doc_id)
        if not d:
            raise KeyError(doc_id)
        agent = agent or ""
        if d["agent"] != agent:
            self.db.execute("UPDATE rag_docs SET agent=? WHERE id=?", (agent, doc_id))
            self._cache = None
            self._unlink(d)
            if self.graph is not None:
                sample = self.db.query("SELECT text FROM rag_chunks WHERE doc_id=? ORDER BY idx LIMIT 8", (doc_id,))
                self._link(agent, d["title"], "\n".join(r["text"] for r in sample))
        return self.get(doc_id)

    def reassign_agent(self, agent: str, to: str = "") -> int:
        """Al borrar un agente sus documentos pasan al grafo general (no se pierde la biblioteca)."""
        docs = [d["id"] for d in self.db.query("SELECT id FROM rag_docs WHERE agent=?", (agent,))] if agent else []
        for doc_id in docs:
            self.assign(doc_id, to)
        return len(docs)

    def delete(self, doc_id: int) -> None:
        d = self.get(doc_id)
        if d:
            self._unlink(d)
            for other in d["meta"].get("part_ids") or []:   # proyecto importado en varias partes
                if other != doc_id:
                    self.delete(other)
        self.db.execute("DELETE FROM rag_chunks WHERE doc_id=?", (doc_id,))
        self.db.execute("DELETE FROM rag_docs WHERE id=?", (doc_id,))
        if d and d["path"] and str(self.files) in d["path"]:
            Path(d["path"]).unlink(missing_ok=True)
        if d and d["meta"].get("sandbox") and d["meta"].get("archive"):   # proyecto descomprimido (ZIP, RAR...)
            root = paths.sandbox_dir().resolve()
            folder = (root / d["meta"]["sandbox"]).resolve()
            if folder != root and folder.is_relative_to(root):
                shutil.rmtree(folder, ignore_errors=True)
        self._cache = None

    def stats(self) -> dict:
        by = {r["agent"] or "general": r["n"] for r in self.db.query("SELECT agent, COUNT(*) n FROM rag_docs GROUP BY agent")}
        return {"documents": sum(by.values()), "chunks": self.db.one("SELECT COUNT(*) n FROM rag_chunks")["n"],
                "by_scope": by}
