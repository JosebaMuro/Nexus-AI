// Selector de inicio, descargas de la oficina y drivers de la GPU (misma salida que la version Python).
#include "core/system.h"

#include <algorithm>
#include <ctime>

#include "core/services.h"
#include "hardware/hardware.h"
#include "hardware/hw_profiles.h"
#include "llm/llm.h"
#include "models/runtimes.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/crypto.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::sys2 {

static auto L = log::get("drivers");

void open_url(const std::string& url) {
  if (sys::is_windows())
    proc::Process().start({"cmd", "/c", "start", "", url}, {.detached = true});
  else if (sys::is_macos())
    proc::Process().start({"open", url}, {.detached = true});
  else
    proc::Process().start({"xdg-open", url}, {.detached = true});
}

// ------------------------------------------------------------------ selector CPU/GPU/NPU
static std::pair<bool, std::string> engine_state(Services& s, const std::string& engine) {
  bool gguf = s.llm->resolver("gguf")("").has_value();
  if (engine == "llamaserver") {
    bool have = s.runtimes->exe("llama", s.profile, s.config.str("hardware.llama_variant", "")).has_value();
    if (!have) return {false, "falta el binario llama-server"};
    if (gguf) return {true, "incluido · CPU, GPU y NPU"};
    return {false, "incluido; falta un modelo GGUF (vista Modelos)"};
  }
  if (hw::android()) return {false, "no disponible en Android: usa llama-server"};
  // llama-cpp-python es Python: la edicion C++ usa siempre llama-server (el mismo llama.cpp en otro proceso)
  return {false, "la edición C++ usa llama-server (el mismo llama.cpp)"};
}

json accel_options(Services& s) {
  const hw::GPU* gpu = hw::best_gpu(s.hw);
  json engines = json::array();
  for (const char* e : {"auto", "llamaserver", "llamacpp"}) {
    if (std::string(e) == "auto") {
      engines.push_back({{"id", e}, {"label", hw::engine_label(e)}, {"available", true}, {"note", "el mejor que haya"}});
      continue;
    }
    auto [ok, note] = engine_state(s, e);
    engines.push_back({{"id", e}, {"label", hw::engine_label(e)}, {"available", ok}, {"note", note}});
  }
  json accels = json::array();
  for (const char* a : {"auto", "cpu", "gpu", "npu"}) {
    json p = hw::plan(s.hw, a);
    std::string id = a, label;
    if (id == "auto") label = "Automático";
    if (id == "cpu") label = "CPU";
    if (id == "gpu") label = "GPU" + (gpu ? " · " + gpu->name : std::string());
    if (id == "npu") label = "NPU" + (s.hw.npus.empty() ? std::string() : " · " + s.hw.npus[0].name);
    accels.push_back({{"id", id}, {"label", label}, {"available", p["ok"]}, {"note", p["note"]}});
  }
  json eff = json::object(), gpu_llama = json::object();
  std::string gpu_error;
  if (auto ls = std::dynamic_pointer_cast<llm::LlamaServerBackend>(s.llm->backend("llamaserver"))) {
    eff = ls->effective();
    gpu_llama = ls->gpu();
    gpu_error = ls->gpu_error();
  }
  json gpus = json::array(), npus = json::array();
  for (auto& g : s.hw.gpus) gpus.push_back(g.name);
  for (auto& n : s.hw.npus) npus.push_back(n.name);
  return {{"gpu_llama", gpu_llama},
          {"gpu_error", gpu_error},
          {"gpus", hw::profiles::gpus(s)},
          {"perfil_hw", s.config.get("hardware.perfil", "auto")},
          {"engine", s.config.str("llm.engine", "auto")},
          {"accel", s.config.str("hardware.accel", "auto")},
          {"ask", truthy(s.config.get("startup.ask_accel", true))},
          {"engines", engines},
          {"accels", accels},
          {"hardware", {{"cpu", s.hw.cpu_name}, {"cores", s.hw.cores_physical}, {"ram_gb", s.hw.ram_gb},
                        {"gpus", gpus}, {"npus", npus}, {"android", hw::android()}}},
          {"profile", {{"accelerator", s.profile["accelerator"]}, {"device", s.profile["device_name"]},
                       {"gpu_layers", s.profile["llm"]["gpu_layers"]}}},
          {"effective", eff.is_null() ? json::object() : eff},
          {"selected", s.llm->select().first}};
}

