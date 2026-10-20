"""Seleccion del perfil optimo a partir del hardware detectado.

El perfil decide, por carga de trabajo (LLM, imagen, musica, voz, embeddings), el
dispositivo, tamano de modelo recomendado, contexto, hilos, capas en GPU y uso de NPU.
"""
from __future__ import annotations

import os
import sys

from .detect import GPU, HardwareInfo

# tamanos de modelo (miles de millones de parametros) que solemos recomendar
MODEL_TIERS = [0.5, 1.5, 3, 4, 8, 14, 32, 70]
GB_PER_B_Q4 = 0.65   # GGUF Q4_K_M ~0.6 GB por B de parametros + overhead


def _best_gpu(hw: HardwareInfo) -> GPU | None:
    if not hw.gpus:
        return None
    rank = {"nvidia": 4, "amd": 3, "intel": 2, "apple": 3, "qualcomm": 1}
    return max(hw.gpus, key=lambda g: (not g.integrated, rank.get(g.vendor, 0), g.vram_gb))


def choose_accelerator(hw: HardwareInfo, override: str = "") -> tuple[str, GPU | None]:
    gpu = _best_gpu(hw)
    if override:
        return override, gpu
    rt = hw.runtimes or {}
    if gpu is None:
        return "cpu", None
    if android():
        # movil: la GPU solo si llama-server con GPU la ha visto (Adreno por OpenCL, el resto por Vulkan)
        return (rt.get("android_accel", {}).get("gpu_backend") or "cpu"), gpu
    if gpu.vendor == "nvidia":
        # VRAM 0 = tarjeta vista en Linux sin nvidia-smi (nouveau o driver sin instalar): CUDA no funcionaria
        return ("cuda" if gpu.vram_gb > 0 else "cpu"), gpu
    if gpu.vendor == "apple":
        return "metal", gpu
    if gpu.integrated:
        # Las iGPU modestas rara vez superan a la CPU en LLM; OpenVINO si esta disponible
        if gpu.vendor == "intel" and "GPU" in rt.get("openvino_devices", []):
            return "openvino", gpu
        return "cpu", gpu
    if gpu.vendor == "amd":
        return ("rocm" if sys.platform.startswith("linux") else "vulkan"), gpu
    if gpu.vendor == "intel":
        return ("xpu" if rt.get("torch_xpu") else "vulkan"), gpu
    return "cpu", gpu


def android() -> bool:
    """Nexus-Office corre dentro de la app Android autonoma (o en Termux)."""
    return os.environ.get("NEXUS_ANDROID") == "1" or hasattr(sys, "getandroidapilevel")


def _tier_at_most(b: float) -> float:
    fit = [t for t in MODEL_TIERS if t <= b]
    return fit[-1] if fit else MODEL_TIERS[0]


