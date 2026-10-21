// Perfil optimo (profiles.py), selector CPU/GPU/NPU (accel_choice.py) y autoajuste (tuner.py).
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>

#include "hardware/hardware.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::hw {

static const auto LT = log::get("tuner");
static const std::vector<double> MODEL_TIERS = {0.5, 1.5, 3, 4, 8, 14, 32, 70};
static const double GB_PER_B_Q4 = 0.65;

// numero como lo escribiria Python: entero si lo es (3), decimal si no (1.5)
static json pynum(double v) {
  if (std::floor(v) == v && std::fabs(v) < 1e15) return (long long)v;
  return v;
}

bool android() { return sys::is_android(); }

const GPU* best_gpu(const HardwareInfo& hw) {
  if (hw.gpus.empty()) return nullptr;
  auto rank = [](const std::string& v) {
    if (v == "nvidia") return 4;
    if (v == "amd" || v == "apple") return 3;
    if (v == "intel") return 2;
    if (v == "qualcomm") return 1;
    return 0;
  };
  const GPU* best = &hw.gpus[0];
  for (auto& g : hw.gpus) {
    auto key = [&](const GPU& x) { return std::make_tuple(!x.integrated, rank(x.vendor), x.vram_gb); };
    if (key(g) > key(*best)) best = &g;
  }
  return best;
}

static bool has_str(const json& arr, const std::string& s) {
  if (!arr.is_array()) return false;
  for (auto& x : arr)
    if (x.is_string() && x.get<std::string>() == s) return true;
  return false;
}

std::pair<std::string, const GPU*> choose_accelerator(const HardwareInfo& hw, const std::string& ov) {
  const GPU* gpu = best_gpu(hw);
  if (!ov.empty()) return {ov, gpu};
  const json& rt = hw.runtimes;
  if (!gpu) return {"cpu", nullptr};
  if (android()) {
    std::string b = rt.contains("android_accel") ? jstr(rt["android_accel"], "gpu_backend") : "";
    return {b.empty() ? "cpu" : b, gpu};
  }
  if (gpu->vendor == "nvidia") return {gpu->vram_gb > 0 ? "cuda" : "cpu", gpu};
  if (gpu->vendor == "apple") return {"metal", gpu};
  if (gpu->integrated) {
    if (gpu->vendor == "intel" && has_str(rt.value("openvino_devices", json()), "GPU")) return {"openvino", gpu};
    return {"cpu", gpu};
  }
  if (gpu->vendor == "amd") return {sys::is_linux() ? "rocm" : "vulkan", gpu};
  if (gpu->vendor == "intel") return {truthy(rt.value("torch_xpu", json(false))) ? "xpu" : "vulkan", gpu};
  return {"cpu", gpu};
}

static double tier_at_most(double b) {
  double best = MODEL_TIERS[0];
  for (double t : MODEL_TIERS)
    if (t <= b) best = t;
  return best;
}

