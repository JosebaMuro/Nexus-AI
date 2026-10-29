"""SQLite compartido (WAL) con esquema versionado. Una conexion protegida por lock."""
from __future__ import annotations

import json
import sqlite3
import threading
from contextlib import contextmanager
from typing import Any, Iterable

from . import paths

SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY, title TEXT, created REAL, updated REAL);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT, role TEXT, content TEXT,
    agent TEXT, ts REAL, meta TEXT);
CREATE INDEX IF NOT EXISTS ix_messages_session ON messages(session_id, id);
CREATE TABLE IF NOT EXISTS memories (
    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, content TEXT, importance REAL,
    embedding BLOB, embedder TEXT, created REAL, last_access REAL, access_count INTEGER DEFAULT 0,
    source TEXT, agent TEXT);
CREATE INDEX IF NOT EXISTS ix_memories_kind ON memories(kind);
CREATE TABLE IF NOT EXISTS entities (
    id INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL DEFAULT '', key TEXT, name TEXT, type TEXT,
    weight REAL DEFAULT 1, mentions INTEGER DEFAULT 1, first_seen REAL, last_seen REAL,
    category TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', source TEXT NOT NULL DEFAULT '', UNIQUE(scope, key));
CREATE TABLE IF NOT EXISTS relations (
    id INTEGER PRIMARY KEY AUTOINCREMENT, src INTEGER, dst INTEGER, rel TEXT,
    weight REAL DEFAULT 1, last_seen REAL, scope TEXT NOT NULL DEFAULT '', UNIQUE(src, dst, rel));
CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT, message_id INTEGER, score INTEGER, comment TEXT, ts REAL);
CREATE TABLE IF NOT EXISTS profile (key TEXT PRIMARY KEY, value TEXT, updated REAL);
CREATE TABLE IF NOT EXISTS lounge (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, agent TEXT, kind TEXT, content TEXT, meta TEXT);
CREATE TABLE IF NOT EXISTS agent_skills (
    agent TEXT, topic TEXT, ok REAL DEFAULT 0, fail REAL DEFAULT 0, last REAL, PRIMARY KEY(agent, topic));
