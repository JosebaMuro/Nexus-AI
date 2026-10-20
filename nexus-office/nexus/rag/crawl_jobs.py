"""Rastreos por lotes: dominios enteros que no caben de una vez se procesan en lotes correlativos y reanudables.

Un *trabajo* de rastreo guarda en data/rag/crawls/<id>.json todo lo necesario para seguir donde se quedo:
la cola de URLs pendientes (en orden de descubrimiento), las visitadas, las huellas del contenido ya leido y la
lista de lotes ya indexados. Cada lote es un documento propio de la biblioteca RAG
(«Documentación: X · lote 003 (págs. 401-600)»), asi la biblioteca crece por partes ordenadas y una caida, un
reinicio o una pausa no pierden nada: al reanudar se sigue por la siguiente pagina de la cola.

El tamano de lote se ajusta a la RAM del equipo (rag.crawl_batch_pages en config.yaml, 0 = automatico).
"""
from __future__ import annotations

import json
import os
import re
import threading
import time
import uuid
from pathlib import Path

from .. import log, paths

L = log.get("crawl_jobs")
MAX_DOMAIN_PAGES = 100_000          # tope de un trabajo completo (suma de todos sus lotes)
RUNNING, PAUSED, DONE, INTERRUPTED, FAILED = "en curso", "pausado", "completado", "interrumpido", "error"
_running: dict[str, threading.Event] = {}
_threads: dict[str, threading.Thread] = {}
_lock = threading.Lock()


def auto_batch(profile: dict | None = None) -> tuple[int, int]:
    """(paginas, caracteres) por lote segun la RAM: mas pequeno en moviles y equipos modestos."""
    try:
        from ..config import get_config
        n = int(get_config().get("rag.crawl_batch_pages", 0) or 0)
    except Exception:  # noqa: BLE001
        n = 0
    ram = float((profile or {}).get("ram_gb") or 8)
    try:
        from ..hardware.profiles import android
        mobile = android()
    except Exception:  # noqa: BLE001
        mobile = False
    pages = 50 if ram < 4 else 100 if ram < 8 or mobile else 200 if ram < 16 else 300
    chars = 2_000_000 if ram < 8 or mobile else 6_000_000
    if n > 0:
        pages = n
    from .crawler import MAX_PAGES_LIMIT
    return max(5, min(pages, MAX_PAGES_LIMIT)), chars


def _dir() -> Path:
    return paths.sub("rag", "crawls")


def _path(job_id: str) -> Path:
    if not re.fullmatch(r"[a-z0-9-]{6,40}", job_id or ""):
        raise KeyError(job_id)
    return _dir() / f"{job_id}.json"


def save(job: dict) -> None:
    job["updated"] = time.time()
    p = _path(job["id"])
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(job, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, p)       # atomico: un corte a mitad no deja el trabajo corrupto


def get(job_id: str) -> dict | None:
    try:
        p = _path(job_id)
    except KeyError:
        return None
    if not p.exists():
        return None
    job = json.loads(p.read_text(encoding="utf-8"))
    if job.get("status") == RUNNING and job_id not in _running:
        job["status"] = INTERRUPTED   # Nexus-Office se cerro a mitad: se puede reanudar
    return job


def create(start_url: str, max_pages: int, max_depth: int, scope: str, agent: str, learn: bool,
           batch_pages: int, batch_chars: int) -> dict:
    job = {"id": time.strftime("%Y%m%d%H%M%S-") + uuid.uuid4().hex[:6], "url": start_url, "start": "", "prefix": "",
           "scope": scope, "agent": agent or "", "learn": learn, "max_pages": max(1, min(int(max_pages), MAX_DOMAIN_PAGES)),
           "max_depth": max(0, int(max_depth)), "batch_pages": batch_pages, "batch_chars": batch_chars,
           "status": RUNNING, "reason": "", "pages_done": 0, "fetched": 0, "skipped": 0, "pending": 0, "batches": [],
           "title": "", "site": "", "state": None, "created": time.time(), "seconds": 0.0, "error": ""}
    save(job)
    return job