def build_profile(hw: HardwareInfo, override: str = "") -> dict:
    acc, gpu = choose_accelerator(hw, override)
    gpu_offload = acc in ("cuda", "rocm", "vulkan", "metal", "xpu", "opencl") and gpu is not None
    ram = hw.ram_gb
    cpu_budget = max(0.5, min(ram * 0.5, ram - 3.0))
    if gpu_offload and gpu and not gpu.integrated and not android():
        budget = gpu.vram_gb * 0.85
    elif acc == "metal":
        budget = ram * 0.65
    else:
        budget = cpu_budget
    max_b = _tier_at_most(budget / GB_PER_B_Q4)
    if not gpu_offload:
        # la CPU limita la velocidad: con pocos nucleos priorizamos respuesta fluida
        compute_cap = 3 if hw.cores_physical <= 2 else 4 if hw.cores_physical <= 4 else 8 if hw.cores_physical <= 8 else 14
        if "avx2" not in hw.cpu_flags and hw.arch.lower() not in ("arm64", "aarch64"):
            compute_cap = min(compute_cap, 1.5)
        max_b = min(max_b, compute_cap)
    recommended_b = _tier_at_most(max_b * 0.6) if max_b > 1.5 else max_b
    if android():
        # movil: Android y las demas apps necesitan su RAM y la CPU se calienta; modelos pequenos y fluidos
        max_b = 4 if ram >= 11 else 3 if ram >= 6 else 1.5
        recommended_b = {4: 3, 3: 1.5}.get(max_b, 0.5)
        budget = min(budget, max(0.5, ram * 0.45))
        from ..android_tune import core_layout, thread_candidates
        layout = core_layout()
    ctx_mem = budget - recommended_b * GB_PER_B_Q4
    n_ctx = 16384 if ctx_mem >= 8 else 8192 if ctx_mem >= 3 else 4096 if ctx_mem >= 1.2 else 2048
    max_ctx = min(32768, n_ctx * 4)
    if android():   # en el movil el prompt se procesa despacio: contextos moderados
        n_ctx, max_ctx = min(n_ctx, 4096), min(max_ctx, 8192)

    vram = gpu.vram_gb if gpu else 0.0
    torch_device = {"cuda": "cuda", "rocm": "cuda", "metal": "mps", "xpu": "xpu"}.get(acc, "cpu")
    if sys.platform == "win32" and gpu and gpu.vendor in ("amd", "intel") and not gpu.integrated:
        torch_device = "dml"
    image_gpu = torch_device != "cpu" and (vram >= 4 or acc == "metal")

    npu = hw.npus[0] if hw.npus else None
    npu_ready = False
    if npu:
        rt = hw.runtimes or {}
        npu_ready = ("NPU" in rt.get("openvino_devices", []) or
                     any(p in rt.get("onnxruntime_providers", []) for p in
                         ("VitisAIExecutionProvider", "QNNExecutionProvider", "CoreMLExecutionProvider")))

    phys = max(1, hw.cores_physical)
    return {
        "id": f"{acc}-{(gpu.vendor if gpu else 'cpu')}-{'igpu' if gpu and gpu.integrated else 'dgpu' if gpu else 'only'}",
        "accelerator": acc,
        "device_name": gpu.name if gpu and acc != "cpu" else hw.cpu_name,
        "vram_gb": vram,
        "ram_gb": ram,
        "tier": "alto" if max_b >= 14 else "medio" if max_b >= 4 else "ligero",
        "llm": {
            "max_model_b": max_b,
            "recommended_model_b": recommended_b,
            "quant": "Q4_K_M" if budget < 24 else "Q5_K_M",
            "n_ctx": n_ctx,
            "max_ctx": max_ctx,
            "n_threads": layout["fast"] if android() else phys,   # movil: los nucleos rapidos; los pequenos frenan
            "n_batch": 512 if gpu_offload or ram >= 16 else 256,
            "gpu_layers": -1 if gpu_offload else 0,
            # Android: dispositivo de llama.cpp (GPUOpenCL, Vulkan0) para no repartir el modelo entre dos backends
            **({"device": (hw.runtimes or {}).get("android_accel", {}).get("gpu_device", "") if gpu_offload else ""}
               if android() else {}),
            "llamacpp_wheel": {"cuda": "cu124", "metal": "metal", "vulkan": "vulkan", "rocm": "rocm",
                               "xpu": "vulkan", "opencl": "opencl"}.get(acc, "cpu"),
            "flash_attn": acc in ("cuda", "metal"),
            # Android: los hilos los mide y ajusta en vivo nexus.android_tune (temperatura y bateria incluidas)
            **({"live_threads": True, "thread_candidates": thread_candidates(layout)} if android() else {}),
        },
        "image": {
            "device": torch_device,
            "neural": image_gpu,
            "resolution": 1024 if vram >= 10 else 512,
            "steps": 4,
            "suggested_model": "stabilityai/sdxl-turbo" if vram >= 10 else "stabilityai/sd-turbo",
        },
        "music": {"device": torch_device, "neural": image_gpu, "suggested_model": "facebook/musicgen-small"},
        "stt": {
            "device": "cuda" if acc == "cuda" else "cpu",
            "model": ("small" if vram >= 4 else "base") if acc == "cuda" else ("base" if phys >= 4 else "tiny"),
            "compute_type": "float16" if acc == "cuda" else "int8",
        },
        "embeddings": {"device": "npu" if npu_ready else torch_device},
        "npu": {
            "present": bool(npu),
            "name": npu.name if npu else "",
            "vendor": npu.vendor if npu else "",
            "runtime": npu.runtime_hint if npu else "",
            "ready": npu_ready,
            "offload": ["embeddings", "stt"] if npu_ready else [],
            "hint": "" if not npu or npu_ready else
                    ("llama.cpp no puede usar esta NPU en Android (su SDK es propietario): el modelo va en la "
                     + ("GPU" if gpu_offload else "CPU") if android() else
                     f"Instala el runtime '{npu.runtime_hint}' para usar la NPU"),
        },
    }
