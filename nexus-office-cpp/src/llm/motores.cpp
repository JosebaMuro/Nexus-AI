// Motores aparte de llama.cpp: donde esta su biblioteca, en que orden se prueban los aceleradores, el servidor
// «nexus-office motor» y el backend que lo arranca. Ver motores.h.
#include "llm/motores.h"

#include <httplib.h>

#include <chrono>
#include <thread>

#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/dynlib.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::motores {

static const auto L = log::get("motores");

const std::vector<Info>& lista() {
  static const std::vector<Info> v = {{"litert", "LiteRT-LM (Google)", "litertlm", "litert-lm"},
                                      {"onnx", "ONNX Runtime GenAI (Microsoft)", "onnx-genai", "onnxruntime-genai"},
                                      {"genie", "Qualcomm Genie (QAIRT)", "genie", "Genie"}};
  return v;
}

const Info* info(const std::string& id) {
  for (auto& i : lista())
    if (i.id == id) return &i;
  return nullptr;
}

std::string motor_de_formato(const std::string& formato) {
  for (auto& i : lista())
    if (i.formato == formato) return i.id;
  return "";
}

// la biblioteca dentro de una carpeta (o de sus subcarpetas: los paquetes de PyPI la dejan en litert_lm/)
static std::optional<fs::path> lib_in(const fs::path& dir, const std::string& name) {
  if (!is_dir(dir)) return std::nullopt;
  if (is_file(dir / name)) return dir / name;
  for (auto& p : glob(dir, name, true))
    if (is_file(p)) return p;
  return std::nullopt;
}

static std::pair<std::optional<fs::path>, std::string> buscar(const std::string& motor, const std::string& variante = "") {
  const Info* i = info(motor);
  if (!i) return {std::nullopt, ""};
  std::string name = dynlib_name(i->lib);
  std::string forced = env(("NEXUS_RUNTIME_" + upper(motor)).c_str());
  if (!forced.empty()) {
    if (is_file(P(forced))) return {P(forced), "embedded"};
    if (auto p = lib_in(P(forced), name)) return {p, "embedded"};
  }
  if (!variante.empty())
    for (auto& base : {paths::sub({"runtimes"}) / motor, runtimes::bundled_dir() / motor})
      if (auto p = lib_in(base / variante, name)) return {p, variante};
  for (auto& base : {paths::sub({"runtimes"}) / motor, runtimes::bundled_dir() / motor}) {
    if (!is_dir(base)) continue;
    std::vector<fs::path> vars;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(base, ec))
      if (e.is_directory()) vars.push_back(e.path());
    std::sort(vars.begin(), vars.end());
    for (auto& v : vars)
      if (auto p = lib_in(v, name)) return {p, S(v.filename())};
    if (auto p = lib_in(base, name)) return {p, "default"};
  }
  // app Android: las bibliotecas de los motores van junto a las de llama-server (lib/arm64-v8a de la APK)
  std::string cpu = env("NEXUS_RUNTIME_LLAMA_CPU");
  if (!cpu.empty() && is_file(P(cpu).parent_path() / name)) return {P(cpu).parent_path() / name, "embedded"};
  return {std::nullopt, ""};
}

std::optional<fs::path> lib_path(const std::string& motor, const std::string& variante) {
  return buscar(motor, variante).first;
}

