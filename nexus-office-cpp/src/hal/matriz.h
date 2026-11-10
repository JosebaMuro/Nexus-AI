// Matriz de motores por chip (dossier de arquitectura, «Unified Multi-Platform AI Agent Engine»): que motor usa cada
// tarea (LLM, difusion, vision) segun el procesador, la GPU y la NPU del equipo.
//
//   chip         LLM                                  difusion                      vision (percepcion)
//   Snapdragon   Genie (QNN) > LiteRT-LM NPU > llama  ONNX Runtime QNN (HTP)        ONNX Runtime QNN
//   Tensor       LiteRT-LM (NPU/GPU) > llama.cpp      ONNX Runtime NNAPI/XNNPACK    ONNX Runtime NNAPI
//   NVIDIA       llama.cpp CUDA > ONNX GenAI CUDA     ONNX Runtime TensorRT/CUDA    ONNX Runtime CUDA
//   AMD          llama.cpp ROCm/Vulkan > ONNX DML     ONNX Runtime ROCm/DirectML    ONNX Runtime ROCm/DirectML
//   Intel        ONNX GenAI OpenVINO > llama.cpp      ONNX Runtime OpenVINO         ONNX Runtime OpenVINO
//   Apple        llama.cpp Metal                      ONNX Runtime CoreML           ONNX Runtime CoreML
//   CPU          llama.cpp > LiteRT-LM CPU            ONNX Runtime XNNPACK/CPU      ONNX Runtime XNNPACK/CPU
#pragma once
#include <string>

#include "util/json.h"

namespace nx::hal {

// chip principal para elegir motores: snapdragon | tensor | mediatek | exynos | nvidia | amd | intel | apple | cpu
std::string chip(const json& perfil);
// {chip, llm: [motores en orden], difusion: [proveedores], vision: [proveedores], nota}
json matriz(const json& perfil);

}  // namespace nx::hal
