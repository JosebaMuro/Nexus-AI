// Velocidad con GPU y NPU: capas en la GPU (-ngl auto y su reserva), slots, autoajuste que no saca capas a la CPU y
// capas elegidas en la configuracion (hardware.capas).
#include "check.h"
#include "core/config.h"
#include "hardware/hardware.h"
#include "hardware/hw_profiles.h"
#include "llm/llm.h"
#include "models/runtimes.h"
#include "util/fs.h"
#include "util/sysinfo.h"
#include "util/text.h"

using namespace nx;

namespace {

fs::path tmpdir(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() / ("nexus-rend-" + name + "-" + std::to_string(::time(nullptr)));
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}

std::vector<std::string> lines_of(const fs::path& p) {
  auto t = read_file(p);
  return t ? splitlines(*t) : std::vector<std::string>{};
}

#ifndef _WIN32
// llama-server de escritorio falso; con old=true es anterior a «-ngl auto» y falla como llama.cpp (stoi)
void fake_desktop(const fs::path& p, bool old) {
  write_file(p, std::string("#!/usr/bin/env python3\n"
                            "import sys, http.server\n"
                            "a = sys.argv[1:]\n"
                            "if '--list-devices' in a or '--version' in a:\n"
                            "    print('Available devices:\\n  Vulkan0: NVIDIA GeForce RTX 4070 (12282 MiB, 11000 MiB free)')\n"
                            "    sys.exit(0)\n") +
                    (old ? "if a[a.index('-ngl') + 1] == 'auto':\n"
                           "    print('error while handling argument \"-ngl\": stoi'); sys.exit(1)\n"
                         : "") +
                    "open(sys.argv[0] + '.cmd', 'a').write(' '.join(a) + '\\n')\n"
                    "class H(http.server.BaseHTTPRequestHandler):\n"
                    "    def log_message(self, *x): pass\n"
                    "    def do_GET(self):\n"
                    "        self.send_response(200); self.end_headers(); self.wfile.write(b'{}')\n"
                    "http.server.HTTPServer(('127.0.0.1', int(a[a.index('--port') + 1])), H).serve_forever()\n");
  fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                         fs::perms::others_exec);
}
#endif

}  // namespace

#ifndef _WIN32
TEST("rendimiento: escritorio con GPU pide -ngl auto (y 999 si el llama-server es antiguo)") {
  for (bool old : {false, true}) {
    fs::path d = tmpdir(old ? "viejo" : "nuevo");
    set_env("NEXUS_DATA_DIR", S(d / "nexus"));
    for (auto* k : {"NEXUS_RUNTIME_LLAMA", "NEXUS_RUNTIME_LLAMA_CPU", "NEXUS_RUNTIME_LLAMA_GPU"}) set_env(k, "");
    mkdirs(paths::logs_dir());
    fs::path exe = paths::sub({"runtimes", "llama", "vulkan"}) / "llama-server";
    fake_desktop(exe, old);
    write_file(d / "m.gguf", "GGUF");
    runtimes::RuntimeManager rt;
    json profile = {{"id", "vulkan-nvidia-dgpu"}, {"accelerator", "vulkan"}, {"llm", {{"gpu_layers", -1}}}};
    llm::LlamaServerBackend b(&rt, [&](const std::string&) { return std::make_optional(std::make_pair(S(d / "m.gguf"), json{{"id", "m"}})); },
                              [&] { return profile; }, [] { return std::string(); });
    b.ensure("", {{"n_ctx", 4096}, {"gpu_layers", -1}});
    auto cmds = lines_of(S(exe) + ".cmd");
    CHECK_EQ(b.variant, std::string("vulkan"));
    CHECK(!cmds.empty() && contains(cmds.back(), old ? "-ngl 999" : "-ngl auto") && contains(cmds.back(), "-np 2"));
    b.stop();
    // capas elegidas a mano: el numero tal cual
    b.ensure("", {{"n_ctx", 4096}, {"gpu_layers", 20}});
    cmds = lines_of(S(exe) + ".cmd");
    CHECK(!cmds.empty() && contains(cmds.back(), "-ngl 20"));
    b.stop();
  }
}
#endif

