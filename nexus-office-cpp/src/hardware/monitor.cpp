// Monitorizacion en vivo y resumen de sesion para el autoajuste (monitor.py).
#include <chrono>
#include <cmath>

#include "hardware/hardware.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::hw {

static const auto LM = log::get("monitor");
static const size_t HISTORY_MAX = 50;

static fs::path history_file() { return paths::data_dir() / "perf_history.json"; }
static fs::path running_marker() { return paths::data_dir() / ".running"; }

json load_history() {
  auto t = read_file(history_file());
  if (!t) return json::array();
  json j = jparse(*t);
  return j.is_array() ? j : json::array();
}

Monitor::Monitor(double interval, json s) : settings(std::move(s)), interval_(interval), started_(now()) {
  session_id = uuid_hex().substr(0, 10);
  nvsmi_ = find_nvidia_smi();
  previous_unclean = path_exists(running_marker());
  if (previous_unclean) {
    json j = jparse(read_text(running_marker()));
    previous_info = j.is_object() ? j : json::object();
  }
}

Monitor::~Monitor() {
  stop_ = true;
  if (th_.joinable()) th_.join();
}

void Monitor::start() {
  heartbeat();
  sys::cpu_percent();
  th_ = std::thread([this] { loop(); });
}

json Monitor::stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  json s = summary(true);
  json hist = load_history();
  hist.push_back(s);
  if (hist.size() > HISTORY_MAX) hist = json(std::vector<json>(hist.end() - HISTORY_MAX, hist.end()));
  write_file(history_file(), jdump(hist, 1));
  remove_file(running_marker());
  LM.info("Sesion ", session_id, " guardada (", fnum(jnum(s["avg_tps"]), 1), " tok/s medio)");
  return s;
}

json Monitor::gpu_sample() {
  if (nvsmi_.empty()) return json::object();
  auto r = proc::run({nvsmi_, "--query-gpu=utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"},
                     3);
  if (!r.ok()) return json::object();
  auto lines = splitlines(trim(r.out));
  if (lines.empty()) return json::object();
  auto p = split(lines[0], ",");
  if (p.size() < 3) return json::object();
  double util = std::atof(p[0].c_str()), used = std::atof(p[1].c_str()), total = std::atof(p[2].c_str());
  if (total <= 0) return json::object();
  return {{"gpu_util", util}, {"vram_used_gb", round_to(used / 1024, 2)}, {"vram_pct", round_to(100 * used / total, 1)}};
}

void Monitor::loop() {
  auto next = std::chrono::steady_clock::now();
  while (!stop_) {
    next += std::chrono::milliseconds((long long)(interval_ * 1000));
    while (!stop_ && std::chrono::steady_clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (stop_) break;
    try {
      auto vm = sys::memory();
      json s = {{"t", round_to(now(), 1)},
                {"cpu", sys::cpu_percent()},
                {"ram_pct", vm.percent},
                {"ram_used_gb", round_to(vm.used / 1073741824.0, 2)},
                {"proc_rss_gb", round_to(sys::process_rss() / 1073741824.0, 3)}};
      json g = gpu_sample();
      for (auto it = g.begin(); it != g.end(); ++it) s[it.key()] = it.value();
      bool beat;
      {
        std::lock_guard<std::mutex> lk(mu_);
        samples_.push_back(s);
        if (samples_.size() > 300) samples_.pop_front();
        peak_ram_ = std::max(peak_ram_, vm.percent);
        peak_vram_ = std::max(peak_vram_, jnum(s.value("vram_pct", json(0.0))));
        cpu_acc_ += jnum(s["cpu"]);
        ++n_;
        beat = n_ % 5 == 0;
      }
      if (beat) heartbeat();
    } catch (const std::exception& e) {
      LM.debug("muestra fallida: ", e.what());
    }
  }
}

void Monitor::heartbeat() {
  json j;
  {
    std::lock_guard<std::mutex> lk(mu_);
    int oom = 0;
    for (auto& e : errors_)
      if (e["kind"] == "oom") ++oom;
    j = {{"session", session_id}, {"peak_ram_pct", peak_ram_}, {"peak_vram_pct", peak_vram_},
         {"settings", settings},  {"oom_errors", oom},         {"t", now()}};
  }
  write_file(running_marker(), jdump(j));
}

void Monitor::record_llm(const std::string& backend, const std::string& model, int tokens, double seconds, double ttft) {
  double tps = seconds > 0 ? tokens / seconds : 0.0;
  std::lock_guard<std::mutex> lk(mu_);
  llm_calls_.push_back({{"backend", backend},
                        {"model", model},
                        {"tokens", tokens},
                        {"seconds", round_to(seconds, 3)},
                        {"tps", round_to(tps, 2)},
                        {"ttft", round_to(ttft, 3)},
                        {"t", now()}});
}

std::vector<json> Monitor::llm_calls() {
  std::lock_guard<std::mutex> lk(mu_);
  return llm_calls_;
}

void Monitor::record_error(const std::string& kind, const std::string& message) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    errors_.push_back({{"kind", kind}, {"message", uleft(message, 300)}, {"t", now()}});
  }
  LM.warning("Error registrado [", kind, "]: ", uleft(message, 200));
}

static json tail(const std::vector<json>& v, size_t n) {
  return json(std::vector<json>(v.size() > n ? v.end() - (long)n : v.begin(), v.end()));
}

json Monitor::live() {
  json s = summary();
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<json> smp(samples_.begin(), samples_.end());
  return {{"session", session_id},
          {"uptime_s", (long long)std::llround(now() - started_)},
          {"current", smp.empty() ? json::object() : smp.back()},
          {"samples", tail(smp, 90)},
          {"llm_calls", tail(llm_calls_, 20)},
          {"errors", tail(errors_, 20)},
          {"summary", s}};
}

json Monitor::summary(bool final_) {
  std::lock_guard<std::mutex> lk(mu_);
  long tok = 0;
  double secs = 0, ttft = 0;
  int ncalls = 0, oom = 0;
  for (auto& c : llm_calls_) {
    if (c["backend"] == "builtin") continue;
    ++ncalls;
    tok += c["tokens"].get<long>();
    secs += c["seconds"].get<double>();
    ttft += c["ttft"].get<double>();
  }
  for (auto& e : errors_)
    if (e["kind"] == "oom") ++oom;
  return {{"session", session_id},
          {"started", started_},
          {"duration_s", (long long)std::llround(now() - started_)},
          {"clean_exit", final_},
          {"llm_calls", ncalls},
          {"avg_tps", secs > 0 ? round_to(tok / secs, 2) : 0.0},
          {"avg_ttft", ncalls ? round_to(ttft / ncalls, 2) : 0.0},
          {"peak_ram_pct", peak_ram_},
          {"peak_vram_pct", peak_vram_},
          {"avg_cpu", n_ ? round_to(cpu_acc_ / n_, 1) : 0.0},
          {"oom_errors", oom},
          {"errors", (int)errors_.size()},
          {"settings", settings}};
}

}  // namespace nx::hw
