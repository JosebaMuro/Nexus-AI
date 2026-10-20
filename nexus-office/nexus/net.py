"""Descargas HTTP reanudables con progreso (usadas por modelos y runtimes)."""
from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Callable

import httpx

UA = {"User-Agent": "nexus-office/1.0"}


def download(url: str, dest: Path, headers: dict | None = None, progress: Callable[[int, int], None] | None = None,
             cancel: Callable[[], bool] | None = None, retries: int = 3) -> Path:
    """Descarga ``url`` a ``dest`` usando ``dest.part`` y cabecera Range para reanudar."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    hdrs = {**UA, **(headers or {})}
    for attempt in range(retries):
        have = part.stat().st_size if part.exists() else 0
        req = dict(hdrs)
        if have:
            req["Range"] = f"bytes={have}-"
        try:
            with httpx.stream("GET", url, headers=req, follow_redirects=True, timeout=httpx.Timeout(30, read=120)) as r:
                if r.status_code == 416:  # ya completo
                    break
                r.raise_for_status()
                if have and r.status_code != 206:
                    have = 0  # el servidor no soporta reanudar
                total = int(r.headers.get("content-length", 0)) + have
                mode = "ab" if have else "wb"
                done = have
                last = 0.0
                with open(part, mode) as fh:
                    for chunk in r.iter_bytes(1 << 20):
                        if cancel and cancel():
                            raise InterruptedError("descarga cancelada")
                        fh.write(chunk)
                        done += len(chunk)
                        if progress and time.time() - last > 0.3:
                            progress(done, total)
                            last = time.time()
                if progress:
                    progress(done, total)
            break
        except InterruptedError:
            raise
        except (httpx.HTTPError, OSError):
            if attempt == retries - 1:
                raise
            time.sleep(2 * (attempt + 1))
    os.replace(part, dest)
    return dest


def get_json(url: str, headers: dict | None = None, params: dict | None = None, timeout: float = 20):
    r = httpx.get(url, headers={**UA, **(headers or {})}, params=params, timeout=timeout, follow_redirects=True)
    r.raise_for_status()
    return r.json()
