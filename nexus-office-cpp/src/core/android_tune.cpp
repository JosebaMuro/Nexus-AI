// Autoajuste continuo de Nexus-Office en la app Android autonoma (android_tune.py).
//
// Al arrancar y mientras Nexus-Office esta en marcha, se adapta solo al movil:
// * Nucleos: cuenta los nucleos rapidos (los grandes y el principal) por el modelo de CPU de /proc/cpuinfo o, si no,
//   por la frecuencia maxima de cada nucleo. Los nucleos pequenos frenan a llama.cpp: se empieza con los rapidos.
// * Hilos medidos: prueba unos pocos numeros de hilos con las respuestas reales (velocidad de generacion de
//   llama-server) y se queda con el mas rapido. Lo guarda por modelo, variante de llama.cpp y version de la app, y
//   repasa de vez en cuando.
// * Temperatura y bateria: la app escribe el estado termico, la bateria y el ahorro de energia en device_state.json.
//   Con el movil caliente o la bateria baja usa menos hilos, y los recupera despues.
// * Memoria: si Android se queda sin RAM libre, reduce el contexto para el siguiente arranque de llama-server.
// * NPU, GPU o CPU: si el movil tiene una NPU (Hexagon) o una GPU que llama.cpp puede usar (hardware/android_accel),
//   mide unas respuestas con ella y otras con la CPU y se queda con lo mas rapido; despues ajusta los hilos.
// * Modelo: si va muy lento propone uno mas pequeno; si sobra velocidad y RAM, uno mayor (advice).
// Los cambios se aplican entre respuestas: llama-server se reinicia con los nuevos valores en la siguiente.
#include "core/android_tune.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

#include "core/db.h"
#include "core/services.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/models.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/http.h"
#include "util/sysinfo.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::android_tune {

