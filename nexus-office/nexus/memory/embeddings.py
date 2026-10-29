"""Embeddings con degradacion elegante:
modelo GGUF de embeddings (Qwen3-Embedding, en llama-server) > sentence-transformers (si esta) > Ollama (si
responde) > hashing determinista (siempre disponible).

Todos ofrecen ``embed(textos)`` para documentos y ``embed_query(textos, tarea)`` para consultas. Los modelos que
entienden instrucciones (Qwen3-Embedding) reciben una distinta segun la tarea: buscar en documentacion, recordar,
clasificar por especialidad, agrupar por tema o detectar duplicados. Los demas ignoran la tarea.
"""
from __future__ import annotations

import importlib.util
import re
import socket
import subprocess
import threading
import time
import unicodedata
import zlib
from pathlib import Path

import numpy as np

from .. import log

L = log.get("embeddings")
_WORD = re.compile(r"\w+", re.U)


def normalize(text: str) -> str:
    text = unicodedata.normalize("NFKD", text.lower())
    return "".join(c for c in text if not unicodedata.combining(c))


def tokens(text: str) -> list[str]:
    return _WORD.findall(normalize(text))


# Instrucciones por tarea (en ingles: Qwen3-Embedding rinde mejor asi aunque el texto sea espanol)
TASKS = {
    "search": "Given a question, retrieve passages from technical documentation, manuals and project files that answer it",
    "memory": "Given a user message, retrieve stored facts, preferences and past conversation notes relevant to it",
    "classify": "Identify the field of expertise or knowledge area this text belongs to",
    "organize": "Identify the main topic of this document to organize a documentation library",
    "dedupe": "Retrieve names of concepts that refer to the same thing as this concept",
    "graph": "Given a query, retrieve knowledge graph entities and facts related to it",
    "tools": "Given a user request in any language, retrieve the software tool (function) that should be used to fulfil it",
    "route": "Given a user request in any language, retrieve the specialist assistant whose field of expertise fits it best",
    "claim": "Given a statement, retrieve passages that state the same fact or contradict it",
}


class Embedder:
    """Interfaz comun. ``slow``: en CPU tarda (modelo neuronal): la biblioteca indexa al momento con hashing y
    completa los vectores semanticos en segundo plano (nexus.memory.embed_worker)."""
    id = "base"
    slow = False
    instructions = False

    def embed(self, texts: list[str]) -> np.ndarray:
        raise NotImplementedError

    def embed_query(self, texts: list[str], task: str = "search") -> np.ndarray:
        return self.embed(texts)

    def info(self) -> dict:
        return {"id": self.id, "slow": self.slow, "instructions": self.instructions,
                "kind": self.id.split(":", 1)[0].split("-", 1)[0]}

    def close(self) -> None:
        pass


class HashingEmbedder(Embedder):
    """Feature hashing de palabras, bigramas y n-gramas de caracteres. Sin dependencias, estable entre procesos."""

    def __init__(self, dims: int = 768) -> None:
        self.dims = dims
        self.id = f"hash-{dims}"

    def _feats(self, text: str):
        words = tokens(text)
        for w in words:
            yield w, 1.0
            if len(w) > 4:
                padded = f"<{w}>"
                for i in range(len(padded) - 3):
                    yield "#" + padded[i:i + 4], 0.35
        for a, b in zip(words, words[1:]):
            yield f"{a}_{b}", 0.6

    def embed(self, texts: list[str]) -> np.ndarray:
        if len(texts) >= PARALLEL_MIN:   # lotes grandes (rastreos, ZIP): un trozo por nucleo
            from .. import parallel
            if parallel.process_pool() is not None:
                parts = parallel.split(list(texts), parallel.configured_workers())
                futs = [parallel.submit_cpu(_hash_embed, self.dims, p) for p in parts]
                return np.vstack([f.result() for f in futs])
        return self._embed(texts)

    def _embed(self, texts: list[str]) -> np.ndarray:
        out = np.zeros((len(texts), self.dims), dtype=np.float32)
        for row, text in enumerate(texts):
            for feat, w in self._feats(text):
                h = zlib.crc32(feat.encode())
                out[row, h % self.dims] += w if (h >> 31) & 1 else -w
            n = np.linalg.norm(out[row])
            if n > 0:
                out[row] /= n
        return out


PARALLEL_MIN = 256


def _hash_embed(dims: int, texts: list[str]) -> np.ndarray:
    return HashingEmbedder(dims)._embed(texts)


def instruct(text: str, task: str) -> str:
    """Formato de consulta de Qwen3-Embedding: los documentos van sin instruccion."""
    return f"Instruct: {TASKS.get(task, task)}\nQuery:{text}"


