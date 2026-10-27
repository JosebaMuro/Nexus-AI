// Perfiles de hardware y eleccion de GPU cuando hay varias (tests/test_hw_perfiles.py): llama-server
// --list-devices, llvmpipe y dos tarjetas.
#include <httplib.h>

#include <thread>

#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "hardware/gpu_devices.h"
#include "hardware/hardware.h"
#include "hardware/hw_profiles.h"
#include "server/app.h"
#include "util/fs.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/text.h"

using namespace nx;
namespace gd = nx::hw::gpu_devices;
namespace hp = nx::hw::profiles;

namespace {

const std::string SALIDA = "ggml_vulkan: Found 3 Vulkan devices:\n"
                           "Available devices:\n"
                           "  Vulkan0: Intel(R) UHD Graphics 770 (ADL-S GT1) (15872 MiB, 15000 MiB free)\n"
                           "  Vulkan1: NVIDIA GeForce RTX 3060 (12288 MiB, 11520 MiB free)\n"
                           "  Vulkan2: llvmpipe (LLVM 15.0.7, 256 bits) (32000 MiB, 32000 MiB free)\n";

hw::GPU gpu(const std::string& vendor, const std::string& name, double vram, bool integrated = false) {
  hw::GPU g;
  g.vendor = vendor;
  g.name = name;
  g.vram_gb = vram;
  g.integrated = integrated;
  return g;
}

void datos(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() /
             ("nexus-hwp-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
}

}  // namespace

TEST("perfiles: dispositivos de llama.cpp y eleccion de una GPU") {
  json devs = gd::parse(SALIDA);
  CHECK(devs.size() == 3 && devs[0]["id"] == "Vulkan0" && devs[2]["id"] == "Vulkan2");
  CHECK(devs[2]["software"] == true && devs[0]["integrated"] == true && devs[1]["integrated"] == false);
  CHECK(devs[1]["backend"] == "Vulkan" && devs[1]["mem_mb"] == 12288);
  CHECK(gd::pick(devs)["id"] == "Vulkan1");   // la dedicada, aunque la integrada tenga mas memoria
  CHECK(gd::pick(devs, "Intel UHD 770")["id"] == "Vulkan0");
  CHECK(gd::pick(devs, "vulkan0")["id"] == "Vulkan0");
  CHECK(gd::pick(devs, "llvmpipe")["id"] == "Vulkan1");   // nunca la de software
  CHECK(gd::pick(json::array({devs[2]})).is_null());
  CHECK(gd::needs_device_flag(devs) && !gd::needs_device_flag(json::array({devs[1]})));
}

TEST("perfiles: elegir la GPU cambia el perfil") {
  hw::HardwareInfo h;
  h.ram_gb = 32;
  h.cores_physical = 8;
  h.gpus = {gpu("intel", "Intel UHD Graphics 770", 0, true), gpu("nvidia", "RTX 3060", 12), gpu("amd", "Radeon RX 6600", 8)};
  hw::select_gpu("");
  CHECK(hw::best_gpu(h)->name == "RTX 3060");
  hw::select_gpu("RX 6600");
  CHECK(hw::best_gpu(h)->name == "Radeon RX 6600");
  h.runtimes = {{"llama_devices", gd::parse(SALIDA)}};
  hw::select_gpu("Vulkan0");
  CHECK(hw::best_gpu(h)->name == "Intel UHD Graphics 770");
  // NVIDIA sin nvidia-smi (VRAM 0): Vulkan si el llama-server de Vulkan la ve, con la memoria que dice llama.cpp
  hw::select_gpu("");
  hw::HardwareInfo n;
  n.ram_gb = 32;
  n.cores_physical = 8;
  n.gpus = {gpu("nvidia", "NVIDIA GeForce RTX 3060", 0)};
  CHECK(hw::choose_accelerator(n).first == "cpu");
  n.runtimes = {{"llama_devices", gd::parse(SALIDA)}};
  CHECK(hw::choose_accelerator(n).first == "vulkan");
  json prof = hw::build_profile(n);
  CHECK(prof["vram_gb"] == 12.0 && prof["llm"]["gpu_select"] == "NVIDIA GeForce RTX 3060");
}

TEST("perfiles: crear, superponer, activar y borrar") {
  datos("crud");
  Config& cfg = get_config();
  std::set<std::string> ids;
  for (auto& p : hp::lista(cfg)) ids.insert(jstr(p, "id"));
  for (auto* id : {"auto", "rendimiento", "ahorro", "cpu", "grande"}) CHECK(ids.count(id));
  json p = hp::guardar(cfg, {{"nombre", "Mi Portátil"}, {"acelerador", "gpu"}, {"gpu", "RTX 3060"}, {"capas_gpu", "20"},
                             {"kv", "ram"}, {"contexto", 4096}, {"motor", "raro"}});
  CHECK(jstr(p, "id") == "mi-port-til");
  CHECK(p["capas_gpu"] == 20 && p["motor"] == "auto" && hp::get(cfg, jstr(p, "id")).is_object());
  json prof = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 16384}, {"gpu_layers", -1}, {"n_threads", 8}, {"n_batch", 512}}}};
  hp::superponer(prof, p);
  CHECK(prof["llm"]["gpu_layers"] == 20 && prof["llm"]["kv_ram"] == true && prof["llm"]["n_ctx"] == 4096);
  CHECK(prof["llm"]["gpu_select"] == "RTX 3060" && prof["perfil_hw"]["id"] == p["id"]);
  json prof2 = {{"llm", {{"n_ctx", 8192}, {"gpu_layers", 0}, {"n_threads", 8}, {"n_batch", 512}}}};
  hp::superponer(prof2, hp::get(cfg, "ahorro"));
  CHECK(prof2["llm"]["n_threads"] == 4 && prof2["llm"]["n_ctx"] == 2048);
  json res = hp::activar(jstr(p, "id"), cfg);   // sin servicios: queda para el arranque
  CHECK(cfg.get("hardware.perfil") == p["id"] && cfg.get("hardware.gpu") == "RTX 3060");
  CHECK(res["perfil"]["id"] == p["id"]);
  CHECK(hp::borrar(cfg, jstr(p, "id")) && cfg.get("hardware.perfil") == "auto");
  CHECK(!hp::borrar(cfg, "rendimiento"));   // los de serie no se borran
  hw::select_gpu("");
}