namespace {

const auto L = log::get("android");

// CPU part de los nucleos pequenos de ARM (Cortex-A32/A35/A53/A55/A510/A520): el resto son grandes o principales
const std::set<std::string> LITTLE_PARTS = {"0xd01", "0xd03", "0xd04", "0xd05", "0xd46", "0xd80"};
constexpr size_t MIN_SAMPLES = 3;    // respuestas por numero de hilos antes de decidir
constexpr int MIN_TOKENS = 16;       // respuestas mas cortas no dan una velocidad fiable
constexpr int REEXPLORE_AFTER = 80;  // respuestas con el mejor numero de hilos antes de volver a medir los demas
constexpr double SLOW_TPS = 4.0, FAST_TPS = 14.0;
const std::vector<std::string> ANDROID_MODELS = {"qwen3.5-0.8b", "qwen3.5-2b", "qwen3.5-4b"};   // de menor a mayor
// escalera de la version anterior (Qwen3-VL Thinking): quien ya lo tiene descargado sube o baja dentro de ella
const std::vector<std::string> PREVIOUS_MODELS = {"qwen3-vl-2b-thinking", "qwen3-vl-4b-thinking"};
constexpr double STATE_MAX_AGE = 300;   // segundos: un estado del movil mas viejo no se tiene en cuenta

std::string f1(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

double median(const json& arr) {
  std::vector<double> v;
  if (arr.is_array())
    for (auto& x : arr) v.push_back(jnum(x));
  return median(v);
}

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int index_of(const std::vector<std::string>& v, const std::string& x) {
  return (int)(std::find(v.begin(), v.end(), x) - v.begin());
}

}  // namespace

// ------------------------------------------------------------------ deteccion de nucleos
json core_layout(const fs::path& cpuinfo, const fs::path& sys_cpu) {
  int total = std::max(1, sys::cores_logical());
  std::vector<std::string> parts;
  if (auto t = read_file(cpuinfo))
    for (auto& line : splitlines(*t))
      if (starts_with(lower(line), "cpu part") && contains(line, ":"))
        parts.push_back(lower(trim(line.substr(line.find(':') + 1))));
  if (parts.size() >= 2) {
    int fast = 0;
    for (auto& p : parts) fast += !LITTLE_PARTS.count(p);
    if (fast) return {{"total", (int)parts.size()}, {"fast", fast}, {"source", "cpuinfo"}};
  }
  std::vector<long> freqs;
  std::vector<fs::path> dirs;
  static const Rx CPU(R"(cpu[0-9]+)");
  std::error_code ec;
  if (is_dir(sys_cpu))
    for (auto& e : fs::directory_iterator(sys_cpu, ec))
      if (std::string n = S(e.path().filename()); CPU.match(n)) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (auto& d : dirs)
    if (auto t = read_file(d / "cpufreq" / "cpuinfo_max_freq")) {
      try {
        freqs.push_back(std::stol(trim(*t)));
      } catch (...) {
      }
    }
  if (freqs.size() >= 2) {
    long low = *std::min_element(freqs.begin(), freqs.end());
    int fast = 0;
    for (long f : freqs) fast += f > low;
    if (fast >= 2) return {{"total", (int)freqs.size()}, {"fast", fast}, {"source", "cpufreq"}};
    // un solo nucleo por encima del resto no basta: se usan tambien los del grupo siguiente
    return {{"total", (int)freqs.size()}, {"fast", std::min((int)freqs.size(), 4)}, {"source", "cpufreq"}};
  }
  return {{"total", total}, {"fast", std::min(total, 4)}, {"source", "none"}};
}

std::vector<int> thread_candidates(const json& layout) {
  int fast = std::max(1, (int)jnum(layout["fast"])), total = std::max(1, (int)jnum(layout["total"]));
  std::set<int> c = {fast > 2 ? std::max(2, fast - 1) : fast, fast, std::min({total, fast + 2, 8})};
  return {c.begin(), c.end()};
}

// ------------------------------------------------------------------ estado del movil
json device_state(const fs::path& path, double now_s) {
  auto t = read_file(path);
  json st = t ? jparse(*t) : json();
  if (!st.is_object()) return json::object();
  double ts = jnum(st.value("t", json(0)));
  if ((now_s ? now_s : now()) - ts / (ts > 1e11 ? 1000 : 1) > STATE_MAX_AGE) return json::object();
  return st;
}

std::pair<int, std::string> throttle(int threads, const json& st) {
  int thermal = (int)jnum(st.value("thermal", json(0)));
  int battery = st.contains("battery") && !st["battery"].is_null() ? (int)jnum(st["battery"]) : 100;
  if (thermal >= 3) return {std::max(1, (threads + 1) / 2), "movil caliente (estado termico " + std::to_string(thermal) + ")"};
  bool ps = truthy(st.value("power_save", json(false)));
  if (ps || (battery <= 15 && !truthy(st.value("charging", json(false)))))
    return {std::max(1, (threads + 1) / 2), ps ? std::string("ahorro de energia") : "bateria al " + std::to_string(battery) + "%"};
  if (thermal == 2) return {std::max(1, threads - 1), "movil templado"};
  return {threads, ""};
}

// ------------------------------------------------------------------ eleccion de hilos y de modelo
double gen_tps(const json& call) {
  double gen = jnum(call.value("seconds", json(0))) - jnum(call.value("ttft", json(0)));
  return gen > 0.05 ? jnum(call["tokens"]) / gen : jnum(call.value("tps", json(0.0)));
}

std::pair<int, bool> choose(const std::vector<int>& cands, const json& trials) {
  for (int c : cands) {
    std::string k = std::to_string(c);
    if (!trials.contains(k) || trials[k].size() < MIN_SAMPLES) return {c, true};
  }
  return {*std::max_element(cands.begin(), cands.end(),
                            [&](int a, int b) { return median(trials[std::to_string(a)]) < median(trials[std::to_string(b)]); }),
          false};
}

json model_advice(const std::string& model, double best_tps, double ram_gb, const json& catalog) {
  std::map<std::string, json> by_id;
  for (auto& r : catalog) by_id[jstr(r, "id")] = r;
  auto entry = [&](const std::string& target, const std::string& why) {
    json r = by_id.count(target) ? by_id[target] : json{{"id", target}};
    return json{{"from", model}, {"id", target}, {"name", jstr(r, "name", target)},
                {"size_mb", r.value("size_mb", json(0))}, {"installed", truthy(r.value("installed", json(false)))},
                {"reason", why}};
  };
  const json& legacy = models::legacy_llm();
  if (legacy.contains(model) && best_tps > 0) {   // modelo sin razonamiento de una version anterior
    std::string l = legacy[model].get<std::string>();
    std::string target = has(ANDROID_MODELS, l) ? l : ANDROID_MODELS[0];
    if (best_tps < SLOW_TPS) target = ANDROID_MODELS[0];
    return entry(target, "tu modelo actual no razona: este ve imagenes y razona paso a paso antes de responder");
  }
  const std::vector<std::string>* ladder = has(ANDROID_MODELS, model) ? &ANDROID_MODELS
                                           : has(PREVIOUS_MODELS, model) ? &PREVIOUS_MODELS : nullptr;
  if (!ladder || best_tps <= 0) return nullptr;
  int i = index_of(*ladder, model);
  if (best_tps < SLOW_TPS && i > 0)
    return entry((*ladder)[i - 1], "va a " + f1(best_tps) + " tokens/s: uno mas pequeno respondera mas rapido");
  if (best_tps > FAST_TPS && i + 1 < (int)ladder->size()) {
    const std::string& next = (*ladder)[i + 1];
    double need = by_id.count(next) ? jnum(by_id[next].value("min_ram_gb", json(99))) : 99;
    if (ram_gb >= need) return entry(next, "va a " + f1(best_tps) + " tokens/s y hay RAM: uno mayor dara mejores respuestas");
  }
  return nullptr;
}

std::pair<bool, std::string> warmup_ok(const json& st, double avail_gb, double model_gb) {
  if ((int)jnum(st.value("thermal", json(0))) >= 2) return {false, "movil caliente"};
  if (st.contains("battery") && !st["battery"].is_null() && (int)jnum(st["battery"]) < 30 &&
      !truthy(st.value("charging", json(false))))
    return {false, "bateria al " + std::to_string((int)jnum(st["battery"])) + "%"};
  if (truthy(st.value("power_save", json(false)))) return {false, "ahorro de energia"};
  if (avail_gb < model_gb + 0.5) return {false, "poca RAM libre (" + f1(avail_gb) + " GB)"};
  return {true, ""};
}

// ------------------------------------------------------------------ LiveTuner
LiveTuner::LiveTuner(Services& s, double interval, json layout)
    : s_(s), interval_(interval), layout_(layout.is_null() ? core_layout() : std::move(layout)) {
  cands_ = thread_candidates(layout_);
  file_ = paths::data_dir() / "android_tune.json";
  std::string sf = env("NEXUS_DEVICE_STATE");
  state_file = !sf.empty() ? P(sf) : paths::data_dir().parent_path() / "device_state.json";
  auto t = read_file(file_);
  state_ = t ? jparse(*t) : json();
  if (!state_.is_object()) state_ = json::object();
  json llm = s_.llm->llm_profile();
  gpu_device_ = truthy(llm.value("gpu_layers", json(0))) ? jstr(llm, "device") : "";
}

LiveTuner::~LiveTuner() { stop(); }

void LiveTuner::save() {
  json& log = state_["log"];
  if (!log.is_array()) log = json::array();
  while (log.size() > 30) log.erase(log.begin());
  write_file(file_, jdump(state_, 1));
}

std::string LiveTuner::variant() {
  json llm = s_.llm->llm_profile();
  if (truthy(llm.value("gpu_layers", json(0))) && !jstr(llm, "device").empty() && !env("NEXUS_RUNTIME_LLAMA_GPU").empty())
    return "gpu:" + jstr(llm, "device");
  std::string cpu = env("NEXUS_RUNTIME_LLAMA_CPU");
  if (cpu.empty()) cpu = env("NEXUS_RUNTIME_LLAMA");
  return cpu.empty() ? "" : S(P(cpu).filename());
}

std::string LiveTuner::key() {
  std::vector<std::string> c;
  for (int x : cands_) c.push_back(std::to_string(x));
  return jstr(s_.models->active(), "llm") + "|" + variant() + "|" + NEXUS_VERSION + "|[" + join(c, ", ") + "]";
}

void LiveTuner::note(const std::string& msg) {
  last_reason_ = msg;
  if (!state_["log"].is_array()) state_["log"] = json::array();
  state_["log"].push_back({{"t", (long long)std::llround(now())}, {"msg", msg}});
  L.info("Autoajuste: ", msg);
  auto& r = s_.tuning_reasons;
  r.push_back(msg);
  if (r.size() > 12) r.erase(r.begin(), r.end() - 12);
}

bool LiveTuner::set_gpu(bool on) {
  json llm = s_.llm->llm_profile();
  int want_l = on ? -1 : 0;
  std::string want_d = on ? gpu_device_ : "";
  if ((int)jnum(llm.value("gpu_layers", json(0))) == want_l && jstr(llm, "device") == want_d) return false;
  s_.llm->update_llm_profile({{"gpu_layers", want_l}, {"device", want_d}});
  if (s_.monitor && s_.monitor->settings.is_object()) s_.monitor->settings["gpu_layers"] = want_l;
  return true;
}

bool LiveTuner::accel_step(const std::vector<json>& fresh) {
  json llm = s_.llm->llm_profile();
  if (truthy(llm.value("gpu_layers", json(0))) && !jstr(llm, "device").empty() && jstr(llm, "device") != gpu_device_)
    gpu_device_ = jstr(llm, "device");   // la NPU fallo al arrancar y llamaserver paso a la GPU: se mide esta
  if (gpu_device_.empty()) return false;
  std::string k = jstr(s_.models->active(), "llm") + "|" + gpu_device_ + "|" + NEXUS_VERSION;
  json acc = state_.value("accel", json());
  if (!acc.is_object() || jstr(acc, "key") != k)
    acc = {{"key", k}, {"gpu", json::array()}, {"cpu", json::array()}, {"choice", nullptr}};
  if (jstr(llm, "device").empty() && (int)jnum(llm.value("gpu_layers", json(0))) == 0 && jstr(acc, "mode") == "gpu") {
    acc["choice"] = "cpu";   // la GPU fallo al arrancar (android_accel::mark_gpu_broken)
    note("La GPU fallo con llama.cpp en este movil: el modelo sigue en la CPU");
  }
  std::string mode = jstr(acc, "mode");
  if (acc["choice"].is_null()) {
    if (!mode.empty() && !fresh.empty() && !truthy(state_.value("throttled", json(""))))
      for (auto& c : fresh) acc[mode].push_back(round_to(gen_tps(c), 2));
    if (acc["gpu"].size() < MIN_SAMPLES) {
      mode = "gpu";
    } else if (acc["cpu"].size() < MIN_SAMPLES) {
      mode = "cpu";
    } else {
      double g = median(acc["gpu"]), c = median(acc["cpu"]);
      acc["choice"] = g >= c ? "gpu" : "cpu";
      note(std::string(g >= c ? "GPU" : "CPU") + " mas rapida: " + f1(std::max(g, c)) + " tokens/s (GPU " + f1(g) +
           ", CPU " + f1(c) + ")");
    }
  }
  if (!acc["choice"].is_null()) mode = jstr(acc, "choice");
  if (s_.llm->inflight == 0 && set_gpu(mode == "gpu") && acc["choice"].is_null())
    note("Midiendo la velocidad con la " + (mode == "gpu" ? "GPU (" + gpu_device_ + ")" : std::string("CPU")));
  acc["mode"] = mode;
  state_["accel"] = acc;
  return acc["choice"].is_null();
}

json LiveTuner::step() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::vector<json> calls = s_.monitor ? s_.monitor->llm_calls() : std::vector<json>();
  std::vector<json> fresh;
  for (size_t i = std::min(seen_, calls.size()); i < calls.size(); ++i) {
    std::string b = jstr(calls[i], "backend");
    if ((b == "llamaserver" || b == "llamacpp") && jnum(calls[i].value("tokens", json(0))) >= MIN_TOKENS)
      fresh.push_back(calls[i]);
  }
  seen_ = calls.size();

