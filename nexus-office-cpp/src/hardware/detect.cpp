// Deteccion de hardware multiplataforma (nexus/hardware/detect.py). Cada sonda va protegida: si falla, se omite.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "hardware/android_accel.h"
#include "hardware/hardware.h"
#include "util/http.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace nx::hw {

static const auto L = log::get("hardware");

static const std::vector<std::pair<std::string, std::string>> VENDOR_IDS = {
    {"10de", "nvidia"}, {"1002", "amd"}, {"1022", "amd"}, {"8086", "intel"},
    {"5143", "qualcomm"}, {"qcom", "qualcomm"}, {"106b", "apple"}};

static std::string vendor_id(const std::string& id) {
  for (auto& [k, v] : VENDOR_IDS)
    if (k == id) return v;
  return "unknown";
}

static const Rx& npu_pattern() {
  static const Rx r(R"(\b(NPU|AI Boost|Ryzen\s*AI|IPU|Hexagon|Neural Process\w*|XDNA)\b)", Rx::I);
  return r;
}

json HardwareInfo::to_json() const {
  json g = json::array(), n = json::array();
  for (auto& x : gpus)
    g.push_back({{"vendor", x.vendor}, {"name", x.name}, {"vram_gb", x.vram_gb}, {"integrated", x.integrated},
                 {"driver", x.driver}, {"compute_capability", x.compute_capability}, {"source", x.source}});
  for (auto& x : npus)
    n.push_back({{"vendor", x.vendor}, {"name", x.name}, {"runtime_hint", x.runtime_hint}, {"source", x.source}});
  return {{"os", os},         {"os_version", os_version},
          {"arch", arch},     {"cpu_name", cpu_name},
          {"cores_physical", cores_physical}, {"cores_logical", cores_logical},
          {"cpu_flags", cpu_flags}, {"ram_gb", ram_gb},
          {"disk_free_gb", disk_free_gb}, {"gpus", g},
          {"npus", n},        {"runtimes", runtimes},
          {"python", python}};
}

HardwareInfo HardwareInfo::from_json(const json& j) {
  HardwareInfo h;
  h.os = jstr(j, "os");
  h.os_version = jstr(j, "os_version");
  h.arch = jstr(j, "arch");
  h.cpu_name = jstr(j, "cpu_name");
  h.cores_physical = jget<int>(j, "cores_physical", 1);
  h.cores_logical = jget<int>(j, "cores_logical", 1);
  h.cpu_flags = jget<std::vector<std::string>>(j, "cpu_flags", {});
  h.ram_gb = jnum(j.value("ram_gb", json(0)));
  h.disk_free_gb = jnum(j.value("disk_free_gb", json(0)));
  for (auto& g : j.value("gpus", json::array()))
    h.gpus.push_back({jstr(g, "vendor"), jstr(g, "name"), jnum(g.value("vram_gb", json(0))),
                      jget<bool>(g, "integrated", false), jstr(g, "driver"), jstr(g, "compute_capability"),
                      jstr(g, "source")});
  for (auto& n : j.value("npus", json::array()))
    h.npus.push_back({jstr(n, "vendor"), jstr(n, "name"), jstr(n, "runtime_hint"), jstr(n, "source")});
  h.runtimes = j.value("runtimes", json::object());
  h.python = jstr(j, "python", h.python);
  return h;
}

static std::string run(const std::vector<std::string>& cmd, double timeout = 15) {
  auto r = proc::run(cmd, timeout);
  return r.started && !r.timed_out && r.code == 0 ? valid_utf8(r.out) : "";
}

std::string vendor_from_text(const std::string& text) {
  std::string t = lower(text);
  static const Rx ven(R"(ven_([0-9a-z]{4}))");
  if (auto m = ven.search(t)) {
    std::string v = vendor_id(m->str(1));
    if (v != "unknown") return v;
  }
  static const std::vector<std::pair<std::string, std::string>> keys = {
      {"nvidia", "nvidia"}, {"geforce", "nvidia"}, {"quadro", "nvidia"}, {"radeon", "amd"},
      {"amd", "amd"},       {"intel", "intel"},    {"arc", "intel"},     {"adreno", "qualcomm"},
      {"qualcomm", "qualcomm"}, {"apple", "apple"}};
  for (auto& [k, v] : keys)
    if (contains(t, k)) return v;
  return "unknown";
}