json accel_apply(Services& s, const std::string& engine, const std::string& accel, std::optional<bool> remember) {
  static const std::vector<std::string> E = {"auto", "llamaserver", "llamacpp"}, A = {"auto", "cpu", "gpu", "npu"};
  if (std::find(E.begin(), E.end(), engine) == E.end() || std::find(A.begin(), A.end(), accel) == A.end())
    throw std::invalid_argument("motor ('auto', 'llamaserver', 'llamacpp') y acelerador ('auto', 'cpu', 'gpu', 'npu')");
  std::vector<std::string> notes;
  if (engine == "llamacpp") notes.push_back("la edición C++ no incluye llama-cpp-python: se usa llama-server");
  auto [profile, p] = hw::build_for(s.hw, accel, accel == "auto" ? s.config.str("hardware.profile_override", "") : "");
  s.profile = profile;
  s.config.set("llm.engine", engine);
  s.config.set("hardware.accel", accel);
  s.config.set("hardware.llama_variant", p["variant"]);
  if (remember) s.config.set("startup.ask_accel", !*remember);
  s.llm->rebuild();
  if (!jstr(p, "note").empty()) notes.insert(notes.begin(), jstr(p, "note"));
  return {{"engine", engine},
          {"accel", accel},
          {"ok", truthy(p["ok"]) && notes.empty()},
          {"note", join(notes, "; ")},
          {"variant", p["variant"]},
          {"selected", s.llm->select().first},
          {"profile", {{"accelerator", profile["accelerator"]}, {"gpu_layers", profile["llm"]["gpu_layers"]}}}};
}

// ------------------------------------------------------------------ descargas de la oficina
static const std::vector<std::tuple<std::string, std::string, std::string>> PLATFORMS = {
    {"windows-x64", R"(windows-x64\.(exe|zip)$)", "Windows x64 (Intel/AMD)"},
    {"windows-arm64", R"(windows-arm64\.(exe|zip)$)", "Windows ARM64 (Snapdragon X)"},
    {"linux-x86_64", R"((linux-x86_64\.(run|tar\.gz)|_amd64\.deb)$)", "Linux x86_64"},
    {"linux-aarch64", R"((linux-aarch64\.(run|tar\.gz)|_arm64\.deb)$)", "Linux ARM64 / Raspberry Pi"},
    {"macos-arm64", R"(macos-arm64\.(tar\.gz|zip|dmg)$)", "macOS Apple Silicon"},
    {"macos-x86_64", R"(macos-x86_64\.(tar\.gz|zip|dmg)$)", "macOS Intel"},
    {"android", R"(\.apk$)", "Android"},
    {"codigo", R"(codigo-fuente\.zip$)", "Código fuente"},
    {"docs", R"(\.(pdf|txt|md)$)", "Documentación"},
};

static std::string host_platform() {
  std::string m = lower(sys::arch());
  bool arm = m == "arm64" || m == "aarch64" || m == "armv8l";
  if (sys::is_windows()) return arm ? "windows-arm64" : "windows-x64";
  if (sys::is_macos()) return arm ? "macos-arm64" : "macos-x86_64";
  if (hw::android()) return "android";
  return arm ? "linux-aarch64" : "linux-x86_64";
}

json downloads_listing() {
  fs::path base = paths::sub({"descargas"});
  std::vector<fs::path> all;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && !starts_with(S(it->path().filename()), ".")) all.push_back(it->path());
  }
  std::sort(all.begin(), all.end());
  json files = json::array();
  for (auto& f : all) {
    std::string name = S(f.filename()), pid = "otros", label = "Otros";
    for (auto& [id, pat, lbl] : PLATFORMS)
      if (Rx(pat, Rx::I).test(name)) {
        pid = id;
        label = lbl;
        break;
      }
    std::string rel = S(f.lexically_relative(base));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    files.push_back({{"name", name}, {"path", rel}, {"size", fsize(f)}, {"platform", pid}, {"label", label}});
  }
  std::string host = host_platform(), host_label = host;
  for (auto& [id, _, lbl] : PLATFORMS)
    if (id == host) host_label = lbl;
  return {{"folder", S(base)}, {"host", host}, {"host_label", host_label}, {"files", files}};
}