  // 0) GPU o CPU: mientras se comparan, los hilos no se tocan
  if (accel_step(fresh)) {
    applied_ = 0;
    save();
    return report();
  }

  std::string k = key();
  if (jstr(state_, "key") != k) {
    if (!jstr(state_, "key").empty()) note("Modelo, variante de llama.cpp o version nuevos: se vuelven a medir los hilos");
    state_ = {{"key", k}, {"trials", json::object()}, {"since_best", 0},
              {"log", state_.value("log", json::array())}, {"pending_switch", state_.value("pending_switch", json())},
              {"accel", state_.value("accel", json())}};
  }
  // ordered_json guarda las claves en un vector: se crean todas antes de tomar la referencia a «trials»
  for (const char* k : {"log", "best", "throttled", "advice", "since_best", "pending_switch", "accel"})
    if (!state_.contains(k)) state_[k] = nullptr;
  if (!state_["trials"].is_object()) state_["trials"] = json::object();
  json& trials = state_["trials"];
  json st = device_state(state_file);

  // 1) velocidades de las respuestas nuevas con los hilos que habia (si no estaban limitados)
  if (!fresh.empty() && applied_ && !truthy(state_.value("throttled", json("")))) {
    json& lst = trials[std::to_string(applied_)];
    if (!lst.is_array()) lst = json::array();
    for (auto& c : fresh) lst.push_back(round_to(gen_tps(c), 2));
    while (lst.size() > 8) lst.erase(lst.begin());
    state_["since_best"] = (int)jnum(state_.value("since_best", json(0))) + (int)fresh.size();
  }

