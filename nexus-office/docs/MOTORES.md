# Motores de inferencia por hardware

Desde la 4.0.0 Agente IA (edición Python) elige el **motor de inferencia** que mejor acelera el chip del equipo
donde se ejecuta, en lugar de usar siempre llama.cpp. Cada fabricante tiene su motor optimizado: los kernels de
TensorRT-LLM para NVIDIA, OpenVINO para la NPU de Intel, Genie o ONNX Runtime con QNN para la NPU Hexagon de
Qualcomm, LiteRT-LM para la TPU de los Pixel... llama.cpp sigue de respaldo en todos los equipos.

## Motores

| Motor | Aceleradores | Formato de modelo | Va en el paquete offline |
|---|---|---|---|
| **ONNX Runtime GenAI** | CUDA, DirectML (cualquier GPU en Windows), QNN (NPU Snapdragon X), OpenVINO, VitisAI (NPU Ryzen AI), CPU | carpeta con `genai_config.json` | Windows x64 (DirectML), Windows ARM64, Linux, macOS |
| **OpenVINO GenAI** | NPU Intel Core Ultra, iGPU Xe y Arc, CPU | carpeta con `openvino_model.xml` | Windows x64, Linux x86_64, macOS |
| **LiteRT-LM** | NPU de Tensor, Snapdragon, MediaTek e Intel; GPU; CPU (XNNPACK) | `.litertlm` | Windows x64 (con NPU Intel), Linux, macOS Apple Silicon (rueda `litert-lm-api` y `motores-npu.yml`) |
| **ExecuTorch** | XNNPACK (CPU), QNN, Vulkan, CoreML, MPS (según el `.pte`) | `.pte` + tokenizer | Windows x64 y macOS Apple Silicon; en Linux aparte (arrastra torch con CUDA, unos 3 GB): `nexus setup --all` con internet |
| **MLC LLM** | CUDA, ROCm, Vulkan, Metal, OpenCL (Adreno, Mali) | carpeta con `mlc-chat-config.json` | aparte (no hay rueda en PyPI): `nexus setup --all` con internet |
| **TensorRT-LLM** | NVIDIA (Tensor Cores) | checkpoint de Hugging Face o `rank0.engine` | aparte (varios GB): `nexus setup --all` con internet |
| **Qualcomm Genie** | NPU Hexagon (HTP) | carpeta de Qualcomm AI Hub con `genie_config.json` | Windows ARM64 y Linux ARM64 (QAIRT, de `motores-npu.yml`) |
| **llama.cpp** | CPU, CUDA, ROCm, Vulkan, Metal, SYCL | `.gguf` | siempre (respaldo) |

Si falta el paquete o la biblioteca de un motor, Agente IA dice por qué y usa el siguiente.

> **Android (APK autónoma) en la 4.0.0:** el Python de la app usa su propio cargador (musl) y no puede cargar las
> bibliotecas de Android de LiteRT-LM ni de Genie dentro del proceso. En el móvil sigue llama.cpp con CPU y GPU; la NPU
> del móvil por LiteRT-LM y Genie llegará con un proceso auxiliar nativo (como el de la edición C++).

## Qué motor va con cada chip

Agente IA resume el hardware en una familia y parte de este orden (ver `nexus/motores/chips.py`):

| Chip | Orden de partida |
|---|---|
| Snapdragon (móvil y X Elite/Plus) | Genie (NPU) → ONNX Runtime QNN → LiteRT-LM NPU → ExecuTorch QNN → MLC (Adreno) → llama.cpp |
| Google Tensor | LiteRT-LM NPU → LiteRT-LM GPU → MLC → ExecuTorch → llama.cpp |
| MediaTek, Exynos | LiteRT-LM NPU → LiteRT-LM GPU → MLC → ExecuTorch → llama.cpp |
| NVIDIA | TensorRT-LLM → ONNX Runtime CUDA (→ DirectML en Windows) → MLC CUDA → llama.cpp CUDA |
| AMD | (Windows: NPU Ryzen AI con VitisAI) → ONNX Runtime DirectML/ROCm → MLC → ExecuTorch Vulkan → llama.cpp |
| Intel | OpenVINO NPU → OpenVINO GPU → ONNX Runtime OpenVINO → DirectML/MLC → OpenVINO CPU → llama.cpp |
| Apple Silicon | MLC Metal → llama.cpp Metal → ExecuTorch MPS |
| Solo CPU | llama.cpp → OpenVINO CPU (x86) o LiteRT-LM (ARM) → ONNX Runtime CPU → ExecuTorch |

## El banco manda sobre la tabla

La tabla es solo el punto de partida. En **Sistema → Motores de inferencia → MEDIR TODOS LOS MOTORES** (o
`nexus motores banco` en la terminal) Agente IA genera una respuesta corta con cada modelo de texto instalado en su
motor y mide los **tokens por segundo** y el **tiempo hasta el primer token**. Desde ese momento se elige el más
rápido medido en tu equipo, aunque la tabla diga otra cosa (por ejemplo, si en tu portátil llama.cpp en la iGPU
supera a OpenVINO en la NPU). El resultado queda en `data/motores/banco.json`.