std::vector<std::string> variantes(const std::string& motor) {
  const Info* i = info(motor);
  std::vector<std::string> out;
  if (!i) return out;
  std::string name = dynlib_name(i->lib);
  for (auto& base : {paths::sub({"runtimes"}) / motor, runtimes::bundled_dir() / motor}) {
    if (!is_dir(base)) continue;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(base, ec)) {
      std::string v = S(e.path().filename());
      if (e.is_directory() && lib_in(e.path(), name) && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
  }
  // Android: una sola carpeta (la de la APK) con todo
  if (out.empty() && buscar(motor).first) out.push_back("embedded");
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Acelerador> aceleradores(const std::string& motor) {
  if (motor == "litert")
    return {{"cpu", "CPU (XNNPACK)", ""}, {"gpu", "GPU (OpenCL, Vulkan, Metal o WebGPU)", ""},
            {"npu", "NPU (Google Tensor, Snapdragon Hexagon, MediaTek APU o Intel)", ""}};
  if (motor == "onnx")
    return {{"modelo", "El proveedor del modelo", ""},     {"cpu", "CPU", "cpu"},
            {"cuda", "NVIDIA CUDA", "cuda"},               {"dml", "DirectML (cualquier GPU en Windows)", "dml"},
            {"qnn", "NPU Hexagon (QNN, Snapdragon X)", "qnn"}, {"OpenVINO", "Intel OpenVINO (NPU, GPU y CPU)", "openvino"},
            {"VitisAI", "AMD Ryzen AI (VitisAI)", "vitisai"}};
  if (motor == "genie") return {{"npu", "NPU Hexagon (HTP)", ""}};
  return {};
}

// proveedor para el que se exporto un modelo ONNX (genai_config.json: model.decoder.session_options.provider_options)
static std::string proveedor_onnx(const json& entry) {
  fs::path dir = P(jstr(entry, "path"));
  if (!is_dir(dir)) dir = dir.parent_path();
  json cfg = jparse(read_text(dir / "genai_config.json"));
  if (!cfg.is_object()) return "";
  json po = cfg.value("model", json::object()).value("decoder", json::object()).value("session_options", json::object())
                .value("provider_options", json::array());
  if (po.is_array())
    for (auto& o : po)
      if (o.is_object() && !o.empty()) return lower(o.begin().key());
  return "";
}

std::string variante_para(const std::string& motor, const std::string& acelerador, const json& entry) {
  if (motor != "onnx") return "";
  std::string a = lower(acelerador);
  if (a == "modelo") a = proveedor_onnx(entry);
  if (a.empty() || a == "cpu") return "cpu";
  if (a == "dml" || a == "directml") return "dml";
  if (a == "qnn") return "qnn";
  if (a == "cuda" || a == "tensorrt" || a == "nvtensorrtrtx") return "cuda";
  if (a == "openvino") return "openvino";
  if (a == "vitisai") return "vitisai";
  return "";
}

json estado() {
  json out = json::object();
  for (auto& i : lista()) {
    auto [p, v] = buscar(i.id);
    json acc = json::array();
    auto vs = variantes(i.id);
    for (auto& a : aceleradores(i.id))
      acc.push_back({{"id", a.id}, {"nombre", a.nombre},
                     {"incluido", a.variante.empty() ? !vs.empty()
                                                      : std::find(vs.begin(), vs.end(), a.variante) != vs.end()}});
    out[i.id] = {{"nombre", i.nombre}, {"formato", i.formato}, {"lib", p ? json(S(*p)) : json()}, {"variante", v},
                 {"variantes", vs}, {"aceleradores", acc}};
  }
  return out;
}

// codigo del SoC para el que esta compilado un modelo de NPU (los .litertlm de NPU llevan el SoC en el nombre:
// ..._sm8750.litertlm, ..._mt6991.litertlm, ..._tensor_g5...); "" si es un modelo general
static std::string soc_del_modelo(const std::string& name) {
  static const Rx R(R"((sm\d{4}|mt\d{4}|qcs\d{4}|tensor[_-]?g\d|gs\d{3}|zuma(?:pro)?|laguna|npu))", Rx::I);
  auto m = R.search(name);
  return m ? lower(m->str(1)) : "";
}

std::vector<std::string> orden(const std::string& motor, const json& profile, const json& entry) {
  std::string acc = jstr(profile, "accelerator", "cpu");
  json npu = profile.value("npu", json::object());
  if (!npu.is_object()) npu = json::object();
  std::string forced = jstr(profile.value("llm", json::object()), "motor_acelerador");
  std::vector<std::string> out;
  if (motor == "onnx") {
    // los modelos ONNX se exportan para un proveedor concreto: el suyo primero, la CPU si falla
    if (!forced.empty()) out.push_back(forced);
    out.push_back("modelo");
    out.push_back("cpu");
  } else if (motor == "genie") {
    // los binarios de contexto de Genie estan compilados para la NPU Hexagon: no hay otro acelerador
    out.push_back("npu");
  } else {
    if (!forced.empty()) out.push_back(forced);
    std::string want = soc_del_modelo(S(P(jstr(entry, "path")).filename()) + " " + jstr(entry, "name"));
    std::string soc = lower(jstr(npu, "soc") + " " + jstr(npu, "name"));
    bool tensor_ok = starts_with(want, "tensor") || want == "zuma" || want == "zumapro" || want == "laguna" ||
                     starts_with(want, "gs");
    bool npu_model = !want.empty() &&
                     (want == "npu" || contains(soc, want) ||
                      (tensor_ok && (contains(soc, "tensor") || contains(soc, "zuma") || contains(soc, "laguna"))));
    if (npu_model && truthy(npu.value("present", json(false)))) out.push_back("npu");
    // GPU: la del perfil (escritorio) o la del movil (OpenCL/Vulkan/WebGPU en LiteRT)
    if (acc != "cpu" || (sys::is_android() && truthy(profile.value("gpu_present", json(false)))) ||
        (sys::is_android() && jnum(profile.value("vram_gb", json(0))) > 0))
      out.push_back("gpu");
    out.push_back("cpu");
  }
  std::vector<std::string> uniq;
  for (auto& x : out)
    if (std::find(uniq.begin(), uniq.end(), x) == uniq.end()) uniq.push_back(x);
  return uniq;
}

json mensajes_simples(const json& messages) {
  json out = json::array();
  if (!messages.is_array()) return out;
  for (auto& m : messages) {
    if (!m.is_object()) continue;
    std::string role = jstr(m, "role", "user");
    if (role != "system" && role != "assistant") role = "user";
    json c = m.value("content", json(""));
    std::string text;
    if (c.is_string()) text = c.get<std::string>();
    else if (c.is_array())
      for (auto& p : c)
        if (p.is_object() && jstr(p, "type", "text") == "text") text += jstr(p, "text");
    // turnos seguidos del mismo papel se juntan: las plantillas de Gemma y otras exigen alternancia
    if (!out.empty() && jstr(out.back(), "role") == role && role != "system")
      out.back()["content"] = jstr(out.back(), "content") + "\n\n" + text;
    else
      out.push_back({{"role", role}, {"content", text}});
  }
  return out;
}

// ---------------------------------------------------------------- servidor del motor
static std::string sse(const json& j) { return "data: " + jdump(j) + "\n\n"; }

int main_motor(const std::vector<std::string>& argv) {
  if (argv.empty() || !info(argv[0])) {
    std::fprintf(stderr, "uso: nexus-office motor <litert|onnx|genie> --modelo RUTA --puerto N [--acelerador cpu|gpu|npu]\n");
    return 2;
  }
  std::string motor = argv[0];
  std::map<std::string, std::string> a;
  for (size_t i = 1; i + 1 < argv.size(); i += 2)
    if (starts_with(argv[i], "--")) a[argv[i].substr(2)] = argv[i + 1];
  Config c;
  c.modelo = P(a["modelo"]);
  c.acelerador = a.count("acelerador") ? a["acelerador"] : "cpu";
  c.hilos = std::atoi(a["hilos"].c_str());
  c.n_ctx = a.count("ctx") ? std::max(512, std::atoi(a["ctx"].c_str())) : 4096;
  c.cache = a.count("cache") ? P(a["cache"]) : fs::path();
  c.dispatch = a.count("dispatch") ? P(a["dispatch"]) : fs::path();
  auto lib = a.count("lib") ? std::optional<fs::path>(P(a["lib"])) : lib_path(motor);
  if (!lib) {
    std::fprintf(stderr, "No hay biblioteca de %s en este equipo\n", info(motor)->nombre.c_str());
    return 3;
  }
  c.lib = *lib;
  if (c.dispatch.empty()) {
    // NPU: las bibliotecas «dispatch» del fabricante van junto a LiteRT-LM o en vendors/<fabricante>/dispatch
    c.dispatch = c.lib.parent_path();
    for (auto& d : glob(c.lib.parent_path() / "vendors", "dispatch", true))
      if (is_dir(d)) {
        c.dispatch = d;
        break;
      }
  }
  int port = std::atoi(a["puerto"].c_str());
  std::unique_ptr<Motor> m;
  double t0 = now();
  try {
    m = motor == "litert" ? abrir_litert(c) : motor == "genie" ? abrir_genie(c) : abrir_onnx(c);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ERROR al cargar %s con %s: %s\n", S(c.modelo.filename()).c_str(), c.acelerador.c_str(), e.what());
    return 4;
  }
  std::printf("motor %s listo: acelerador=%s modelo=%s en %.1fs\n", motor.c_str(), m->acelerador().c_str(),
              S(c.modelo.filename()).c_str(), now() - t0);
  std::fflush(stdout);
  std::mutex gen_mu;   // un solo turno a la vez: la API de los motores no admite llamadas solapadas
  std::string model_id = stem(c.modelo);
  httplib::Server srv;
  srv.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content(jdump({{"status", "ok"}, {"motor", motor}, {"acelerador", m->acelerador()}}), "application/json");
  });
  srv.Get("/v1/models", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content(jdump({{"object", "list"}, {"data", json::array({{{"id", model_id}, {"object", "model"}}})}}),
                    "application/json");
  });
  srv.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
    json body = jparse(req.body);
    if (!body.is_object() || !body.contains("messages")) {
      res.status = 400;
      res.set_content(jdump({{"error", {{"message", "falta messages"}}}}), "application/json");
      return;
    }
    Params p;
    p.max_tokens = jget<int>(body, "max_tokens", 1024);
    p.temperature = jget<double>(body, "temperature", 0.7);
    p.top_p = jget<double>(body, "top_p", 0.95);
    p.top_k = jget<int>(body, "top_k", 40);
    p.seed = jget<long long>(body, "seed", -1);
    json messages = body["messages"];
    std::string id = "chatcmpl-" + std::to_string((long long)(now() * 1000));
    if (!jget<bool>(body, "stream", false)) {
      std::lock_guard<std::mutex> lk(gen_mu);
      std::string text;
      try {
        m->generar(messages, p, [&](const std::string& piece) {
          text += piece;
          return true;
        });
      } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(jdump({{"error", {{"message", e.what()}}}}), "application/json");
        return;
      }
      res.set_content(jdump({{"id", id},
                             {"object", "chat.completion"},
                             {"model", model_id},
                             {"choices", json::array({{{"index", 0},
                                                       {"message", {{"role", "assistant"}, {"content", text}}},
                                                       {"finish_reason", "stop"}}})}}),
                      "application/json");
      return;
    }
    res.set_chunked_content_provider("text/event-stream", [&, messages, p, id](size_t, httplib::DataSink& sink) {
      std::lock_guard<std::mutex> lk(gen_mu);
      auto chunk = [&](const json& delta, const json& finish) {
        return sse({{"id", id},
                    {"object", "chat.completion.chunk"},
                    {"model", model_id},
                    {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}});
      };
      std::string first = chunk({{"role", "assistant"}}, nullptr);
      sink.write(first.data(), first.size());
      try {
        m->generar(messages, p, [&](const std::string& piece) {
          std::string s = chunk({{"content", piece}}, nullptr);
          return sink.write(s.data(), s.size());
        });
      } catch (const std::exception& e) {
        std::string s = sse({{"error", {{"message", e.what()}}}});
        sink.write(s.data(), s.size());
      }
      std::string end = chunk(json::object(), "stop") + "data: [DONE]\n\n";
      sink.write(end.data(), end.size());
      sink.done();
      return true;
    });
  });
  if (!srv.listen("127.0.0.1", port)) {
    std::fprintf(stderr, "No se pudo escuchar en el puerto %d\n", port);
    return 5;
  }
  return 0;
}

}  // namespace nx::motores