  // 2) hilos: explorar, despues el mejor; repasar los demas de vez en cuando
  auto [target, exploring] = choose(cands_, trials);
  if (!exploring && jnum(state_.value("since_best", json(0))) >= REEXPLORE_AFTER) {
    std::vector<std::string> others;
    for (int c : cands_)
      if (c != target) {
        trials.erase(std::to_string(c));
        others.push_back(std::to_string(c));
      }
    state_["since_best"] = 0;
    note("Repaso periodico: se vuelve a medir con " + join(others, ", ") + " hilos");
    std::tie(target, exploring) = choose(cands_, trials);
  }
  if (!exploring && (state_.value("best", json()).is_null() || (int)jnum(state_["best"]) != target)) {
    note("Mas rapido con " + std::to_string(target) + " hilos (" + f1(median(trials[std::to_string(target)])) +
         " tokens/s)");
    state_["best"] = target;
  }

  // 3) temperatura y bateria
  auto [limited, why] = throttle(target, st);
  if (why.empty() != jstr(state_, "throttled").empty())
    note(!why.empty() ? "Menos hilos (" + std::to_string(limited) + ") por " + why
                      : "Vuelve a " + std::to_string(target) + " hilos: movil en condiciones normales");
  state_["throttled"] = why;

  // 4) memoria libre de Android: menos contexto para el siguiente arranque
  json llm = s_.llm->llm_profile();
  double avail = sys::memory().available / 1073741824.0;
  if (avail <= 0) avail = 99;
  int n_ctx = (int)jnum(llm.value("n_ctx", json(4096)));
  if (avail < 0.6 && n_ctx > 2048) {
    n_ctx = std::max(2048, n_ctx / 2);
    s_.llm->update_llm_profile({{"n_ctx", n_ctx}});
    note("Poca RAM libre (" + f1(avail) + " GB): contexto reducido a " + std::to_string(n_ctx));
  }