json build_profile(const HardwareInfo& hw, const std::string& ov) {
  auto [acc, gpu] = choose_accelerator(hw, ov);
  static const std::set<std::string> OFF = {"cuda", "rocm", "vulkan", "metal", "xpu", "opencl", "hexagon"};
  bool gpu_offload = OFF.count(acc) && gpu != nullptr;
  double ram = hw.ram_gb;
  double cpu_budget = std::max(0.5, std::min(ram * 0.5, ram - 3.0));
  double budget;
  if (gpu_offload && gpu && !gpu->integrated && !android()) budget = gpu->vram_gb * 0.85;
  else if (acc == "metal") budget = ram * 0.65;
  else budget = cpu_budget;
  double max_b = tier_at_most(budget / GB_PER_B_Q4);
  if (!gpu_offload) {
    int pc = hw.cores_physical;
    double cap = pc <= 2 ? 3 : pc <= 4 ? 4 : pc <= 8 ? 8 : 14;
    std::string a = lower(hw.arch);
    if (std::find(hw.cpu_flags.begin(), hw.cpu_flags.end(), "avx2") == hw.cpu_flags.end() && a != "arm64" &&
        a != "aarch64")
      cap = std::min(cap, 1.5);
    max_b = std::min(max_b, cap);
  }
  double recommended_b = max_b > 1.5 ? tier_at_most(max_b * 0.6) : max_b;
  int phys = std::max(1, hw.cores_physical);
  int n_threads = phys;
  if (android()) {
    max_b = ram >= 11 ? 4 : ram >= 6 ? 3 : 1.5;
    recommended_b = max_b == 4 ? 3 : max_b == 3 ? 1.5 : 0.5;
    budget = std::min(budget, std::max(0.5, ram * 0.45));
    n_threads = std::max(1, std::min(phys, 4));   // movil: los nucleos rapidos
  }
  double ctx_mem = budget - recommended_b * GB_PER_B_Q4;
  int n_ctx = ctx_mem >= 8 ? 16384 : ctx_mem >= 3 ? 8192 : ctx_mem >= 1.2 ? 4096 : 2048;
  int max_ctx = std::min(32768, n_ctx * 4);
  if (android()) {
    n_ctx = std::min(n_ctx, 4096);
    max_ctx = std::min(max_ctx, 8192);
  }
  double vram = gpu ? gpu->vram_gb : 0.0;
  std::string torch_device = acc == "cuda" || acc == "rocm" ? "cuda" : acc == "metal" ? "mps" : acc == "xpu" ? "xpu" : "cpu";
  if (sys::is_windows() && gpu && (gpu->vendor == "amd" || gpu->vendor == "intel") && !gpu->integrated)
    torch_device = "dml";
  bool image_gpu = torch_device != "cpu" && (vram >= 4 || acc == "metal");

  const NPU* npu = hw.npus.empty() ? nullptr : &hw.npus[0];
  bool npu_ready = false;
  bool llm_npu = android() && acc == "hexagon";
  if (npu) {
    const json& rt = hw.runtimes;
    json prov = rt.value("onnxruntime_providers", json::array());
    npu_ready = has_str(rt.value("openvino_devices", json()), "NPU") || has_str(prov, "VitisAIExecutionProvider") ||
                has_str(prov, "QNNExecutionProvider") || has_str(prov, "CoreMLExecutionProvider");
  }
  std::string wheel = acc == "cuda" ? "cu124" : acc == "metal" ? "metal" : acc == "vulkan" || acc == "xpu" ? "vulkan"
                      : acc == "rocm" ? "rocm" : acc == "opencl" ? "opencl" : "cpu";
  json llm = {{"max_model_b", pynum(max_b)},
              {"recommended_model_b", pynum(recommended_b)},
              {"quant", budget < 24 ? "Q4_K_M" : "Q5_K_M"},
              {"n_ctx", n_ctx},
              {"max_ctx", max_ctx},
              {"n_threads", n_threads},
              {"n_batch", gpu_offload || ram >= 16 ? 512 : 256},
              {"gpu_layers", gpu_offload ? -1 : 0}};
  if (android())
    llm["device"] = gpu_offload && hw.runtimes.contains("android_accel") ? jstr(hw.runtimes["android_accel"], "gpu_device")
                                                                          : "";
  llm["llamacpp_wheel"] = wheel;
  llm["flash_attn"] = acc == "cuda" || acc == "metal";
  std::string hint;
  if (llm_npu) hint = "El modelo de texto va en la NPU (llama.cpp Hexagon); si falla, en la GPU o en la CPU";
  else if (!npu || npu_ready) hint = "";
  else if (android())
    hint = std::string("llama.cpp no puede usar esta NPU en Android (su SDK es propietario): el modelo va en la ") +
           (gpu_offload ? "GPU" : "CPU");
  else hint = "Instala el runtime '" + npu->runtime_hint + "' para usar la NPU";
  std::string stt_model = acc == "cuda" ? (vram >= 4 ? "small" : "base") : (phys >= 4 ? "base" : "tiny");
  return {
      {"id", cat(acc, "-", gpu ? gpu->vendor : "cpu", "-", gpu && gpu->integrated ? "igpu" : gpu ? "dgpu" : "only")},
      {"accelerator", acc},
      {"device_name", gpu && acc != "cpu" ? gpu->name : hw.cpu_name},
      {"vram_gb", vram},
      {"ram_gb", ram},
      {"tier", max_b >= 14 ? "alto" : max_b >= 4 ? "medio" : "ligero"},
      {"llm", llm},
      {"image",
       {{"device", torch_device},
        {"neural", image_gpu},
        {"resolution", vram >= 10 ? 1024 : 512},
        {"steps", 4},
        {"suggested_model", vram >= 10 ? "stabilityai/sdxl-turbo" : "stabilityai/sd-turbo"}}},
      {"music", {{"device", torch_device}, {"neural", image_gpu}, {"suggested_model", "facebook/musicgen-small"}}},
      {"stt",
       {{"device", acc == "cuda" ? "cuda" : "cpu"},
        {"model", stt_model},
        {"compute_type", acc == "cuda" ? "float16" : "int8"}}},
      {"embeddings", {{"device", npu_ready ? "npu" : torch_device}}},
      {"npu",
       {{"present", npu != nullptr},
        {"name", npu ? npu->name : ""},
        {"vendor", npu ? npu->vendor : ""},
        {"runtime", npu ? npu->runtime_hint : ""},
        {"ready", npu_ready || llm_npu},
        {"offload", npu_ready ? json::array({"embeddings", "stt"}) : llm_npu ? json::array({"llm"}) : json::array()},
        {"hint", hint}}},
  };
}