std::optional<fs::path> downloads_resolve(const std::string& rel) {
  fs::path base = resolve(paths::sub({"descargas"}));
  fs::path p = resolve(base / P(rel));
  if (is_file(p) && is_relative_to(p, base) && p != base) return p;
  return std::nullopt;
}

// ------------------------------------------------------------------ drivers de la GPU
static const int MAX_AGE_DAYS = 365;
static const std::map<std::string, std::string> PAGES = {
    {"nvidia", "https://www.nvidia.com/es-es/software/nvidia-app/"},
    {"amd", "https://www.amd.com/es/support/download/drivers.html"},
    {"intel", "https://www.intel.es/content/www/es/es/support/detect.html"}};

static std::string vendor_of(const std::string& text) {
  std::string t = lower(text);
  if (contains(t, "ven_10de") || contains(t, "nvidia")) return "nvidia";
  if (contains(t, "ven_1002") || contains(t, "amd") || contains(t, "radeon")) return "amd";
  if (contains(t, "ven_8086") || contains(t, "intel")) return "intel";
  return "otro";
}

static std::string run_out(const std::vector<std::string>& cmd, double timeout = 30) {
  auto r = proc::run(cmd, timeout, {.capture = true});
  return r.started ? r.out : "";
}

static json installed_drivers(Services& s) {
  json out = json::array();
  if (sys::is_windows()) {
    std::string raw = run_out({"powershell", "-NoProfile", "-Command",
                               "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,"
                               "@{n='Date';e={$_.DriverDate.ToString('yyyy-MM-dd')}},PNPDeviceID | ConvertTo-Json -Compress"});
    json data = trim(raw).empty() ? json::array() : jparse(raw);
    if (data.is_discarded()) data = json::array();
    if (!data.is_array()) data = json::array({data});
    for (auto& v : data) {
      std::string name = jstr(v, "Name");
      if (icontains(name, "basic display")) continue;
      out.push_back({{"name", name},
                     {"vendor", vendor_of(jstr(v, "PNPDeviceID") + " " + name)},
                     {"version", jstr(v, "DriverVersion")},
                     {"date", jstr(v, "Date")}});
    }
  } else if (sys::is_linux()) {
    std::string smi = hw::find_nvidia_smi();
    bool have_nvidia = false;
    if (!smi.empty())
      for (auto& line : splitlines(run_out({smi, "--query-gpu=name,driver_version", "--format=csv,noheader"}))) {
        auto parts = split(line, ",");
        if (parts.size() >= 2) {
          out.push_back({{"name", trim(parts[0])}, {"vendor", "nvidia"}, {"version", trim(parts[1])}, {"date", ""}});
          have_nvidia = true;
        }
      }
    std::string mesa_info;
    for (auto cmd : {std::vector<std::string>{"glxinfo", "-B"}, std::vector<std::string>{"vulkaninfo", "--summary"},
                     std::vector<std::string>{"eglinfo", "-B"}})
      if (!proc::which(cmd[0]).empty()) {
        std::string o = run_out(cmd);
        if (contains(o, "Mesa")) {
          mesa_info = o;
          break;
        }
      }
    static const Rx MESA(R"(Mesa (\d+\.\d+(?:\.\d+)?))");
    auto mesa = MESA.search(mesa_info);
    if (mesa) out.push_back({{"name", "Mesa (AMD/Intel)"}, {"vendor", "mesa"}, {"version", mesa->str(1)}, {"date", ""}});
    for (auto& g : s.hw.gpus) {   // GPUs que ni nvidia-smi ni Mesa han reportado
      if ((g.vendor == "nvidia" && have_nvidia) || ((g.vendor == "amd" || g.vendor == "intel") && mesa)) continue;
      std::string name = g.name.empty() ? upper(g.vendor) + " GPU" : g.name;
      out.push_back({{"name", name + " (" + (g.driver.empty() ? "sin driver" : g.driver) + ")"},
                     {"vendor", g.vendor},
                     {"version", ""},
                     {"date", ""}});
    }
  }
  return out;
}