  // 5) aplicar entre respuestas (llama-server se reinicia en la siguiente con los nuevos valores)
  int threads = (int)jnum(llm.value("n_threads", json(0)));
  if (threads != limited && s_.llm->inflight == 0) {
    if (exploring && why.empty()) note("Midiendo la velocidad con " + std::to_string(limited) + " hilos");
    s_.llm->update_llm_profile({{"n_threads", limited}});
    threads = limited;
    if (s_.monitor && s_.monitor->settings.is_object()) s_.monitor->settings["n_threads"] = limited;
  }
  if (threads == limited) applied_ = target;

  // 6) modelo: cambiar al aceptado cuando termine de descargarse; proponer otro si va lento o sobra velocidad
  std::string pending = jstr(state_, "pending_switch");
  if (!pending.empty() && !s_.models->catalog.get(pending).is_null() && s_.llm->inflight == 0) activate(pending);
  state_["advice"] = nullptr;
  std::string tk = std::to_string(target);
  if (!exploring && trials.contains(tk) && !trials[tk].empty())
    state_["advice"] = model_advice(jstr(s_.models->active(), "llm"), median(trials[tk]),
                                    jnum(s_.profile.value("ram_gb", json(0))), s_.models->recommended());
  save();
  return report();
}

void LiveTuner::activate(const std::string& mid) {
  s_.models->activate(mid);
  s_.llm->rebuild();
  state_["pending_switch"] = nullptr;
  note("Modelo cambiado a " + mid);
}

