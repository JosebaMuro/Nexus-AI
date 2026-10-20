"""Eleccion manual, al arrancar, del motor llama y del acelerador (CPU, GPU o NPU).

* Motor: ``auto`` · ``llamaserver`` (llama.cpp oficial como proceso aparte: vision, CPU/GPU/NPU) · ``llamacpp``
  (llama-cpp-python dentro de Nexus-Office: CPU, y Metal en Mac). Los dos van incluidos en los instaladores; Ollama
  no se ofrece aqui (es otra aplicacion), aunque se sigue usando si esta instalado y no hay otro motor.
* Acelerador: ``auto`` · ``cpu`` · ``gpu`` · ``npu``.

La eleccion se traduce al perfil de hardware (capas en la GPU, dispositivo de llama.cpp, variante del binario y
entorno) y se comprueba de verdad: tras arrancar llama-server se lee su registro para saber donde ha quedado el
modelo («offloaded 29/29 layers to GPU», el dispositivo Vulkan/CUDA/Metal, HTP de Hexagon u OpenVINO NPU). Si lo
pedido no esta disponible se dice por que y se usa lo siguiente que funcione (NPU -> GPU -> CPU).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from .profiles import _best_gpu, android, build_profile

ENGINES = ("auto", "llamaserver", "llamacpp")
ACCELS = ("auto", "cpu", "gpu", "npu")
ENGINE_LABEL = {"auto": "Automático", "llamaserver": "llama-server (llama.cpp, proceso aparte)",
                "llamacpp": "llama.cpp integrado (llama-cpp-python)"}


def _android_devices(hw) -> list[dict]:
    rt = (hw.runtimes or {}).get("android_accel") or {}
    devs = rt.get("devices")
    if devs is None:
        try:
            from . import android_accel
            devs = android_accel.detect().get("gpu", {}).get("devices", [])
        except Exception:  # noqa: BLE001
            devs = []
    return devs or []


def plan(hw, accel: str) -> dict:
    """Que hacer para ``accel`` en este equipo: {"override", "variant", "device", "env", "ok", "note"}."""
    accel = accel if accel in ACCELS else "auto"
    gpu = _best_gpu(hw)
    if accel == "auto":
        return {"override": "", "variant": "", "device": None, "env": {}, "ok": True, "note": ""}
    if accel == "cpu":
        return {"override": "cpu", "variant": "cpu", "device": "", "env": {}, "ok": True, "note": ""}
    if android():
        from . import android_accel
        devs = _android_devices(hw)
        if accel == "npu":
            htp = next((d for d in devs if android_accel.is_npu_dev(d["dev"])), None)
            if htp:
                return {"override": "hexagon", "variant": "", "device": htp["dev"], "env": {}, "ok": True, "note": ""}
            accel = "gpu"
            note = "Este móvil no tiene una NPU que llama.cpp pueda usar (Hexagon HTP v73+); se usa la GPU"
        else:
            note = ""
        g = android_accel.pick(devs, npu=False)
        if g:
            return {"override": g["backend"], "variant": "", "device": g["dev"], "env": {}, "ok": accel == "gpu" and not note,
                    "note": note}
        return {"override": "cpu", "variant": "cpu", "device": "", "env": {}, "ok": False,
                "note": (note + "; " if note else "") + "ninguna GPU utilizable por llama.cpp: se usa la CPU"}
    if accel == "npu":
        if any(n.vendor == "intel" for n in hw.npus or []):
            # llama.cpp con OpenVINO: GGML_OPENVINO_DEVICE=NPU, un solo slot y contexto explicito
            return {"override": "openvino", "variant": "openvino", "device": "", "ok": True, "note": "",
                    "env": {"GGML_OPENVINO_DEVICE": "NPU"}, "npu": True}
        name = hw.npus[0].name if hw.npus else ""
        note = (f"llama.cpp no puede ejecutar el modelo de texto en la NPU {name} (solo Intel con OpenVINO en "
                "escritorio); la NPU se usa para embeddings y voz si su runtime está instalado"
                if name else "No se ha detectado ninguna NPU")
        sub = plan(hw, "gpu" if gpu else "cpu")
        return {**sub, "ok": False, "note": note + (f"; el modelo va en la {'GPU' if gpu else 'CPU'}")}
    # GPU de escritorio
    if gpu is None:
        return {"override": "cpu", "variant": "cpu", "device": "", "env": {}, "ok": False,
                "note": "No se ha detectado ninguna GPU: se usa la CPU"}
    if sys.platform == "darwin":
        return {"override": "metal", "variant": "metal", "device": "", "env": {}, "ok": True, "note": ""}
    if gpu.vendor == "nvidia" and gpu.vram_gb > 0:
        return {"override": "cuda", "variant": "cuda", "device": "", "env": {}, "ok": True, "note": ""}
    note = "" if not gpu.integrated else "GPU integrada: puede ir más lenta que la CPU con modelos grandes"
    return {"override": "vulkan", "variant": "vulkan", "device": "", "env": {}, "ok": True, "note": note}


def options(services) -> dict:
    """Lo que ve el selector: hardware, motores y aceleradores con su disponibilidad, y la eleccion actual."""
    hw, cfg, llm = services.hw, services.config, services.llm
    gpu = _best_gpu(hw)
    engines = []
    for e in ENGINES:
        if e == "auto":
            engines.append({"id": e, "label": ENGINE_LABEL[e], "available": True, "note": "el mejor que haya"})
            continue
        ok, note = _engine_state(services, e)
        engines.append({"id": e, "label": ENGINE_LABEL[e], "available": ok, "note": note})
    accels = []
    for a in ACCELS:
        p = plan(hw, a)
        label = {"auto": "Automático", "cpu": "CPU", "gpu": f"GPU{' · ' + gpu.name if gpu else ''}",
                 "npu": f"NPU{' · ' + hw.npus[0].name if hw.npus else ''}"}[a]
        accels.append({"id": a, "label": label, "available": p["ok"], "note": p["note"]})
    eff = getattr(llm.backends.get("llamaserver"), "effective", None) or {}
    return {"engine": cfg.get("llm.engine", "auto"), "accel": cfg.get("hardware.accel", "auto"),
            "ask": bool(cfg.get("startup.ask_accel", True)), "engines": engines, "accels": accels,
            "hardware": {"cpu": hw.cpu_name, "cores": hw.cores_physical, "ram_gb": hw.ram_gb,
                         "gpus": [g.name for g in hw.gpus], "npus": [n.name for n in hw.npus or []],
                         "android": android()},
            "profile": {"accelerator": services.profile["accelerator"], "device": services.profile["device_name"],
                        "gpu_layers": services.profile["llm"]["gpu_layers"]},
            "effective": eff, "selected": llm.select()[0]}


def _engine_state(services, engine: str) -> tuple[bool, str]:
    from .. import runtimes as rt
    gguf = services.llm._resolve("gguf")("") is not None
    if engine == "llamaserver":
        have = services.runtimes.exe("llama", services.profile, services.config.get("hardware.llama_variant", "")) is not None
        if not have:
            return False, "falta el binario llama-server"
        return (True, "incluido · CPU, GPU y NPU") if gguf else (False, "incluido; falta un modelo GGUF (vista Modelos)")
    if android():
        return False, "no disponible en Android: usa llama-server"
    if rt.llama_python_ready():
        note = "instalado · CPU" + (" y Metal" if sys.platform == "darwin" else "")
    elif rt.bundled_llama_python():
        note = "incluido; se instala al elegirlo · CPU" + (" y Metal" if sys.platform == "darwin" else "")
    else:
        return False, "este paquete no lo incluye"
    return (True, note) if gguf else (False, note + "; falta un modelo GGUF")


def build_for(hw, accel: str, legacy_override: str = "") -> tuple[dict, dict]:
    """(perfil, plan) para el acelerador elegido."""
    p = plan(hw, accel)
    profile = build_profile(hw, p["override"] or legacy_override)
    llm = profile["llm"]
    if p["device"] is not None and android():
        llm["device"] = p["device"]
        llm["gpu_layers"] = -1 if p["device"] else 0
    if p.get("npu"):   # OpenVINO NPU: grafo estatico, un slot y contexto moderado
        llm["gpu_layers"], llm["device_env"], llm["single_slot"] = 0, dict(p["env"]), True
        llm["n_ctx"] = min(llm["n_ctx"], 4096)
        profile["npu"]["offload"] = sorted(set(profile["npu"].get("offload", [])) | {"llm"})
    if accel == "cpu":
        llm["gpu_layers"], llm["device"] = 0, ""
    profile["choice"] = {"accel": accel, **{k: p[k] for k in ("ok", "note", "variant")}}
    return profile, p


def apply(services, engine: str = "auto", accel: str = "auto", remember: bool = True) -> dict:
    """Aplica la eleccion: perfil nuevo, variante del binario, motor preferido. Reinicia el LLM."""
    if engine not in ENGINES or accel not in ACCELS:
        raise ValueError(f"motor {ENGINES} y acelerador {ACCELS}")
    cfg = services.config
    notes = []
    if engine == "llamacpp":
        from .. import runtimes as rt
        err = rt.install_llama_python()
        if err:
            raise ValueError(err)
        if accel in ("gpu", "npu") and sys.platform != "darwin":
            notes.append("el motor integrado incluido usa la CPU; para GPU o NPU elige llama-server")
    profile, p = build_for(services.hw, accel, cfg.get("hardware.profile_override", "") if accel == "auto" else "")
    services.profile.clear()
    services.profile.update(profile)
    cfg.set("llm.engine", engine)
    cfg.set("hardware.accel", accel)
    cfg.set("hardware.llama_variant", p["variant"])
    if remember is not None:
        cfg.set("startup.ask_accel", not remember)
    services.llm.rebuild()
    if p["note"]:
        notes.insert(0, p["note"])
    return {"engine": engine, "accel": accel, "ok": p["ok"] and not notes, "note": "; ".join(notes), "variant": p["variant"],
            "selected": services.llm.select()[0], "profile": {"accelerator": profile["accelerator"],
                                                               "gpu_layers": profile["llm"]["gpu_layers"]}}


# ------------------------------------------------------------------ comprobacion en el registro de llama-server
OFFLOAD = re.compile(r"offloaded (\d+)/(\d+) layers to GPU")
DEVICE_PATTERNS = [   # del mas concreto al mas general: se usa el primero que aparezca
    (re.compile(r"using device (CUDA\d+)\s*\(([^)]*)\)"), None),
    (re.compile(r"using device (Vulkan\d+)\s*\(([^)]*)\)"), None),
    (re.compile(r"using device (Metal|MTL\d*)\s*\(([^)]*)\)"), None),
    (re.compile(r"using device (HTP\d+[:\d]*)\s*\(([^)]*)\)"), None),
    (re.compile(r"using device (GPUOpenCL|OpenCL\d*)\s*\(([^)]*)\)"), None),
    (re.compile(r"ggml_cuda_init: found (\d+) CUDA devices?"), "CUDA"),
    (re.compile(r"ggml_vulkan: \d+ = ([^|(\n]+)"), "Vulkan"),
    (re.compile(r"(?:OpenVINO|OPENVINO)[^\n]*device[^\n]*?\b(NPU|GPU|CPU)\b"), "OpenVINO"),
]


def effective_from_log(text: str, env: dict | None = None) -> dict:
    """Donde ha quedado el modelo segun el registro de llama-server de este arranque."""
    out: dict = {"device": "CPU", "offloaded": "", "detail": ""}
    m = OFFLOAD.findall(text)
    if m:
        a, b = m[-1]
        out["offloaded"] = f"{a}/{b}"
    for pat, kind in DEVICE_PATTERNS:
        found = pat.findall(text)
        if not found:
            continue
        last = found[-1]
        if kind == "CUDA":
            out["detail"] = f"CUDA ({last} dispositivo{'s' if last != '1' else ''})"
        elif kind == "Vulkan":
            out["detail"] = f"Vulkan · {last.strip()}"
        elif kind == "OpenVINO":
            out["detail"] = f"OpenVINO {last}"
        else:
            out["detail"] = f"{last[0]} · {last[1].strip()}"
        break      # los patrones van del mas concreto («using device X») al mas general
    gpu_layers = m and int(m[-1][0]) > 0
    if (env or {}).get("GGML_OPENVINO_DEVICE") == "NPU" and "openvino" in text.lower():
        out["device"] = "NPU (OpenVINO)"
    elif out["detail"].startswith("HTP"):
        out["device"] = "NPU (Hexagon)"
    elif gpu_layers:
        out["device"] = "GPU"
    return out


def read_log_since(path: Path, offset: int) -> str:
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read(2_000_000).decode("utf-8", "replace")
    except OSError:
        return ""
