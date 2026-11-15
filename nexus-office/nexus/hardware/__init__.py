"""Hardware: deteccion, perfiles, monitorizacion y autoajuste."""
from __future__ import annotations

import json
import os
import platform
import sys
import threading
import time
from pathlib import Path

from .. import log
from .detect import HardwareInfo, detect, load_cached, save
from .monitor import Monitor, load_history
from .profiles import build_profile
from .tuner import load_state, save_state, tune

L = log.get("hardware")


CADUCA_S = 7 * 86400      # pasado este tiempo se detecta de nuevo antes de arrancar
ESTADO = {"origen": "", "cambio": False, "detalle": ""}   # para la interfaz: cache o deteccion, y si algo cambio


def _mtime(p: Path) -> int:
    try:
        return int(p.stat().st_mtime)
    except OSError:
        return 0


def huella() -> str:
    """Lo que, si cambia, obliga a detectar de nuevo antes de arrancar: sistema, CPU, RAM, versiones de la app y de
    Python, y los runtimes y paquetes instalados (fecha de sus carpetas). Es barata: no lanza procesos."""
    from .. import __version__, paths
    try:
        import psutil
        ram = round(psutil.virtual_memory().total / 2**30)
    except Exception:  # noqa: BLE001
        ram = 0
    import sysconfig
    carpetas = [paths.sub("runtimes"), paths.ROOT / "bundled" / "runtimes",
                Path(sysconfig.get_paths().get("purelib", "")), Path(sysconfig.get_paths().get("platlib", ""))]
    partes = [platform.system(), platform.release(), platform.machine(), str(os.cpu_count()), str(ram),
              sys.version.split()[0], __version__] + [str(_mtime(c)) for c in carpetas]
    return "|".join(partes)


def _archivo_huella() -> Path:
    from .. import paths
    return paths.data_dir() / "hardware.huella.json"


def _resumen(hw: HardwareInfo) -> str:
    """Lo que importa para elegir motor y perfil: GPU, NPU y runtimes con aceleracion."""
    r = hw.runtimes or {}
    clave = {"gpus": sorted((g.name, round(g.vram_gb or 0, 1)) for g in hw.gpus), "npus": sorted(n.name for n in hw.npus),
             "rt": {k: r.get(k) for k in ("llama_cpp_gpu_offload", "torch_cuda", "torch_xpu", "torch_mps",
                                          "onnxruntime_providers", "openvino_devices", "torch_directml")},
             "llama": sorted(d.get("name", "") for d in r.get("llama_devices") or [] if isinstance(d, dict))}
    return json.dumps(clave, sort_keys=True, default=str)


def detectar(ollama_url: str = "http://127.0.0.1:11434", rapida: bool = True) -> HardwareInfo:
    """Deteccion con cache: si nada ha cambiado desde la ultima vez (misma huella, menos de una semana) arranca con lo
    guardado y vuelve a detectar en segundo plano; lo que encuentre distinto se guarda para el siguiente arranque y se
    avisa en ESTADO. Asi el arranque no espera a PowerShell/WMI en Windows ni a las sondas de Android."""
    h = huella()
    f = _archivo_huella()
    cache = load_cached() if rapida else None
    try:
        guardada = json.loads(f.read_text(encoding="utf-8")) if cache else {}
    except (OSError, ValueError):
        guardada = {}
    if cache and guardada.get("huella") == h and time.time() - float(guardada.get("ts", 0)) < CADUCA_S:
        ESTADO.update(origen="cache", cambio=False, detalle="")

        def revisar() -> None:
            try:
                nuevo = detect(ollama_url=ollama_url)
                if _resumen(nuevo) != _resumen(cache):
                    ESTADO.update(cambio=True, detalle="Ha cambiado el hardware o sus runtimes: se aplica al reiniciar "
                                                       "o con «Detectar de nuevo».")
                    L.warning("Hardware distinto del guardado: %s", ESTADO["detalle"])
                save(nuevo)
                f.write_text(json.dumps({"huella": huella(), "ts": time.time()}), encoding="utf-8")
            except Exception as e:  # noqa: BLE001
                L.warning("Deteccion de hardware en segundo plano fallo: %s", e)

        threading.Thread(target=revisar, name="nexus-hw-detect", daemon=True).start()
        return cache
    hw = detect(ollama_url=ollama_url)
    save(hw)
    try:
        f.write_text(json.dumps({"huella": h, "ts": time.time()}), encoding="utf-8")
    except OSError:
        pass
    ESTADO.update(origen="deteccion", cambio=False, detalle="")
    return hw


def startup(override: str = "", adaptive: bool = True, ollama_url: str = "http://127.0.0.1:11434",
            previous_unclean: bool = False, previous_info: dict | None = None,
            rapida: bool = True) -> tuple[HardwareInfo, dict, list[str]]:
    """Detecta hardware (con cache, ver detectar), construye el perfil y aplica el autoajuste basado en sesiones
    previas. Tras un cierre sucio se detecta siempre de nuevo."""
    hw = detectar(ollama_url, rapida and not previous_unclean)
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


__all__ = ["ESTADO", "HardwareInfo", "Monitor", "build_profile", "detect", "detectar", "huella", "load_cached",
           "startup", "tune"]
