// Autoajuste continuo en la app Android autonoma (android_tune.py): nucleos rapidos, hilos medidos con las respuestas
// reales, temperatura y bateria, memoria, NPU/GPU o CPU segun lo que vaya mas rapido y consejo de modelo.
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx {

class Services;

namespace android_tune {

json core_layout(const fs::path& cpuinfo = "/proc/cpuinfo", const fs::path& sys_cpu = "/sys/devices/system/cpu");
std::vector<int> thread_candidates(const json& layout);
json device_state(const fs::path& path, double now_s = 0);
std::pair<int, std::string> throttle(int threads, const json& st);
double gen_tps(const json& call);
std::pair<int, bool> choose(const std::vector<int>& cands, const json& trials);
json model_advice(const std::string& model, double best_tps, double ram_gb, const json& catalog);
std::pair<bool, std::string> warmup_ok(const json& st, double avail_gb, double model_gb);

class LiveTuner {
 public:
  explicit LiveTuner(Services& s, double interval = 15.0, json layout = nullptr);
  ~LiveTuner();
  void start();
  void stop();
  json step();
  json report();
  json switch_model(const std::string& mid);   // std::invalid_argument si no es un modelo previsto para Android
  void reset_trials();
  fs::path state_file;

 private:
  bool accel_step(const std::vector<json>& fresh);
  bool set_gpu(bool on);
  void note(const std::string& msg);
  void activate(const std::string& mid);
  std::string variant();
  std::string key();
  void save();
  void loop();
  Services& s_;
  double interval_;
  json layout_;
  std::vector<int> cands_;
  fs::path file_;
  json state_;
  std::string gpu_device_, last_reason_;
  size_t seen_ = 0;
  int applied_ = 0;
  std::recursive_mutex mu_;
  std::mutex wait_mu_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
  std::thread th_;
};

// tras cada arranque: limpia temporales y registros, optimiza la base de datos, aplica lo aprendido y precarga el modelo
std::vector<std::string> after_start(Services& s, LiveTuner* tuner);

}  // namespace android_tune
}  // namespace nx