// ---------------------------------------------------------------- selector de inicio
const std::vector<std::string> ENGINES = {"auto", "llamaserver", "llamacpp"};
const std::vector<std::string> ACCELS = {"auto", "cpu", "gpu", "npu"};

std::string engine_label(const std::string& e) {
  if (e == "auto") return "Automático";
  if (e == "llamaserver") return "llama-server (llama.cpp, proceso aparte)";
  if (e == "llamacpp") return "llama.cpp integrado (llama-cpp-python)";
  return e;
}

static json mkplan(const std::string& ov, const std::string& variant, const json& device, bool ok,
                   const std::string& note, json env = json::object()) {
  return {{"override", ov}, {"variant", variant}, {"device", device}, {"env", env}, {"ok", ok}, {"note", note}};
}

json plan(const HardwareInfo& hw, const std::string& accel_in) {
  std::string accel = std::find(ACCELS.begin(), ACCELS.end(), accel_in) != ACCELS.end() ? accel_in : "auto";
  const GPU* gpu = best_gpu(hw);
  if (accel == "auto") return mkplan("", "", nullptr, true, "");
  if (accel == "cpu") return mkplan("cpu", "cpu", "", true, "");
  if (android()) {
    // la edicion C++ en Android usa la CPU (la deteccion Vulkan/OpenCL/Hexagon del movil sigue en la version Python)
    std::string note = accel == "npu" ? "Este móvil no tiene una NPU que llama.cpp pueda usar (Hexagon HTP v73+); se usa la GPU"
                                      : "";
    return mkplan("cpu", "cpu", "", false,
                  (note.empty() ? "" : note + "; ") + "ninguna GPU utilizable por llama.cpp: se usa la CPU");
  }
  if (accel == "npu") {
    for (auto& n : hw.npus)
      if (n.vendor == "intel") {
        json p = mkplan("openvino", "openvino", "", true, "", {{"GGML_OPENVINO_DEVICE", "NPU"}});
        p["npu"] = true;
        return p;
      }
    std::string name = hw.npus.empty() ? "" : hw.npus[0].name;
    std::string note = !name.empty()
                           ? "llama.cpp no puede ejecutar el modelo de texto en la NPU " + name +
                                 " (solo Intel con OpenVINO en escritorio); la NPU se usa para embeddings y voz si su "
                                 "runtime está instalado"
                           : "No se ha detectado ninguna NPU";
    json sub = plan(hw, gpu ? "gpu" : "cpu");
    sub["ok"] = false;
    sub["note"] = note + "; el modelo va en la " + (gpu ? "GPU" : "CPU");
    return sub;
  }
  if (!gpu) return mkplan("cpu", "cpu", "", false, "No se ha detectado ninguna GPU: se usa la CPU");
  if (sys::is_macos()) return mkplan("metal", "metal", "", true, "");
  if (gpu->vendor == "nvidia" && gpu->vram_gb > 0) return mkplan("cuda", "cuda", "", true, "");
  return mkplan("vulkan", "vulkan", "", true,
                gpu->integrated ? "GPU integrada: puede ir más lenta que la CPU con modelos grandes" : "");
}

