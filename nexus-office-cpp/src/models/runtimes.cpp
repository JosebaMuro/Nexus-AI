#include "models/runtimes.h"

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

#include "util/archive.h"
#include "util/http.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::runtimes {

static const auto L = log::get("runtimes");

static std::vector<std::string> exe_names(const std::string& rt) {
  if (rt == "llama") return {"llama-server"};
  if (rt == "sd") return {"sd-cli", "sd"};
  if (rt == "piper") return {"piper"};
  return {rt};
}
static std::string repo(const std::string& rt) {
  return rt == "llama" ? "ggml-org/llama.cpp" : rt == "sd" ? "leejet/stable-diffusion.cpp" : "";
}

static std::string os_tag() { return sys::is_windows() ? "win" : sys::is_macos() ? "mac" : "linux"; }
static std::string arch_tag() {
  std::string a = lower(sys::arch());
  return a == "arm64" || a == "aarch64" ? "arm64" : "x64";
}

std::string asset_pattern(const std::string& runtime, const std::string& variant) {
  std::string o = os_tag(), a = arch_tag();
  if (runtime == "llama") {
    if (o == "win") {
      std::string v = variant == "rocm" ? R"(rocm-[\d.]+)" : variant == "openvino" ? R"(openvino-[\d.]+)"
                      : variant == "cuda" ? R"(cuda-12\.\d)" : re_escape(variant);
      return R"(^llama-b\d+-bin-win-)" + v + "-" + a + R"(\.zip$)";
    }
    if (o == "mac") return R"(^llama-b\d+-bin-macos-)" + a + R"(\.tar\.gz$)";
    std::string v = variant == "cpu" ? "" : variant == "rocm" ? R"(rocm-[\d.]+-)" : variant == "openvino" ? R"(openvino-[\d.]+-)"
                    : variant == "cuda" ? R"(cuda-12\.\d-)" : variant == "sycl" ? "sycl-fp16-" : variant + "-";
    return R"(^llama-b\d+-bin-ubuntu-)" + v + a + R"(\.tar\.gz$)";
  }
  if (runtime == "sd") {
    if (o == "win") {
      std::string v = variant == "cuda" ? "cuda12" : variant == "rocm" ? R"(rocm-[\d.]+)" : variant;
      return R"(^sd-.*-bin-win-)" + v + R"(-x64\.zip$)";
    }
    if (o == "mac") return R"(^sd-.*-bin-Darwin-.*-arm64\.zip$)";
    std::string v = variant == "cpu" ? "" : variant == "vulkan" ? "-vulkan" : variant == "rocm" ? R"(-rocm-[\d.]+)" : "-" + variant;
    return R"(^sd-.*-bin-Linux-.*-x86_64)" + v + R"(\.zip$)";
  }
  throw std::invalid_argument(runtime);
}

static std::string cudart_pattern(const std::string& runtime) {
  if (os_tag() != "win") return "";
  return runtime == "llama" ? R"(^cudart-llama-bin-win-cuda-12\.\d-x64\.zip$)" : R"(^cudart-sd-bin-win-cu12-x64\.zip$)";
}

std::vector<std::string> candidates(const json& profile, const std::string& runtime) {
  std::string acc = jstr(profile, "accelerator");
  if (os_tag() == "mac") return {"metal"};
  std::string id = jstr(profile, "id");
  auto parts = split(id, "-");
  std::string vendor = parts.size() > 1 ? parts[1] : "cpu";
  bool igpu = ends_with(id, "igpu");
  if (acc == "cuda") return {"cuda", "vulkan", "cpu"};
  if (vendor == "amd" && !igpu) return {"vulkan", "rocm", "cpu"};
  if (vendor == "intel" && !igpu)
    return runtime == "llama" ? std::vector<std::string>{"vulkan", "sycl", "cpu"} : std::vector<std::string>{"vulkan", "cpu"};
  if (vendor == "intel" && igpu && runtime == "llama") return {"cpu", "openvino", "vulkan"};
  return {"cpu"};
}

fs::path bundled_dir() { return paths::root() / "bundled" / "runtimes"; }

static bool has_avx2() {
  std::string a = lower(sys::arch());
  if (a != "x86_64" && a != "amd64") return true;
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  return __builtin_cpu_supports("avx2");
#elif defined(_WIN32)
  return IsProcessorFeaturePresent(40);
#else
  return true;
#endif
}