TEST("perfiles: la API lista, guarda, activa y elige GPU") {
  datos("api");
  auto s = std::make_unique<Services>(false);
  server::App app(s.get());
  s->start();
  int port = net::free_port(18997);
  std::thread th([&] { app.listen("127.0.0.1", port); });
  httplib::Client c("127.0.0.1", port);
  for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto body = [](const httplib::Result& r) { return r ? jparse(r->body) : json(); };

  json r = body(c.Get("/api/hw/perfiles"));
  bool rend = false;
  for (auto& p : r["perfiles"]) rend |= p["id"] == "rendimiento";
  CHECK(truthy(r["activo"]) && rend && r.contains("gpus"));
  auto put = c.Put("/api/hw/perfiles/prueba", R"({"nombre": "Prueba", "acelerador": "cpu", "contexto": 2048})",
                   "application/json");
  CHECK(put && put->status == 200 && body(put)["id"] == "prueba");
  auto act = c.Post("/api/hw/perfiles/prueba/activar", "", "application/json");
  CHECK(act && act->status == 200);
  CHECK(body(act)["llm"]["n_ctx"].get<int>() <= 4096);
  CHECK(body(c.Get("/api/accel"))["perfil_hw"] == "prueba");
  CHECK(body(c.Get("/api/status"))["perfil_hw"] == "Prueba");
  auto a2 = c.Post("/api/hw/perfiles/auto/activar", "", "application/json");
  CHECK(a2 && a2->status == 200);
  auto del = c.Delete("/api/hw/perfiles/prueba");
  CHECK(del && del->status == 200);
  auto nada = c.Post("/api/hw/perfiles/nada/activar", "", "application/json");
  CHECK(nada && nada->status == 404);
  auto g = c.Post("/api/hw/gpu", R"({"gpu": ""})", "application/json");
  CHECK(g && g->status == 200 && body(g).contains("acelerador"));

  app.stop();
  th.join();
  s->shutdown();
  hw::select_gpu("");
}
