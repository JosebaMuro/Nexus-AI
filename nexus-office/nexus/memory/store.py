"""Memoria a largo plazo: conversaciones, hechos, preferencias, episodios, ejemplos y lecciones.

Recuperacion = similitud semantica x importancia x recencia (decaimiento exponencial).

Ambitos: los hechos, preferencias y pautas de comportamiento del usuario son comunes (``agent=""``); los ejemplos y
lo aprendido por un agente en su especialidad son suyos (``agent=<id>``) y ningun otro agente los recupera.
"""
from __future__ import annotations

import math
import time
import uuid

import numpy as np

from .. import log
from ..db import DB
from .embeddings import HashingEmbedder

L = log.get("memory")
KINDS = ("fact", "preference", "behavior", "episode", "summary", "example", "lesson", "note", "skill")


class MemoryStore:
    def __init__(self, db: DB, embedder, decay_days: float = 30.0) -> None:
        self.db = db
        self.embedder = embedder
        self.decay_days = decay_days
        self._cache: dict | None = None
        self.fast = HashingEmbedder()
        self.on_pending = None

    # ------------------------------------------------------------ conversaciones
    def new_session(self, title: str = "Nueva conversacion") -> str:
        sid = uuid.uuid4().hex[:12]
        now = time.time()
        self.db.execute("INSERT INTO sessions(id, title, created, updated) VALUES(?,?,?,?)", (sid, title, now, now))
        return sid

    def ensure_session(self, sid: str | None, title: str = "") -> str:
        if sid and self.db.one("SELECT id FROM sessions WHERE id=?", (sid,)):
            return sid
        return self.new_session(title[:60] or "Nueva conversacion")

    def add_message(self, session_id: str, role: str, content: str, agent: str = "", meta: str = "") -> int:
        now = time.time()
        cur = self.db.execute("INSERT INTO messages(session_id, role, content, agent, ts, meta) VALUES(?,?,?,?,?,?)",
                              (session_id, role, content, agent, now, meta))
        self.db.execute("UPDATE sessions SET updated=? WHERE id=?", (now, session_id))
        return int(cur.lastrowid)

    def history(self, session_id: str, limit: int = 20) -> list[dict]:
        rows = self.db.query("SELECT * FROM messages WHERE session_id=? ORDER BY id DESC LIMIT ?", (session_id, limit))
        return list(reversed(rows))

    def sessions(self, limit: int = 50) -> list[dict]:
        return self.db.query("SELECT s.*, (SELECT COUNT(*) FROM messages m WHERE m.session_id=s.id) AS n "
                             "FROM sessions s ORDER BY updated DESC LIMIT ?", (limit,))

    def delete_session(self, sid: str) -> None:
        self.db.execute("DELETE FROM messages WHERE session_id=?", (sid,))
        self.db.execute("DELETE FROM sessions WHERE id=?", (sid,))

    # ------------------------------------------------------------ memoria semantica
    def _vec(self, text: str) -> np.ndarray:
        return self.embedder.embed([text])[0]

    def _matrix(self) -> dict:
        """{"sem": (matriz, ids), "pend": (matriz, ids)}: recuerdos con vector del modelo activo y pendientes (hashing)
        de que el indexador de fondo los convierta cuando el modelo es lento."""
        if self._cache is None:
            cur = self.embedder.id
            slow = getattr(self.embedder, "slow", False)
            ok = {cur, self.fast.id} if slow else {cur}
            rows = self.db.query("SELECT id, embedding, embedder, content FROM memories")
            stale = [r for r in rows if r["embedder"] not in ok or r["embedding"] is None]
            if stale:  # el embedder cambio: re-indexar (con hashing si el modelo nuevo es lento)
                emb = self.fast if slow else self.embedder
                vecs = emb.embed([r["content"] for r in stale])
                self.db.executemany("UPDATE memories SET embedding=?, embedder=? WHERE id=?",
                                    ((v.astype(np.float32).tobytes(), emb.id, r["id"]) for r, v in zip(stale, vecs)))
                for r, v in zip(stale, vecs):
                    r["embedding"], r["embedder"] = v.astype(np.float32).tobytes(), emb.id
                if slow and self.on_pending:
                    self.on_pending()

            def build(sel):
                if not sel:
                    return np.zeros((0, 1), dtype=np.float32), []
                return np.vstack([np.frombuffer(r["embedding"], dtype=np.float32) for r in sel]), [r["id"] for r in sel]
            self._cache = {"sem": build([r for r in rows if r["embedder"] == cur]),
                           "pend": build([r for r in rows if r["embedder"] != cur])}
        return self._cache

    def remember(self, content: str, kind: str = "note", importance: float = 0.5, source: str = "",
                 agent: str = "", dedupe: float = 0.93) -> int:
        content = content.strip()
        if not content:
            return 0
        vec = self._vec(content)
        mat, ids = self._matrix()["sem"]
        if len(ids) and mat.shape[1] == vec.shape[0]:
            sims = mat @ vec
            j = int(np.argmax(sims))
            if sims[j] >= dedupe:
                self.db.execute("UPDATE memories SET importance=MIN(1.0, importance+0.1), last_access=?, "
                                "access_count=access_count+1 WHERE id=?", (time.time(), ids[j]))
                return ids[j]
        now = time.time()
        cur = self.db.execute(
            "INSERT INTO memories(kind, content, importance, embedding, embedder, created, last_access, source, agent)"
            " VALUES(?,?,?,?,?,?,?,?,?)",
            (kind, content, float(importance), vec.astype(np.float32).tobytes(), self.embedder.id, now, now, source, agent))
        self._cache = None
        return int(cur.lastrowid)

    def recall(self, query: str, k: int = 6, kinds: tuple[str, ...] | None = None, agent: str | None = None,
               min_score: float = 0.12) -> list[dict]:
        cache = self._matrix()
        sims: dict[int, float] = {}
        for key in ("sem", "pend"):
            mat, ids = cache[key]
            if not ids:
                continue
            if key == "sem":
                q = (self.embedder.embed_query([query], "memory") if hasattr(self.embedder, "embed_query")
                     else self.embedder.embed([query]))[0]
            else:
                q = self.fast.embed([query])[0]
            if mat.shape[1] != q.shape[0]:
                self._cache = None
                continue
            sims.update(zip(ids, (float(x) for x in mat @ q)))
        if not sims:
            return []
        meta = {r["id"]: r for r in self.db.query(
            "SELECT id, kind, content, importance, created, last_access, access_count, agent FROM memories")}
        now = time.time()
        scored = []
        for mid, sim in sims.items():
            r = meta.get(mid)
            if not r or (kinds and r["kind"] not in kinds) or (agent and r["agent"] and r["agent"] != agent):
                continue
            age_days = (now - (r["last_access"] or r["created"])) / 86400
            recency = math.exp(-age_days / max(self.decay_days, 1))
            score = 0.75 * float(sim) + 0.15 * r["importance"] + 0.10 * recency
            if sim >= min_score:
                scored.append({**r, "similarity": round(float(sim), 3), "score": round(score, 3)})
        scored.sort(key=lambda r: r["score"], reverse=True)
        top = scored[:k]
        for r in top:
            self.db.execute("UPDATE memories SET last_access=?, access_count=access_count+1 WHERE id=?", (now, r["id"]))
        return top

    def list(self, kind: str | None = None, limit: int = 100, offset: int = 0, agent: str | None = None) -> list[dict]:
        """agent=None: todos; "": solo los comunes; "x": solo los propios de x."""
        where, params = [], []
        if kind:
            where.append("kind=?")
            params.append(kind)
        if agent is not None:
            where.append("agent=?")
            params.append(agent)
        sql = " WHERE " + " AND ".join(where) if where else ""
        return self.db.query("SELECT id, kind, content, importance, created, access_count, source, agent "
                             f"FROM memories{sql} ORDER BY id DESC LIMIT ? OFFSET ?", (*params, limit, offset))

    def move(self, mid: int, agent: str) -> None:
        """Pasa un recuerdo a comun ("") o a la memoria propia de un agente."""
        self.db.execute("UPDATE memories SET agent=? WHERE id=?", (agent or "", mid))

    def forget(self, mid: int) -> None:
        self.db.execute("DELETE FROM memories WHERE id=?", (mid,))
        self._cache = None

    def adjust(self, mid: int, delta: float) -> None:
        self.db.execute("UPDATE memories SET importance=MAX(0.0, MIN(1.0, importance+?)) WHERE id=?", (delta, mid))

    def decay(self) -> int:
        """Olvido gradual de lo que no se usa; elimina episodios irrelevantes."""
        cutoff = time.time() - self.decay_days * 86400
        self.db.execute("UPDATE memories SET importance=importance*0.95 WHERE last_access < ? AND kind IN "
                        "('episode','note','summary')", (cutoff,))
        n = self.db.execute("DELETE FROM memories WHERE importance < 0.05 AND kind='episode'").rowcount
        if n:
            self._cache = None
        return n

    def stats(self) -> dict:
        rows = self.db.query("SELECT kind, COUNT(*) AS n FROM memories GROUP BY kind")
        msgs = self.db.one("SELECT COUNT(*) AS n FROM messages")["n"]
        by_agent = {r["agent"] or "general": r["n"] for r in self.db.query(
            "SELECT agent, COUNT(*) n FROM memories GROUP BY agent")}
        return {"by_kind": {r["kind"]: r["n"] for r in rows}, "messages": msgs, "embedder": self.embedder.id,
                "by_scope": by_agent}