std::pair<json, json> build_for(const HardwareInfo& hw, const std::string& accel, const std::string& legacy) {
  json p = plan(hw, accel);
  std::string ov = p["override"].get<std::string>();
  json profile = build_profile(hw, ov.empty() ? legacy : ov);
  json& llm = profile["llm"];
  if (!p["device"].is_null() && android()) {
    llm["device"] = p["device"];
    llm["gpu_layers"] = truthy(p["device"]) ? -1 : 0;
  }
  if (truthy(p.value("npu", json(false)))) {
    llm["gpu_layers"] = -1;
    llm["device_env"] = p["env"];
    llm["single_slot"] = true;
    llm["n_ctx"] = std::min(llm["n_ctx"].get<int>(), 4096);
    std::set<std::string> off;
    for (auto& x : profile["npu"]["offload"]) off.insert(x.get<std::string>());
    off.insert("llm");
    profile["npu"]["offload"] = json(std::vector<std::string>(off.begin(), off.end()));
  }
  if (accel == "cpu") {
    llm["gpu_layers"] = 0;
    llm["device"] = "";
  }
  profile["choice"] = {{"accel", accel}, {"ok", p["ok"]}, {"note", p["note"]}, {"variant", p["variant"]}};
  return {profile, p};
}

json effective_from_log(const std::string& text, const json& env) {
  json out = {{"device", "CPU"}, {"offloaded", ""}, {"detail", ""}};
  static const Rx OFFLOAD(R"(offloaded (\d+)/(\d+) layers to GPU)");
  auto offs = OFFLOAD.finditer(text);
  if (!offs.empty()) out["offloaded"] = offs.back().str(1) + "/" + offs.back().str(2);
  struct Pat {
    Rx rx;
    const char* kind;
  };
  static const std::vector<std::pair<std::string, const char*>> defs = {
      {R"(using device (OPENVINO\d+)\s*\(([^)]*)\))", nullptr},
      {R"(using device (CUDA\d+)\s*\(([^)]*)\))", nullptr},
      {R"(using device (Vulkan\d+)\s*\(([^)]*)\))", nullptr},
      {R"(using device (Metal|MTL\d*)\s*\(([^)]*)\))", nullptr},
      {R"(using device (HTP\d+[:\d]*)\s*\(([^)]*)\))", nullptr},
      {R"(using device (GPUOpenCL|OpenCL\d*)\s*\(([^)]*)\))", nullptr},
      {R"(ggml_cuda_init: found (\d+) CUDA devices?)", "CUDA"},
      {R"(ggml_vulkan: \d+ = ([^|(\n]+))", "Vulkan"},
      {R"((?:OpenVINO|OPENVINO)[^\n]*device[^\n]*?\b(NPU|GPU|CPU)\b)", "OpenVINO"},
  };
  static std::vector<std::unique_ptr<Rx>> pats;
  static std::once_flag once;
  std::call_once(once, [] {
    for (auto& d : defs) pats.push_back(std::make_unique<Rx>(d.first));
  });
  std::string detail;
  for (size_t i = 0; i < defs.size(); ++i) {
    auto found = pats[i]->finditer(text);
    if (found.empty()) continue;
    auto& last = found.back();
    std::string kind = defs[i].second ? defs[i].second : "";
    if (kind == "CUDA") detail = "CUDA (" + last.str(1) + " dispositivo" + (last.str(1) != "1" ? "s" : "") + ")";
    else if (kind == "Vulkan") detail = "Vulkan · " + trim(last.str(1));
    else if (kind == "OpenVINO") detail = "OpenVINO " + last.str(1);
    else detail = last.str(1) + " · " + trim(last.str(2));
    break;
  }
  out["detail"] = detail;
  bool gpu_layers = !offs.empty() && std::atoi(offs.back().str(1).c_str()) > 0;
  std::string ov = jstr(env, "GGML_OPENVINO_DEVICE");
  if (!ov.empty() && (starts_with(detail, "OPENVINO") || (gpu_layers && contains(lower(text), "openvino"))))
    out["device"] = ov == "NPU" ? "NPU (OpenVINO)" : ov == "GPU" ? "GPU (OpenVINO)" : "CPU (OpenVINO)";
  else if (starts_with(detail, "HTP"))
    out["device"] = "NPU (Hexagon)";
  else if (gpu_layers)
    out["device"] = "GPU";
  return out;
}

std::string read_log_since(const fs::path& p, uint64_t offset) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  f.seekg((std::streamoff)offset);
  std::string buf(2'000'000, '\0');
  f.read(buf.data(), (std::streamsize)buf.size());
  buf.resize((size_t)f.gcount());
  return valid_utf8(buf);
}