TEST("rendimiento: el autoajuste no saca capas de la GPU cuando estan todas pedidas") {
  json profile = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 32768}, {"n_batch", 512}, {"gpu_layers", -1}, {"n_threads", 8}}},
                  {"vram_gb", 12}};
  // VRAM llena en la sesion anterior: antes pasaba a 28 capas y se quedaba asi
  json hist = json::array({{{"llm_calls", 5}, {"avg_tps", 30}, {"peak_ram_pct", 50}, {"peak_vram_pct", 97},
                            {"clean_exit", true}, {"settings", {{"n_threads", 8}}}}});
  auto t = hw::tune(profile, hist, false, 16, json{{"overrides", json::object()}, {"thread_trials", json::object()}, {"log", json::array()}});
  CHECK_EQ((int)jnum(t.profile["llm"]["gpu_layers"]), -1);
  // un 28 guardado por una version anterior se descarta
  auto t2 = hw::tune(profile, json::array(), false, 16,
                     json{{"overrides", {{"gpu_layers", 28}}}, {"thread_trials", json::object()}, {"log", json::array()}});
  CHECK_EQ((int)jnum(t2.profile["llm"]["gpu_layers"]), -1);
  // con la VRAM casi llena no se agranda el contexto aunque sobre RAM
  CHECK_EQ((int)jnum(t.profile["llm"]["n_ctx"]), 8192);
  json hist2 = hist;
  hist2[0]["peak_vram_pct"] = 50;
  auto t3 = hw::tune(profile, hist2, false, 16, json{{"overrides", json::object()}, {"thread_trials", json::object()}, {"log", json::array()}});
  CHECK(jnum(t3.profile["llm"]["n_ctx"]) > 8192);
}

TEST("rendimiento: capas en la GPU/NPU desde la configuracion") {
  namespace hp = hw::profiles;
  CHECK(hp::norm_capas("auto") == "auto" && hp::norm_capas("todas") == "todas" && hp::norm_capas(0) == "ninguna");
  CHECK(hp::norm_capas(24) == 24 && hp::norm_capas("16") == 16);
  bool threw = false;
  try {
    hp::norm_capas("muchas");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  reset_config();
  Config& cfg = get_config();
  json gpu = {{"llm", {{"gpu_layers", -1}}}}, cpu = {{"llm", {{"gpu_layers", 0}}}};
  cfg.set("hardware.capas", 24);
  hp::aplicar_capas(gpu, cfg);
  hp::aplicar_capas(cpu, cfg);
  CHECK(gpu["llm"]["gpu_layers"] == 24 && gpu["llm"]["capas_usuario"] == 24);
  CHECK(cpu["llm"]["gpu_layers"] == 0);   // sin GPU ni NPU no hay donde ponerlas
  cfg.set("hardware.capas", "ninguna");
  hp::aplicar_capas(gpu, cfg);
  CHECK(gpu["llm"]["gpu_layers"] == 0);
  cfg.set("hardware.capas", "auto");
}

TEST("rendimiento: los nucleos de ARM se cuentan todos") {
#if defined(__aarch64__) || defined(__arm__)
  CHECK_EQ(sys::cores_physical(), sys::cores_logical());
#else
  CHECK(sys::cores_physical() >= 1 && sys::cores_physical() <= sys::cores_logical());
#endif
}

// ---- 3.2.2: autoajuste de la cache KV, lote fisico, cache de prompts e hilos con el modelo entero en la GPU
namespace {
json estado_vacio() {
  return json{{"overrides", json::object()}, {"thread_trials", json::object()}, {"log", json::array()}};
}
}  // namespace

TEST("rendimiento 3.2.2: con la VRAM llena, cache KV en 8 bits antes de tocar capas o lote") {
  json profile = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 32768}, {"n_batch", 1024}, {"n_ubatch", 1024},
                           {"gpu_layers", -1}, {"n_threads", 8}, {"kv_quant", ""}, {"cache_ram_mb", 2048}}},
                  {"vram_gb", 12}};
  json hist = json::array({{{"llm_calls", 5}, {"avg_tps", 30}, {"peak_ram_pct", 50}, {"peak_vram_pct", 96},
                            {"clean_exit", true}, {"settings", {{"n_threads", 8}}}}});
  auto t = hw::tune(profile, hist, false, 16, estado_vacio());
  CHECK_EQ(jstr(t.profile["llm"], "kv_quant"), std::string("q8_0"));
  CHECK_EQ((int)jnum(t.profile["llm"]["n_ubatch"]), 1024);
  CHECK_EQ((int)jnum(t.profile["llm"]["gpu_layers"]), -1);
  CHECK_EQ(jstr(t.state["overrides"], "kv_quant"), std::string("q8_0"));
  // la sesion siguiente sigue llena: ahora baja el lote fisico
  auto t2 = hw::tune(profile, hist, false, 16, t.state);
  CHECK_EQ(jstr(t2.profile["llm"], "kv_quant"), std::string("q8_0"));
  CHECK_EQ((int)jnum(t2.profile["llm"]["n_ubatch"]), 512);
}

