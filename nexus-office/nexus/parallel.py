"""Paralelismo para tareas pesadas de CPU (rastreo, lectura de documentos, embeddings).

Python ejecuta el codigo puro de un hilo en un unico nucleo (GIL), asi que para aprovechar todo el
procesador las tareas de CPU van a un *pool de procesos* compartido (un proceso por nucleo). Las de
red o disco van a hilos. Donde no se pueden crear procesos (app Android, entornos congelados o
con NEXUS_NO_PROCESSES=1) todo cae a hilos sin cambiar el resultado.

Ajuste: performance.workers en config.yaml (0 = automatico, todos los nucleos logicos).
"""
from __future__ import annotations

import atexit
import os
import sys
import threading
from concurrent.futures import Executor, Future, ProcessPoolExecutor, ThreadPoolExecutor
from typing import Any, Callable, Iterable

from . import log

L = log.get("parallel")
_lock = threading.Lock()
_procs: ProcessPoolExecutor | None = None
_threads: ThreadPoolExecutor | None = None
_broken = False


def cpu_count() -> int:
    try:
        return max(1, len(os.sched_getaffinity(0)))   # respeta los limites del contenedor / taskset
    except (AttributeError, OSError):
        return max(1, os.cpu_count() or 1)


def configured_workers() -> int:
    try:
        from .config import get_config
        n = int(get_config().get("performance.workers", 0) or 0)
    except Exception:  # noqa: BLE001 - sin configuracion (tests, herramientas sueltas)
        n = 0
    return max(1, min(n, 64)) if n > 0 else cpu_count()


def processes_available() -> bool:
    if _broken or os.environ.get("NEXUS_NO_PROCESSES") == "1":
        return False
    if hasattr(sys, "getandroidapilevel") or "ANDROID_ARGUMENT" in os.environ or os.environ.get("NEXUS_ANDROID") == "1":
        return False                    # Android: no hay ejecutable de Python con el que lanzar procesos
    return not getattr(sys, "frozen", False)


def process_pool() -> Executor | None:
    global _procs
    if not processes_available() or configured_workers() < 2:
        return None
    with _lock:
        if _procs is None:
            import multiprocessing as mp
            # forkserver/spawn: un fork desde un servidor con hilos puede bloquearse
            method = "forkserver" if "forkserver" in mp.get_all_start_methods() else "spawn"
            _procs = ProcessPoolExecutor(max_workers=configured_workers(), mp_context=mp.get_context(method))
        return _procs


def thread_pool() -> ThreadPoolExecutor:
    global _threads
    with _lock:
        if _threads is None:
            _threads = ThreadPoolExecutor(max_workers=max(4, configured_workers() * 2), thread_name_prefix="nexus-io")
        return _threads


def submit_cpu(fn: Callable, *args: Any) -> Future:
    """Ejecuta fn(*args) en otro nucleo si se puede; si no, en un hilo. fn debe ser de nivel de modulo."""
    global _broken
    pool = process_pool()
    if pool is not None:
        try:
            return pool.submit(fn, *args)
        except Exception as e:  # noqa: BLE001 - pool roto (proceso muerto, sin memoria...): seguimos con hilos
            L.warning("pool de procesos no disponible (%s); se usan hilos", e)
            _broken = True
    return thread_pool().submit(fn, *args)


def run_cpu(fn: Callable, *args: Any) -> Any:
    """Como submit_cpu pero esperando el resultado; si el proceso falla por el pool, se repite en este hilo."""
    global _broken
    fut = submit_cpu(fn, *args)
    try:
        return fut.result()
    except Exception as e:  # noqa: BLE001
        from concurrent.futures.process import BrokenProcessPool
        if isinstance(e, BrokenProcessPool):
            _broken = True
            return fn(*args)
        raise


def map_cpu(fn: Callable, items: Iterable, min_items: int = 2) -> list:
    """[fn(x) for x in items] repartido entre nucleos, conservando el orden."""
    items = list(items)
    if len(items) < min_items or process_pool() is None:
        return [fn(x) for x in items]
    futs = [submit_cpu(fn, x) for x in items]
    return [f.result() for f in futs]


def split(items: list, parts: int) -> list[list]:
    parts = max(1, min(parts, len(items)))
    k, m = divmod(len(items), parts)
    out, i = [], 0
    for p in range(parts):
        n = k + (1 if p < m else 0)
        out.append(items[i:i + n])
        i += n
    return out


def shutdown() -> None:
    global _procs, _threads
    with _lock:
        if _procs is not None:
            _procs.shutdown(wait=False, cancel_futures=True)
            _procs = None
        if _threads is not None:
            _threads.shutdown(wait=False, cancel_futures=True)
            _threads = None


atexit.register(shutdown)