bool is_integrated(const std::string& vendor, const std::string& name) {
  std::string n = lower(name);
  if (vendor == "nvidia") return false;
  if (vendor == "intel") {
    static const Rx r(R"(arc.*\b[ab]\d{3})");
    return !r.test(n);
  }
  if (vendor == "amd") {
    static const Rx d(R"(\brx\b|radeon pro|instinct|\bw\d{4})");
    static const Rx i(R"(radeon\(tm\) graphics|vega \d+|radeon \d{3}m|radeon graphics)");
    if (d.test(n)) return false;
    return i.test(n) || !contains(n, "rx");
  }
  return vendor == "qualcomm" || vendor == "apple";
}

// ---------------------------------------------------------------- CPU
static std::vector<std::string> cpu_flags() {
  std::vector<std::string> flags;
  std::string machine = lower(sys::arch());
  if (machine == "arm64" || machine == "aarch64") return {"neon"};
#ifdef _WIN32
  const std::pair<const char*, DWORD> feats[] = {{"sse2", 10}, {"sse3", 13}, {"avx", 39}, {"avx2", 40}, {"avx512f", 41}};
  for (auto& [n, c] : feats)
    if (IsProcessorFeaturePresent(c)) flags.push_back(n);
#else
  if (is_file("/proc/cpuinfo")) {
    std::string txt = read_text("/proc/cpuinfo");
    static const Rx r(R"(^flags\s*:\s*(.*)$)", Rx::M);
    std::vector<std::string> have;
    if (auto m = r.search(txt)) have = split_ws(m->str(1));
    for (const char* f : {"sse2", "sse3", "avx", "avx2", "fma", "f16c", "avx512f", "avx512_vnni", "amx_tile"})
      if (std::find(have.begin(), have.end(), f) != have.end()) flags.push_back(f);
  } else if (sys::is_macos()) {
    std::string out = lower(run({"sysctl", "-n", "machdep.cpu.features", "machdep.cpu.leaf7_features"}));
    for (const char* f : {"sse2", "sse3", "avx1.0", "avx2", "avx512f"})
      if (contains(out, f)) flags.push_back(f);
  }
#endif
  return flags;
}

static std::string cpu_name() {
  if (sys::is_macos()) {
    std::string s = trim(run({"sysctl", "-n", "machdep.cpu.brand_string"}));
    return s.empty() ? sys::arch() : s;
  }
  if (is_file("/proc/cpuinfo")) {
    std::string txt = read_text("/proc/cpuinfo");
    static const Rx mn(R"(^model name\s*:\s*(.*)$)", Rx::M);
    if (auto m = mn.search(txt)) return trim(m->str(1));
    std::string lscpu = proc::which("lscpu");
    if (!lscpu.empty()) {
      static const Rx r(R"(^Model name:\s*(.+)$)", Rx::M);
      std::string out = run({lscpu});
      if (auto m = r.search(out)) {
        std::string v = trim(m->str(1));
        if (v != "-") return v;
      }
    }
    for (const char* f : {"/sys/firmware/devicetree/base/model", "/proc/device-tree/model"}) {
      std::string model = trim(read_text(P(f)), std::string("\0 \n", 3));
      if (!model.empty()) return model;
    }
    static const Rx hw(R"(^Hardware\s*:\s*(.*)$)", Rx::M);
    if (auto m = hw.search(txt)) return trim(m->str(1));
  }
#ifdef _WIN32
  HKEY k;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) ==
      ERROR_SUCCESS) {
    wchar_t buf[256];
    DWORD sz = sizeof buf;
    if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
      RegCloseKey(k);
      return trim(S(fs::path(std::wstring(buf))));
    }
    RegCloseKey(k);
  }
#endif
  return sys::arch();
}

