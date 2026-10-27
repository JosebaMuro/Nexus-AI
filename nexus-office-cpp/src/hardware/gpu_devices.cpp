// Dispositivos de llama.cpp y eleccion de una GPU (hardware/gpu_devices.py).
#include "hardware/gpu_devices.h"

#include <mutex>
#include <set>

#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::hw::gpu_devices {

namespace {

const auto L = log::get("gpu");
std::mutex g_mu;
std::map<std::string, json> g_cache;

// «  Vulkan0: NVIDIA GeForce RTX 3060 (12288 MiB, 11520 MiB free)» / «  CUDA1: Tesla T4 (15095 MiB, 14000 MiB free)»
const Rx& line_rx() {
  static const Rx R(R"(^\s*([A-Za-z][A-Za-z_]*\d+)\s*:\s*(.+?)\s*\((\d+)\s*MiB,\s*(\d+)\s*MiB free\)\s*$)");
  return R;
}
const Rx& software_rx() {
  static const Rx R(R"(llvmpipe|lavapipe|swiftshader|softpipe|microsoft basic render)", Rx::I);
  return R;
}
const Rx& integrated_rx() {
  static const Rx R(R"(\b(UHD|HD Graphics|Iris|Intel\(R\) Graphics|Radeon\(TM\) Graphics|Radeon Graphics|)"
                    R"(Radeon \d{3}M|Vega \d+ Graphics|Adreno|Mali|PowerVR|Apple M\d))",
                    Rx::I);
  return R;
}

std::vector<std::string> words(const std::string& text) {
  static const std::set<std::string> SKIP = {"nvidia", "amd", "intel", "corporation", "geforce", "radeon"};
  std::vector<std::string> out;
  std::string w;
  for (unsigned char ch : lower(text) + " ") {
    if (std::isalnum(ch)) {
      w += char(ch);
    } else if (!w.empty()) {
      if (!SKIP.count(w)) out.push_back(w);
      w.clear();
    }
  }
  return out;
}

}  // namespace

json parse(const std::string& text) {
  json out = json::array();
  for (auto& line : splitlines(text)) {
    auto m = line_rx().match(line);
    if (!m) continue;
    std::string dev = m->str(1), name = trim(m->str(2));
    std::string backend = dev;
    while (!backend.empty() && std::isdigit(static_cast<unsigned char>(backend.back()))) backend.pop_back();
    out.push_back({{"id", dev}, {"backend", backend}, {"name", name}, {"mem_mb", std::stoll(m->str(3))},
                   {"free_mb", std::stoll(m->str(4))}, {"software", software_rx().search(name).has_value()},
                   {"integrated", integrated_rx().search(name).has_value()}});
  }
  return out;
}

json list_devices(const fs::path& exe, const std::map<std::string, std::string>& env) {
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(exe, ec);
  if (ec) return json::array();
  std::string key = S(exe) + "|" + std::to_string(mtime.time_since_epoch().count());
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
  }
  proc::Options o;
  o.capture = true;
  o.env = env;
  o.cwd = exe.parent_path();
  auto r = proc::run({S(exe), "--list-devices"}, 60, o);
  json devs = parse(valid_utf8(r.out + "\n" + r.err));
  if (!devs.empty()) {
    std::vector<std::string> shown;
    for (auto& d : devs)
      shown.push_back(jstr(d, "id") + "=" + jstr(d, "name") + (d["software"].get<bool>() ? " (software)" : ""));
    L.info("Dispositivos de llama.cpp: ", join(shown, ", "));
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache[key] = devs;
  return devs;
}

bool matches(const json& d, const std::string& want) {
  std::string w = lower(trim(want));
  if (w.empty()) return false;
  if (w == lower(jstr(d, "id"))) return true;
  // por nombre: todas las palabras significativas de lo elegido estan en el nombre del dispositivo
  auto ws = words(w);
  std::string name = lower(jstr(d, "name"));
  if (ws.empty()) return false;
  for (auto& x : ws)
    if (!contains(name, x)) return false;
  return true;
}

json pick(const json& devs, const std::string& preferred) {
  json real = json::array();
  for (auto& d : devs)
    if (!d.value("software", false)) real.push_back(d);
  if (real.empty()) return nullptr;
  if (!preferred.empty())
    for (auto& d : real)
      if (matches(d, preferred)) return d;
  const json* best = &real[0];
  for (auto& d : real) {
    auto k = [](const json& x) { return std::make_pair(!x.value("integrated", false), x.value("mem_mb", 0LL)); };
    if (k(d) > k(*best)) best = &d;
  }
  return *best;
}

bool needs_device_flag(const json& devs) { return devs.is_array() && devs.size() > 1; }

}  // namespace nx::hw::gpu_devices
