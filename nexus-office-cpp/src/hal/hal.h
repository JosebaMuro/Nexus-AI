// Capa de Abstraccion de Hardware (HAL): la capa inferior de las tres del runtime.
//
//   Orquestacion multiagente   agents/, tools/, office/, pipeline/ (orquestador)
//   Motor core multiplataforma llm/, memory/, rag/, media/, models/, server/, pipeline/ (percepcion y difusion)
//   Abstraccion de hardware    hal/ (asignador zero-copy, rings lock-free, SDK compilados) y hardware/ (deteccion,
//                              perfil, selector CPU/GPU/NPU)
//
// Las capas de arriba no conocen ningun SDK de acelerador: piden memoria y colas a hal/ y el dispositivo al perfil de
// hardware/. Los SDK que se compilan dentro se eligen con las opciones NEXUS_SDK_* de CMake (ver CMakeLists.txt y
// CMakePresets.json); el resto se carga en tiempo de ejecucion (llama.cpp, sd.cpp, LiteRT-LM, ONNX GenAI, Genie).
#pragma once
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::hal {

// SDK de hardware activados al compilar (NEXUS_SDK_VULKAN, NEXUS_SDK_OPENCL, NEXUS_SDK_CUDA, NEXUS_SDK_QNN...)
std::vector<std::string> sdk_compilados();
// artefacto de esta compilacion: "biblioteca compartida", "ejecutable + DLL", "ELF autocontenido"...
std::string artefacto();

// paso de un mensaje entre dos hilos por los rings lock-free: media de ida (nanosegundos) en `vueltas` ida y vuelta
struct Latencia {
  double ida_ns = 0, vuelta_ns = 0;
  long vueltas = 0;
};
Latencia medir_latencia(long vueltas = 200000);

// estado de la capa: SDK, artefacto, asignador y acelerador elegido por el perfil
json estado(const json& perfil);

}  // namespace nx::hal
