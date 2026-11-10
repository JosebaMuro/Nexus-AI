"""Eleccion del motor y del acelerador para el hardware que ejecuta Agente IA.

1. ``chips.chip()`` resume el hardware (snapdragon, nvidia, intel...) y ``chips.matriz()`` da el orden de partida.
2. Solo cuentan los motores instalados (paquete o biblioteca) y con un modelo de su formato en el catalogo.
3. Si el banco (``banco.py``) ya midio este equipo, manda la velocidad real (tokens/s) sobre el orden de partida.
4. llama.cpp (GGUF) queda siempre de respaldo.

Con ``llm.motor`` distinto de «auto» se usa ese motor (si esta) y con ``llm.motor_acelerador`` ese acelerador.
"""
from __future__ import annotations

import sys

from . import MOTORES, caidas, chips, disponible, motor_de_formato, plataforma
from . import banco as banco_mod


def contexto(hw: dict, profile: dict) -> dict:
    fam = chips.chip(hw, profile)
    return {"chip": fam, "plataforma": plataforma(), "matriz": chips.matriz(fam, plataforma())}


def orden_aceleradores(motor: str, entry: dict, hw: dict, profile: dict, forzado: str = "") -> list[str]:
    """Aceleradores a probar, en orden, para un motor y un modelo en este equipo."""
    m = contexto(hw, profile)["matriz"]
    out = [forzado] if forzado else []
    out += [p["acelerador"] for p in m["orden"] if p["motor"] == motor]
    if motor == "onnx":
        out = (out or []) + ["modelo", "cpu"]
    elif motor == "openvino":
        out += ["GPU", "CPU"]
    elif motor == "litert":
        android = plataforma().startswith("android")
        npu = bool((profile.get("npu") or {}).get("present"))
        out += (["npu"] if npu else []) + (["gpu"] if android or profile.get("accelerator") not in ("cpu", "") else []) + ["cpu"]
    elif motor == "mlc":
        acc = profile.get("accelerator", "cpu")
        gpu = {"cuda": "cuda", "rocm": "rocm", "metal": "metal", "vulkan": "vulkan"}.get(acc)
        out += ([gpu] if gpu else []) + (["vulkan"] if sys.platform != "darwin" else []) + ["cpu"]
    elif motor == "genie":
        out = ["npu"]
    elif motor == "tensorrt":
        out = ["cuda"]
    elif motor == "executorch":
        out += ["xnnpack"]
    vistos, uniq = set(), []
    for a in out:
        if a and a not in vistos:
            vistos.add(a)
            uniq.append(a)
    return uniq


def candidatos(catalog_items: list[dict], hw: dict, profile: dict, banco: dict | None = None) -> list[dict]:
    """Modelos de texto instalados con su motor, ordenados de mejor a peor para este equipo."""
    ctx = contexto(hw, profile)
    orden = [p["motor"] for p in ctx["matriz"]["orden"]]
    medidas = (banco if banco is not None else banco_mod.cargar()).get("resultados", {})
    caidos = caidas()
    out = []
    for e in catalog_items:
        if e.get("kind") != "llm":
            continue
        motor = motor_de_formato(e.get("format", ""))
        if not motor:
            continue
        ok, motivo = disponible(motor)
        rank = orden.index(motor) if motor in orden else len(orden)
        med = medidas.get(f"{motor}:{e['id']}")
        out.append({"id": e["id"], "nombre": e.get("name", e["id"]), "motor": motor, "formato": e.get("format"),
                    "instalado": ok, "motivo": motivo, "rango": rank, "tps": (med or {}).get("tps"),
                    "acelerador_medido": (med or {}).get("acelerador"),
                    "caido": any(k.startswith(motor + "-") for k in caidos)})
    # medido primero (mas tokens/s), luego el orden de la matriz; lo no instalado al final
    out.sort(key=lambda c: (not c["instalado"], c["tps"] is None, -(c["tps"] or 0), c["rango"]))
    return out


def elegir(catalog_items: list[dict], hw: dict, profile: dict, motor_forzado: str = "auto") -> dict | None:
    cands = [c for c in candidatos(catalog_items, hw, profile) if c["instalado"]]
    if motor_forzado and motor_forzado != "auto":
        forz = [c for c in cands if c["motor"] == motor_forzado]
        if forz:
            return forz[0]
    return cands[0] if cands else None


def estado(catalog_items: list[dict], hw: dict, profile: dict, config=None) -> dict:
    ctx = contexto(hw, profile)
    motores = []
    for mid, info in MOTORES.items():
        ok, motivo = disponible(mid)
        motores.append({"id": mid, "nombre": info.nombre, "formato": info.formato, "instalado": ok, "motivo": motivo,
                        "aceleradores": list(info.aceleradores)})
    motores.append({"id": "llamaserver", "nombre": "llama.cpp (llama-server)", "formato": "gguf", "instalado": True,
                    "motivo": "", "aceleradores": ["cpu", "cuda", "rocm", "vulkan", "metal", "sycl"]})
    elegido = elegir(catalog_items, hw, profile, config.get("llm.motor", "auto") if config else "auto")
    return {**ctx, "motores": motores, "modelos": candidatos(catalog_items, hw, profile), "elegido": elegido,
            "banco": banco_mod.cargar(), "caidas": caidas()}
