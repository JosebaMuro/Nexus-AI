// Backend principal: llama-server (llama.cpp oficial) como subproceso (llm/llamaserver.py).
#include <algorithm>
#include <chrono>
#include <thread>

#include "hardware/android_accel.h"
#include "hardware/gpu_devices.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

namespace nx::llm {

static const auto L = log::get("llamaserver");

namespace {
struct GpuFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};
}  // namespace

LlamaServerBackend::LlamaServerBackend(runtimes::RuntimeManager* rt, Resolver resolve, std::function<json()> profile,
                                       std::function<std::string()> variant)
    : OpenAICompatBackend("http://127.0.0.1:0/v1"), rt_(rt), resolve_(std::move(resolve)), profile_(std::move(profile)),
      variant_getter_(std::move(variant)) {}

LlamaServerBackend::~LlamaServerBackend() { stop(); }

bool LlamaServerBackend::available() {
  return rt_ && rt_->exe("llama", profile_(), variant_getter_()).has_value() && resolve_("").has_value();
}

std::vector<std::string> LlamaServerBackend::models() {
  auto r = resolve_("");
  if (!r) return {};
  return {jstr(r->second, "id")};
}

bool LlamaServerBackend::supports_vision(const std::string& model) {
  auto r = resolve_(model);
  return r && truthy(r->second.value("mmproj", json()));
}

json LlamaServerBackend::gpu() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return gpu_;
}

std::string LlamaServerBackend::gpu_error() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return gpu_error_;
}

json LlamaServerBackend::effective() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return effective_;
}

void LlamaServerBackend::stop() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (proc_ && proc_->running()) {
    proc_->terminate();
    if (!proc_->wait(10)) proc_->kill();
  }
  proc_.reset();
  key_.clear();
}

void LlamaServerBackend::ensure(const std::string& model, const json& opts_in) {
  // Android: si la NPU o la GPU no arrancan con este driver, se apunta y se pasa a la siguiente (NPU -> GPU -> CPU)
  json opts = opts_in;
  for (int i = 0; i < 3; ++i) {
    try {
      ensure_once(model, opts);
      return;
    } catch (const GpuFailed& e) {
      if (nx::env("NEXUS_RUNTIME_LLAMA_CPU").empty()) {   // escritorio: la GPU elegida no arranca -> CPU
        L.warning(e.what(), ": se sigue en la CPU");
        {
          std::lock_guard<std::recursive_mutex> lk(mu_);
          gpu_error_ = uleft(e.what(), 300);
        }
        opts["gpu_layers"] = 0;
        opts["device"] = "";
        continue;
      }
      json nxt = gpu_failed(e.what(), jstr(opts, "device"));
      opts.update(nxt);
    }
  }
  opts["gpu_layers"] = 0;
  opts["device"] = "";
  ensure_once(model, opts);
}

json LlamaServerBackend::gpu_failed(const std::string& reason, const std::string& device) {
  // la NPU o la GPU no sirve para llama.cpp en este movil (driver): se apunta y se sigue con la GPU (si fallo la NPU
  // y hay GPU utilizable) o con la CPU. Devuelve las opciones nuevas del modelo.
  hw::android_accel::mark_gpu_broken(reason, device);
  json nxt = {{"gpu_layers", 0}, {"device", ""}};
  if (hw::android_accel::is_npu_dev(device)) {
    json use = hw::android_accel::detect()["gpu"].value("use", json());
    if (use.is_object() && !jstr(use, "dev").empty() && !hw::android_accel::is_npu_dev(jstr(use, "dev")))
      nxt = {{"gpu_layers", -1}, {"device", jstr(use, "dev")}};
  }
  L.warning(reason, ": se sigue en ", jstr(nxt, "device").empty() ? std::string("CPU") : jstr(nxt, "device"));
  if (set_llm_opts) set_llm_opts(nxt);
  return nxt;
}