CREATE TABLE IF NOT EXISTS agent_proposals (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, spec TEXT, reason TEXT, status TEXT, proposer TEXT);
CREATE TABLE IF NOT EXISTS rag_docs (
    id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, source TEXT, path TEXT, kind TEXT, pages INTEGER,
    chars INTEGER, added REAL, meta TEXT, agent TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS rag_chunks (
    id INTEGER PRIMARY KEY AUTOINCREMENT, doc_id INTEGER, idx INTEGER, page INTEGER, text TEXT,
    embedding BLOB, embedder TEXT);
CREATE INDEX IF NOT EXISTS ix_rag_chunks_doc ON rag_chunks(doc_id);
CREATE TABLE IF NOT EXISTS entity_vecs (
    entity_id INTEGER PRIMARY KEY, embedder TEXT, key TEXT, vec BLOB);
CREATE TABLE IF NOT EXISTS tool_calls (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, agent TEXT, session_id TEXT, tool TEXT, args TEXT,
    risk TEXT, decision TEXT, approver TEXT, status TEXT, result TEXT, duration REAL);
"""
# indices sobre columnas que las bases antiguas no tienen: se crean despues de migrar
INDEXES = """
CREATE INDEX IF NOT EXISTS ix_entities_scope ON entities(scope, weight DESC);
CREATE INDEX IF NOT EXISTS ix_relations_src ON relations(src);
CREATE INDEX IF NOT EXISTS ix_relations_dst ON relations(dst);
CREATE INDEX IF NOT EXISTS ix_relations_scope ON relations(scope, weight DESC);
CREATE INDEX IF NOT EXISTS ix_rag_docs_agent ON rag_docs(agent);
CREATE INDEX IF NOT EXISTS ix_entities_category ON entities(scope, category);
CREATE INDEX IF NOT EXISTS ix_rag_chunks_embedder ON rag_chunks(embedder);
"""


class DB:
    def __init__(self, path: str | None = None) -> None:
        self.path = str(path or paths.db_file())
        self._lock = threading.RLock()
        self._depth = 0   # >0 dentro de transaction(): las escrituras se confirman todas juntas al final
        self.conn = sqlite3.connect(self.path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        with self._lock:
            self.conn.execute("PRAGMA journal_mode=WAL")
            self.conn.execute("PRAGMA synchronous=NORMAL")
            self.conn.executescript(SCHEMA)
            self._migrate()
            self.conn.executescript(INDEXES)
            self.conn.commit()

    def _columns(self, table: str) -> set[str]:
        return {r[1] for r in self.conn.execute(f"PRAGMA table_info({table})")}

    def _migrate(self) -> None:
        """Bases de versiones anteriores: grafo por ambitos (general + uno por agente), RAG con destino y
        categorias de conocimiento (1.6: que es comun y que es saber hacer de cada agente)."""
        if "scope" not in self._columns("entities"):
            # la clave unica pasa de (key) a (scope, key): SQLite exige rehacer la tabla. Se conservan los ids.
            self.conn.executescript("""
                ALTER TABLE entities RENAME TO entities_v1;
                CREATE TABLE entities (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL DEFAULT '', key TEXT, name TEXT, type TEXT,
                    weight REAL DEFAULT 1, mentions INTEGER DEFAULT 1, first_seen REAL, last_seen REAL, UNIQUE(scope, key));
                INSERT INTO entities(id, scope, key, name, type, weight, mentions, first_seen, last_seen)
                    SELECT id, '', key, name, type, weight, mentions, first_seen, last_seen FROM entities_v1;
                DROP TABLE entities_v1;
            """)
        if "scope" not in self._columns("relations"):
            self.conn.execute("ALTER TABLE relations ADD COLUMN scope TEXT NOT NULL DEFAULT ''")
        if "agent" not in self._columns("rag_docs"):
            self.conn.execute("ALTER TABLE rag_docs ADD COLUMN agent TEXT NOT NULL DEFAULT ''")
        cols = self._columns("entities")
        for col in ("category", "note", "source"):
            if col not in cols:
                self.conn.execute(f"ALTER TABLE entities ADD COLUMN {col} TEXT NOT NULL DEFAULT ''")

    @contextmanager
    def transaction(self):
        """Agrupa muchas escrituras en una sola confirmacion (y las deshace todas si algo falla)."""
        with self._lock:
            self._depth += 1
            try:
                yield self
            except BaseException:
                if self._depth == 1:
                    self.conn.rollback()
                raise
            finally:
                self._depth -= 1
                if self._depth == 0 and self.conn.in_transaction:
                    self.conn.commit()

    def execute(self, sql: str, params: Iterable[Any] = ()) -> sqlite3.Cursor:
        with self._lock:
            cur = self.conn.execute(sql, tuple(params))
            if not self._depth:
                self.conn.commit()
            return cur

    def returning(self, sql: str, params: Iterable[Any] = ()) -> dict | None:
        """INSERT/UPDATE ... RETURNING: lee la fila antes de confirmar."""
        with self._lock:
            row = self.conn.execute(sql, tuple(params)).fetchone()
            if not self._depth:
                self.conn.commit()
            return dict(row) if row else None

    def executemany(self, sql: str, rows: Iterable[Iterable[Any]]) -> None:
        """Muchas filas en una sola transaccion (mucho mas rapido que una confirmacion por fila)."""
        with self._lock:
            self.conn.executemany(sql, [tuple(r) for r in rows])
            if not self._depth:
                self.conn.commit()

    def query(self, sql: str, params: Iterable[Any] = ()) -> list[dict]:
        with self._lock:
            return [dict(r) for r in self.conn.execute(sql, tuple(params)).fetchall()]

    def one(self, sql: str, params: Iterable[Any] = ()) -> dict | None:
        rows = self.query(sql, params)
        return rows[0] if rows else None

    # perfil clave/valor (preferencias aprendidas)
    def get_profile(self) -> dict:
        return {r["key"]: json.loads(r["value"]) for r in self.query("SELECT key, value FROM profile")}

    def set_profile(self, key: str, value: Any) -> None:
        import time
        self.execute("INSERT INTO profile(key, value, updated) VALUES(?,?,?) "
                     "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated=excluded.updated",
                     (key, json.dumps(value, ensure_ascii=False), time.time()))

    def close(self) -> None:
        with self._lock:
            self.conn.close()