def public(job: dict) -> dict:
    """Lo que ve la interfaz: sin la cola ni las listas internas."""
    out = {k: v for k, v in job.items() if k != "state"}
    st = job.get("state") or {}
    out["pending"] = len(st.get("queue") or ()) if st else job.get("pending", 0)
    out["running"] = job["id"] in _running
    return out


def list_jobs() -> list[dict]:
    out = []
    for p in sorted(_dir().glob("*.json"), reverse=True):
        try:
            job = get(p.stem)
        except (ValueError, OSError):
            continue
        if job:
            out.append(public(job))
    return out


def find_resumable(start_url: str, agent: str = "") -> dict | None:
    """El ultimo trabajo sin terminar de esa URL (para «sigue rastreando ...»)."""
    from .crawler import normalize
    n = normalize(start_url)
    for j in list_jobs():
        if j["status"] != DONE and j["agent"] == (agent or "") and n in (normalize(j["url"]), j.get("start")):
            return get(j["id"])
    return None


def delete(job_id: str, docs: bool = False, rag=None) -> bool:
    stop(job_id)
    job = get(job_id)
    if not job:
        return False
    if docs and rag is not None:
        for b in job["batches"]:
            rag.delete(b["doc_id"])
    _path(job_id).unlink(missing_ok=True)
    return True


def stop(job_id: str) -> bool:
    ev = _running.get(job_id)
    if ev:
        ev.set()
    return ev is not None


def is_running(job_id: str) -> bool:
    return job_id in _running


def run(services, job_id: str, emit=None, fetch=None, delay: float = 0.1, max_seconds: float | None = None,
        connections: int | None = None, batch_seconds: float = 900) -> dict:
    """Ejecuta (o reanuda) un trabajo hasta terminarlo, pausarlo o agotar `max_seconds`. Devuelve el trabajo."""
    ev = _claim(job_id)
    return _execute(services, job_id, ev, emit=emit, fetch=fetch, delay=delay, max_seconds=max_seconds,
                    connections=connections, batch_seconds=batch_seconds)


def _claim(job_id: str) -> threading.Event:
    with _lock:
        if job_id in _running:
            raise RuntimeError("ese rastreo ya esta en marcha")
        ev = _running[job_id] = threading.Event()
    return ev


def _execute(services, job_id: str, ev: threading.Event, **kw) -> dict:
    from .crawler import crawl_batches
    try:
        job = get(job_id)
        if not job:
            raise KeyError(job_id)
        if job["status"] == DONE:
            return job
        job["status"], job["reason"], job["error"] = RUNNING, "", ""
        save(job)
        return crawl_batches(services, job, should_stop=ev.is_set, **kw)
    finally:
        with _lock:
            _running.pop(job_id, None)
            _threads.pop(job_id, None)


def run_background(services, job_id: str, emit=None, **kw) -> None:
    """Lanza el trabajo en un hilo; el progreso llega por `emit` (WebSocket) y por list_jobs()."""
    ev = _claim(job_id)       # antes de arrancar el hilo: la lista ya lo muestra en curso

    def body():
        try:
            _execute(services, job_id, ev, emit=emit, **kw)
        except Exception as e:  # noqa: BLE001
            L.warning("Rastreo %s fallido: %s", job_id, e)
            job = get(job_id)
            if job:
                job.update(status=FAILED, error=str(e)[:300])
                save(job)
        if emit:
            emit({"type": "crawl_job", "job": job_id})
            emit({"type": "graph_update"})
    th = threading.Thread(target=body, name=f"nexus-crawl-{job_id}", daemon=True)
    _threads[job_id] = th
    th.start()


def wait(job_id: str, timeout: float | None = None) -> None:
    th = _threads.get(job_id)
    if th:
        th.join(timeout)