Entre los modelos medidos que van cómodos (8 tokens/s o más) se elige **el mayor**, que suele responder mejor, y a
igual tamaño el motor más rápido. Si ninguno llega a esa velocidad, el más rápido. Antes de medir se pide una
respuesta mínima para que la carga del modelo (que se apunta aparte) no cuente en el tiempo hasta el primer token.
Al cambiar de motor se descarga el modelo del anterior para no tener dos en memoria.

```bash
python -m nexus motores          # chip, orden, motores instalados y modelos con su motor
python -m nexus motores banco    # mide todos y muestra la tabla de tokens/s
```

## Aceleradores y caídas

Cada motor prueba sus aceleradores en orden (por ejemplo NPU → GPU → CPU). Si uno no arranca con un modelo, se
recuerda y se usa el siguiente. Mientras un motor carga o genera, Agente IA deja una marca en disco: si el proceso
se cae (un driver de NPU o GPU que falla), al volver a arrancar esa combinación queda apuntada como **caída** y no
se usa hasta pulsar *OLVIDAR CAÍDAS*.

Se puede fijar el motor y el acelerador en el mismo panel, o en `config.yaml`:

```yaml
llm:
  motor: auto            # auto | onnx | openvino | litert | executorch | mlc | tensorrt | genie | llamaserver
  motor_acelerador: ""   # "" = automático; p. ej. NPU, GPU, CPU (OpenVINO), cuda, dml, qnn (ONNX), npu (LiteRT)
```

## Modelos para cada motor

En **Modelos** aparecen, además de los GGUF, los recomendados para los motores de tu chip:

| Modelo | Motor | Repositorio |
|---|---|---|
| Qwen2.5 1.5B int4 | OpenVINO | `OpenVINO/Qwen2.5-1.5B-Instruct-int4-ov` |
| Phi-3.5 mini int4 | OpenVINO | `OpenVINO/Phi-3.5-mini-instruct-int4-ov` |
| Qwen3 8B int4 | OpenVINO | `OpenVINO/Qwen3-8B-int4-ov` |
| Phi-4 mini (CPU/QNN) | ONNX Runtime GenAI | `microsoft/Phi-4-mini-instruct-onnx` (`cpu_and_mobile/...`) |
| Phi-4 mini (GPU) | ONNX Runtime GenAI | `microsoft/Phi-4-mini-instruct-onnx` (`gpu/...`) |
| Qwen2.5 1.5B / Gemma 4 E2B | LiteRT-LM | `litert-community/...` |
| Qwen2.5 1.5B / Qwen3 4B q4f16 | MLC LLM | `mlc-ai/...-MLC` |
| Qwen3 4B 8da4w | ExecuTorch | `pytorch/Qwen3-4B-INT8-INT4` |
| Qwen2.5 1.5B | TensorRT-LLM | `Qwen/Qwen2.5-1.5B-Instruct` (se compila para tu GPU la primera vez) |

Los modelos de Genie salen de Qualcomm AI Hub compilados para cada Snapdragon: se importan como carpeta en
*Modelos → Importar*. También se puede importar cualquier carpeta o fichero de estos formatos: Agente IA reconoce el
formato y lo asigna a su motor.

## Pruebas

- `tests/test_motores.py`: formatos, matriz por chip, selector, banco, caídas y backend con un motor simulado.
- `.github/workflows/motores-python.yml`: descarga modelos reales y mide llama.cpp, ONNX Runtime GenAI y OpenVINO
  GenAI en los runners de GitHub (CPU x86_64 y ARM64); la tabla queda en el resumen de la ejecución.
- Primera medida en los runners (2026-11-10, solo CPU, respuesta corta):

  | Runner | Modelo | Motor | tok/s | 1.er token |
  |---|---|---|---|---|
  | x86_64 | Qwen2.5 1.5B | llama.cpp (Q4_K_M) | 24.7 | 4.50 s |
  | x86_64 | Qwen2.5 1.5B | OpenVINO (int4) | 15.3 | 7.41 s |
  | x86_64 | Phi-4 mini | ONNX Runtime GenAI (int4) | 12.0 | 6.65 s |
  | x86_64 | Phi-4 mini | llama.cpp (Q4_K_M) | 10.5 | 4.04 s |
  | x86_64 | Phi-3.5 mini | OpenVINO (int4) | 5.2 | 17.68 s |
  | ARM64 | Qwen2.5 1.5B | llama.cpp | 31.5 | 2.21 s |
  | ARM64 | Phi-4 mini | llama.cpp | 12.8 | 3.82 s |
  | ARM64 | Phi-4 mini | ONNX Runtime GenAI | 12.1 | 5.05 s |

  En CPU pura llama.cpp sigue siendo igual o más rápido salvo Phi-4 mini en x86 con ONNX Runtime: la ventaja de los
  otros motores está en la NPU y la GPU, y por eso manda el banco de cada equipo.
- Las NPU y GPU no se pueden probar en GitHub: se miden en el equipo de cada usuario con el banco.
