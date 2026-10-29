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