static std::string npu_runtime(const std::string& vendor) {
  if (vendor == "intel") return "openvino";
  if (vendor == "amd") return "onnxruntime-vitisai (Ryzen AI SDK)";
  if (vendor == "qualcomm") return "onnxruntime-qnn";
  if (vendor == "apple") return "coreml";
  return "";
}

// ---------------------------------------------------------------- Windows
#ifdef _WIN32
static const char* WIN_PS = R"PS(
$v = @(Get-CimInstance Win32_VideoController | Select-Object Name,AdapterRAM,DriverVersion,PNPDeviceID)
$p = @(Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -eq 'ComputeAccelerator' -or
       $_.Name -match '(?i)\b(NPU|AI Boost|Ryzen\s*AI|IPU|Hexagon|Neural Process\w*|XDNA)\b' } |
       Select-Object Name,PNPClass,PNPDeviceID,Manufacturer,Status)
$c = @(Get-CimInstance Win32_Processor | Select-Object Name)
@{video=$v; npu=$p; cpu=$c} | ConvertTo-Json -Depth 3 -Compress
)PS";

static std::map<std::string, double> win_registry_vram() {
  std::map<std::string, double> out;
  HKEY root;
  const wchar_t* base = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
  for (DWORD i = 0; i < 64; ++i) {
    wchar_t sub[256];
    DWORD sl = 256;
    if (RegEnumKeyExW(root, i, sub, &sl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
    wchar_t desc[512];
    DWORD dl = sizeof desc;
    if (RegQueryValueExW(k, L"DriverDesc", nullptr, nullptr, (LPBYTE)desc, &dl) == ERROR_SUCCESS) {
      unsigned long long size = 0;
      for (const wchar_t* val : {L"HardwareInformation.qwMemorySize", L"HardwareInformation.MemorySize"}) {
        unsigned char raw[16] = {0};
        DWORD rl = sizeof raw, type = 0;
        if (RegQueryValueExW(k, val, nullptr, &type, raw, &rl) == ERROR_SUCCESS) {
          std::memcpy(&size, raw, std::min<DWORD>(rl, 8));
          if (size) break;
        }
      }
      if (size) out[S(fs::path(std::wstring(desc)))] = round_to(size / 1073741824.0, 2);
    }
    RegCloseKey(k);
  }
  RegCloseKey(root);
  return out;
}

static json as_list(const json& j) {
  if (j.is_array()) return j;
  if (j.is_object()) return json::array({j});
  return json::array();
}

static void detect_windows(HardwareInfo& info) {
  std::string raw = run({"powershell", "-NoProfile", "-NonInteractive", "-Command", WIN_PS}, 40);
  json data = trim(raw).empty() ? json::object() : jparse(raw);
  if (!data.is_object()) data = json::object();
  auto cpus = as_list(data.value("cpu", json()));
  if (!cpus.empty()) {
    std::string n = trim(jstr(cpus[0], "Name"));
    if (!n.empty()) info.cpu_name = n;
  }
  auto reg = win_registry_vram();
  for (auto& v : as_list(data.value("video", json()))) {
    std::string name = trim(jstr(v, "Name"));
    std::string vendor = vendor_from_text(jstr(v, "PNPDeviceID") + " " + name);
    std::string ln = lower(name);
    if (contains(ln, "basic display") || contains(ln, "remote") || vendor == "unknown") continue;
    double vram = reg.count(name) ? reg[name] : round_to(jnum(v.value("AdapterRAM", json(0))) / 1073741824.0, 2);
    info.gpus.push_back({vendor, name, vram, is_integrated(vendor, name), jstr(v, "DriverVersion"), "", "wmi"});
  }
  for (auto& p : as_list(data.value("npu", json()))) {
    std::string name = trim(jstr(p, "Name"));
    if (!(jstr(p, "PNPClass") == "ComputeAccelerator" || npu_pattern().test(name))) continue;
    std::string vendor = vendor_from_text(jstr(p, "PNPDeviceID") + " " + jstr(p, "Manufacturer") + " " + name);
    info.npus.push_back({vendor, name, npu_runtime(vendor), "pnp"});
  }
}
#endif

// ---------------------------------------------------------------- Linux / macOS
static std::string which_extra(const std::string& tool, std::vector<std::string> extra = {}) {
  std::string exe = proc::which(tool);
  if (!exe.empty()) return exe;
  std::vector<std::string> dirs = {"/usr/sbin", "/sbin", "/usr/local/bin", "/usr/bin"};
  dirs.insert(dirs.end(), extra.begin(), extra.end());
  for (auto& d : dirs) {
    fs::path c = P(d) / tool;
    if (is_file(c)) return S(c);
  }
  return "";
}

std::string find_nvidia_smi() {
#ifdef _WIN32
  std::string exe = proc::which("nvidia-smi");
  if (exe.empty()) {
    fs::path c = P(env("SystemRoot", "C:\\Windows")) / "System32" / "nvidia-smi.exe";
    if (is_file(c)) exe = S(c);
  }
  return exe;
#else
  return which_extra("nvidia-smi", {"/usr/lib/wsl/lib", "/opt/nvidia/bin"});
#endif
}

static std::map<std::string, std::string> lspci_names() {
  std::map<std::string, std::string> names;
  std::string exe = which_extra("lspci");
  if (exe.empty()) return names;
  static const Rx tok(R"("[^"]*"|\S+)");
  static const Rx cls(R"(VGA|3D|Display)");
  for (auto& line : splitlines(run({exe, "-mm", "-D"}))) {
    std::vector<std::string> parts;
    for (auto& t : tok.findall(line)) parts.push_back(trim(t, "\""));
    if (parts.size() >= 4 && cls.test(parts[1])) names[lower(parts[0])] = parts[2] + " " + parts[3];
  }
  return names;
}

static bool amd_integrated(const std::string& name, double vram, bool named) {
  static const Rx apu(
      R"(renoir|cezanne|lucienne|barcelo|rembrandt|phoenix|hawk point|strix|krackan|raphael|granite ridge|mendocino|van gogh|picasso|raven|dali|pollock|carrizo|stoney|kaveri|radeon(\(tm\))? graphics|radeon \d{3}m|vega \d+ mobile|\bvega \d\b)",
      Rx::I);
  static const Rx dgpu(R"(\brx\b|radeon pro|instinct|firepro|\br[579]\b|\bw\d{4}\b|\bmi\d{2,3}\b)", Rx::I);
  if (named && dgpu.test(name)) return false;
  if (named && apu.test(name)) return true;
  return vram < 2.1;
}

static double largest_bar_gb(const fs::path& dev) {
  unsigned long long best = 0;
  for (auto& line : splitlines(read_text(dev / "resource"))) {
    auto p = split_ws(line);
    if (p.size() < 2) continue;
    unsigned long long a = std::strtoull(p[0].c_str(), nullptr, 16), b = std::strtoull(p[1].c_str(), nullptr, 16);
    if (b > a) best = std::max(best, b - a + 1);
  }
  return best >= (1ULL << 30) ? round_to(best / 1073741824.0, 2) : 0.0;
}

static void detect_linux(HardwareInfo& info) {
  fs::path root = "/sys";
  auto names = lspci_names();
  std::set<std::string> seen;
  std::vector<std::pair<int, fs::path>> cards;
  static const Rx card(R"(card\d+)");
  for (auto& c : glob(root / "class" / "drm", "card*"))
    if (std::string fn = S(c.filename()); card.fullmatch(fn)) cards.push_back({std::atoi(S(c.filename()).c_str() + 4), c});
  std::sort(cards.begin(), cards.end());
  for (auto& [_, c] : cards) {
    fs::path dev = c / "device";
    auto vraw = read_file(dev / "vendor");
    if (!vraw) continue;
    std::string vid = replace_all(lower(trim(*vraw)), "0x", "");
    std::error_code ec;
    std::string slot = lower(S(fs::canonical(dev, ec).filename()));
    if (ec) slot = S(c.filename());
    if (seen.count(slot)) continue;
    seen.insert(slot);
    std::string driver = S(fs::read_symlink(dev / "driver", ec).filename());
    if (ec) driver = "";
    std::string vendor = vendor_id(vid);
    if (vendor == "unknown") continue;
    double vram = 0;
    auto mem = read_file(dev / "mem_info_vram_total");
    if (mem) vram = round_to(std::atof(trim(*mem).c_str()) / 1073741824.0, 2);
    std::string name = names.count(slot) ? names[slot] : "";
    bool named = !name.empty();
    if (!named) name = upper(vendor) + " GPU";
    bool integ = vendor == "amd" ? amd_integrated(name, vram, named) : is_integrated(vendor, name);
    if (vendor == "intel" && !integ && !vram) vram = largest_bar_gb(dev);
    info.gpus.push_back({vendor, name, vram, integ, driver, "", "sysfs"});
  }
  for (auto& a : glob(root / "class" / "accel", "accel*")) {
    std::error_code ec;
    std::string drv = S(fs::read_symlink(a / "device" / "driver", ec).filename());
    if (ec) drv = "";
    std::string vendor = drv == "intel_vpu" ? "intel" : drv == "amdxdna" ? "amd" : drv == "qaic" ? "qualcomm" : "unknown";
    info.npus.push_back({vendor, cat(drv.empty() ? "accel" : drv, " (", S(a.filename()), ")"), npu_runtime(vendor),
                         "sysfs"});
  }
}

static void detect_macos(HardwareInfo& info) {
  if (sys::arch() == "arm64") {
    info.gpus.push_back({"apple", info.cpu_name + " GPU", round_to(info.ram_gb * 0.7, 1), true, "", "", "apple-silicon"});
    info.npus.push_back({"apple", "Apple Neural Engine", "coreml", "apple-silicon"});
  }
}

std::vector<GPU> parse_nvidia_smi(const std::string& out) {
  std::vector<GPU> found;
  for (auto& line : splitlines(trim(out))) {
    std::vector<std::string> parts;
    for (auto& p : split(line, ",")) parts.push_back(trim(p));
    if (parts.size() < 3 || parts[0].empty()) continue;
    double vram = 0;
    char* end = nullptr;
    double mb = std::strtod(parts[1].c_str(), &end);
    if (end && end != parts[1].c_str() && *end == 0) vram = round_to(mb / 1024, 2);
    found.push_back({"nvidia", parts[0], vram, false, parts[2], parts.size() > 3 ? parts[3] : "", "nvidia-smi"});
  }
  return found;
}

static void detect_nvidia(HardwareInfo& info) {
  std::string exe = find_nvidia_smi();
  if (exe.empty()) return;
  std::string fmt = "--format=csv,noheader,nounits";
  std::string out = run({exe, "--query-gpu=name,memory.total,driver_version,compute_cap", fmt});
  if (trim(out).empty()) out = run({exe, "--query-gpu=name,memory.total,driver_version", fmt});
  auto found = parse_nvidia_smi(out);
  if (!found.empty()) {
    std::erase_if(info.gpus, [](const GPU& g) { return g.vendor == "nvidia"; });
    info.gpus.insert(info.gpus.end(), found.begin(), found.end());
  }
}

static json probe_runtimes(const std::string& ollama_url) {
  // La edicion C++ no usa paquetes de Python: los runtimes son los binarios incluidos (llama-server, sd-cli, piper)
  json rt = json::object();
  rt["llama_cpp"] = false;
  rt["torch_directml"] = false;
  for (const char* m : {"diffusers", "transformers", "faster_whisper", "pyttsx3", "edge_tts", "piper", "vosk"})
    rt[m] = false;
  rt["cpp_edition"] = true;
  auto r = http::get(ollama_url + "/api/version", {}, 1.0);
  if (r.status == 200) {
    json j = r.json_body();
    rt["ollama"] = j.is_object() && j.contains("version") ? j["version"] : json(false);
  } else {
    rt["ollama"] = false;
  }
  return rt;
}

// App Android autonoma: SoC, GPU (la que llama.cpp puede usar por Vulkan/OpenCL) y NPU (Hexagon en llama.cpp en los
// Snapdragon recientes). Ver android_accel.
static void detect_android(HardwareInfo& info) {
  json acc = android_accel::detect();
  json s = acc["soc"], g = acc["gpu"], n = acc["npu"];
  if (!jstr(s, "name").empty()) info.cpu_name = jstr(s, "name");
  info.gpus.clear();   // en Android /sys/class/drm no es accesible: la GPU la da la sonda
  json use = g.value("use", json());
  if (!use.is_object()) use = json::object();
  bool on_npu = jstr(use, "backend") == "hexagon";
  if (!jstr(g, "name").empty()) {
    json dev = use;
    if (on_npu) {
      dev = json::object();
      for (auto& d : g.value("devices", json::array()))
        if (jstr(d, "name") == jstr(g, "name")) {
          dev = d;
          break;
        }
    }
    GPU gpu;
    gpu.vendor = jstr(g, "vendor");
    gpu.name = jstr(g, "name");
    gpu.vram_gb = round_to(jnum(dev.value("mem_mb", json(0))) / 1024, 2);
    gpu.integrated = true;
    gpu.driver = jstr(g, "gl_vendor");
    gpu.source = !dev.empty() ? "android-" + jstr(dev, "backend") : "android-egl";
    info.gpus.push_back(gpu);
  }
  if (n.is_object()) {
    NPU x;
    x.vendor = jstr(n, "vendor");
    x.name = jstr(n, "name");
    x.runtime_hint = jstr(n, "runtime_hint");
    x.source = truthy(n.value("usable", json(false))) ? "android-hexagon" : jstr(n, "source");
    info.npus.push_back(x);
  }
  info.runtimes["android_accel"] = {{"gpu_backend", jstr(use, "backend")}, {"gpu_device", jstr(use, "dev")},
                                    {"gpu_devices", g.value("devices", json::array())}, {"gpu_broken", jstr(g, "broken")},
                                    {"gpu_note", jstr(g, "note")}, {"gpu_binary", g.value("binary", false)},
                                    {"vulkan", jstr(g, "vulkan")}, {"soc", s}};
}

HardwareInfo detect(const std::string& ollama_url, bool probe) {
  HardwareInfo info;
  info.os = sys::os_name();
  info.os_version = sys::os_version();
  info.arch = sys::arch();
  try {
    info.cpu_name = cpu_name();
  } catch (const std::exception& e) {
    L.warning("Sonda cpu fallo: ", e.what());
    info.cpu_name = info.arch;
  }
  info.cores_logical = sys::cores_logical();
  info.cores_physical = sys::cores_physical();
  try {
    info.cpu_flags = cpu_flags();
  } catch (...) {
  }
  info.ram_gb = round_to(sys::memory().total / 1073741824.0, 2);
  info.disk_free_gb = round_to(disk_free_gb(paths::data_dir()), 1);
  auto safe = [&](const char* name, void (*fn)(HardwareInfo&)) {
    try {
      fn(info);
    } catch (const std::exception& e) {
      L.warning("Sonda ", name, " fallo: ", e.what());
    }
  };
#ifdef _WIN32
  safe("windows", detect_windows);
#endif
  if (sys::is_linux() && !sys::is_android()) safe("linux", detect_linux);
  if (sys::is_android()) safe("android", detect_android);
  if (sys::is_macos()) safe("macos", detect_macos);
  if (!sys::is_android()) safe("nvidia", detect_nvidia);
  if (probe) {
    json rt = probe_runtimes(ollama_url);
    for (auto it = info.runtimes.begin(); it != info.runtimes.end(); ++it) rt[it.key()] = it.value();
    info.runtimes = rt;
  }
  return info;
}

void save(const HardwareInfo& info) { write_file(paths::hardware_file(), jdump(info.to_json(), 2)); }

bool load_cached(HardwareInfo& out) {
  auto t = read_file(paths::hardware_file());
  if (!t) return false;
  json j = jparse(*t);
  if (!j.is_object()) return false;
  out = HardwareInfo::from_json(j);
  return true;
}

}  // namespace nx::hw
