"""Logging a consola y a data/logs/nexus.log, con un buffer en memoria para el panel admin."""
from __future__ import annotations

import collections
import logging
import logging.handlers

from . import paths

_RING: collections.deque[str] = collections.deque(maxlen=500)
_configured = False


class _RingHandler(logging.Handler):
    def emit(self, record: logging.LogRecord) -> None:
        _RING.append(self.format(record))


def setup(level: int = logging.INFO) -> None:
    global _configured
    if _configured:
        return
    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%H:%M:%S")
    root = logging.getLogger("nexus")
    root.setLevel(level)
    console = logging.StreamHandler()
    console.setFormatter(fmt)
    fileh = logging.handlers.RotatingFileHandler(paths.logs_dir() / "nexus.log", maxBytes=2_000_000,
                                                 backupCount=3, encoding="utf-8")
    fileh.setFormatter(fmt)
    ring = _RingHandler()
    ring.setFormatter(fmt)
    for h in (console, fileh, ring):
        root.addHandler(h)
    _configured = True


def get(name: str) -> logging.Logger:
    return logging.getLogger(f"nexus.{name}")


def recent(n: int = 200) -> list[str]:
    return list(_RING)[-n:]
