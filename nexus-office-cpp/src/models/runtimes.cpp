#include "models/runtimes.h"

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

#include "util/archive.h"
#include "util/crypto.h"
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
    // nombres de las Releases oficiales de ggml-org/llama.cpp (2026): llama-bNNNN-bin-<so>-<variante>-<arq>
    auto v_of = [&](const std::string& v) -> std::string {
      if (v == "rocm") return R"(rocm-[\d.]+)";
      if (v == "openvino") return R"(openvino-[\d.]+)";
      if (v == "cuda") return R"(cuda-12\.\d)";
      if (v == "cuda13") return R"(cuda-13\.\d)";
      if (v == "sycl") return o == "win" ? "sycl" : "sycl-fp16";
      if (v == "cpu-compat") return "cpu";
      return re_escape(v);
    };
    if (o == "win") return R"(^llama-b\d+-bin-win-)" + v_of(variant) + "-" + a + R"(\.zip$)";
    if (o == "mac") return R"(^llama-b\d+-bin-macos-)" + a + R"(\.tar\.gz$)";
    // Snapdragon en Linux ARM (CPU, GPU Adreno por OpenCL y NPU Hexagon en el mismo paquete)
    if (variant == "snapdragon") return R"(^llama-b\d+-bin-linux-arm64-snapdragon\.tar\.gz$)";
    std::string v = variant == "cpu" || variant == "cpu-compat" ? "" : v_of(variant) + "-";
    return R"(^llama-b\d+-bin-ubuntu-)" + v + a + R"(\.tar\.gz$)";
  }
  if (runtime == "sd") {
    // leejet/stable-diffusion.cpp: sd-master-<commit>-bin-win-<build>-x64.zip, ...-Linux-Ubuntu-24.04-x86_64[-vulkan]
    if (o == "win") {
      std::string v = variant == "cuda" ? "cuda12" : variant == "rocm" ? R"(rocm-[\d.]+)" : variant == "cpu" ? "avx2"
                      : variant == "cpu-compat" ? "noavx" : variant;
      return R"(^sd-.*-bin-win-)" + v + R"(-x64\.zip$)";
    }
    if (o == "mac") return R"(^sd-.*-bin-Darwin-.*-arm64\.zip$)";
    std::string arch = a == "arm64" ? "(aarch64|arm64)" : "x86_64";
    std::string v = variant == "cpu" || variant == "cpu-compat" ? "" : variant == "vulkan" ? "-vulkan"
                    : variant == "rocm" ? R"(-rocm-[\d.]+)" : "-" + variant;
    return R"(^sd-.*-bin-Linux-.*-)" + arch + v + R"(\.zip$)";
  }
  throw std::invalid_argument(runtime);
}

static std::string cudart_pattern(const std::string& runtime, const std::string& variant) {
  std::string ver = variant == "cuda13" ? R"(13\.\d)" : R"(12\.\d)";
  if (os_tag() == "win")
    return runtime == "llama" ? R"(^cudart-llama-bin-win-cuda-)" + ver + "-" + arch_tag() + R"(\.zip$)"
                              : R"(^cudart-sd-bin-win-cu12-x64\.zip$)";
  if (os_tag() == "linux" && runtime == "llama")
    return R"(^cudart-llama-b\d+-bin-ubuntu-cuda-)" + ver + "-" + arch_tag() + R"(\.tar\.gz$)";
  return "";
}

