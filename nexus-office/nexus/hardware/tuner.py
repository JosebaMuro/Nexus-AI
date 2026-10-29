"""Autoajuste adaptativo: en cada arranque revisa el rendimiento de sesiones anteriores
y readapta la configuracion (contexto, hilos, lotes, capas GPU, tamano de modelo).

Para los hilos usa exploracion/explotacion: prueba cada candidato al menos una vez y
despues se queda con el que mejores tokens/s ha dado de media.
"""
from __future__ import annotations

import copy
import json
import statistics
import time

from .. import log, paths

L = log.get("tuner")


def load_state() -> dict:
    f = paths.tuning_file()
    if f.exists():
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {"overrides": {}, "thread_trials": {}, "log": []}


def save_state(state: dict) -> None:
    state["log"] = state.get("log", [])[-40:]
    paths.tuning_file().write_text(json.dumps(state, indent=2, ensure_ascii=False), encoding="utf-8")


def thread_candidates(profile: dict, cores_logical: int) -> list[int]:
    phys = profile["llm"]["n_threads"]
    return sorted({max(1, phys - 1), phys, max(phys, cores_logical)})


MIN_CTX, MIN_BATCH = 2048, 128   # por debajo, el prompt de sistema y las herramientas no caben o va muy lento


def tune(profile: dict, history: list[dict], previous_unclean: bool, cores_logical: int,
         state: dict | None = None, previous_info: dict | None = None) -> tuple[dict, dict, list[str]]:
    """Devuelve (perfil_ajustado, nuevo_estado, motivos)."""
    state = copy.deepcopy(state or load_state())
    llm = dict(profile["llm"])
    llm.update(state.get("overrides", {}))
    llm["n_ctx"] = max(min(MIN_CTX, profile["llm"]["n_ctx"]), llm["n_ctx"])
    llm["n_batch"] = max(min(MIN_BATCH, profile["llm"]["n_batch"]), llm["n_batch"])
    reasons: list[str] = []
    last = history[-1] if history else None
    info = previous_info or {}
    # un cierre abrupto solo indica problema si hubo presion de memoria (no si el usuario cerro el proceso)
    memory_crash = previous_unclean and (info.get("peak_ram_pct", 0) > 88 or info.get("peak_vram_pct", 0) > 95
                                         or info.get("oom_errors", 0) > 0 or not info)
    if previous_unclean and not memory_crash:
        reasons.append("La sesion anterior termino de forma abrupta sin presion de memoria: se mantiene la configuracion")

    # 1) registrar el rendimiento de la ultima sesion con su numero de hilos
    live = profile["llm"].get("live_threads")   # Android: los hilos los ajusta nexus.android_tune en vivo
    if live:
        llm["n_threads"] = profile["llm"]["n_threads"]
    if not live and last and last.get("llm_calls", 0) >= 1 and last.get("avg_tps", 0) > 0:
        used = str(last.get("settings", {}).get("n_threads", llm["n_threads"]))
        state.setdefault("thread_trials", {}).setdefault(used, []).append(last["avg_tps"])
        state["thread_trials"][used] = state["thread_trials"][used][-8:]

    # 2) estabilidad: cierre inesperado o falta de memoria -> modo conservador
    if memory_crash or (last and last.get("oom_errors")):
        llm["n_ctx"] = max(min(MIN_CTX, profile["llm"]["n_ctx"]), llm["n_ctx"] // 2)
        llm["n_batch"] = max(min(MIN_BATCH, profile["llm"]["n_batch"]), llm["n_batch"] // 2)
        if llm["gpu_layers"] not in (0, -1):
            llm["gpu_layers"] = int(llm["gpu_layers"] * 0.75)
        elif llm["gpu_layers"] == -1 and last and last.get("oom_errors"):
            llm["gpu_layers"] = 24
        reasons.append("Cierre inesperado o falta de memoria en la sesion anterior: contexto y lote reducidos")
    elif last:
        ram = last.get("peak_ram_pct", 0)
        vram = last.get("peak_vram_pct", 0)
        if ram > 92:
            llm["n_ctx"] = max(min(MIN_CTX, profile["llm"]["n_ctx"]), int(llm["n_ctx"] * 0.75))
            llm["n_batch"] = max(min(MIN_BATCH, profile["llm"]["n_batch"]), llm["n_batch"] // 2)
            reasons.append(f"RAM al {ram:.0f}%: contexto reducido a {llm['n_ctx']}")
        elif ram < 85 and last.get("clean_exit") and (llm["n_ctx"] < profile["llm"]["n_ctx"] or llm["n_batch"] < profile["llm"]["n_batch"]):
            llm["n_ctx"] = min(profile["llm"]["n_ctx"], llm["n_ctx"] * 2)
            llm["n_batch"] = min(profile["llm"]["n_batch"], llm["n_batch"] * 2)
            reasons.append(f"Sesion estable (RAM {ram:.0f}%): recuperando contexto {llm['n_ctx']} y lote {llm['n_batch']}")
        elif ram < 65 and last.get("llm_calls", 0) >= 3 and llm["n_ctx"] < profile["llm"]["max_ctx"]:
            llm["n_ctx"] = min(profile["llm"]["max_ctx"], int(llm["n_ctx"] * 1.5) // 256 * 256)
            reasons.append(f"Holgura de RAM ({ram:.0f}%): contexto ampliado a {llm['n_ctx']}")
        if vram > 95 and llm["gpu_layers"] != 0:
            llm["gpu_layers"] = 28 if llm["gpu_layers"] == -1 else max(0, int(llm["gpu_layers"] * 0.8))
            reasons.append(f"VRAM al {vram:.0f}%: menos capas en GPU ({llm['gpu_layers']})")
        elif 0 < vram < 70 and llm["gpu_layers"] > 0:
            llm["gpu_layers"] = int(llm["gpu_layers"] * 1.25) + 1
            reasons.append(f"VRAM con holgura ({vram:.0f}%): mas capas en GPU ({llm['gpu_layers']})")
        if last.get("llm_calls", 0) >= 3 and 0 < last.get("avg_tps", 0) < 4:
            llm["suggest_smaller_model"] = True
            reasons.append(f"Velocidad baja ({last['avg_tps']} tok/s): se recomienda un modelo mas pequeno")
        elif last.get("avg_tps", 0) >= 4:
            llm.pop("suggest_smaller_model", None)

    # 3) hilos: explorar candidatos no probados, despues explotar el mejor
    cands = thread_candidates(profile, cores_logical)
    trials = {} if live else state.get("thread_trials", {})
    untested = [c for c in cands if str(c) not in trials]
    if trials and untested:
        llm["n_threads"] = untested[0]
        reasons.append(f"Explorando {untested[0]} hilos para medir rendimiento")
    elif trials:
        best = max(cands, key=lambda c: statistics.mean(trials.get(str(c), [0])))
        if best != llm["n_threads"]:
            reasons.append(f"Mejor rendimiento medido con {best} hilos")
        llm["n_threads"] = best

    tuned = copy.deepcopy(profile)
    tuned["llm"] = llm
    keys = ("n_ctx", "n_batch", "gpu_layers", "n_threads", "suggest_smaller_model")
    state["overrides"] = {k: llm[k] for k in keys if k in llm and llm.get(k) != profile["llm"].get(k)}
    if reasons:
        state.setdefault("log", []).append({"ts": time.time(), "reasons": reasons, "overrides": state["overrides"]})
        for r in reasons:
            L.info("Autoajuste: %s", r)
    return tuned, state, reasons