TEST("rendimiento 3.2.2: primera palabra lenta con VRAM libre agranda el lote fisico") {
  json profile = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 32768}, {"n_batch", 512}, {"n_ubatch", 0}, {"gpu_layers", -1},
                           {"n_threads", 8}, {"cache_ram_mb", 2048}}},
                  {"vram_gb", 16}};
  json hist = json::array({{{"llm_calls", 6}, {"avg_tps", 40}, {"avg_ttft", 4.0}, {"peak_ram_pct", 70},
                            {"peak_vram_pct", 55}, {"clean_exit", true}, {"settings", {{"n_threads", 8}}}}});
  auto t = hw::tune(profile, hist, false, 16, estado_vacio());
  CHECK_EQ((int)jnum(t.profile["llm"]["n_ubatch"]), 1024);
  CHECK((int)jnum(t.profile["llm"]["n_batch"]) >= 1024);
  // en la CPU no
  json cpu = profile;
  cpu["llm"]["gpu_layers"] = 0;
  auto t2 = hw::tune(cpu, hist, false, 16, estado_vacio());
  CHECK_EQ((int)jnum(t2.profile["llm"]["n_ubatch"]), 0);
}

TEST("rendimiento 3.2.2: cache de prompts segun la RAM y el autoajuste la reduce y la recupera") {
  CHECK(hw::cache_ram_for(16, false) >= 1536 && hw::cache_ram_for(16, false) <= 1664);
  CHECK_EQ(hw::cache_ram_for(4, true), 256);
  CHECK_EQ(hw::cache_ram_for(256, false), 8192);
  json profile = {{"llm", {{"n_ctx", 4096}, {"max_ctx", 16384}, {"n_batch", 256}, {"gpu_layers", 0}, {"n_threads", 4},
                           {"kv_quant", "q8_0"}, {"cache_ram_mb", 1024}}}};
  json lleno = json::array({{{"llm_calls", 2}, {"avg_tps", 8}, {"peak_ram_pct", 91}, {"clean_exit", true},
                             {"settings", {{"n_threads", 4}}}}});
  auto t = hw::tune(profile, lleno, false, 4, estado_vacio());
  CHECK_EQ((int)jnum(t.profile["llm"]["cache_ram_mb"]), 512);
  json holgado = lleno;
  holgado[0]["peak_ram_pct"] = 60;
  auto t2 = hw::tune(profile, holgado, false, 4, t.state);
  CHECK_EQ((int)jnum(t2.profile["llm"]["cache_ram_mb"]), 1024);
}