std::vector<std::string> bundled_variants(const std::string& runtime) {
  fs::path base = bundled_dir() / runtime;
  if (!is_dir(base)) return {};
  std::vector<std::string> have;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(base, ec))
    if (e.is_directory()) have.push_back(S(e.path().filename()));
  std::sort(have.begin(), have.end());
  std::vector<std::string> order = has_avx2() ? std::vector<std::string>{"cpu", "cpu-compat"} : std::vector<std::string>{"cpu-compat"};
  std::vector<std::string> out;
  for (auto& v : order)
    if (std::find(have.begin(), have.end(), v) != have.end()) out.push_back(v);
  for (auto& v : have)
    if (std::find(order.begin(), order.end(), v) == order.end() && !starts_with(v, "cpu")) out.push_back(v);
  return out;
}

std::map<std::string, std::string> lib_env(const fs::path& exe) {
  std::map<std::string, std::string> e;
  if (sys::is_linux()) {
    std::string cur = env("LD_LIBRARY_PATH");
    e["LD_LIBRARY_PATH"] = S(exe.parent_path()) + (cur.empty() ? "" : ":" + cur);
  } else if (sys::is_macos()) {
    // piper de macOS enlaza @rpath/libespeak-ng.1.dylib sin rpath a su carpeta: dyld la busca aqui por su nombre
    std::string cur = env("DYLD_FALLBACK_LIBRARY_PATH");
    e["DYLD_FALLBACK_LIBRARY_PATH"] = S(exe.parent_path()) + (cur.empty() ? ":/usr/local/lib:/usr/lib" : ":" + cur);
  }
  return e;
}

std::optional<fs::path> find_in(const fs::path& d, const std::string& runtime) {
  if (!is_dir(d)) return std::nullopt;
  std::string suffix = sys::is_windows() ? ".exe" : "";
  for (auto& name : exe_names(runtime))
    for (auto& p : glob(d, name + suffix, true))
      if (is_file(p)) return p;
  return std::nullopt;
}

static std::mutex usable_mu;
static std::map<std::string, bool> usable;

