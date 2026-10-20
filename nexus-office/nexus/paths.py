"""Rutas portables: todo se resuelve relativo a la carpeta del proyecto.

Se puede redirigir el directorio de datos con la variable NEXUS_DATA_DIR
(util para tests o para ejecutar varias instancias).
"""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "nexus"
WEB = PACKAGE / "web"


def data_dir() -> Path:
    d = Path(os.environ.get("NEXUS_DATA_DIR", ROOT / "data"))
    d.mkdir(parents=True, exist_ok=True)
    return d


def sub(*parts: str) -> Path:
    p = data_dir().joinpath(*parts)
    p.mkdir(parents=True, exist_ok=True)
    return p


def models_dir() -> Path:
    return sub("models")


def outputs_dir() -> Path:
    return sub("outputs")


def logs_dir() -> Path:
    return sub("logs")


def agents_dir() -> Path:
    return sub("agents")


def community_tools_dir() -> Path:
    return sub("tools", "community")


def proposals_dir() -> Path:
    return sub("tools", "proposals")


def sandbox_dir() -> Path:
    return sub("sandbox")


def config_file() -> Path:
    return data_dir() / "config.yaml"


def hardware_file() -> Path:
    return data_dir() / "hardware.json"


def tuning_file() -> Path:
    return data_dir() / "tuning.json"


def db_file() -> Path:
    return data_dir() / "nexus.db"
