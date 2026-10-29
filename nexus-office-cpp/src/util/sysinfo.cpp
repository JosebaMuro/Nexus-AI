#include "util/sysinfo.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "util/fs.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace nx::sys {

bool is_windows() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}
bool is_macos() {
#ifdef __APPLE__
  return true;
#else
  return false;
#endif
}
bool is_linux() {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}
bool is_android() {
#ifdef __ANDROID__
  return true;
#else
  return env("NEXUS_ANDROID") == "1";
#endif
}

int pid() {
#ifdef _WIN32
  return (int)GetCurrentProcessId();
#else
  return (int)getpid();
#endif
}

Memory memory() {
  Memory m;
#ifdef _WIN32
  MEMORYSTATUSEX st{};
  st.dwLength = sizeof st;
  if (GlobalMemoryStatusEx(&st)) {
    m.total = (double)st.ullTotalPhys;
    m.available = (double)st.ullAvailPhys;
  }
#elif defined(__APPLE__)
  int64_t total = 0;
  size_t len = sizeof total;
  sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
  m.total = (double)total;
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
    double page = (double)sysconf(_SC_PAGESIZE);
    m.available = (vm.free_count + vm.inactive_count) * page;
  }
#else
  std::string t = read_text("/proc/meminfo");
  auto field = [&](const char* k) -> double {
    size_t p = t.find(k);
    if (p == std::string::npos) return -1;
    return std::atof(t.c_str() + p + std::strlen(k)) * 1024.0;
  };
  m.total = std::max(0.0, field("MemTotal:"));
  m.available = field("MemAvailable:");
  if (m.available < 0) m.available = std::max(0.0, field("MemFree:"));
#endif
  m.used = m.total - m.available;
  m.percent = m.total > 0 ? round_to(100.0 * (m.total - m.available) / m.total, 1) : 0;
  return m;
}

double cpu_percent() {
  static std::mutex mu;
  static unsigned long long last_idle = 0, last_total = 0;
  std::lock_guard<std::mutex> lk(mu);
  unsigned long long idle = 0, total = 0;
#ifdef _WIN32
  FILETIME i, k, u;
  if (!GetSystemTimes(&i, &k, &u)) return 0;
  auto v = [](FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
  idle = v(i);
  total = v(k) + v(u);   // el tiempo de kernel incluye el ocioso
#elif defined(__APPLE__)
  host_cpu_load_info_data_t cpu;
  mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &cnt) != KERN_SUCCESS) return 0;
  for (int s = 0; s < CPU_STATE_MAX; ++s) total += cpu.cpu_ticks[s];
  idle = cpu.cpu_ticks[CPU_STATE_IDLE];
#else
  std::string t = read_text("/proc/stat");
  if (!starts_with(t, "cpu ")) return 0;
  auto parts = split_ws(t.substr(0, t.find('\n')));
  for (size_t i = 1; i < parts.size(); ++i) {
    unsigned long long x = std::strtoull(parts[i].c_str(), nullptr, 10);
    if (i <= 8) total += x;   // user..steal (guest ya va incluido en user)
    if (i == 4 || i == 5) idle += x;
  }
#endif
  double pct = 0;
  if (last_total && total > last_total)
    pct = 100.0 * (1.0 - (double)(idle - last_idle) / (double)(total - last_total));
  last_idle = idle;
  last_total = total;
  return round_to(std::max(0.0, std::min(100.0, pct)), 1);
}

double process_rss() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return (double)pmc.WorkingSetSize;
  return 0;
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt) == KERN_SUCCESS)
    return (double)info.resident_size;
  return 0;
#else
  std::string t = read_text("/proc/self/statm");
  auto p = split_ws(t);
  if (p.size() < 2) return 0;
  return std::atof(p[1].c_str()) * (double)sysconf(_SC_PAGESIZE);
#endif
}

int cores_logical() {
  unsigned n = std::thread::hardware_concurrency();
  return n ? (int)n : 1;
}

int cores_physical() {
#ifdef _WIN32
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  std::string buf(len, '\0');
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(),
                                        &len))
    return cores_logical();
  int n = 0;
  for (DWORD off = 0; off < len;) {
    auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
    if (p->Relationship == RelationProcessorCore) ++n;
    off += p->Size;
  }
  return n ? n : cores_logical();
#elif defined(__APPLE__)
  int n = 0;
  size_t len = sizeof n;
  if (sysctlbyname("hw.physicalcpu", &n, &len, nullptr, 0) == 0 && n > 0) return n;
  return cores_logical();
#elif defined(__aarch64__) || defined(__arm__)
  // ARM (moviles, Snapdragon X, Raspberry Pi) no tiene SMT, y core_id se repite en cada grupo de nucleos (big.LITTLE):
  // contar pares (paquete, nucleo) daba 4 en un Snapdragon X de 12 nucleos
  return cores_logical();
#else
  // pares (paquete, nucleo) distintos en sysfs
  std::set<std::string> cores;
  for (int i = 0; i < 4096; ++i) {
    fs::path base = P(cat("/sys/devices/system/cpu/cpu", i, "/topology"));
    if (!is_dir(base)) {
      if (i > 0 && !is_dir(P(cat("/sys/devices/system/cpu/cpu", i)))) break;
      continue;
    }
    cores.insert(trim(read_text(base / "physical_package_id")) + ":" + trim(read_text(base / "core_id")));
  }
  return cores.empty() ? cores_logical() : (int)cores.size();
#endif
}

std::string os_name() {
#ifdef _WIN32
  return "Windows";
#elif defined(__APPLE__)
  return "Darwin";
#else
  return "Linux";
#endif
}

std::string os_version() {
#ifdef _WIN32
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  RTL_OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&v);
  return cat(v.dwMajorVersion, ".", v.dwMinorVersion, ".", v.dwBuildNumber);
#else
  struct utsname u{};
  if (uname(&u) != 0) return "";
  return u.version;
#endif
}

std::string arch() {
#ifdef _WIN32
  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "AMD64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "unknown";
  }
#else
  struct utsname u{};
  if (uname(&u) != 0) return "";
  return u.machine;
#endif
}

std::string hostname() {
#ifdef _WIN32
  wchar_t buf[256];
  DWORD n = 256;
  if (GetComputerNameW(buf, &n)) return S(fs::path(std::wstring(buf, n)));
  return "equipo";
#else
  char buf[256] = {0};
  gethostname(buf, sizeof buf - 1);
  return buf;
#endif
}

}  // namespace nx::sys
