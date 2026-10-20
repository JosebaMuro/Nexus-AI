"""Indexacion semantica progresiva.

Un modelo de embeddings neuronal (Qwen3-Embedding en llama-server) es mucho mas preciso que el hashing, pero en una
CPU modesta calcula del orden de un fragmento por segundo. Para que importar un PDF, un ZIP o un rastreo siga siendo
inmediato, la biblioteca y la memoria guardan primero un vector de hashing (milisegundos) y este hilo los sustituye
por vectores semanticos poco a poco, en segundo plano:

* primero los recuerdos (pocos y los que mas se usan en cada turno) y despues los fragmentos RAG, de lo mas
  reciente a lo mas antiguo;
* en lotes pequenos, y cediendo la CPU mientras el LLM esta respondiendo o hay una consulta esperando;
* mientras tanto la busqueda combina ambos: semantica en lo ya convertido y hashing en lo pendiente.

Al cambiar de modelo de embeddings ocurre lo mismo: nada se bloquea y todo se recalcula aqui.
"""
from __future__ import annotations

import threading
import time

import numpy as np

from .. import log

L = log.get("embeddings")
BATCH = 4            # fragmentos por peticion: el chat nunca espera mas de un lote
REFRESH_EVERY = 256  # fragmentos convertidos entre recargas de la matriz de busqueda


class EmbedWorker:
    def __init__(self, services) -> None:
        self.s = services
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.state = {"running": False, "done": 0, "pending": 0, "rate": 0.0, "error": "", "phase": ""}

    # ------------------------------------------------------------ control
    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            self.wake()
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, name="nexus-embeddings", daemon=True)
        self._thread.start()

    def wake(self) -> None:
        self._wake.set()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()

    def pending(self) -> dict:
        emb = self.s.embedder.id
        mem = self.s.db.one("SELECT COUNT(*) n FROM memories WHERE embedder IS NOT ?", (emb,))["n"]
        rag = self.s.db.one("SELECT COUNT(*) n FROM rag_chunks WHERE embedder IS NOT ?", (emb,))["n"]
        return {"memories": mem, "chunks": rag, "total": mem + rag}

    def status(self) -> dict:
        info = self.s.embedder.info() if hasattr(self.s.embedder, "info") else {"id": self.s.embedder.id}
        return {**self.state, **self.pending(), "embedder": info}

    # ------------------------------------------------------------ bucle
    def _busy(self) -> bool:
        llm = getattr(self.s, "llm", None)
        return bool(llm is not None and getattr(llm, "inflight", 0) > 0) or getattr(self.s.embedder, "waiting", 0) > 0

    def _loop(self) -> None:
        while not self._stop.is_set():
            emb = self.s.embedder
            if not getattr(emb, "slow", False):
                self.state.update(running=False, phase="")
                self._wake.wait(30)
                self._wake.clear()
                continue
            try:
                worked = self._step(emb)
                self.state["error"] = ""
            except Exception as e:  # noqa: BLE001
                L.warning("Indexacion semantica en pausa: %s", e)
                self.state.update(error=str(e)[:300], running=False)
                self._wake.wait(60)
                self._wake.clear()
                continue
            if not worked:
                if self.state["running"]:
                    self.s.rag._cache = None
                    self.s.memory._cache = None
                    L.info("Indexacion semantica completa (%d vectores)", self.state["done"])
                self.state.update(running=False, phase="", pending=0)
                self._wake.wait(30)
                self._wake.clear()

    def _step(self, emb) -> bool:
        """Convierte un lote. False si no queda nada pendiente."""
        while self._busy() and not self._stop.is_set():
            time.sleep(0.5)   # el chat tiene prioridad sobre la indexacion
        db = self.s.db
        rows = db.query("SELECT id, content AS text FROM memories WHERE embedder IS NOT ? ORDER BY last_access DESC "
                        "LIMIT ?", (emb.id, BATCH))
        table = "memories"
        if not rows:
            rows = db.query("SELECT id, text FROM rag_chunks WHERE embedder IS NOT ? ORDER BY doc_id DESC, idx LIMIT ?",
                            (emb.id, BATCH))
            table = "rag_chunks"
        if not rows:
            return False
        if not self.state["running"]:
            self.state.update(running=True, done=0)
        t0 = time.time()
        if table == "rag_chunks":   # el titulo da contexto al fragmento, igual que al indexar
            titles = {r["id"]: r["title"] for r in db.query(
                f"SELECT c.id, d.title FROM rag_chunks c JOIN rag_docs d ON d.id=c.doc_id WHERE c.id IN "
                f"({','.join(str(int(r['id'])) for r in rows)})")}
            texts = [f"{titles.get(r['id'], '')}\n{r['text']}" for r in rows]
        else:
            texts = [r["text"] or "" for r in rows]
        vecs = emb.embed(texts)
        if emb is not self.s.embedder:   # el modelo cambio mientras calculaba: se descarta el lote
            return True
        db.executemany(f"UPDATE {table} SET embedding=?, embedder=? WHERE id=?",
                       ((v.astype(np.float32).tobytes(), emb.id, r["id"]) for r, v in zip(rows, vecs)))
        n = len(rows)
        dt = max(time.time() - t0, 1e-3)
        self.state["done"] += n
        self.state["rate"] = round(0.7 * self.state["rate"] + 0.3 * (n / dt), 2) if self.state["rate"] else round(n / dt, 2)
        self.state["phase"] = "recuerdos" if table == "memories" else "biblioteca"
        if table == "memories":
            self.s.memory._cache = None
        elif self.state["done"] % REFRESH_EVERY < n:
            self.s.rag._cache = None
        return True
