"""Hardware: deteccion, perfiles, monitorizacion y autoajuste."""
from __future__ import annotations

from .. import log
from .detect import HardwareInfo, detect, load_cached, save
from .monitor import Monitor, load_history
from .profiles import build_profile
from .tuner import load_state, save_state, tune

L = log.get("hardware")


def startup(override: str = "", adaptive: bool = True, ollama_url: str = "http://127.0.0.1:11434",
            previous_unclean: bool = False, previous_info: dict | None = None) -> tuple[HardwareInfo, dict, list[str]]:
    """Detecta hardware, construye el perfil y aplica el autoajuste basado en sesiones previas."""
    hw = detect(ollama_url=ollama_url)
    save(hw)
    profile = build_profile(hw, override)
    reasons: list[str] = []
    if adaptive:
        profile, state, reasons = tune(profile, load_history(), previous_unclean, hw.cores_logical,
                                       previous_info=previous_info)
        save_state(state)
    L.info("Perfil %s | acelerador=%s | modelo recomendado <= %sB | ctx=%s hilos=%s",
           profile["id"], profile["accelerator"], profile["llm"]["recommended_model_b"],
           profile["llm"]["n_ctx"], profile["llm"]["n_threads"])
    return hw, profile, reasons


__all__ = ["HardwareInfo", "Monitor", "build_profile", "detect", "load_cached", "startup", "tune"]
