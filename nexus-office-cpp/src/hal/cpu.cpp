// Nucleos grandes y pequenos (ver cpu.h).
#include "hal/cpu.h"

#include <algorithm>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#else
#include <sched.h>
#endif

namespace nx::hal {

namespace {

Nucleos leer() {
  Nucleos n;
  n.logicos = (int)std::max(1u, std::thread::hardware_concurrency());
  n.fuente = "ninguna";
#if defined(__linux__)
  // frecuencia maxima de cada CPU: los grandes son los que llegan al menos al 85 % de la mas rapida
  std::vector<std::pair<int, long long>> f;
  for (int i = 0; i < n.logicos; ++i) {
    std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq");
    long long v = 0;
    if (in >> v && v > 0) f.push_back({i, v});
  }
  if ((int)f.size() == n.logicos) {
    long long mx = 0;
    for (auto& [i, v] : f) mx = std::max(mx, v);
    for (auto& [i, v] : f) (v * 100 >= mx * 85 ? n.grandes : n.pequenos).push_back(i);
    n.fuente = "cpufreq";
  }
#elif defined(_WIN32)
  // clase de eficiencia de cada CPU (Windows 10+): los grandes tienen la clase mas alta
  ULONG len = 0;
  GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
  if (len) {
    std::vector<char> buf(len);
    auto* p = (SYSTEM_CPU_SET_INFORMATION*)buf.data();
    if (GetSystemCpuSetInformation(p, len, &len, GetCurrentProcess(), 0)) {
      struct C {
        unsigned long id;
        int indice, clase;
      };
      std::vector<C> c;
      for (char* q = buf.data(); q < buf.data() + len;) {
        auto* s = (SYSTEM_CPU_SET_INFORMATION*)q;
        if (s->Type == CpuSetInformation)
          c.push_back({(unsigned long)s->CpuSet.Id, (int)s->CpuSet.LogicalProcessorIndex, (int)s->CpuSet.EfficiencyClass});
        q += s->Size;
      }
      int mx = 0;
      for (auto& x : c) mx = std::max(mx, x.clase);
      for (auto& x : c) {
        (x.clase == mx ? n.grandes : n.pequenos).push_back(x.indice);
        if (x.clase == mx) n.ids_grandes.push_back(x.id);
      }
      if (!c.empty()) n.fuente = "cpu-sets";
    }
  }
#elif defined(__APPLE__)
  int p = 0;
  size_t sz = sizeof(p);
  if (sysctlbyname("hw.perflevel0.logicalcpu", &p, &sz, nullptr, 0) == 0 && p > 0) {
    for (int i = 0; i < n.logicos; ++i) (i < p ? n.grandes : n.pequenos).push_back(i);
    n.fuente = "perflevel";
  }
#endif
  if (n.grandes.empty()) {
    n.pequenos.clear();
    for (int i = 0; i < n.logicos; ++i) n.grandes.push_back(i);
  }
  return n;
}

}  // namespace

const Nucleos& nucleos() {
  static const Nucleos n = leer();
  return n;
}

int hilos_calculo() { return std::max(1, (int)nucleos().grandes.size()); }

bool fijar_hilo_grandes() {
  const Nucleos& n = nucleos();
  if (n.pequenos.empty()) return false;   // todos iguales: que decida el planificador
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i : n.grandes) CPU_SET(i, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
#elif defined(_WIN32)
  std::vector<ULONG> ids(n.ids_grandes.begin(), n.ids_grandes.end());
  return SetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), (ULONG)ids.size()) != 0;
#elif defined(__APPLE__)
  // macOS no deja fijar nucleos: la clase de servicio alta lleva el hilo a los nucleos de rendimiento
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0;
#else
  return false;
#endif
}

std::string mascara_grandes() {
  const Nucleos& n = nucleos();
  if (n.pequenos.empty() || n.grandes.empty()) return "";
  int mx = *std::max_element(n.grandes.begin(), n.grandes.end());
  std::vector<int> nib((mx / 4) + 1, 0);
  for (int i : n.grandes) nib[i / 4] |= 1 << (i % 4);
  std::string s = "0x";
  for (int k = (int)nib.size() - 1; k >= 0; --k) s += "0123456789abcdef"[nib[k]];
  return s;
}

json estado_nucleos() {
  const Nucleos& n = nucleos();
  return {{"logicos", n.logicos}, {"grandes", n.grandes}, {"pequenos", n.pequenos}, {"fuente", n.fuente},
          {"hilos_calculo", hilos_calculo()}, {"mascara", mascara_grandes()}};
}

}  // namespace nx::hal