json LiveTuner::switch_model(const std::string& mid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!has(ANDROID_MODELS, mid) && !has(PREVIOUS_MODELS, mid))
    throw std::invalid_argument("Modelo no previsto para Android: " + mid);
  if (!s_.models->catalog.get(mid).is_null()) {
    activate(mid);
    save();
    return {{"activated", mid}};
  }
  state_["pending_switch"] = mid;
  save();
  return {{"job", s_.models->download_recommended(mid)}};
}

void LiveTuner::reset_trials() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  state_["trials"] = json::object();
  state_["best"] = nullptr;
}

json LiveTuner::report() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json trials = state_.value("trials", json::object());
  if (!trials.is_object()) trials = json::object();
  json tps = json::object();
  for (auto& [k, v] : trials.items())
    if (!v.empty()) tps[k] = round_to(median(v), 2);
  json accel = json::object();
  if (state_.value("accel", json()).is_object())
    for (auto& [k, v] : state_["accel"].items())
      if (k != "key") accel[k] = v;
  json log = state_.value("log", json::array());
  json last = json::array();
  for (size_t i = log.size() > 10 ? log.size() - 10 : 0; i < log.size(); ++i) last.push_back(log[i]);
  json llm = s_.llm->llm_profile();
  std::string th = jstr(state_, "throttled");
  return {{"layout", layout_}, {"candidates", cands_}, {"n_threads", llm.value("n_threads", json())},
          {"n_ctx", llm.value("n_ctx", json())}, {"best", state_.value("best", json())},
          {"exploring", choose(cands_, trials).second}, {"tps", tps}, {"throttled", th},
          {"device", device_state(state_file)}, {"llama_variant", variant()}, {"gpu", gpu_device_},
          {"accel", accel}, {"advice", state_.value("advice", json())},
          {"pending_switch", state_.value("pending_switch", json())}, {"last", last_reason_}, {"log", last}};
}

void LiveTuner::loop() {
  while (!stop_) {
    {
      std::unique_lock<std::mutex> lk(wait_mu_);
      cv_.wait_for(lk, std::chrono::milliseconds((long)(interval_ * 1000)), [this] { return stop_.load(); });
    }
    if (stop_) break;
    try {
      step();
    } catch (const std::exception& e) {
      L.warning("Autoajuste: ", e.what());
    }
  }
}