std::vector<std::string> candidates(const json& profile, const std::string& runtime) {
  std::string acc = jstr(profile, "accelerator");
  std::string a = arch_tag();
  if (os_tag() == "mac") return a == "arm64" ? std::vector<std::string>{"metal", "cpu"} : std::vector<std::string>{"cpu"};
  std::string id = jstr(profile, "id");
  auto parts = split(id, "-");
  std::string vendor = parts.size() > 1 ? parts[1] : "cpu";
  bool igpu = ends_with(id, "igpu");
  bool llama = runtime == "llama";
  if (acc == "cuda") {
    // Blackwell (RTX 50, capacidad 12.x) y posteriores: CUDA 13; las demas, CUDA 12 (drivers desde 2023)
    double cc = std::atof(jstr(profile, "compute_capability").c_str());
    if (llama && cc >= 10) return {"cuda13", "cuda", "vulkan", "cpu"};
    return {"cuda", "vulkan", "cpu"};
  }
  if (vendor == "amd" && !igpu)
    return os_tag() == "linux" ? std::vector<std::string>{"rocm", "vulkan", "cpu"} : std::vector<std::string>{"vulkan", "rocm", "cpu"};
  // APU de AMD (Radeon 780M/890M, Strix Halo): Vulkan con la memoria compartida va mejor que la CPU
  if (vendor == "amd" && igpu) return acc == "vulkan" ? std::vector<std::string>{"vulkan", "cpu"} : std::vector<std::string>{"cpu", "vulkan"};
  if (vendor == "intel" && !igpu) return llama ? std::vector<std::string>{"vulkan", "sycl", "cpu"} : std::vector<std::string>{"vulkan", "cpu"};
  if (vendor == "intel" && igpu && llama) return {"cpu", "openvino", "vulkan"};
  if (vendor == "qualcomm") {
    // Snapdragon X (Windows ARM): GPU Adreno por OpenCL; Linux ARM: paquete Snapdragon (CPU, Adreno y NPU Hexagon)
    // NPU Hexagon en Windows ARM: la build Hexagon de llama.cpp si va en el paquete
    if (os_tag() == "win" && llama && acc == "hexagon") return {"hexagon", "opencl-adreno", "cpu"};
    if (os_tag() == "win" && llama) return acc == "opencl" ? std::vector<std::string>{"opencl-adreno", "cpu"} : std::vector<std::string>{"cpu", "opencl-adreno"};
    if (os_tag() == "linux" && llama) return acc == "hexagon" || acc == "opencl" ? std::vector<std::string>{"snapdragon", "cpu"} : std::vector<std::string>{"cpu", "snapdragon"};
  }
  if (acc == "vulkan") return {"vulkan", "cpu"};
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

json RuntimeManager::import_pack(const fs::path& zip) {
  if (!is_file(zip)) throw std::runtime_error("No existe " + S(zip));
  fs::path tmp = paths::sub({"cache"}) / ("paquete-" + std::to_string((long long)(now() * 1000)));
  rm_rf(tmp);
  if (!archive::extract(zip, tmp)) {
    rm_rf(tmp);
    throw std::runtime_error("No se pudo abrir " + S(zip.filename()));
  }
  // runtimes/<rt>/<variante> (o <rt>/<variante> si se comprimio sin la carpeta de arriba)
  fs::path base = is_dir(tmp / "runtimes") ? tmp / "runtimes" : tmp;
  json out = json::array();
  std::error_code ec;
  for (auto& r : fs::directory_iterator(base, ec)) {
    if (!r.is_directory()) continue;
    std::string rt = S(r.path().filename());
    for (auto& v : fs::directory_iterator(r.path(), ec)) {
      if (!v.is_directory()) continue;
      fs::path dest = root_ / rt / v.path().filename();
      rm_rf(dest);
      mkdirs(dest.parent_path());
      fs::rename(v.path(), dest, ec);
      if (ec) {   // otra unidad: se copia
        ec.clear();
        fs::copy(v.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      }
#ifndef _WIN32
      for (auto& f : glob(dest, "*", true))
        if (is_file(f) && S(f.filename()).find('.') == std::string::npos)
          fs::permissions(f, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
      out.push_back(rt + "/" + S(v.path().filename()));
    }
  }
  rm_rf(tmp);
  if (out.empty()) throw std::runtime_error(S(zip.filename()) + " no trae runtimes/<runtime>/<variante>");
  L.info("Paquete de runtimes importado: ", S(zip.filename()), " -> ", jdump(out));
  return out;
}

json RuntimeManager::import_found() {
  json done = jparse(read_text(root_ / "paquetes.json"));
  if (!done.is_object()) done = json::object();
  json out = json::array();
  for (auto& dir : {paths::root(), paths::root().parent_path(), paths::sub({"paquetes"})})
    for (auto& z : glob(dir, "Nexus-Office-runtime-*.zip")) {
      std::string key = cat(S(z.filename()), "|", fsize(z));
      if (done.contains(key)) continue;
      try {
        for (auto& v : import_pack(z)) out.push_back(v);
        done[key] = now();
        mkdirs(root_);
        write_file(root_ / "paquetes.json", jdump(done));
      } catch (const std::exception& e) {
        L.warning("No se pudo importar ", S(z), ": ", e.what());
      }
    }
  return out;
}

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
    if (!starts_with(v, "cpu") || v == preferred)   // cpu-compat elegida a mano (CPU sin AVX2 mal detectada)
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
    std::string cp = starts_with(variant, "cuda") ? cudart_pattern(runtime, variant) : "";
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
