"""Dispositivos de llama.cpp (``llama-server --list-devices``) y eleccion de UNA GPU cuando hay varias.

Por que: con dos tarjetas (o una integrada y otra dedicada) llama.cpp reparte el modelo entre todas las que ve, y en
Linux Vulkan ademas ofrece «llvmpipe», un renderizador por software que va mas lento que la CPU. Eso es lo que hacia
que la aceleracion por GPU «no funcionara» o «se liara» con dos tarjetas. Aqui se lee la lista real de dispositivos
del binario que se va a usar, se descartan los de software y se elige uno: el que diga el usuario (Ajustes, barra
superior o perfil de hardware) o, si no, la dedicada con mas memoria. Se pasa a llama-server con ``--device``.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

from .. import log

L = log.get("gpu")

# «  Vulkan0: NVIDIA GeForce RTX 3060 (12288 MiB, 11520 MiB free)» / «  CUDA1: Tesla T4 (15095 MiB, 14000 MiB free)»
LINE = re.compile(r"^\s*([A-Za-z][A-Za-z_]*\d+)\s*:\s*(.+?)\s*\((\d+)\s*MiB,\s*(\d+)\s*MiB free\)\s*$")
SOFTWARE = re.compile(r"llvmpipe|lavapipe|swiftshader|softpipe|microsoft basic render", re.I)
INTEGRATED = re.compile(r"\b(UHD|HD Graphics|Iris|Intel\(R\) Graphics|Radeon\(TM\) Graphics|Radeon Graphics|"
                        r"Radeon \d{3}M|Vega \d+ Graphics|Adreno|Mali|PowerVR|Apple M\d)", re.I)

_CACHE: dict[tuple[str, float], list[dict]] = {}


def parse(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        dev, name, total, free = m.group(1), m.group(2).strip(), int(m.group(3)), int(m.group(4))
        backend = re.sub(r"\d+$", "", dev)
        out.append({"id": dev, "backend": backend, "name": name, "mem_mb": total, "free_mb": free,
                    "software": bool(SOFTWARE.search(name)), "integrated": bool(INTEGRATED.search(name))})
    return out


def list_devices(exe: Path | str, env: dict | None = None) -> list[dict]:
    """Dispositivos que ve este llama-server (cacheado por binario). [] si no lo sabe (version antigua, error...)."""
    p = Path(exe)
    try:
        key = (str(p), p.stat().st_mtime)
    except OSError:
        return []
    if key in _CACHE:
        return _CACHE[key]
    devs: list[dict] = []
    try:
        r = subprocess.run([str(p), "--list-devices"], capture_output=True, text=True, timeout=60, cwd=str(p.parent),
                           env=env, errors="replace")
        devs = parse((r.stdout or "") + "\n" + (r.stderr or ""))
    except (OSError, subprocess.SubprocessError) as e:
        L.info("llama-server --list-devices no disponible: %s", e)
    _CACHE[key] = devs
    if devs:
        L.info("Dispositivos de llama.cpp: %s", ", ".join(f"{d['id']}={d['name']}" + (" (software)" if d["software"] else "")
                                                     for d in devs))
    return devs


def _match(d: dict, want: str) -> bool:
    w = want.strip().lower()
    if not w:
        return False
    if w == d["id"].lower():
        return True
    # por nombre: todas las palabras significativas de lo elegido estan en el nombre del dispositivo
    words = [x for x in re.split(r"[^a-z0-9]+", w) if x and x not in ("nvidia", "amd", "intel", "corporation", "geforce", "radeon")]
    name = d["name"].lower()
    return bool(words) and all(x in name for x in words)


def pick(devs: list[dict], preferred: str = "") -> dict | None:
    """La GPU que se usa: la preferida si existe; si no, la dedicada con mas memoria. Nunca una de software."""
    real = [d for d in devs if not d["software"]]
    if not real:
        return None
    if preferred:
        for d in real:
            if _match(d, preferred):
                return d
    return max(real, key=lambda d: (not d["integrated"], d["mem_mb"]))


def needs_device_flag(devs: list[dict]) -> bool:
    """Hay que fijar el dispositivo si llama.cpp ve mas de uno (varias GPU, o una real y llvmpipe)."""
    return len(devs) > 1
