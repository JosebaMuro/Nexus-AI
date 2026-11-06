// Matriz de motores por chip (ver matriz.h).
#include "hal/matriz.h"

#include "media/difusion_onnx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::hal {

std::string chip(const json& perfil) {
  std::string acc = jstr(perfil, "accelerator"), id = lower(jstr(perfil, "id")), dev = lower(jstr(perfil, "device_name"));
  json npu = perfil.value("npu", json::object());
  if (!npu.is_object()) npu = json::object();
  std::string n = lower(jstr(npu, "vendor") + " " + jstr(npu, "name") + " " + jstr(npu, "soc"));
  if (contains(n, "qualcomm") || contains(n, "snapdragon") || contains(n, "hexagon") || contains(n, "qcom") ||
      contains(n, "sm8") || contains(n, "x1e") || acc == "hexagon")
    return "snapdragon";
  if (contains(n, "tensor") || contains(n, "zuma") || contains(n, "laguna") || contains(n, "gs101") || contains(n, "gs201"))
    return "tensor";
  if (contains(n, "mediatek") || contains(n, "dimensity") || contains(n, " mt6")) return "mediatek";
  if (contains(n, "exynos") || contains(n, "samsung")) return "exynos";
  if (acc == "cuda" || contains(id, "nvidia")) return "nvidia";
  if (acc == "rocm" || contains(id, "amd")) return "amd";
  if (acc == "metal" || contains(id, "apple")) return "apple";
  if (acc == "xpu" || contains(id, "intel") || contains(n, "intel")) return "intel";
  return "cpu";
}

json matriz(const json& perfil) {
  std::string c = chip(perfil);
  json llm;
  std::string nota;
  if (c == "snapdragon") {
    llm = {"genie", "litert", "llamaserver"};
    nota = "NPU Hexagon: Genie (QNN) para el texto y ONNX Runtime QNN para la imagen y la vision";
  } else if (c == "tensor") {
    llm = {"litert", "llamaserver"};
    nota = "Google Tensor: LiteRT-LM con la NPU o la GPU; imagen y vision con NNAPI";
  } else if (c == "mediatek" || c == "exynos") {
    llm = {"litert", "llamaserver"};
    nota = "NPU del movil por LiteRT-LM (dispatch del fabricante) y NNAPI";
  } else if (c == "nvidia") {
    llm = {"llamaserver", "onnx"};
    nota = "NVIDIA: llama.cpp CUDA para el texto; TensorRT (o CUDA) para la imagen";
  } else if (c == "amd") {
    llm = {"llamaserver", "onnx"};
    nota = "AMD: llama.cpp ROCm o Vulkan; imagen con ROCm o DirectML";
  } else if (c == "intel") {
    llm = {"onnx", "llamaserver"};
    nota = "Intel: OpenVINO (NPU o GPU) con ONNX Runtime; llama.cpp de respaldo";
  } else if (c == "apple") {
    llm = {"llamaserver"};
    nota = "Apple: llama.cpp con Metal; imagen con CoreML";
  } else {
    llm = {"llamaserver", "litert"};
    nota = "Solo CPU: llama.cpp y XNNPACK en los nucleos grandes";
  }
  auto dif = difusion::proveedores_para(perfil);
  return {{"chip", c}, {"llm", llm}, {"difusion", dif}, {"vision", dif}, {"nota", nota}};
}

}  // namespace nx::hal