static json driver_options(const std::string& vendor) {
  json opts = json::array();
  if (sys::is_windows()) {
    if (vendor == "intel" && !proc::which("winget").empty())
      opts.push_back({{"id", "intel-dsa"},
                      {"label", "Intel Driver & Support Assistant (oficial, via winget)"},
                      {"detail", "Instala el asistente oficial de Intel, que detecta y actualiza los drivers"}});
    if (PAGES.count(vendor))
      opts.push_back({{"id", vendor + "-web"}, {"label", "Pagina oficial de drivers de " + upper(vendor)}, {"detail", PAGES.at(vendor)}});
    opts.push_back({{"id", "windows-update"},
                    {"label", "Windows Update (actualizaciones opcionales de drivers)"},
                    {"detail", "Abre Configuracion > Windows Update > Actualizaciones opcionales"}});
  } else if (sys::is_linux() && !hw::android()) {
    if (!proc::which("ubuntu-drivers").empty() && vendor == "nvidia")
      opts.push_back({{"id", "ubuntu-drivers"},
                      {"label", "ubuntu-drivers install (driver NVIDIA recomendado)"},
                      {"detail", "pkexec ubuntu-drivers install"}});
    const std::vector<std::pair<std::string, std::string>> PMS = {
        {"apt-get", "pkexec sh -c 'apt-get update && apt-get install -y --only-upgrade mesa-vulkan-drivers libgl1-mesa-dri'"},
        {"dnf", "pkexec dnf upgrade -y 'mesa*'"},
        {"pacman", "pkexec pacman -Syu --noconfirm mesa vulkan-icd-loader"}};
    for (auto& [pm, cmd] : PMS)
      if (!proc::which(pm).empty() && (vendor == "amd" || vendor == "intel" || vendor == "mesa")) {
        opts.push_back({{"id", "mesa-" + pm}, {"label", "Actualizar Mesa/Vulkan (" + pm + ")"}, {"detail", cmd}});
        break;
      }
    if (PAGES.count(vendor))
      opts.push_back({{"id", vendor + "-web"}, {"label", "Pagina oficial de drivers de " + upper(vendor)}, {"detail", PAGES.at(vendor)}});
  } else if (sys::is_macos()) {
    opts.push_back({{"id", "macos-update"}, {"label", "Actualizacion de software de macOS"}, {"detail", "Los drivers de GPU llegan con macOS"}});
  } else {
    opts.push_back({{"id", "system"},
                    {"label", "Actualizacion del sistema"},
                    {"detail", "En Android los drivers de GPU llegan con las actualizaciones del sistema"}});
  }
  return opts;
}

static std::optional<std::pair<int, int>> nvidia_version(const std::string& v, bool windows) {
  std::string digits;
  for (char c : v)
    if (isdigit((unsigned char)c)) digits += c;
  if (windows && digits.size() >= 5) {
    std::string tail = digits.substr(digits.size() - 5);
    return std::make_pair(std::stoi(tail.substr(0, 3)), std::stoi(tail.substr(3)));
  }
  static const Rx R(R"((\d+)\.(\d+))");
  auto m = R.match(v);
  if (!m) return std::nullopt;
  return std::make_pair(std::stoi(m->str(1)), std::stoi(m->str(2)));
}