std::optional<fs::path> bundled_exe(const std::string& runtime, const std::string& variant) {
  auto p = find_in(bundled_dir() / runtime / variant, runtime);
  if (!p) return std::nullopt;
#ifndef _WIN32
  std::error_code ec;
  fs::permissions(*p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
  if (starts_with(variant, "cpu")) return p;
  std::lock_guard<std::mutex> lk(usable_mu);
  std::string key = S(*p);
  if (!usable.count(key)) {
    proc::Options o;
    o.cwd = p->parent_path();
    o.env = lib_env(*p);
    auto r = proc::run({key, "--version"}, 30, o);
    usable[key] = r.ok();
    if (!usable[key])
      L.info("Runtime ", runtime, " ", variant, " incluido pero no arranca en este equipo (sin driver de GPU): se usa otro");
  }
  return usable[key] ? p : std::nullopt;
}

RuntimeManager::RuntimeManager() : root_(paths::sub({"runtimes"})) {}

std::optional<fs::path> RuntimeManager::find_exe(const std::string& runtime, const std::string& variant) {
  return find_in(root_ / runtime / variant, runtime);
}

json RuntimeManager::installed() {
  json out = json::object();
  for (const char* rt : {"llama", "sd"}) {
    std::vector<std::string> v;
    fs::path base = root_ / rt;
    std::error_code ec;
    if (is_dir(base))
      for (auto& e : fs::directory_iterator(base, ec))
        if (e.is_directory() && find_exe(rt, S(e.path().filename()))) v.push_back(S(e.path().filename()));
    std::sort(v.begin(), v.end());
    out[rt] = v;
  }
  return out;
}

std::optional<Found> RuntimeManager::exe(const std::string& runtime, const json& profile, const std::string& preferred) {
  std::string forced = env(("NEXUS_RUNTIME_" + upper(runtime)).c_str());
  if (!forced.empty() && is_file(P(forced))) return Found{P(forced), "embedded"};
  std::vector<std::string> order;
  if (!preferred.empty()) order.push_back(preferred);
  for (auto& c : candidates(profile, runtime)) order.push_back(c);
  for (auto& v : installed().value(runtime, json::array())) order.push_back(v.get<std::string>());
  for (auto& v : order) {
    if (auto p = find_exe(runtime, v)) return Found{*p, v};
    if (!starts_with(v, "cpu"))
      if (auto p = bundled_exe(runtime, v)) return Found{*p, v};
  }
  for (auto& v : bundled_variants(runtime))
    if (starts_with(v, "cpu"))
      if (auto p = bundled_exe(runtime, v)) return Found{*p, "cpu"};
  for (auto& name : exe_names(runtime)) {
    if (name == "sd") continue;
    std::string f = proc::which(name);
    if (!f.empty()) return Found{P(f), "system"};
  }
  return std::nullopt;
}

void RuntimeManager::set_progress(const std::string& key, json v) {
  std::lock_guard<std::mutex> lk(mu_);
  progress_[key] = std::move(v);
}

json RuntimeManager::progress() {
  std::lock_guard<std::mutex> lk(mu_);
  return progress_;
}

fs::path RuntimeManager::install(const std::string& runtime, const std::string& variant) {
  std::string key = runtime + ":" + variant;
  set_progress(key, {{"status", "buscando"}, {"done", 0}, {"total", 0}});
  try {
    json releases = http::get_json("https://api.github.com/repos/" + repo(runtime) + "/releases", {}, {{"per_page", "10"}});
    Rx pat(asset_pattern(runtime, variant));
    json rel = json::object();
    std::vector<json> assets;
    for (auto& r : releases) {
      rel = r;
      assets.clear();
      for (auto& a : r.value("assets", json::array()))
        if (pat.test(jstr(a, "name"))) assets.push_back(a);
      if (!assets.empty()) break;
    }
    if (assets.empty())
      throw std::runtime_error("No hay binario '" + variant + "' para " + os_tag() + "-" + arch_tag() +
                               " en las ultimas releases");
    std::vector<json> wanted = {assets[0]};
    std::string cp = variant == "cuda" ? cudart_pattern(runtime) : "";
    if (!cp.empty()) {
      Rx c(cp);
      for (auto& a : rel["assets"])
        if (c.test(jstr(a, "name"))) {
          wanted.push_back(a);
          break;
        }
    }
    fs::path dest = root_ / runtime / variant;
    fs::path tmp = dest;
    tmp += ".tmp";
    rm_rf(tmp);
    mkdirs(tmp);
    json names = json::array();
    for (auto& a : wanted) {
      std::string name = jstr(a, "name");
      names.push_back(name);
      fs::path arc = paths::sub({"cache"}) / name;
      set_progress(key, {{"status", "descargando " + name}, {"done", 0}, {"total", a.value("size", json(0))}});
      http::download(jstr(a, "browser_download_url"), arc, {}, [&](uint64_t d, uint64_t t) {
        std::lock_guard<std::mutex> lk(mu_);
        progress_[key]["done"] = d;
        progress_[key]["total"] = t;
      });
      {
        std::lock_guard<std::mutex> lk(mu_);
        progress_[key]["status"] = "extrayendo";
      }
      if (!archive::extract(arc, tmp)) throw std::runtime_error("no se pudo extraer " + name);
      remove_file(arc);
    }
    rm_rf(dest);
    fs::rename(tmp, dest);
    write_file(dest / "nexus_runtime.json", jdump({{"tag", rel.value("tag_name", json())}, {"assets", names}}));
    auto exe = find_exe(runtime, variant);
    if (!exe) throw std::runtime_error("Ejecutable no encontrado tras extraer");
#ifndef _WIN32
    std::error_code ec;
    for (auto& f : fs::directory_iterator(exe->parent_path(), ec))
      fs::permissions(f.path(), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                      fs::perm_options::add, ec);
#endif
    set_progress(key, {{"status", "instalado"}, {"done", 1}, {"total", 1}, {"tag", rel.value("tag_name", json())}});
    L.info("Runtime ", runtime, " (", variant, ") instalado: ", S(*exe));
    return *exe;
  } catch (const std::exception& e) {
    set_progress(key, {{"status", "error"}, {"error", e.what()}});
    throw;
  }
}

}  // namespace nx::runtimes
