// Hardware: deteccion (CPU, RAM, GPU, NPU), perfil optimo, selector CPU/GPU/NPU, autoajuste y monitor en vivo.
// Mismo formato que la version Python (data/hardware.json, tuning.json, perf_history.json).
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::hw {

struct GPU {
  std::string vendor, name;
  double vram_gb = 0;
  bool integrated = false;
  std::string driver, compute_capability, source;
};

struct NPU {
  std::string vendor, name, runtime_hint, source;
};

struct HardwareInfo {
  std::string os, os_version, arch, cpu_name;
  int cores_physical = 1, cores_logical = 1;
  std::vector<std::string> cpu_flags;
  double ram_gb = 0, disk_free_gb = 0;
  std::vector<GPU> gpus;
  std::vector<NPU> npus;
  json runtimes = json::object();
  std::string python = "C++ " NEXUS_VERSION;   // campo heredado: aqui identifica la edicion

  json to_json() const;
  static HardwareInfo from_json(const json& j);
};

// deteccion
HardwareInfo detect(const std::string& ollama_url = "http://127.0.0.1:11434", bool probe_runtimes = true);
void save(const HardwareInfo& info);
bool load_cached(HardwareInfo& out);
std::string find_nvidia_smi();
std::string vendor_from_text(const std::string& text);
bool is_integrated(const std::string& vendor, const std::string& name);
std::vector<GPU> parse_nvidia_smi(const std::string& out);

// perfiles
const GPU* best_gpu(const HardwareInfo& hw);
std::pair<std::string, const GPU*> choose_accelerator(const HardwareInfo& hw, const std::string& override_ = "");
json build_profile(const HardwareInfo& hw, const std::string& override_ = "");
bool android();

// selector de inicio (accel_choice)
extern const std::vector<std::string> ENGINES;
extern const std::vector<std::string> ACCELS;
std::string engine_label(const std::string& e);
json plan(const HardwareInfo& hw, const std::string& accel);
std::pair<json, json> build_for(const HardwareInfo& hw, const std::string& accel, const std::string& legacy_override = "");
json effective_from_log(const std::string& text, const json& env = json::object());
std::string read_log_since(const fs::path& p, uint64_t offset);

// autoajuste (tuner)
json load_state();
void save_state(json state);
std::vector<int> thread_candidates(const json& profile, int cores_logical);
struct TuneResult {
  json profile, state;
  std::vector<std::string> reasons;
};
TuneResult tune(const json& profile, const json& history, bool previous_unclean, int cores_logical,
                const json& state = nullptr, const json& previous_info = nullptr);

// monitor
json load_history();
class Monitor {
 public:
  explicit Monitor(double interval = 2.0, json settings = json::object());
  ~Monitor();
  void start();
  json stop();
  void record_llm(const std::string& backend, const std::string& model, int tokens, double seconds, double ttft);
  void record_error(const std::string& kind, const std::string& message);
  json live();
  json summary(bool final_ = false);
  bool previous_unclean = false;
  json previous_info = json::object();
  json settings;
  std::string session_id;

 private:
  void loop();
  void heartbeat();
  json gpu_sample();
  double interval_, started_;
  std::mutex mu_;
  std::deque<json> samples_;
  std::vector<json> llm_calls_, errors_;
  double peak_ram_ = 0, peak_vram_ = 0, cpu_acc_ = 0;
  long n_ = 0;
  std::atomic<bool> stop_{false};
  std::thread th_;
  std::string nvsmi_;
};

struct Startup {
  HardwareInfo hw;
  json profile;
  std::vector<std::string> reasons;
};
Startup startup(const std::string& override_ = "", bool adaptive = true,
                const std::string& ollama_url = "http://127.0.0.1:11434", bool previous_unclean = false,
                const json& previous_info = nullptr);

}  // namespace nx::hw