// ======================================================================== backend
namespace nx::llm {

static const auto LM = log::get("motores");

MotorBackend::MotorBackend(std::string motor, Resolver resolve, std::function<json()> profile)
    : OpenAICompatBackend("http://127.0.0.1:0/v1"), motor_(std::move(motor)), resolve_(std::move(resolve)),
      profile_(std::move(profile)) {}

MotorBackend::~MotorBackend() { stop(); }

bool MotorBackend::available() { return motores::lib_path(motor_).has_value() && resolve_("").has_value(); }

std::vector<std::string> MotorBackend::models() {
  auto r = resolve_("");
  if (!r) return {};
  return {jstr(r->second, "id")};
}

json MotorBackend::effective() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return effective_;
}

void MotorBackend::stop() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (proc_ && proc_->running()) {
    proc_->terminate();
    if (!proc_->wait(10)) proc_->kill();
  }
  proc_.reset();
  key_.clear();
}

void MotorBackend::ensure(const std::string& model, const json& opts) {
  auto resolved = resolve_(model);
  const motores::Info* inf = motores::info(motor_);
  if (!resolved) throw std::runtime_error("No hay modelo " + inf->formato + " activo. Descarga uno en Modelos.");
  auto& [path, entry] = *resolved;
  if (!motores::lib_path(motor_)) throw std::runtime_error(inf->nombre + " no esta instalado en este equipo (Sistema > Runtimes)");
  int n_ctx = (int)jnum(opts.value("n_ctx", json(4096)), 4096), hilos = (int)jnum(opts.value("n_threads", json(0)), 0);
  std::string key = cat(path, "|", n_ctx, "|", hilos);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (proc_ && proc_->running() && key_ == key) return;
  stop();
  if (key != cat(jstr(effective_, "key"))) fallidos_.clear();
  fs::path log_path = paths::logs_dir() / ("motor-" + motor_ + ".log");
  std::vector<std::string> errores;
  for (auto& acel : motores::orden(motor_, profile_(), entry)) {
    if (std::find(fallidos_.begin(), fallidos_.end(), acel) != fallidos_.end()) continue;
    // cada proveedor de ONNX Runtime va en su carpeta (cuda, dml, qnn, openvino...): la de este acelerador
    std::string var = motores::variante_para(motor_, acel, entry);
    auto vs = motores::variantes(motor_);
    bool embebido = vs.size() == 1 && vs[0] == "embedded";
    if (!var.empty() && var != "cpu" && !embebido && std::find(vs.begin(), vs.end(), var) == vs.end()) {
      errores.push_back(acel + ": no va incluido en este paquete (falta " + motor_ + "/" + var + ")");
      continue;
    }
    auto lib = motores::lib_path(motor_, var);
    if (!lib) continue;
    port = net::free_port(8191);
    std::vector<std::string> cmd = {S(paths::exe_path()), "motor", motor_, "--modelo", path, "--puerto",
                                    std::to_string(port), "--acelerador", acel, "--lib", S(*lib), "--ctx",
                                    std::to_string(n_ctx), "--cache", S(paths::sub({"cache", "motores", motor_}))};
    if (hilos > 0) cmd.insert(cmd.end(), {"--hilos", std::to_string(hilos)});
    proc::Options po;
    po.cwd = lib->parent_path();
    po.output_file = log_path;
    po.env = runtimes::lib_env(*lib);
    // ONNX Runtime GenAI busca libonnxruntime.so(.1) a su lado; los paquetes la traen con la version en el nombre
    if (motor_ == "onnx" && !sys::is_windows())
      for (auto& o : glob(lib->parent_path(), "libonnxruntime.*"))
        if (!contains(S(o.filename()), "genai") && !contains(S(o.filename()), "providers")) po.env["ORT_LIB_PATH"] = S(o);
    if (sys::is_android()) {
      // NPU: las bibliotecas del fabricante (QNN, NeuroPilot...) estan en /vendor
      po.env["LD_LIBRARY_PATH"] = S(lib->parent_path()) + ":/vendor/lib64:/system/vendor/lib64:/system/lib64";
      po.env["ADSP_LIBRARY_PATH"] = S(lib->parent_path()) + ";/vendor/dsp/cdsp;/vendor/lib/rfsa/adsp;/dsp";
    }
    LM.info("Arrancando ", inf->nombre, " [", acel, "]: ", path);
    proc_ = std::make_unique<proc::Process>();
    if (!proc_->start(cmd, po)) {
      errores.push_back(acel + ": " + proc_->error());
      proc_.reset();
      break;
    }
    double t0 = now();
    bool ok = false;
    while (now() - t0 < 600) {
      if (!proc_->running()) break;
      if (http::get(cat("http://127.0.0.1:", port, "/health"), {}, 1).status == 200) {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (ok) {
      base_url_ = cat("http://127.0.0.1:", port, "/v1");
      key_ = key;
      std::string requested = motores::orden(motor_, profile_(), entry)[0];
      effective_ = json::object();
      effective_["motor"] = motor_;
      effective_["acelerador"] = acel;
      effective_["variante"] = var;
      effective_["lib"] = S(*lib);
      effective_["requested"] = requested;
      effective_["device"] = acel == "npu" ? "NPU" : acel == "cpu" ? "CPU" : "GPU";
      effective_["ok"] = acel == requested;
      effective_["at"] = now();
      effective_["key"] = key;
      effective_["fallidos"] = fallidos_;
      LM.info(inf->nombre, " listo en ", acel, " (", fnum(now() - t0, 1), "s)");
      return;
    }
    std::string tail = read_text(log_path);
    tail = valid_utf8(tail.size() > 600 ? tail.substr(tail.size() - 600) : tail);
    if (proc_->running()) proc_->kill();
    proc_.reset();
    fallidos_.push_back(acel);
    errores.push_back(acel + ": " + trim(uright(tail, 300)));
    LM.warning(inf->nombre, " no arranco en ", acel, ": se prueba el siguiente acelerador");
  }
  effective_ = json::object();
  effective_["key"] = key;
  effective_["fallidos"] = fallidos_;
  throw std::runtime_error(inf->nombre + " no pudo cargar el modelo: " + join(errores, " | "));
}

void MotorBackend::stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) {
  ensure(model, opts.o);
  json msgs = messages;
  for (auto& m : msgs) m["images"] = json::array();
  Opts o2 = opts;
  o2.o.erase("tool_specs");   // sin herramientas nativas: el protocolo de texto de Nexus-Office
  bool produced = false;
  Sink wrap = [&](const std::string& p) {
    produced = true;
    out(p);
  };
  try {
    OpenAICompatBackend::stream_chat(msgs, "local", o2, wrap);
  } catch (const std::exception&) {
    bool alive;
    std::string acel;
    {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      alive = proc_ && proc_->running();
      acel = jstr(effective_, "acelerador");
    }
    if (produced || alive) throw;
    // el motor se cayo (driver de NPU/GPU): se apunta y se reintenta con el siguiente acelerador
    LM.warning("El motor ", motor_, " se detuvo en ", acel, " durante la respuesta; se reintenta");
    {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      if (!acel.empty() && acel != "cpu") fallidos_.push_back(acel);
      key_.clear();
    }
    ensure(model, opts.o);
    OpenAICompatBackend::stream_chat(msgs, "local", o2, out);
  }
}

}  // namespace nx::llm