TEST("rendimiento 3.2.2: RAM llena en la CPU usa cache KV en 8 bits antes de recortar el contexto") {
  json profile = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 16384}, {"n_batch", 512}, {"gpu_layers", 0}, {"n_threads", 4},
                           {"kv_quant", ""}}}};
  json hist = json::array({{{"llm_calls", 2}, {"avg_tps", 8}, {"peak_ram_pct", 94}, {"clean_exit", true},
                            {"settings", {{"n_threads", 4}}}}});
  auto t = hw::tune(profile, hist, false, 4, estado_vacio());
  CHECK_EQ(jstr(t.profile["llm"], "kv_quant"), std::string("q8_0"));
  CHECK_EQ((int)jnum(t.profile["llm"]["n_ctx"]), 8192);
  auto t2 = hw::tune(profile, hist, false, 4, t.state);   // sigue llena: ahora si se recorta
  CHECK((int)jnum(t2.profile["llm"]["n_ctx"]) < 8192);
}

TEST("rendimiento 3.2.2: con el modelo entero en la GPU no se exploran hilos") {
  json profile = {{"llm", {{"n_ctx", 8192}, {"max_ctx", 32768}, {"n_batch", 512}, {"gpu_layers", -1}, {"n_threads", 8}}},
                  {"vram_gb", 12}};
  json hist = json::array({{{"llm_calls", 5}, {"avg_tps", 50}, {"peak_ram_pct", 50}, {"peak_vram_pct", 80},
                            {"clean_exit", true}, {"settings", {{"n_threads", 8}, {"offloaded", "33/33"}}}}});
  json st = estado_vacio();
  st["thread_trials"] = {{"8", {40}}};
  auto t = hw::tune(profile, hist, false, 16, st);
  CHECK_EQ((int)jnum(t.profile["llm"]["n_threads"]), 8);
  CHECK(t.state["thread_trials"]["8"].size() == 1);   // no se apunta otra medida
  // con capas en la CPU (20/33) si se exploran
  hist[0]["settings"]["offloaded"] = "20/33";
  auto t2 = hw::tune(profile, hist, false, 16, st);
  CHECK((int)jnum(t2.profile["llm"]["n_threads"]) != 8);
}

TEST("rendimiento 3.2.2: opciones de llama-server segun su --help") {
  std::string nuevo = "-ub, --ubatch-size N\n--cache-ram, -cram N\n-fa, --flash-attn [on|off|auto]\n";
  auto a = llm::flags_rendimiento_para(nuevo, 1024, 512, true);
  CHECK((a == std::vector<std::string>{"-ub", "1024", "--cache-ram", "512"}));
  std::string viejo = "-ub, --ubatch-size N\n-fa, --flash-attn  enable Flash Attention (default: disabled)\n";
  auto b = llm::flags_rendimiento_para(viejo, 0, 512, true);
  CHECK((b == std::vector<std::string>{"-fa"}));
  CHECK(llm::flags_rendimiento_para(viejo, 0, -1, false).empty());
}

TEST("rendimiento 3.2.2: VRAM de AMD en Linux desde sysfs") {
  auto d = tmpdir("drm");
  std::filesystem::create_directories(d / "card0" / "device");
  std::filesystem::create_directories(d / "card1" / "device");
  std::filesystem::create_directories(d / "card1-DP-1");
  write_file(d / "card0" / "device" / "mem_info_vram_used", "268435456\n");
  write_file(d / "card0" / "device" / "mem_info_vram_total", "536870912\n");   // integrada de 512 MB
  write_file(d / "card1" / "device" / "mem_info_vram_used", "4294967296\n");
  write_file(d / "card1" / "device" / "mem_info_vram_total", "17179869184\n");
  write_file(d / "card1" / "device" / "gpu_busy_percent", "37\n");
  json g = hw::amd_sysfs_vram(d);
  CHECK_EQ(jnum(g["vram_pct"]), 25.0);
  CHECK_EQ(jnum(g["vram_used_gb"]), 4.0);
  CHECK_EQ(jnum(g["gpu_util"]), 37.0);
  CHECK(hw::amd_sysfs_vram(d / "no-existe").empty());
  std::filesystem::remove_all(d);
}
