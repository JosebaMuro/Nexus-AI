// Capa de abstraccion de hardware (ver hal.h).
#include "hal/hal.h"

#include <chrono>
#include <thread>

#include "hal/cpu.h"
#include "hal/matriz.h"
#include "hal/ring.h"
#include "hal/zerocopy.h"

namespace nx::hal {

std::vector<std::string> sdk_compilados() {
  std::vector<std::string> v;
#ifdef NEXUS_SDK_CUDA
  v.push_back("cuda");
#endif
#ifdef NEXUS_SDK_VULKAN
  v.push_back("vulkan");
#endif
#ifdef NEXUS_SDK_OPENCL
  v.push_back("opencl");
#endif
#ifdef NEXUS_SDK_QNN
  v.push_back("qnn");
#endif
#ifdef NEXUS_SDK_NNAPI
  v.push_back("nnapi");
#endif
#ifdef NEXUS_SDK_DIRECTML
  v.push_back("directml");
#endif
#ifdef NEXUS_SDK_OPENVINO
  v.push_back("openvino");
#endif
  return v;
}

std::string artefacto() {
#if defined(NEXUS_CORE_SHARED) && defined(__ANDROID__)
  return "biblioteca compartida (.so dentro de la APK)";
#elif defined(NEXUS_CORE_SHARED) && defined(_WIN32)
  return "ejecutable + DLL";
#elif defined(NEXUS_CORE_SHARED)
  return "ejecutable + biblioteca compartida";
#elif defined(__ANDROID__)
  return "ejecutable dentro de la APK";
#elif defined(__linux__)
  return "ELF autocontenido";
#else
  return "ejecutable autocontenido";
#endif
}

namespace {
// espera activa corta y, si se alarga (un solo nucleo, equipo cargado), se cede el procesador
inline void espera(unsigned& giros) {
  if (++giros % 256 == 0) std::this_thread::yield();
}
}  // namespace

Latencia medir_latencia(long vueltas) {
  // ping-pong: el hilo A empuja un numero, B lo devuelve por el otro ring; la ida es la mitad de la vuelta
  SpscRing<long, 1024> ida, vuelta;
  std::atomic<bool> listo{false};
  std::thread eco([&] {
    listo.store(true, std::memory_order_release);
    long recibidos = 0;
    unsigned giros = 0;
    while (recibidos < vueltas) {
      if (auto v = ida.pop()) {
        while (!vuelta.push(*v)) std::this_thread::yield();
        ++recibidos;
      } else {
        espera(giros);
      }
    }
  });
  while (!listo.load(std::memory_order_acquire)) std::this_thread::yield();
  auto t0 = std::chrono::steady_clock::now();
  for (long i = 0; i < vueltas; ++i) {
    while (!ida.push(i)) std::this_thread::yield();
    unsigned giros = 0;
    while (!vuelta.pop()) espera(giros);
  }
  auto t1 = std::chrono::steady_clock::now();
  eco.join();
  Latencia l;
  l.vueltas = vueltas;
  l.vuelta_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)std::max(1L, vueltas);
  l.ida_ns = l.vuelta_ns / 2;
  return l;
}

json estado(const json& perfil) {
  json acc = json::object();
  if (perfil.is_object())
    for (const char* k : {"accelerator", "device", "gpu_layers", "backend"})
      if (perfil.contains(k)) acc[k] = perfil[k];
  return {{"capa", "abstraccion de hardware"},
          {"sdk_compilados", sdk_compilados()},
          {"artefacto", artefacto()},
          {"acelerador", acc},
          {"asignador", estado_asignador()},
          {"memorias", memorias_disponibles()},
          {"nucleos", estado_nucleos()},
          {"matriz", matriz(perfil)}};
}

}  // namespace nx::hal
