"""Que motor de inferencia va mejor con cada chip (matriz del dossier de arquitectura).

``chip()`` resume el hardware en una familia (snapdragon, tensor, mediatek, exynos, nvidia, amd, intel, apple, cpu)
y ``matriz()`` da, para esa familia y plataforma, los motores en orden de preferencia con el acelerador que se le
pide a cada uno. El orden es el de partida: ``banco.py`` lo corrige con los tokens/s medidos en este equipo.
"""
from __future__ import annotations

import re

FAMILIAS = ("snapdragon", "tensor", "mediatek", "exynos", "nvidia", "amd", "intel", "apple", "cpu")


def _txt(*partes) -> str:
    return " ".join(str(p or "") for p in partes).lower()


def chip(hw: dict, profile: dict | None = None) -> str:
    """Familia del acelerador mas capaz del equipo. ``hw`` es HardwareInfo.to_dict()."""
    profile = profile or {}
    npus = hw.get("npus") or []
    gpus = hw.get("gpus") or []
    soc = (hw.get("runtimes") or {}).get("android", {}).get("soc", {}) if isinstance(hw.get("runtimes"), dict) else {}
    n = _txt(*(f"{x.get('vendor')} {x.get('name')}" for x in npus), soc.get("manufacturer"), soc.get("model"),
             soc.get("name"), hw.get("cpu_name"))
    acc = (profile.get("accelerator") or "").lower()
    if re.search(r"qualcomm|snapdragon|hexagon|qcom|\bsm8\d|x1e|x1p|\bx elite|\bx plus", n) or acc == "hexagon":
        return "snapdragon"
    if re.search(r"\btensor\b|zuma|laguna|gs101|gs201|google", n):
        return "tensor"
    if re.search(r"mediatek|dimensity|\bmt6\d", n):
        return "mediatek"
    if re.search(r"exynos|samsung", n):
        return "exynos"
    dgpu = [g for g in gpus if not g.get("integrated")]
    vend = {g.get("vendor") for g in dgpu}
    if "nvidia" in vend or acc == "cuda":
        return "nvidia"
    if "amd" in vend or acc == "rocm":
        return "amd"
    if "apple" in {g.get("vendor") for g in gpus} or acc == "metal":
        return "apple"
    if any(x.get("vendor") == "amd" for x in npus):
        return "amd"
    if "intel" in vend or acc == "xpu" or any(x.get("vendor") == "intel" for x in npus):
        return "intel"
    if any(g.get("vendor") == "intel" for g in gpus) and "intel" in _txt(hw.get("cpu_name")):
        return "intel"
    return "cpu"


# motor, acelerador preferido, por que
Paso = tuple[str, str, str]


