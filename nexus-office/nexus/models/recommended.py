"""Catalogo curado por niveles. La linea base esta pensada para CPU: cualquier hardware superior
solo puede ir a mejor (modelos mayores, mas contexto, mas resolucion).

Los LLM son todos multimodales con vision Y razonamiento: Qwen3.5 (2026, vision nativa y razonamiento hibrido) y,
como alternativa ya probada, Qwen3-VL Thinking.
"""
from __future__ import annotations

RECOMMENDED: list[dict] = [
    # ---------------------------------------------------------------- LLM multimodales: vision + razonamiento
    # Todos ven imagenes (mmproj) y razonan antes de responder (variantes «Thinking» de Qwen3-VL). Nexus-Office separa
    # el razonamiento de la respuesta y lo muestra plegado en el chat (llm.reasoning en config.yaml).
    # Si el nombre de un fichero cambiase en Hugging Face, la descarga busca en el repositorio el GGUF equivalente.
    # Qwen3.5 (Apache 2.0): vision nativa en todos los tamanos (imagen y video), razonamiento activado por defecto
    # (se apaga con enable_thinking=false para respuestas rapidas, p. ej. el modo en vivo por voz), 201 idiomas,
    # herramientas y 262k de contexto. Necesita llama.cpp con la arquitectura «qwen35» (la incluida la tiene).
    {"id": "qwen3.5-0.8b", "kind": "llm", "name": "Qwen3.5 0.8B · móviles y equipos muy justos", "vision": True,
     "reasoning": True, "params_b": 0.8, "repo": "unsloth/Qwen3.5-0.8B-GGUF", "file": "Qwen3.5-0.8B-Q4_K_M.gguf",
     "mmproj": "mmproj-F16.gguf", "size_mb": 738, "min_ram_gb": 2,
     "desc": "El mas pequeno con vision y razonamiento: moviles de 4-6 GB, Raspberry Pi y CPU antiguas."},
    {"id": "qwen3.5-2b", "kind": "llm", "name": "Qwen3.5 2B · base CPU", "vision": True, "reasoning": True,
     "params_b": 2, "repo": "unsloth/Qwen3.5-2B-GGUF", "file": "Qwen3.5-2B-Q4_K_M.gguf", "mmproj": "mmproj-F16.gguf",
     "size_mb": 1948, "min_ram_gb": 3, "baseline": True,
     "desc": "Ve imagenes, documentos y video, razona paso a paso y usa herramientas. Multilingue (201 idiomas). "
             "Linea base: CPU de 2-4 nucleos y moviles."},
    {"id": "qwen3.5-4b", "kind": "llm", "name": "Qwen3.5 4B · equilibrado", "vision": True, "reasoning": True,
     "params_b": 4, "repo": "unsloth/Qwen3.5-4B-GGUF", "file": "Qwen3.5-4B-Q4_K_M.gguf", "mmproj": "mmproj-F16.gguf",
     "size_mb": 3412, "min_ram_gb": 8, "min_vram_gb": 4,
     "desc": "Razonamiento, vision y uso de herramientas mucho mejores. CPU de 6+ nucleos, GPU de 4 GB o movil de "
             "gama alta."},
    {"id": "qwen3.5-9b", "kind": "llm", "name": "Qwen3.5 9B · alto rendimiento", "vision": True, "reasoning": True,
     "params_b": 9, "repo": "unsloth/Qwen3.5-9B-GGUF", "file": "Qwen3.5-9B-Q4_K_M.gguf", "mmproj": "mmproj-F16.gguf",
     "size_mb": 6598, "min_ram_gb": 16, "min_vram_gb": 8,
     "desc": "Calidad alta en razonamiento, matematicas, documentos y vision. Recomendado con GPU de 8 GB o mas."},
    # Qwen3-VL Thinking (2025): alternativa probada; no compite por el «★ recomendado»
    {"id": "qwen3-vl-2b-thinking", "kind": "llm", "name": "Qwen3-VL 2B Thinking · base CPU", "vision": True, "previous": True,
     "reasoning": True, "params_b": 2, "repo": "Qwen/Qwen3-VL-2B-Thinking-GGUF",
     "file": "Qwen3VL-2B-Thinking-Q4_K_M.gguf", "mmproj": "mmproj-Qwen3VL-2B-Thinking-Q8_0.gguf",
     "size_mb": 1480, "min_ram_gb": 3,
     "desc": "Ve imagenes (OCR, graficos) y razona paso a paso antes de responder. Multilingue (espanol) y con "
             "herramientas. El mas ligero con vision y razonamiento: CPU de 2-4 nucleos y moviles."},
    {"id": "qwen3-vl-4b-thinking", "kind": "llm", "name": "Qwen3-VL 4B Thinking · equilibrado", "vision": True, "previous": True,
     "reasoning": True, "params_b": 4, "repo": "Qwen/Qwen3-VL-4B-Thinking-GGUF",
     "file": "Qwen3VL-4B-Thinking-Q4_K_M.gguf", "mmproj": "mmproj-Qwen3VL-4B-Thinking-Q8_0.gguf",
     "size_mb": 2815, "min_ram_gb": 10, "min_vram_gb": 4,
     "desc": "Razonamiento y vision mucho mejores. CPU de 6+ nucleos, GPU de 4 GB o movil de gama alta."},
    {"id": "qwen3-vl-8b-thinking", "kind": "llm", "name": "Qwen3-VL 8B Thinking · alto rendimiento", "vision": True, "previous": True,
     "reasoning": True, "params_b": 8, "repo": "Qwen/Qwen3-VL-8B-Thinking-GGUF",
     "file": "Qwen3VL-8B-Thinking-Q4_K_M.gguf", "mmproj": "mmproj-Qwen3VL-8B-Thinking-Q8_0.gguf",
     "size_mb": 5512, "min_ram_gb": 16, "min_vram_gb": 7,
     "desc": "Calidad alta en razonamiento, matematicas y vision. Recomendado con GPU de 8 GB o mas."},
    # ---------------------------------------------------------------- imagen (stable-diffusion.cpp)
    {"id": "sdxs-512", "kind": "image", "name": "SDXS-512 · imagen en CPU (1 paso)", "params_b": 0.3,
     "repo": "concedo/sdxs-512-tinySDdistilled-GGUF", "file": "sdxs-512-tinySDdistilled_Q8_0.gguf",
     "size_mb": 651, "min_ram_gb": 4, "baseline": True, "gen": {"steps": 1, "cfg": 1.0, "size": 512},
     "desc": "Destilado de un paso: segundos por imagen en CPU."},
    {"id": "sd-turbo", "kind": "image", "name": "SD-Turbo · calidad media", "params_b": 0.9,
     "repo": "Green-Sky/SD-Turbo-GGUF", "file": "sd_turbo-f16-q8_0.gguf", "size_mb": 1930, "min_ram_gb": 8,
     "gen": {"steps": 2, "cfg": 1.0, "size": 512}, "desc": "Mas detalle; ~4x mas lento que SDXS en CPU."},
    {"id": "sdxl-turbo", "kind": "image", "name": "SDXL-Turbo · alta calidad", "params_b": 3.5,
     "repo": "OlegSkutte/sdxl-turbo-GGUF", "file": "sd_xl_turbo_1.0.q8_0.gguf", "size_mb": 3909, "min_ram_gb": 16,
     "min_vram_gb": 6, "gen": {"steps": 2, "cfg": 1.0, "size": 512}, "desc": "Para GPU de 6 GB o mas."},
    # ---------------------------------------------------------------- embeddings (memoria, grafo y biblioteca RAG)
    # Qwen3-Embedding entiende instrucciones por tarea: Nexus-Office le pide «busca en documentacion tecnica», «clasifica por
    # especialidad», «agrupa por tema» o «detecta duplicados» segun lo que este haciendo (nexus/memory/embeddings.py).
    # Corre en llama-server (el mismo runtime que el LLM, en un proceso aparte) y funciona en CPU sin GPU.
    {"id": "qwen3-embedding-0.6b", "kind": "embed", "name": "Qwen3-Embedding 0.6B · documentación (CPU)",
     "params_b": 0.6, "repo": "Qwen/Qwen3-Embedding-0.6B-GGUF", "file": "Qwen3-Embedding-0.6B-Q8_0.gguf",
     "size_mb": 639, "min_ram_gb": 4, "baseline": True, "dims": 1024, "instructions": True,
     "desc": "Embeddings especializados en buscar, clasificar y organizar documentación (100+ idiomas, español "
             "incluido, hasta 32k tokens). Ordena tu biblioteca y decide qué es conocimiento común y qué es de cada "
             "agente. Ligero en CPU: los documentos se indexan al momento y la precisión semántica se completa en "
             "segundo plano."},
    # ---------------------------------------------------------------- voz
    {"id": "piper-es-davefx", "kind": "tts", "name": "Piper · voz espanola (davefx)", "params_b": 0.02,
     "repo": "rhasspy/piper-voices", "file": "es/es_ES/davefx/medium/es_ES-davefx-medium.onnx",
     "extra": ["es/es_ES/davefx/medium/es_ES-davefx-medium.onnx.json"], "size_mb": 61, "min_ram_gb": 1,
     "baseline": True, "desc": "Voz neuronal offline y rapida en CPU (requiere paquete piper-tts)."},
]