static json evaluate(json d) {
  bool windows = sys::is_windows();
  std::vector<std::string> reasons;
  std::string status = "ok";
  std::string date = jstr(d, "date");
  if (!date.empty()) {
    std::tm tm{};
    if (sscanf(date.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      tm.tm_hour = 12;
      double then = (double)std::mktime(&tm);
      int age = (int)((now() - then) / 86400);
      d["age_days"] = age;
      if (age > MAX_AGE_DAYS) {
        status = "actualizar";
        reasons.push_back("El driver tiene " + std::to_string(age / 30) +
                          " meses; los recientes mejoran rendimiento y compatibilidad");
      }
    }
  }
  std::string vendor = jstr(d, "vendor");
  if (vendor == "nvidia") {
    auto ver = nvidia_version(jstr(d, "version"), windows);
    std::pair<int, int> need = windows ? std::make_pair(551, 61) : std::make_pair(550, 54);
    if (ver && *ver < need) {
      status = "actualizar";
      reasons.push_back("CUDA 12.4 necesita driver " + std::to_string(need.first) + "." + std::to_string(need.second) +
                        " o superior (instalado " + std::to_string(ver->first) + "." + std::to_string(ver->second) + ")");
    }
  } else if (vendor == "intel" && windows) {
    auto parts = split(jstr(d, "version"), ".");
    std::string build = parts.empty() ? "" : parts.back();
    if (is_digit_str(build) && std::stol(build) < 5000) {
      status = "actualizar";
      reasons.push_back("Los drivers Intel anteriores a 31.0.101.5000 tienen problemas con Vulkan/OpenVINO");
    }
  }
  if (jstr(d, "version").empty()) status = "desconocido";
  if (status == "ok") reasons.push_back("Driver al dia para los runtimes de Nexus-Office");
  d["status"] = status;
  d["reasons"] = reasons;
  d["options"] = driver_options(vendor);
  return d;
}

json drivers_report(const std::string&, Services& s) {
  json gpus = json::array();
  bool needs = false;
  for (auto& d : installed_drivers(s)) {
    json e = evaluate(d);
    needs |= jstr(e, "status") == "actualizar";
    gpus.push_back(e);
  }
  return {{"gpus", gpus},
          {"needs_update", needs},
          {"note", "Las actualizaciones las confirma el usuario en el actualizador oficial (requieren permisos de administrador)."}};
}

json drivers_launch(const std::string& option_id) {
  L.info("Actualizacion de drivers solicitada por el usuario: ", option_id);
  if (option_id == "intel-dsa") {
    proc::Process().start({"cmd", "/c", "start", "Nexus-Office - actualizar drivers Intel", "cmd", "/k",
                           "winget install --id Intel.IntelDriverAndSupportAssistant -e --source winget"},
                          {.detached = true, .hide_window = false});
    return {{"launched", true}, {"how", "winget (consola visible)"}};
  }
  if (ends_with(option_id, "-web")) {
    auto v = split(option_id, "-")[0];
    if (!PAGES.count(v)) throw std::invalid_argument("opcion desconocida: " + option_id);
    open_url(PAGES.at(v));
    return {{"launched", true}, {"how", "navegador"}};
  }
  if (option_id == "windows-update") {
    proc::Process().start({"cmd", "/c", "start", "", "ms-settings:windowsupdate-optionalupdates"}, {.detached = true});
    return {{"launched", true}, {"how", "Configuracion de Windows"}};
  }
  if (option_id == "macos-update") {
    proc::Process().start({"open", "x-apple.systempreferences:com.apple.Software-Update-Settings.extension"}, {.detached = true});
    return {{"launched", true}, {"how", "Ajustes del sistema"}};
  }
  for (const char* vendor : {"nvidia", "amd", "mesa"})
    for (auto& o : driver_options(vendor))
      if (jstr(o, "id") == option_id && starts_with(jstr(o, "detail"), "pkexec")) {
        std::string term = proc::which("x-terminal-emulator");
        if (term.empty()) term = proc::which("gnome-terminal");
        if (term.empty()) term = proc::which("konsole");
        std::string cmd = jstr(o, "detail") + "; echo; read -p 'Pulsa Enter para cerrar'";
        if (!term.empty())
          proc::Process().start({term, "-e", "bash", "-c", cmd}, {.detached = true, .hide_window = false});
        else
          proc::Process().start({"bash", "-c", jstr(o, "detail")}, {.detached = true});
        return {{"launched", true}, {"how", "terminal (pkexec pide la contrasena)"}};
      }
  throw std::invalid_argument("opcion desconocida: " + option_id);
}

}  // namespace nx::sys2