def matriz(familia: str, plataforma: str) -> dict:
    """Motores en orden de preferencia para una familia de chip y una plataforma (windows-x86_64, android-arm64...)."""
    win = plataforma.startswith("windows")
    android = plataforma.startswith("android")
    mac = plataforma.startswith("macos")
    arm = plataforma.endswith("arm64")
    pasos: list[Paso]
    if familia == "snapdragon":
        pasos = [("genie", "npu", "Genie (QAIRT) ejecuta el modelo entero en la NPU Hexagon"),
                 ("onnx", "qnn", "ONNX Runtime GenAI con el proveedor QNN (NPU) en Snapdragon X"),
                 ("litert", "npu", "LiteRT-LM con el dispatch de Qualcomm"),
                 ("executorch", "qnn", "ExecuTorch con el delegado QNN"),
                 ("mlc", "opencl" if android else "vulkan", "MLC LLM en la GPU Adreno"),
                 ("llamaserver", "cpu", "llama.cpp en los nucleos grandes (respaldo)")]
        nota = "NPU Hexagon: Genie o ONNX Runtime QNN para el texto; Adreno por MLC; llama.cpp de respaldo"
    elif familia == "tensor":
        pasos = [("litert", "npu", "LiteRT-LM con la TPU de Tensor"),
                 ("litert", "gpu", "LiteRT-LM en la GPU Mali"),
                 ("mlc", "opencl", "MLC LLM en la GPU"),
                 ("executorch", "xnnpack", "ExecuTorch con XNNPACK"),
                 ("llamaserver", "cpu", "llama.cpp (respaldo)")]
        nota = "Google Tensor: LiteRT-LM en la TPU o la GPU"
    elif familia in ("mediatek", "exynos"):
        pasos = [("litert", "npu", "LiteRT-LM con la NPU del fabricante"),
                 ("litert", "gpu", "LiteRT-LM en la GPU"),
                 ("mlc", "opencl", "MLC LLM en la GPU"),
                 ("executorch", "xnnpack", "ExecuTorch con XNNPACK"),
                 ("llamaserver", "cpu", "llama.cpp (respaldo)")]
        nota = "NPU del movil por LiteRT-LM; GPU por LiteRT-LM o MLC"
    elif familia == "nvidia":
        pasos = [("tensorrt", "cuda", "TensorRT-LLM: kernels de Tensor Cores, el mas rapido en NVIDIA"),
                 ("onnx", "cuda", "ONNX Runtime GenAI con CUDA"),
                 ("mlc", "cuda", "MLC LLM con CUDA"),
                 ("llamaserver", "cuda", "llama.cpp CUDA (respaldo y GGUF)")]
        if win:
            pasos.insert(2, ("onnx", "dml", "ONNX Runtime GenAI con DirectML"))
        nota = "NVIDIA: TensorRT-LLM; ONNX Runtime CUDA; llama.cpp CUDA de respaldo"
    elif familia == "amd":
        pasos = [("onnx", "vitisai", "NPU Ryzen AI (XDNA) con VitisAI")] if win else []
        pasos += [("onnx", "dml" if win else "rocm", "ONNX Runtime GenAI en la Radeon"),
                  ("mlc", "vulkan" if win else "rocm", "MLC LLM en la Radeon"),
                  ("executorch", "vulkan", "ExecuTorch con Vulkan"),
                  ("llamaserver", "rocm", "llama.cpp ROCm o Vulkan (respaldo)")]
        nota = "AMD: ONNX Runtime (DirectML/ROCm, NPU Ryzen AI); MLC; llama.cpp de respaldo"
    elif familia == "intel":
        pasos = [("openvino", "NPU", "OpenVINO GenAI en la NPU de Core Ultra"),
                 ("openvino", "GPU", "OpenVINO GenAI en la iGPU Xe o Arc"),
                 ("onnx", "openvino", "ONNX Runtime GenAI con OpenVINO"),
                 ("onnx", "dml", "ONNX Runtime GenAI con DirectML") if win else ("mlc", "vulkan", "MLC LLM con Vulkan"),
                 ("openvino", "CPU", "OpenVINO en la CPU (AMX/AVX-512)"),
                 ("llamaserver", "cpu", "llama.cpp (respaldo)")]
        nota = "Intel: OpenVINO (NPU o GPU); ONNX Runtime; llama.cpp de respaldo"
    elif familia == "apple":
        pasos = [("mlc", "metal", "MLC LLM con Metal"),
                 ("llamaserver", "metal", "llama.cpp con Metal"),
                 ("executorch", "mps", "ExecuTorch con MPS/CoreML")]
        nota = "Apple: MLC o llama.cpp con Metal; ExecuTorch con CoreML"
    else:
        pasos = [("llamaserver", "cpu", "llama.cpp: el mas rapido en CPU con GGUF"),
                 ("openvino", "CPU", "OpenVINO en CPU x86 (AVX-512/AMX)") if not arm else ("litert", "cpu", "LiteRT-LM con XNNPACK"),
                 ("onnx", "cpu", "ONNX Runtime GenAI en CPU (int4)"),
                 ("executorch", "xnnpack", "ExecuTorch con XNNPACK")]
        nota = "Solo CPU: llama.cpp; OpenVINO o ONNX Runtime int4 como alternativa"
    if mac:
        pasos = [p for p in pasos if p[0] not in ("tensorrt", "genie", "openvino") or p[0] == "openvino" and not arm]
    if android:
        pasos = [p for p in pasos if p[0] not in ("tensorrt", "openvino")]
    if not any(p[0] == "llamaserver" for p in pasos):
        pasos.append(("llamaserver", "cpu", "llama.cpp (respaldo)"))
    return {"chip": familia, "plataforma": plataforma, "nota": nota,
            "orden": [{"motor": m, "acelerador": a, "motivo": r} for m, a, r in pasos]}