# ---------------------------------------------------------------- modelos de sector (solo como sugerencia)
# No compiten por el «★ recomendado» general: aparecen cuando el perfil de oficina activo los sugiere
# (nexus/office: SECTOR_MODELS o la clave «modelos» del perfil) y el hardware los mueve con soltura.
SECTOR: list[dict] = [
    {"id": "medgemma-4b", "kind": "llm", "name": "MedGemma 4B · salud (Google)", "vision": True, "params_b": 4,
     "repo": "unsloth/medgemma-4b-it-GGUF", "file": "medgemma-4b-it-Q4_K_M.gguf", "mmproj": "mmproj-F16.gguf",
     "size_mb": 3300, "min_ram_gb": 8, "min_vram_gb": 4, "sector_only": True,
     "desc": "Entrenado con textos e imagenes medicas: terminologia clinica, informes y documentacion sanitaria. "
             "Es apoyo para el profesional colegiado, nunca un diagnostico."},
    {"id": "qwen2.5-coder-7b", "kind": "llm", "name": "Qwen2.5-Coder 7B · informática", "params_b": 7,
     "repo": "Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", "file": "qwen2.5-coder-7b-instruct-q4_k_m.gguf",
     "size_mb": 4680, "min_ram_gb": 12, "min_vram_gb": 6, "sector_only": True,
     "desc": "Especializado en programacion, scripts, redes y configuracion de sistemas."},
]
RECOMMENDED += SECTOR