void LiveTuner::start() {
  try {
    step();   // aplica ya lo aprendido en sesiones anteriores, antes de la primera respuesta
  } catch (...) {
  }
  th_ = std::thread([this] { loop(); });
}

void LiveTuner::stop() {
  stop_ = true;
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

// ------------------------------------------------------------------ optimizacion tras cada arranque
static int clean_tmp(double max_age_s = 86400) {
  fs::path tmp = P(env("TMPDIR"));
  if (env("TMPDIR").empty() || !is_dir(tmp)) return 0;
  int n = 0;
  double limit = now() - max_age_s;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(tmp, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && mtime(it->path()) < limit && fs::remove(it->path(), ec)) ++n;
  }
  return n;
}

static void trim_logs(uint64_t max_bytes = 4u << 20) {
  for (auto& f : glob(paths::logs_dir(), "*.log")) {
    if (fsize(f) <= max_bytes) continue;
    std::string data = read_text(f);
    write_file(f, data.substr(data.size() - max_bytes / 4));
  }
}

std::vector<std::string> after_start(Services& s, LiveTuner* tuner) {
  std::vector<std::string> done;
  double t0 = now();
  int n = clean_tmp();
  if (n) done.push_back(std::to_string(n) + " temporales borrados");
  try {
    trim_logs();
  } catch (...) {
  }
  try {
    s.db->execute("PRAGMA optimize");
    s.db->execute("PRAGMA wal_checkpoint(TRUNCATE)");
    done.push_back("base de datos optimizada");
  } catch (const std::exception& e) {
    L.debug("PRAGMA optimize: ", e.what());
  }
  if (tuner) {
    json r = tuner->step();
    std::string th = jstr(r, "throttled"), var = jstr(r, "llama_variant");
    done.push_back(cat(jdump(r["n_threads"]), " hilos", th.empty() ? "" : " (limitado: " + th + ")", ", contexto ",
                       jdump(r["n_ctx"]), ", ", var.empty() ? "sin llama.cpp" : var));
  }
  std::string backend, model;
  try {
    std::tie(backend, model) = s.llm->select();
  } catch (...) {
  }
  if (backend == "llamaserver") {
    double avail = sys::memory().available / 1073741824.0;
    if (avail <= 0) avail = 99;
    auto resolved = s.llm->resolver("gguf")(model);
    double size = resolved && is_file(P(resolved->first)) ? fsize(P(resolved->first)) / 1073741824.0 : 0.0;
    auto [ok, why] = warmup_ok(tuner ? device_state(tuner->state_file) : json::object(), avail, size);
    if (ok) {
      double t1 = now();
      ++s.llm->inflight;   // el autoajuste no cambia hilos mientras tanto
      try {
        auto ls = std::dynamic_pointer_cast<llm::LlamaServerBackend>(s.llm->backend("llamaserver"));
        ls->ensure(model, s.llm->options());
        // una peticion de 1 token lee los pesos del disco (mmap): la primera respuesta real no lo paga
        http::post_json(cat("http://127.0.0.1:", ls->port, "/completion"), {{"prompt", "Hola"}, {"n_predict", 1}}, {},
                        300);
        done.push_back("modelo precargado en " + std::to_string((long)std::lround(now() - t1)) + " s");
      } catch (const std::exception& e) {
        done.push_back("no se pudo precargar el modelo: " + uleft(e.what(), 120));
      }
      --s.llm->inflight;
    } else {
      done.push_back("modelo sin precargar (" + why + ")");
    }
  }
  std::string msg = "Optimizacion tras el arranque (" + std::to_string((long)std::lround(now() - t0)) + " s): " +
                    join(done, "; ");
  L.info(msg);
  s.tuning_reasons.push_back(msg);
  return done;
}

}  // namespace nx::android_tune