class OllamaEmbedder(Embedder):
    def __init__(self, url: str, model: str) -> None:
        self.url, self.model = url.rstrip("/"), model
        self.id = f"ollama:{model}"
        self.instructions = "qwen3-embedding" in model.lower()

    def embed_query(self, texts: list[str], task: str = "search") -> np.ndarray:
        return self.embed([instruct(t, task) for t in texts] if self.instructions else texts)

    def embed(self, texts: list[str]) -> np.ndarray:
        import httpx
        parts = []
        for i in range(0, len(texts), 128):   # lotes acotados: documentos enormes no agotan el tiempo de espera
            r = httpx.post(f"{self.url}/api/embed", json={"model": self.model, "input": texts[i:i + 128]}, timeout=120)
            r.raise_for_status()
            parts.append(np.asarray(r.json()["embeddings"], dtype=np.float32))
        arr = np.vstack(parts) if parts else np.zeros((0, 1), dtype=np.float32)
        return arr / np.maximum(np.linalg.norm(arr, axis=1, keepdims=True), 1e-9)


class SentenceTransformerEmbedder(Embedder):
    def __init__(self, model: str = "paraphrase-multilingual-MiniLM-L12-v2", device: str = "cpu") -> None:
        from sentence_transformers import SentenceTransformer
        self.model = SentenceTransformer(model, device=device if device in ("cuda", "cpu", "mps") else "cpu")
        self.id = f"st:{model}"

    def embed(self, texts: list[str]) -> np.ndarray:
        return np.asarray(self.model.encode(texts, normalize_embeddings=True, batch_size=64), dtype=np.float32)


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class GGUFEmbedder(Embedder):
    """Modelo de embeddings GGUF (p.ej. Qwen3-Embedding-0.6B) servido por llama-server con ``--embedding``, en un
    proceso aparte del LLM. Si no hay ejecutable de llama.cpp pero si llama-cpp-python, se carga en el proceso.

    Se arranca en el primer uso y se para tras ``idle_s`` segundos sin peticiones (libera RAM en equipos justos)."""
    slow = True
    BATCH = 16
    MAX_CHARS = 6000   # ~1500 tokens: cabe en el lote fisico (-ub) del servidor

    def __init__(self, entry: dict, exe_getter=None, threads: int = 0, dims: int = 0, idle_s: float = 600) -> None:
        self.entry, self.path = entry, entry["path"]
        self.exe_getter = exe_getter or (lambda: None)
        self.threads, self.dims, self.idle_s = threads, dims, idle_s
        meta = entry.get("meta") or {}
        name = f"{entry.get('id', '')} {Path(self.path).name} {meta.get('name', '')}".lower()
        self.instructions = bool(entry.get("instructions")) or "qwen3" in name and "embed" in name
        arch = meta.get("architecture", "")
        self.pooling = "" if meta.get("pooling") is not None else (
            "last" if arch in ("qwen2", "qwen3", "llama", "mistral", "gemma3", "gemma2") else "mean")
        # Qwen3-Embedding agrupa por el ultimo token: debe ser <|endoftext|> si el GGUF no lo anade solo
        self.eos = "<|endoftext|>" if self.instructions and meta.get("add_eos") is not True else ""
        self.id = f"gguf:{entry.get('id') or Path(self.path).stem}" + (f"@{dims}" if dims else "")
        self.proc: subprocess.Popen | None = None
        self.port = 0
        self.local = None              # llama_cpp.Llama si no hay llama-server
        self._lock = threading.Lock()
        self._last = 0.0
        self._timer: threading.Timer | None = None
        self._qcache: dict[tuple[str, str], np.ndarray] = {}
        self.waiting = 0               # consultas esperando: la indexacion de fondo cede el turno

    # ------------------------------------------------------------ servidor
    def _start(self) -> None:
        if self.proc and self.proc.poll() is None or self.local is not None:
            return
        found = self.exe_getter()
        if not found:
            if importlib.util.find_spec("llama_cpp") is None:
                raise RuntimeError("Falta el runtime llama.cpp (Sistema > Runtimes) para el modelo de embeddings")
            from llama_cpp import Llama
            kw = {"pooling_type": 3} if self.pooling == "last" else {}
            self.local = Llama(model_path=self.path, embedding=True, n_ctx=2048, n_batch=2048, verbose=False,
                               n_threads=self.threads or None, **kw)
            return
        exe = Path(found[0] if isinstance(found, tuple) else found)
        from .. import paths
        from ..proc import popen
        self.port = _free_port()
        cmd = [str(exe), "-m", self.path, "--host", "127.0.0.1", "--port", str(self.port), "--embedding",
               "-c", "2048", "-b", "2048", "-ub", "2048", "-np", "1", "-ngl", "0", "--no-webui"]
        if self.pooling:
            cmd += ["--pooling", self.pooling]
        if self.threads:
            cmd += ["-t", str(self.threads)]
        logf = open(paths.logs_dir() / "llama-embed.log", "ab")
        L.info("Arrancando el servidor de embeddings: %s", " ".join(cmd[1:]))
        self.proc = popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=str(exe.parent))
        import httpx
        t0 = time.time()
        while time.time() - t0 < 180:
            if self.proc.poll() is not None:
                tail = (paths.logs_dir() / "llama-embed.log").read_bytes()[-800:].decode(errors="replace")
                if "--no-webui" in cmd and ("--no-webui" in tail or "invalid argument" in tail):
                    cmd.remove("--no-webui")   # versiones antiguas de llama.cpp
                    self.proc = popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=str(exe.parent))
                    continue
                self.proc = None
                raise RuntimeError(f"el servidor de embeddings termino al arrancar: {tail[-400:]}")
            try:
                if httpx.get(f"http://127.0.0.1:{self.port}/health", timeout=1).status_code == 200:
                    L.info("Servidor de embeddings listo en %.1fs", time.time() - t0)
                    return
            except httpx.HTTPError:
                pass
            time.sleep(0.3)
        raise RuntimeError("el servidor de embeddings no respondio a tiempo")

    def _schedule_stop(self) -> None:
        if self._timer:
            self._timer.cancel()
        if self.idle_s > 0:
            self._timer = threading.Timer(self.idle_s, self._idle_stop)
            self._timer.daemon = True
            self._timer.start()

    def _idle_stop(self) -> None:
        with self._lock:
            if time.time() - self._last >= self.idle_s - 1:
                self._stop()

    def _stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        self.local = None

    def close(self) -> None:
        if self._timer:
            self._timer.cancel()
        with self._lock:
            self._stop()

    def running(self) -> bool:
        return bool(self.proc and self.proc.poll() is None or self.local is not None)

    # ------------------------------------------------------------ calculo
    def _prep(self, texts: list[str]) -> list[str]:
        return [(t or " ")[: self.MAX_CHARS] + self.eos for t in texts]

    def _raw(self, texts: list[str]) -> np.ndarray:
        import httpx
        with self._lock:
            self._start()
            self._last = time.time()
            if self.local is not None:
                data = self.local.create_embedding(texts)["data"]
            else:
                r = httpx.post(f"http://127.0.0.1:{self.port}/v1/embeddings", json={"input": texts, "model": "embed"},
                               timeout=600)
                if r.status_code >= 400:
                    raise RuntimeError(f"servidor de embeddings: {r.status_code} {r.text[:200]}")
                data = r.json()["data"]
            self._last = time.time()
        self._schedule_stop()
        arr = np.asarray([d["embedding"] for d in sorted(data, key=lambda d: d.get("index", 0))], dtype=np.float32)
        if self.dims and arr.shape[1] > self.dims:   # Matryoshka: Qwen3-Embedding admite recortar dimensiones
            arr = arr[:, : self.dims]
        return arr / np.maximum(np.linalg.norm(arr, axis=1, keepdims=True), 1e-9)

    def embed(self, texts: list[str]) -> np.ndarray:
        texts = self._prep(list(texts))
        if not texts:
            return np.zeros((0, self.dims or 1), dtype=np.float32)
        return np.vstack([self._raw(texts[i:i + self.BATCH]) for i in range(0, len(texts), self.BATCH)])

    def embed_query(self, texts: list[str], task: str = "search") -> np.ndarray:
        """Consultas: con la instruccion de la tarea, prioridad sobre la indexacion de fondo y cache (un turno de
        chat consulta varias veces lo mismo: recuerdos, ejemplos, lecciones, biblioteca)."""
        keys = [(task, t) for t in texts]
        missing = [k for k in dict.fromkeys(keys) if k not in self._qcache]
        if missing:
            self.waiting += 1
            try:
                vecs = self.embed([instruct(t, k) if self.instructions else t for k, t in missing])
            finally:
                self.waiting -= 1
            for k, v in zip(missing, vecs):
                self._qcache[k] = v
            while len(self._qcache) > 256:
                self._qcache.pop(next(iter(self._qcache)))
        return np.vstack([self._qcache[k] for k in keys]) if keys else np.zeros((0, 1), dtype=np.float32)

    def info(self) -> dict:
        return {**super().info(), "kind": "gguf", "model": self.entry.get("name") or self.entry.get("id"),
                "path": self.path, "running": self.running(), "engine": "llama-cpp-python" if self.local else "llama-server"}


def get_embedder(kind: str = "auto", ollama_url: str = "", ollama_model: str = "nomic-embed-text",
                 device: str = "cpu", gguf_entry: dict | None = None, exe_getter=None, threads: int = 0,
                 dims: int = 0):
    """auto: el modelo GGUF de embeddings activo (Modelos) y, si no hay, lo que este disponible."""
    if gguf_entry and kind in ("auto", "gguf") and Path(gguf_entry.get("path", "")).exists():
        return GGUFEmbedder(gguf_entry, exe_getter, threads, dims)
    if kind in ("auto", "sentence-transformers") and importlib.util.find_spec("sentence_transformers"):
        try:
            return SentenceTransformerEmbedder(device=device)
        except Exception as e:  # noqa: BLE001
            L.warning("sentence-transformers no disponible: %s", e)
    if kind in ("auto", "ollama") and ollama_url:
        try:
            emb = OllamaEmbedder(ollama_url, ollama_model)
            emb.embed(["ping"])
            return emb
        except Exception:  # noqa: BLE001
            if kind == "ollama":
                L.warning("Ollama embeddings no disponible; usando hashing")
    return HashingEmbedder()