# ---------------------------------------------------------------- modelos para los motores por hardware (nexus.motores)
# Mismo uso que los GGUF pero en el formato del motor que mejor acelera cada chip. «carpeta» = solo esa subcarpeta del
# repositorio (las exportaciones de ONNX traen una por proveedor); «repo_completo» = todo el repositorio.
MOTOR_MODELS: list[dict] = [
    {"id": "qwen2.5-1.5b-openvino", "kind": "llm", "name": "Qwen2.5 1.5B · OpenVINO int4 (NPU/GPU Intel)",
     "params_b": 1.5, "repo": "OpenVINO/Qwen2.5-1.5B-Instruct-int4-ov", "repo_completo": True, "format": "openvino",
     "motor": "openvino", "size_mb": 890, "min_ram_gb": 4,
     "desc": "Para la NPU de los Intel Core Ultra (AI Boost) y las iGPU Xe/Arc con OpenVINO GenAI."},
    {"id": "qwen3-8b-openvino", "kind": "llm", "name": "Qwen3 8B · OpenVINO int4 (GPU Intel Arc / NPU)",
     "params_b": 8, "reasoning": True, "repo": "OpenVINO/Qwen3-8B-int4-ov", "repo_completo": True,
     "format": "openvino", "motor": "openvino", "size_mb": 4700, "min_ram_gb": 16,
     "desc": "Razonamiento hibrido; para Arc dedicada o Core Ultra con 16 GB o mas."},
    {"id": "phi3.5-mini-openvino", "kind": "llm", "name": "Phi-3.5 mini · OpenVINO int4", "params_b": 3.8,
     "repo": "OpenVINO/Phi-3.5-mini-instruct-int4-ov", "repo_completo": True, "format": "openvino",
     "motor": "openvino", "size_mb": 2000, "min_ram_gb": 8,
     "desc": "Probado por Intel en la NPU de Core Ultra: buen equilibrio de velocidad y calidad."},
    {"id": "phi4-mini-onnx", "kind": "llm", "name": "Phi-4 mini · ONNX Runtime GenAI (CPU, QNN)", "params_b": 3.8,
     "repo": "microsoft/Phi-4-mini-instruct-onnx", "carpeta": "cpu_and_mobile/cpu-int4-rtn-block-32-acc-level-4",
     "format": "onnx-genai", "motor": "onnx", "size_mb": 2600, "min_ram_gb": 6,
     "desc": "Exportacion int4 de Microsoft para CPU y NPU (QNN en Snapdragon X)."},
    {"id": "phi4-mini-onnx-gpu", "kind": "llm", "name": "Phi-4 mini · ONNX Runtime GenAI (GPU: CUDA o DirectML)",
     "params_b": 3.8, "repo": "microsoft/Phi-4-mini-instruct-onnx", "carpeta": "gpu/gpu-int4-rtn-block-32",
     "format": "onnx-genai", "motor": "onnx", "size_mb": 2700, "min_ram_gb": 6, "min_vram_gb": 4,
     "desc": "Exportacion int4 para GPU: CUDA en NVIDIA y DirectML en cualquier GPU de Windows (AMD, Intel)."},
    {"id": "qwen2.5-1.5b-litert", "kind": "llm", "name": "Qwen2.5 1.5B · LiteRT-LM (movil: NPU, GPU o CPU)",
     "params_b": 1.5, "repo": "litert-community/Qwen2.5-1.5B-Instruct",
     "file": "Qwen2.5-1.5B-Instruct_multi-prefill-seq_q8_ekv4096.litertlm", "format": "litertlm", "motor": "litert",
     "size_mb": 1600, "min_ram_gb": 4,
     "desc": "LiteRT-LM de Google: GPU del movil y CPU con XNNPACK; contexto de 4096."},
    {"id": "gemma4-e2b-litert", "kind": "llm", "name": "Gemma 4 E2B · LiteRT-LM (NPU de Tensor, Snapdragon, MediaTek)",
     "params_b": 2, "repo": "litert-community/gemma-4-E2B-it-litert-lm", "file": "gemma-4-E2B-it.litertlm",
     "format": "litertlm", "motor": "litert", "size_mb": 2700, "min_ram_gb": 4,
     "desc": "Compilado para la NPU de los Pixel y de los moviles Snapdragon/Dimensity; GPU y CPU si no hay NPU."},
    {"id": "qwen2.5-1.5b-mlc", "kind": "llm", "name": "Qwen2.5 1.5B · MLC LLM (GPU: CUDA, Vulkan, Metal, OpenCL)",
     "params_b": 1.5, "repo": "mlc-ai/Qwen2.5-1.5B-Instruct-q4f16_1-MLC", "repo_completo": True, "format": "mlc",
     "motor": "mlc", "size_mb": 830, "min_ram_gb": 4,
     "desc": "q4f16 con kernels de TVM para la GPU de este equipo (Adreno y Mali por OpenCL, Radeon por Vulkan)."},
    {"id": "qwen3-4b-mlc", "kind": "llm", "name": "Qwen3 4B · MLC LLM (GPU)", "params_b": 4, "reasoning": True,
     "repo": "mlc-ai/Qwen3-4B-q4f16_1-MLC", "repo_completo": True, "format": "mlc", "motor": "mlc",
     "size_mb": 2170, "min_ram_gb": 8, "min_vram_gb": 4, "desc": "Razonamiento hibrido en la GPU con MLC."},
    {"id": "qwen3-4b-executorch", "kind": "llm", "name": "Qwen3 4B · ExecuTorch 8da4w (CPU ARM y x86, XNNPACK)",
     "params_b": 4, "reasoning": True, "repo": "pytorch/Qwen3-4B-INT8-INT4", "file": "model.pte",
     "extra": ["tokenizer.json", "tokenizer_config.json", "chat_template.jinja", "config.json"],
     "format": "executorch", "motor": "executorch", "size_mb": 3300, "min_ram_gb": 8,
     "desc": "PyTorch en el dispositivo: int8 dinamico + int4 en pesos con XNNPACK."},
    {"id": "qwen2.5-1.5b-tensorrt", "kind": "llm", "name": "Qwen2.5 1.5B · TensorRT-LLM (NVIDIA)", "params_b": 1.5,
     "repo": "Qwen/Qwen2.5-1.5B-Instruct", "repo_completo": True, "format": "tensorrt", "motor": "tensorrt",
     "size_mb": 3100, "min_ram_gb": 8, "min_vram_gb": 6,
     "desc": "Checkpoint de Hugging Face que TensorRT-LLM compila para tu GPU NVIDIA la primera vez (minutos)."},
]
RECOMMENDED += MOTOR_MODELS

