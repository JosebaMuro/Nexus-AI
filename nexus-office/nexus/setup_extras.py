"""Instalacion de extras segun el hardware detectado: runtimes nativos, voz, NPU y (si hay GPU) PyTorch."""
from __future__ import annotations

from . import hardware, log, wheelhouse
from .runtimes import RuntimeManager, bundled_variants, candidates

L = log.get("setup")


def plan(profile: dict, hw) -> list[dict]:
    steps = [
        {"name": "Runtime LLM llama.cpp", "runtime": ("llama", candidates(profile, "llama")[0]),
         "reason": f"binario optimizado para '{profile['accelerator']}'"},
        {"name": "Runtime imagen sd.cpp", "runtime": ("sd", candidates(profile, "sd")[0]), "reason": "generacion de imagen nativa"},
        {"name": "Voz neural offline", "pip": ["piper-tts"], "reason": "texto a voz Piper (CPU)"},
        {"name": "Reconocimiento de voz", "pip": ["faster-whisper"], "reason": f"Whisper {profile['stt']['model']} {profile['stt']['compute_type']}"},
        {"name": "Voz del sistema", "pip": ["pyttsx3"], "reason": "respaldo SAPI/espeak"},
    ]
    npu = profile["npu"]
    if npu["present"] and not npu["ready"]:
        pkg = {"intel": ["openvino"], "qualcomm": ["onnxruntime-qnn"], "amd": ["onnxruntime"]}.get(npu["vendor"])
        if pkg:
            steps.append({"name": f"Runtime NPU ({npu['vendor']})", "pip": pkg, "reason": npu["hint"]})
    acc = profile["accelerator"]
    if profile["music"]["neural"]:
        idx = {"cuda": "https://download.pytorch.org/whl/cu124", "rocm": "https://download.pytorch.org/whl/rocm6.2",
               "xpu": "https://download.pytorch.org/whl/xpu"}.get(acc)
        torch = ["torch"] if idx or acc == "metal" else ["torch-directml"] if profile["image"]["device"] == "dml" else []
        if torch:
            steps.append({"name": "PyTorch acelerado + MusicGen", "pip": torch + ["transformers", "scipy"],
                          "index": idx, "reason": "musica neuronal en GPU", "optional": True})
    return steps


def run(auto: bool = True, include_optional: bool = False) -> None:
    log.setup()
    hw = hardware.detect()
    profile = hardware.build_profile(hw)
    rm = RuntimeManager()
    for step in plan(profile, hw):
        # opcional (PyTorch): se instala solo si sus wheels vienen en el paquete offline (no descarga GB)
        if step.get("optional") and not include_optional and not ("pip" in step and wheelhouse.has_local(step["pip"])):
            L.info("Opcional (omitido, usa --all): %s", step["name"])
            continue
        L.info("==> %s (%s)", step["name"], step["reason"])
        try:
            if "runtime" in step:
                rt, variant = step["runtime"]
                if rm.find_exe(rt, variant):
                    continue
                bundled = bundled_variants(rt)
                if bundled and (variant.startswith("cpu") or variant in bundled):
                    L.info("Incluido en el paquete: %s %s (sin descargar)", rt, variant if variant in bundled else bundled[0])
                    continue
                if wheelhouse.offline_only():
                    L.info("Modo offline: %s", "se usa el incluido en el paquete (CPU)" if bundled
                           else "omitido; se instala luego desde Sistema > Runtimes")
                    continue
                try:
                    rm.install(rt, variant)
                except Exception as e:  # noqa: BLE001
                    if not bundled:
                        raise
                    L.warning("No se pudo instalar %s %s (%s): se usa el incluido en el paquete (CPU)", rt, variant, e)
            else:
                # wheelhouse/ primero (sin red) y nunca compilar: ver nexus/wheelhouse.py
                wheelhouse.pip_install(step["pip"], index=step.get("index"))
        except Exception as e:  # noqa: BLE001
            L.error("Fallo en '%s': %s (se continua)", step["name"], e)
    L.info("Extras completados. Descarga modelos recomendados desde la pestana Modelos.")
