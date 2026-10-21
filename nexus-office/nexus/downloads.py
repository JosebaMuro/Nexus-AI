"""Descargas de la oficina: Nexus-Office sirve los instaladores de cada plataforma a los equipos y moviles de la red.

Copia los instaladores (la carpeta de una entrega: windows/, linux/, macos/, android/...) en <datos>/descargas/
y aparecen en Sistema > Descargas, con el del sistema anfitrion (el equipo donde corre Nexus-Office) y el del
dispositivo que mira la pagina destacados. Desde las apps Android, el movil pregunta donde guardar cada fichero.
"""
from __future__ import annotations

import platform
import re
from pathlib import Path

from . import paths

# (id, patron del nombre, etiqueta)
PLATFORMS = [
    ("windows-x64", r"windows-x64\.(exe|zip)$", "Windows x64 (Intel/AMD)"),
    ("windows-arm64", r"windows-arm64\.(exe|zip)$", "Windows ARM64 (Snapdragon X)"),
    ("linux-x86_64", r"(linux-x86_64\.(run|tar\.gz)|_amd64\.deb)$", "Linux x86_64"),
    ("linux-aarch64", r"(linux-aarch64\.(run|tar\.gz)|_arm64\.deb)$", "Linux ARM64 / Raspberry Pi"),
    ("macos-arm64", r"macos-arm64\.(tar\.gz|zip|dmg)$", "macOS Apple Silicon"),
    ("macos-x86_64", r"macos-x86_64\.(tar\.gz|zip|dmg)$", "macOS Intel"),
    ("android", r"\.apk$", "Android"),
    ("codigo", r"codigo-fuente\.zip$", "Código fuente"),
    ("docs", r"\.(pdf|txt|md)$", "Documentación"),
]


def folder() -> Path:
    return paths.sub("descargas")


def host_platform() -> str:
    s, m = platform.system(), platform.machine().lower()
    arm = m in ("arm64", "aarch64", "armv8l")
    if s == "Windows":
        return "windows-arm64" if arm else "windows-x64"
    if s == "Darwin":
        return "macos-arm64" if arm else "macos-x86_64"
    from .hardware.profiles import android
    if android():
        return "android"
    return "linux-aarch64" if arm else "linux-x86_64"


def classify(name: str) -> tuple[str, str]:
    for pid, pat, label in PLATFORMS:
        if re.search(pat, name, re.I):
            return pid, label
    return "otros", "Otros"


def listing() -> dict:
    base = folder()
    files = []
    for f in sorted(base.rglob("*")):
        if f.is_file() and not f.name.startswith("."):
            pid, label = classify(f.name)
            files.append({"name": f.name, "path": f.relative_to(base).as_posix(), "size": f.stat().st_size,
                          "platform": pid, "label": label})
    host = host_platform()
    return {"folder": str(base), "host": host, "host_label": dict((p, lbl) for p, _, lbl in PLATFORMS).get(host, host),
            "files": files}


def resolve(rel: str) -> Path | None:
    base = folder().resolve()
    p = (base / rel).resolve()
    return p if p.is_file() and base in p.parents else None