STT_MODELS = {"tiny": 75, "base": 145, "small": 465, "medium": 1500}   # faster-whisper (se descarga al usarse)


def fit(rec: dict, profile: dict) -> str:
    """ideal | ok | pesado | no_cabe"""
    ram, vram = profile["ram_gb"], profile.get("vram_gb", 0)
    gpu = profile["llm"]["gpu_layers"] != 0
    if rec.get("min_ram_gb", 0) > ram and not (gpu and vram >= rec.get("min_vram_gb", 99)):
        return "no_cabe"
    if rec["kind"] == "llm":
        max_b, rec_b = profile["llm"]["max_model_b"], profile["llm"]["recommended_model_b"]
        if rec["params_b"] > max_b:
            return "pesado"
        return "ideal" if rec["params_b"] >= rec_b * 0.99 or rec["params_b"] == max_b else "ok"
    if rec["kind"] == "image":
        neural_gpu = profile["image"]["neural"]
        if rec.get("min_vram_gb") and not (neural_gpu and vram >= rec["min_vram_gb"]):
            return "pesado"
        return "ideal" if (rec.get("baseline") and not neural_gpu) or (neural_gpu and not rec.get("baseline")) else "ok"
    return "ideal"


# en la app Android autonoma solo hay llama-server (LLM y embeddings): la imagen (sd.cpp) y Piper no van en la
# APK, y la voz es la del sistema
ANDROID_KINDS = {"llm", "embed"}