void LlamaServerBackend::ensure_once(const std::string& model, const json& opts) {
  auto resolved = resolve_(model);
  if (!resolved) throw std::runtime_error("No hay modelo GGUF activo. Descarga uno en Modelos.");
  auto& [path, entry] = *resolved;
  json profile = profile_();
  auto found = rt_ ? rt_->exe("llama", profile, variant_getter_()) : std::nullopt;
  if (!found) throw std::runtime_error("Runtime llama.cpp no instalado (Sistema > Runtimes)");
  fs::path exe = found->exe;
  std::string var = found->variant;
  int ngl = (int)jnum(opts.value("gpu_layers", json(0)));
  std::string device;
  std::map<std::string, std::string> env;
  if (var == "embedded" && !nx::env("NEXUS_RUNTIME_LLAMA_CPU").empty()) {
    // app Android: llama-server con GPU o NPU (Vulkan/OpenCL/Hexagon) si el perfil la usa; si no, el de CPU
    device = ngl ? jstr(opts, "device") : "";
    bool gpu = !device.empty() && !nx::env("NEXUS_RUNTIME_LLAMA_GPU").empty();
    exe = P(nx::env(gpu ? "NEXUS_RUNTIME_LLAMA_GPU" : "NEXUS_RUNTIME_LLAMA_CPU"));
    if (hw::android_accel::is_gpu_exe(exe)) env = hw::android_accel::gpu_env(S(exe.parent_path()));
    else ngl = 0, device.clear();
  }
  if (var == "cpu") ngl = 0;
  json llm = profile.value("llm", json::object());
  json extra_env = llm.value("device_env", json::object());
  if (!extra_env.is_object()) extra_env = json::object();
  for (auto& [k, v] : extra_env.items()) env[k] = v.is_string() ? v.get<std::string>() : v.dump();
  if (var != "embedded" && var != "system")
    for (auto& [k, v] : runtimes::lib_env(exe)) env[k] = v;
  bool single = truthy(llm.value("single_slot", json(false)));
  if (ngl && device.empty() && var != "cpu" && var != "embedded") {
    // escritorio: con varias GPU (o una real y llvmpipe en Linux) se fija UNA, la elegida o la mejor
    json devs = hw::gpu_devices::list_devices(exe, env);
    json sel = hw::gpu_devices::pick(devs, jstr(llm, "gpu_select"));
    if (!devs.empty() && sel.is_null()) {
      std::vector<std::string> names;
      for (auto& d : devs) names.push_back(jstr(d, "name"));
      L.warning("llama.cpp solo ve renderizadores por software (", join(names, ", "), "): se usa la CPU");
      ngl = 0;
    } else if (!sel.is_null() && hw::gpu_devices::needs_device_flag(devs)) {
      device = jstr(sel, "id");
    }
    std::lock_guard<std::recursive_mutex> lk(mu_);
    gpu_ = sel.is_null() ? json::object() : sel;
  }
  bool kv_ram = truthy(opts.value("kv_ram", json(false))), mlock = truthy(opts.value("mlock", json(false)));
  std::string kv_quant = jstr(opts, "kv_quant");
  int threads_batch = (int)jnum(opts.value("n_threads_batch", json(0)), 0);
  std::string mmproj = entry.value("mmproj", json()).is_string() ? entry["mmproj"].get<std::string>() : "";
  std::string key = cat(S(exe), "|", path, "|", mmproj, "|", jdump(opts.value("n_ctx", json())), "|",
                        jdump(opts.value("n_threads", json())), "|", jdump(opts.value("n_batch", json())), "|", ngl, "|",
                        device, "|", jdump(extra_env), "|", kv_ram, "|", mlock, "|", kv_quant, "|", threads_batch);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (proc_ && proc_->running() && key_ == key) return;
  stop();
  port = net::free_port(8181);
  std::vector<std::string> cmd = {S(exe),
                                  "-m",
                                  path,
                                  "--host",
                                  "127.0.0.1",
                                  "--port",
                                  std::to_string(port),
                                  "-c",
                                  std::to_string((int)jnum(opts.value("n_ctx", json(4096)), 4096)),
                                  "-b",
                                  std::to_string((int)jnum(opts.value("n_batch", json(256)), 256)),
                                  "-ngl",
                                  std::to_string(ngl == -1 ? 999 : ngl),
                                  "--jinja",
                                  "--no-webui"};
  if (truthy(opts.value("n_threads", json()))) {
    cmd.push_back("-t");
    cmd.push_back(std::to_string((int)jnum(opts["n_threads"])));
  }
  if (!device.empty()) {
    cmd.push_back("--device");
    cmd.push_back(device);
  }
  if (kv_ram && ngl) cmd.push_back("--no-kv-offload");   // perfil de hardware: la cache KV en la RAM (mas capas en la VRAM)
  if (mlock) cmd.push_back("--mlock");                    // perfil de hardware: el modelo fijo en la RAM
  // prompt con todos los hilos logicos; la generacion con los fisicos (-t)
  if (threads_batch > 0 && threads_batch != (int)jnum(opts.value("n_threads", json(0)), 0))
    cmd.insert(cmd.end(), {"-tb", std::to_string(threads_batch)});
  // cache KV en 8 bits con la memoria justa: cabe el doble de contexto (necesita atencion rapida; si este binario o
  // esta GPU no la tienen, se arranca sin ella)
  if (!kv_quant.empty() && !kv_quant_failed_) cmd.insert(cmd.end(), {"-ctk", kv_quant, "-ctv", kv_quant});
  cmd.push_back("-np");
  // un solo slot tambien con la variante de GPU forzada a CPU (-ngl 0): si no, cada slot se queda con medio contexto
  cmd.push_back(var == "cpu" || var == "system" || single || ngl == 0 ? "1" : "2");
  if (mmproj.empty()) {
    cmd.push_back("--cache-reuse");
    cmd.push_back("256");
  } else {
    cmd.push_back("--mmproj");
    cmd.push_back(mmproj);
    if (var == "cpu") {
      cmd.push_back("--image-max-tokens");
      cmd.push_back("384");
    }
  }
  fs::path log_path = paths::logs_dir() / "llama-server.log";
  uint64_t log_from = is_file(log_path) ? fsize(log_path) : 0;
  proc::Options po;
  po.cwd = exe.parent_path();
  po.env = env;
  po.output_file = log_path;
  std::vector<std::string> shown(cmd.begin() + 1, cmd.end());
  L.info("Arrancando llama-server [", var, "]: ", join(shown, " "));
  proc_ = std::make_unique<proc::Process>();
  if (!proc_->start(cmd, po)) {
    std::string e = proc_->error();
    proc_.reset();
    throw std::runtime_error("No se pudo arrancar llama-server: " + e);
  }
  base_url_ = cat("http://127.0.0.1:", port, "/v1");
  key_ = key;
  device_ = device;
  variant = var;
  double t0 = now();
  while (now() - t0 < 300) {
    if (!proc_->running()) {
      std::string tail = read_text(log_path);
      tail = valid_utf8(tail.size() > 1500 ? tail.substr(tail.size() - 1500) : tail);
      if (!kv_quant.empty() && !kv_quant_failed_ && std::find(cmd.begin(), cmd.end(), "-ctk") != cmd.end() &&
          (contains(lower(tail), "quantized v cache") || contains(lower(tail), "flash_attn") ||
           contains(tail, "-ctk") || contains(tail, "cache-type") || contains(lower(tail), "failed to create context"))) {
        L.warning("llama-server no admite la cache KV en ", kv_quant, " aqui: se arranca sin ella");
        kv_quant_failed_ = true;
        auto it = std::find(cmd.begin(), cmd.end(), "-ctk");
        cmd.erase(it, it + 4);
        proc_ = std::make_unique<proc::Process>();
        if (proc_->start(cmd, po)) continue;
      }
      if (contains(tail, "--no-webui") || contains(tail, "invalid argument")) {
        auto it = std::find(cmd.begin(), cmd.end(), "--no-webui");
        if (it != cmd.end()) {
          cmd.erase(it);
          proc_ = std::make_unique<proc::Process>();
          if (proc_->start(cmd, po)) continue;
        }
      }
      proc_.reset();
      key_.clear();
      if (!device.empty())
        throw GpuFailed(cat("llama-server con ", starts_with(device, "HTP") ? "NPU" : "GPU", " (", device,
                            ") termino al arrancar: ", uright(tail, 300)));
      throw std::runtime_error("llama-server termino al arrancar: " + uright(tail, 600));
    }
    if (http::get(cat("http://127.0.0.1:", port, "/health"), {}, 1).status == 200) {
      L.info("llama-server listo en ", fnum(now() - t0, 1), "s");
      json eff = hw::effective_from_log(hw::read_log_since(log_path, log_from), extra_env);
      eff["variant"] = var;
      eff["requested"] = jstr(extra_env, "GGML_OPENVINO_DEVICE") == "NPU" || starts_with(device, "HTP") ? "NPU"
                         : ngl ? "GPU" : "CPU";
      eff["at"] = now();
      eff["ok"] = eff["requested"] == "CPU" || starts_with(jstr(eff, "device"), jstr(eff, "requested"));
      if (!eff["ok"].get<bool>())
        L.warning("Se pidio ", jstr(eff, "requested"), " pero el modelo ha quedado en ", jstr(eff, "device"), " (",
                  jstr(eff, "detail").empty() ? var : jstr(eff, "detail"), ")");
      effective_ = eff;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!device.empty()) {
    stop();
    throw GpuFailed("llama-server con GPU (" + device + ") no respondio a tiempo");
  }
  throw std::runtime_error("llama-server no respondio a tiempo");
}

void LlamaServerBackend::stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) {
  ensure(model, opts.o);
  json msgs = messages;
  if (!supports_vision(model))
    for (auto& m : msgs) m["images"] = json::array();
  bool produced = false;
  Sink wrap = [&](const std::string& p) {
    produced = true;
    out(p);
  };
  try {
    OpenAICompatBackend::stream_chat(msgs, "local", opts, wrap);
  } catch (const ToolsUnsupported&) {
    throw;
  } catch (const std::exception& e) {
    bool alive;
    {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      alive = proc_ && proc_->running();
    }
    if (produced || alive) throw;
    L.warning("llama-server se detuvo durante la peticion; reiniciando y reintentando");
    json o = opts.o;
    std::string dev;
    {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      key_.clear();
      dev = device_;
    }
    if (!dev.empty()) {   // se cayo usando la GPU o la NPU: driver inestable, se sigue en la siguiente
      o.update(gpu_failed("llama-server con GPU (" + dev + ") se detuvo durante una respuesta", dev));
    }
    ensure(model, o);
    Opts o2{o, opts.on_reasoning};
    OpenAICompatBackend::stream_chat(msgs, "local", o2, out);
  }
}

}  // namespace nx::llm