// ---------------------------------------------------------------- autoajuste
json load_state() {
  auto t = read_file(paths::tuning_file());
  if (t) {
    json j = jparse(*t);
    if (j.is_object()) return j;
  }
  return {{"overrides", json::object()}, {"thread_trials", json::object()}, {"log", json::array()}};
}

void save_state(json state) {
  json lg = state.value("log", json::array());
  if (lg.size() > 40) lg = json(std::vector<json>(lg.end() - 40, lg.end()));
  state["log"] = lg;
  write_file(paths::tuning_file(), jdump(state, 2));
}

std::vector<int> thread_candidates(const json& profile, int cores_logical) {
  int phys = profile["llm"]["n_threads"].get<int>();
  std::set<int> s = {std::max(1, phys - 1), phys, std::max(phys, cores_logical)};
  return {s.begin(), s.end()};
}

static const int MIN_CTX = 2048, MIN_BATCH = 128;

TuneResult tune(const json& profile, const json& history, bool previous_unclean, int cores_logical, const json& st,
                const json& previous_info) {
  json state = st.is_object() ? st : load_state();
  const json& base = profile["llm"];
  json llm = base;
  json overrides = state.value("overrides", json::object());
  for (auto it = overrides.begin(); it != overrides.end(); ++it) llm[it.key()] = it.value();
  auto I = [&](const char* k) { return (int)jnum(llm[k]); };
  auto B = [&](const char* k) { return (int)jnum(base[k]); };
  llm["n_ctx"] = std::max(std::min(MIN_CTX, B("n_ctx")), I("n_ctx"));
  llm["n_batch"] = std::max(std::min(MIN_BATCH, B("n_batch")), I("n_batch"));
  std::vector<std::string> reasons;
  json last = history.is_array() && !history.empty() ? history.back() : json();
  json info = previous_info.is_object() ? previous_info : json::object();
  auto L = [&](const json& j, const char* k) { return j.is_object() ? jnum(j.value(k, json(0))) : 0.0; };
  bool memory_crash = previous_unclean && (L(info, "peak_ram_pct") > 88 || L(info, "peak_vram_pct") > 95 ||
                                           L(info, "oom_errors") > 0 || info.empty());
  if (previous_unclean && !memory_crash)
    reasons.push_back("La sesion anterior termino de forma abrupta sin presion de memoria: se mantiene la configuracion");
  bool live = truthy(base.value("live_threads", json(false)));
  if (live) llm["n_threads"] = base["n_threads"];
  if (!live && last.is_object() && L(last, "llm_calls") >= 1 && L(last, "avg_tps") > 0) {
    std::string used =
        std::to_string((int)jnum(last.value("settings", json::object()).value("n_threads", llm["n_threads"])));
    json& tr = state["thread_trials"];
    if (!tr.is_object()) tr = json::object();
    tr[used].push_back(last["avg_tps"]);
    if (tr[used].size() > 8) tr[used] = json(std::vector<json>(tr[used].end() - 8, tr[used].end()));
  }
  if (memory_crash || (last.is_object() && truthy(last.value("oom_errors", json(0))))) {
    llm["n_ctx"] = std::max(std::min(MIN_CTX, B("n_ctx")), I("n_ctx") / 2);
    llm["n_batch"] = std::max(std::min(MIN_BATCH, B("n_batch")), I("n_batch") / 2);
    int gl = I("gpu_layers");
    if (gl != 0 && gl != -1) llm["gpu_layers"] = (int)(gl * 0.75);
    else if (gl == -1 && last.is_object() && truthy(last.value("oom_errors", json(0)))) llm["gpu_layers"] = 24;
    reasons.push_back("Cierre inesperado o falta de memoria en la sesion anterior: contexto y lote reducidos");
  } else if (last.is_object()) {
    double ram = L(last, "peak_ram_pct"), vram = L(last, "peak_vram_pct");
    if (ram > 92) {
      llm["n_ctx"] = std::max(std::min(MIN_CTX, B("n_ctx")), (int)(I("n_ctx") * 0.75));
      llm["n_batch"] = std::max(std::min(MIN_BATCH, B("n_batch")), I("n_batch") / 2);
      reasons.push_back(cat("RAM al ", fnum(ram, 0), "%: contexto reducido a ", I("n_ctx")));
    } else if (ram < 85 && truthy(last.value("clean_exit", json(false))) &&
               (I("n_ctx") < B("n_ctx") || I("n_batch") < B("n_batch"))) {
      llm["n_ctx"] = std::min(B("n_ctx"), I("n_ctx") * 2);
      llm["n_batch"] = std::min(B("n_batch"), I("n_batch") * 2);
      reasons.push_back(cat("Sesion estable (RAM ", fnum(ram, 0), "%): recuperando contexto ", I("n_ctx"), " y lote ",
                            I("n_batch")));
    } else if (ram < 65 && L(last, "llm_calls") >= 3 && I("n_ctx") < B("max_ctx")) {
      llm["n_ctx"] = std::min(B("max_ctx"), (int)(I("n_ctx") * 1.5) / 256 * 256);
      reasons.push_back(cat("Holgura de RAM (", fnum(ram, 0), "%): contexto ampliado a ", I("n_ctx")));
    }
    int gl = I("gpu_layers");
    if (vram > 95 && gl != 0) {
      llm["gpu_layers"] = gl == -1 ? 28 : std::max(0, (int)(gl * 0.8));
      reasons.push_back(cat("VRAM al ", fnum(vram, 0), "%: menos capas en GPU (", I("gpu_layers"), ")"));
    } else if (vram > 0 && vram < 70 && gl > 0) {
      llm["gpu_layers"] = (int)(gl * 1.25) + 1;
      reasons.push_back(cat("VRAM con holgura (", fnum(vram, 0), "%): mas capas en GPU (", I("gpu_layers"), ")"));
    }
    double tps = L(last, "avg_tps");
    if (L(last, "llm_calls") >= 3 && tps > 0 && tps < 4) {
      llm["suggest_smaller_model"] = true;
      reasons.push_back(cat("Velocidad baja (", pyfloat(tps), " tok/s): se recomienda un modelo mas pequeno"));
    } else if (tps >= 4) {
      llm.erase("suggest_smaller_model");
    }
  }
  auto cands = thread_candidates(profile, cores_logical);
  json trials = live ? json::object() : state.value("thread_trials", json::object());
  std::vector<int> untested;
  for (int c : cands)
    if (!trials.contains(std::to_string(c))) untested.push_back(c);
  if (!trials.empty() && !untested.empty()) {
    llm["n_threads"] = untested[0];
    reasons.push_back(cat("Explorando ", untested[0], " hilos para medir rendimiento"));
  } else if (!trials.empty()) {
    auto mean = [&](int c) {
      json v = trials.value(std::to_string(c), json::array({0}));
      double s = 0;
      for (auto& x : v) s += jnum(x);
      return v.empty() ? 0.0 : s / v.size();
    };
    int best = cands[0];
    for (int c : cands)
      if (mean(c) > mean(best)) best = c;
    if (best != I("n_threads")) reasons.push_back(cat("Mejor rendimiento medido con ", best, " hilos"));
    llm["n_threads"] = best;
  }
  json tuned = profile;
  tuned["llm"] = llm;
  json ov = json::object();
  for (const char* k : {"n_ctx", "n_batch", "gpu_layers", "n_threads", "suggest_smaller_model"})
    if (llm.contains(k) && (!base.contains(k) || llm[k] != base[k])) ov[k] = llm[k];
  state["overrides"] = ov;
  if (!reasons.empty()) {
    if (!state["log"].is_array()) state["log"] = json::array();
    state["log"].push_back({{"ts", now()}, {"reasons", reasons}, {"overrides", ov}});
    for (auto& r : reasons) LT.info("Autoajuste: ", r);
  }
  return {tuned, state, reasons};
}

Startup startup(const std::string& ov, bool adaptive, const std::string& ollama_url, bool previous_unclean,
                const json& previous_info) {
  Startup s;
  s.hw = detect(ollama_url);
  save(s.hw);
  s.profile = build_profile(s.hw, ov);
  if (adaptive) {
    auto t = tune(s.profile, load_history(), previous_unclean, s.hw.cores_logical, nullptr, previous_info);
    s.profile = t.profile;
    s.reasons = t.reasons;
    save_state(t.state);
  }
  log::get("hardware").info("Perfil ", jstr(s.profile, "id"), " | acelerador=", jstr(s.profile, "accelerator"),
                            " | modelo recomendado <= ", jdump(s.profile["llm"]["recommended_model_b"]),
                            "B | ctx=", jdump(s.profile["llm"]["n_ctx"]), " hilos=",
                            jdump(s.profile["llm"]["n_threads"]));
  return s;
}

}  // namespace nx::hw