# modelos de catalogos anteriores (sin razonamiento): siguen funcionando si ya estan descargados
LEGACY_LLM = {"smolvlm-500m": "qwen3.5-0.8b", "qwen3-vl-2b": "qwen3.5-2b", "qwen3-vl-4b": "qwen3.5-4b",
              "qwen3-vl-8b": "qwen3.5-9b"}


def recommend(profile: dict, sector: dict[str, str] | None = None, motores_chip: list[str] | None = None) -> list[dict]:
    """Catalogo con su encaje en este hardware; ``sector`` = {id: motivo} sugeridos por el perfil de oficina;
    ``motores_chip`` = motores en el orden de la matriz para el chip de este equipo (nexus.motores.chips)."""
    from ..hardware.profiles import android
    sector = sector or {}
    motores_chip = motores_chip or []
    out = []
    for r in RECOMMENDED:
        if android() and r["kind"] not in ANDROID_KINDS:
            continue
        if r.get("motor"):
            # modelos de motor: solo los de motores que la matriz pone para este chip, por delante de llama.cpp
            if r["motor"] not in motores_chip:
                continue
            item = {**r, "fit": fit(r, profile), "motor_rango": motores_chip.index(r["motor"])}
            item["para_tu_chip"] = motores_chip.index(r["motor"]) < motores_chip.index("llamaserver") \
                if "llamaserver" in motores_chip else True
            out.append(item)
            continue
        if r.get("sector_only") and r["id"] not in sector:
            continue
        item = {**r, "fit": fit(r, profile)}
        if r["id"] in sector:
            item["sector"] = sector[r["id"]]
        out.append(item)
    general = [r for r in out if not r.get("sector_only") and not r.get("previous") and not r.get("motor")]
    # marcar el mejor "ideal" por tipo
    for kind in {r["kind"] for r in general}:
        ideal = [r for r in general if r["kind"] == kind and r["fit"] == "ideal"]
        if ideal:
            max(ideal, key=lambda r: r["params_b"])["best"] = True
        elif kind == "llm":   # equipo muy justo: el mas ligero con vision y razonamiento, mejor que ninguno
            fits = [r for r in general if r["kind"] == kind and r["fit"] in ("ok", "pesado")]
            if fits:
                min(fits, key=lambda r: r["params_b"])["best"] = True
    return out
