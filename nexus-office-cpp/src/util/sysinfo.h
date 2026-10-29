// Datos del sistema que la version Python obtenia con psutil y platform.
#pragma once
#include <string>

namespace nx::sys {

struct Memory {
  double total = 0, used = 0, available = 0;   // bytes
  double percent = 0;
};

Memory memory();
double cpu_percent();             // uso total desde la llamada anterior (psutil.cpu_percent(None))
double process_rss();             // bytes del proceso actual
int cores_logical();
int cores_physical();
std::string os_name();            // "Windows" | "Linux" | "Darwin" (platform.system)
std::string os_version();
std::string arch();               // platform.machine: x86_64, AMD64, aarch64, arm64...
std::string hostname();
bool is_windows();
bool is_macos();
bool is_linux();
bool is_android();                // NEXUS_ANDROID=1 o compilado para Android
int pid();

}  // namespace nx::sys
