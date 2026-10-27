// CPU, GPU y NPU en la app Android autonoma (hardware/android_accel) y autoajuste en vivo (core/android_tune): las
// mismas comprobaciones que tests/test_android_accel.py y tests/test_android_tune.py de la edicion Python.
#include <sys/stat.h>

#include "check.h"
#include "core/android_tune.h"
#include "core/config.h"
#include "core/services.h"
#include "hardware/android_accel.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;
namespace aa = nx::hw::android_accel;
namespace at = nx::android_tune;

namespace {

const std::string LIST_ADRENO = "ggml_vulkan: Found 1 Vulkan devices:\n"
                                "ggml_opencl: device: 'QUALCOMM Adreno(TM) 750 (OpenCL 3.0 Adreno(TM) 750)'\n"
                                "Available devices:\n"
                                "  Vulkan0: Adreno (TM) 750 (7623 MiB, 7623 MiB free)\n"
                                "  GPUOpenCL: QUALCOMM Adreno(TM) 750 (7623 MiB, 7623 MiB free)\n";
const std::string LIST_MALI = "Available devices:\n  Vulkan0: Mali-G720-Immortalis MC12 (11520 MiB, 9000 MiB free)\n";
const std::string LIST_SNAPDRAGON = LIST_ADRENO + "  HTP0: Hexagon (0 MiB, 0 MiB free)\n";

fs::path tmpdir(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() / ("nexus-android-" + name + "-" + std::to_string(::time(nullptr)));
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}

void executable(const fs::path& p, const std::string& text) {
  write_file(p, text);
  ::chmod(p.c_str(), 0755);
}

// llama-server falso: --list-devices imprime la lista; si no, sirve /health salvo con un dispositivo «roto»
void fake_server(const fs::path& p, const std::string& listing, const std::string& broken_devices) {
  executable(p, "#!/usr/bin/env python3\n"
                "import sys, http.server\n"
                "a = sys.argv[1:]\n"
                "if '--list-devices' in a:\n"
                "    sys.stdout.write('''" + listing + "''')\n"
                "    sys.exit(0)\n"
                "dev = a[a.index('--device') + 1] if '--device' in a else ''\n"
                "if dev and dev in '" + broken_devices + "'.split(','):\n"
                "    print('driver roto: ' + dev)\n"
                "    sys.exit(1)\n"
                "open(sys.argv[0] + '.cmd', 'a').write(' '.join(a) + '\\n')\n"
                "class H(http.server.BaseHTTPRequestHandler):\n"
                "    def log_message(self, *x): pass\n"
                "    def do_GET(self):\n"
                "        self.send_response(200); self.end_headers(); self.wfile.write(b'{}')\n"
                "http.server.HTTPServer(('127.0.0.1', int(a[a.index('--port') + 1])), H).serve_forever()\n");
}

std::vector<std::string> lines_of(const fs::path& p) {
  auto t = read_file(p);
  return t ? splitlines(*t) : std::vector<std::string>{};
}

}  // namespace

TEST("android: dispositivos de llama-server, NPU Hexagon, GPU y SoC") {
  json devs = aa::parse_devices(LIST_ADRENO);
  CHECK_EQ(devs.size(), (size_t)2);
  CHECK(devs[0]["dev"] == "Vulkan0" && devs[0]["backend"] == "vulkan");
  CHECK(devs[1]["dev"] == "GPUOpenCL" && devs[1]["backend"] == "opencl");
  CHECK(devs[1]["name"] == "QUALCOMM Adreno(TM) 750" && devs[1]["mem_mb"] == 7623);
  CHECK(aa::parse_devices("Available devices:\n  (none)\n").empty());
  CHECK_EQ(jstr(aa::pick(devs), "dev"), std::string("GPUOpenCL"));
  CHECK_EQ(jstr(aa::pick(aa::parse_devices(LIST_MALI)), "dev"), std::string("Vulkan0"));
  std::string only_vk = replace_all(replace_all(LIST_ADRENO, "  GPUOpenCL", "  X"), "X: QUALCOMM", "Y");
  CHECK(aa::pick(aa::parse_devices(only_vk)).is_null());   // Adreno solo con Vulkan: la CPU va mejor
  CHECK(aa::pick(json::array()).is_null());
  CHECK_EQ(aa::gpu_vendor("Mali-G720-Immortalis MC12"), std::string("arm"));
  CHECK_EQ(aa::gpu_vendor("Adreno (TM) 750"), std::string("qualcomm"));

  json sd = aa::parse_devices(LIST_SNAPDRAGON);
  CHECK(sd.back()["dev"] == "HTP0" && sd.back()["backend"] == "hexagon");
  CHECK(aa::parse_devices("  HTP0:1: Hexagon (0 MiB, 0 MiB free)\n")[0]["dev"] == "HTP0:1");
  CHECK_EQ(jstr(aa::pick(sd), "dev"), std::string("HTP0"));
  CHECK_EQ(jstr(aa::pick(sd, false), "dev"), std::string("GPUOpenCL"));
  CHECK_EQ(jstr(aa::pick(sd, true, false), "dev"), std::string("HTP0"));
  CHECK(aa::pick(sd, false, false).is_null());
  CHECK(aa::is_npu_dev("HTP0") && !aa::is_npu_dev("GPUOpenCL") && !aa::is_npu_dev(""));
  CHECK_EQ(split(aa::gpu_env("/data/app/lib/arm64")["ADSP_LIBRARY_PATH"], ";")[0], std::string("/data/app/lib/arm64"));

  struct Case {
    json p, info;
    std::string family, name;
  };
  std::vector<Case> cases = {
      {{{"ro.soc.manufacturer", "QTI"}, {"ro.soc.model", "SM8650"}, {"ro.board.platform", "pineapple"}}, json::object(),
       "qualcomm", "Qualcomm SM8650 (Snapdragon 8 Gen 3)"},
      {{{"ro.board.platform", "mt6989"}, {"ro.hardware", "mt6989"}}, {{"soc_manufacturer", "Mediatek"}, {"soc_model", "MT6989"}},
       "mediatek", "Mediatek MT6989"},
      {{{"ro.board.platform", "zuma"}, {"ro.hardware", "zuma"}}, {{"soc_manufacturer", "Google"}, {"soc_model", "Tensor G3"}},
       "google", "Google Tensor G3"},
      {{{"ro.board.platform", "s5e9945"}}, {{"soc_manufacturer", "Samsung"}, {"soc_model", "s5e9945"}}, "samsung",
       "Samsung s5e9945"},
      {{{"ro.board.platform", "rk3588"}}, json::object(), "", "rk3588"}};
  for (auto& c : cases) {
    json s = aa::soc(c.p, c.info);
    CHECK_EQ(jstr(s, "family"), c.family);
    CHECK_EQ(jstr(s, "name"), c.name);
  }
  auto none = [](const std::vector<std::string>&) { return std::string(); };
  json q = aa::soc({{"ro.soc.manufacturer", "QTI"}, {"ro.soc.model", "SM8650"}}, json::object());
  json n = aa::npu(q, [](const std::vector<std::string>& names) {
    return std::find(names.begin(), names.end(), "libQnnHtp.so") != names.end() ? std::string("/vendor/lib64/libQnnHtp.so")
                                                                                 : std::string();
  });
  CHECK(n["vendor"] == "qualcomm" && contains(jstr(n, "name"), "Hexagon") && n["source"] == "vendor-lib");
  CHECK(starts_with(jstr(aa::npu(aa::soc({{"ro.board.platform", "mt6989"}}, json::object()), none), "name"), "MediaTek APU"));
  CHECK(contains(jstr(aa::npu(aa::soc({{"ro.board.platform", "zuma"}}, json::object()), none), "name"), "Tensor G3 TPU"));
  CHECK(aa::npu(aa::soc({{"ro.board.platform", "rk3588"}}, json::object()), none).is_null());
}

TEST("android: sonda con cache, NPU que falla y vuelta a la GPU y a la CPU") {
  fs::path d = tmpdir("sonda");
  set_env("NEXUS_DATA_DIR", S(d / "nexus"));
  mkdirs(d / "nexus");
  set_env("NEXUS_DEVICE_INFO", S(d / "device_info.json"));
  write_file(d / "device_info.json", R"({"soc_manufacturer": "QTI", "soc_model": "SM8650", "gl_renderer": "Adreno (TM) 750"})");
  mkdirs(d / "lib");
  fs::path exe = d / "lib" / aa::GPU_EXE;
  fake_server(exe, LIST_SNAPDRAGON, "");
  json r = aa::detect(S(d / "lib"));
  CHECK_EQ(jstr(r["gpu"]["use"], "dev"), std::string("HTP0"));
  CHECK_EQ(jstr(r["gpu"], "name"), std::string("QUALCOMM Adreno(TM) 750"));
  CHECK(r["soc"]["family"] == "qualcomm" && r["npu"]["vendor"] == "qualcomm");
  CHECK(truthy(r["npu"]["usable"]) && r["npu"]["device"] == "HTP0");
  aa::mark_gpu_broken("la NPU no arranca", "HTP0");
  r = aa::detect(S(d / "lib"));
  CHECK_EQ(jstr(r["gpu"]["use"], "dev"), std::string("GPUOpenCL"));
  CHECK(!truthy(r["npu"]["usable"]) && jstr(r["gpu"], "broken").empty());
  aa::mark_gpu_broken("ni la GPU", "GPUOpenCL");
  CHECK(aa::detect(S(d / "lib"))["gpu"]["use"].is_null());
  executable(exe, "#!/bin/sh\nexit 1\n");   // otro binario (otro tamano): se repite la sonda
  CHECK(aa::detect(S(d / "lib"))["gpu"]["use"].is_null());
  json nob = aa::detect(S(d / "vacio"));
  CHECK(nob["gpu"]["binary"] == false && nob["gpu"]["use"].is_null());
}

TEST("android: llama-server en la NPU cae a la GPU y despues a la CPU") {
  fs::path d = tmpdir("llama");
  set_env("NEXUS_DATA_DIR", S(d / "nexus"));
  mkdirs(paths::logs_dir());
  mkdirs(d / "lib");
  fs::path cpu = d / "lib" / "libllama-server-i8mm.so", gpu = d / "lib" / aa::GPU_EXE;
  fake_server(cpu, "", "");
  fake_server(gpu, LIST_SNAPDRAGON, "HTP0");
  write_file(d / "m.gguf", "GGUF");
  set_env("NEXUS_RUNTIME_LLAMA", S(cpu));
  set_env("NEXUS_RUNTIME_LLAMA_CPU", S(cpu));
  set_env("NEXUS_RUNTIME_LLAMA_GPU", S(gpu));
  set_env("NEXUS_DEVICE_INFO", S(d / "no.json"));
  runtimes::RuntimeManager rt;
  json profile = {{"llm", {{"gpu_layers", -1}, {"device", "HTP0"}}}};
  llm::LlamaServerBackend b(&rt, [&](const std::string&) { return std::make_optional(std::make_pair(S(d / "m.gguf"), json{{"id", "m"}})); },
                            [&] { return profile; }, [] { return std::string(); });
  b.set_llm_opts = [&](const json& o) { profile["llm"].update(o); };
  // la sonda ve la NPU y la GPU: al fallar la NPU se sigue en la GPU (GPUOpenCL)
  aa::detect(S(d / "lib"));
  set_env("NEXUS_ANDROID_LIBS", S(d / "lib"));
  b.ensure("", {{"n_ctx", 2048}, {"gpu_layers", -1}, {"device", "HTP0"}});
  auto cmds = lines_of(S(gpu) + ".cmd");
  CHECK(!cmds.empty() && contains(cmds.back(), "--device GPUOpenCL") && contains(cmds.back(), "-ngl 999"));
  CHECK(profile["llm"] == json({{"gpu_layers", -1}, {"device", "GPUOpenCL"}}));
  CHECK(contains(jstr(jparse(*read_file(d / "nexus" / "android_accel.json")), "npu_broken"), "HTP0"));
  // CPU: el binario de CPU, sin --device y con -ngl 0
  b.ensure("", {{"n_ctx", 2048}, {"gpu_layers", 0}, {"device", ""}});
  cmds = lines_of(S(cpu) + ".cmd");
  CHECK(!cmds.empty() && !contains(cmds.back(), "--device") && contains(cmds.back(), "-ngl 0"));
  // y si la GPU tampoco arranca, a la CPU
  fake_server(gpu, LIST_SNAPDRAGON, "HTP0,GPUOpenCL");
  b.stop();
  b.ensure("", {{"n_ctx", 2048}, {"gpu_layers", -1}, {"device", "GPUOpenCL"}});
  CHECK(profile["llm"] == json({{"gpu_layers", 0}, {"device", ""}}));
  CHECK_EQ(b.variant, std::string("embedded"));
  CHECK(contains(jstr(jparse(*read_file(d / "nexus" / "android_accel.json")), "gpu_broken"), "GPUOpenCL"));
  b.stop();
  for (auto* k : {"NEXUS_RUNTIME_LLAMA", "NEXUS_RUNTIME_LLAMA_CPU", "NEXUS_RUNTIME_LLAMA_GPU", "NEXUS_ANDROID_LIBS"})
    set_env(k, "");
}

TEST("android: nucleos, candidatos, temperatura, bateria y consejo de modelo") {
  fs::path d = tmpdir("nucleos");
  std::string text;
  std::vector<std::string> parts = {"0xd46", "0xd46", "0xd46", "0xd46", "0xd4d", "0xd4d", "0xd4d", "0xd4e"};
  for (size_t i = 0; i < parts.size(); ++i) text += "processor\t: " + std::to_string(i) + "\nCPU part\t: " + parts[i] + "\n\n";
  write_file(d / "cpuinfo", text);
  CHECK(at::core_layout(d / "cpuinfo", d / "nada") == json({{"total", 8}, {"fast", 4}, {"source", "cpuinfo"}}));
  std::vector<long> fr = {1800000, 1800000, 1800000, 1800000, 2800000, 2800000, 2800000, 3200000};
  for (size_t i = 0; i < fr.size(); ++i) {
    mkdirs(d / "cpu" / ("cpu" + std::to_string(i)) / "cpufreq");
    write_file(d / "cpu" / ("cpu" + std::to_string(i)) / "cpufreq" / "cpuinfo_max_freq", std::to_string(fr[i]));
  }
  CHECK(at::core_layout(d / "nada", d / "cpu") == json({{"total", 8}, {"fast", 4}, {"source", "cpufreq"}}));
  CHECK(at::thread_candidates({{"fast", 4}, {"total", 8}}) == std::vector<int>({3, 4, 6}));
  CHECK(at::thread_candidates({{"fast", 2}, {"total", 8}}) == std::vector<int>({2, 4}));
  CHECK(at::throttle(4, {{"thermal", 3}}) == std::make_pair(2, std::string("movil caliente (estado termico 3)")));
  CHECK(at::throttle(5, {{"thermal", 3}}).first == 3);   // redondea hacia arriba
  CHECK(at::throttle(4, {{"battery", 10}}).second == "bateria al 10%");
  CHECK(at::throttle(4, {{"battery", 10}, {"charging", true}}).first == 4);
  CHECK(at::throttle(4, {{"thermal", 2}}) == std::make_pair(3, std::string("movil templado")));
  CHECK(at::device_state(d / "no.json").empty());
  write_file(d / "st.json", R"({"t": 1000, "thermal": 3})");
  CHECK(at::device_state(d / "st.json").empty());   // caducado
  json cat = json::array({{{"id", "qwen3.5-2b"}, {"name", "Qwen3.5 2B"}, {"min_ram_gb", 6}},
                          {{"id", "qwen3.5-4b"}, {"name", "Qwen3.5 4B"}, {"min_ram_gb", 8}}});
  CHECK_EQ(jstr(at::model_advice("qwen3.5-2b", 2.0, 8, cat), "id"), std::string("qwen3.5-0.8b"));
  CHECK_EQ(jstr(at::model_advice("qwen3.5-2b", 20.0, 12, cat), "id"), std::string("qwen3.5-4b"));
  CHECK(at::model_advice("qwen3.5-2b", 20.0, 4, cat).is_null());
  CHECK(at::model_advice("qwen3.5-2b", 8.0, 12, cat).is_null());
  CHECK(at::warmup_ok({{"battery", 20}}, 8, 2).second == "bateria al 20%");
  CHECK(at::warmup_ok(json::object(), 1.0, 2).first == false);
  CHECK(at::warmup_ok(json::object(), 8, 2).first);
}

TEST("android: el autoajuste compara GPU y CPU y despues mide los hilos") {
  fs::path d = tmpdir("tuner");
  set_env("NEXUS_DATA_DIR", S(d / "nexus"));
  mkdirs(d / "nexus");
  set_env("NEXUS_DEVICE_STATE", S(d / "device_state.json"));
  set_env("NEXUS_RUNTIME_LLAMA_CPU", "/libs/libllama-server-i8mm.so");
  set_env("NEXUS_RUNTIME_LLAMA_GPU", std::string("/libs/") + aa::GPU_EXE);
  reset_config();
  auto s = std::make_unique<Services>(false);
  s->llm->update_llm_profile({{"n_threads", 4}, {"n_ctx", 4096}, {"gpu_layers", -1}, {"device", "Vulkan0"}});
  auto answer = [&](double tps) {
    for (int i = 0; i < 3; ++i) s->monitor->record_llm("llamaserver", "m", 100, 100 / tps + 1, 1.0);
  };
  at::LiveTuner t(*s, 15, {{"fast", 4}, {"total", 8}});
  json r = t.step();
  CHECK(r["gpu"] == "Vulkan0" && r["accel"]["mode"] == "gpu");
  CHECK_EQ((int)jnum(s->llm->llm_profile()["gpu_layers"]), -1);
  answer(12.0);
  t.step();
  CHECK(s->llm->llm_profile()["gpu_layers"] == 0 && s->llm->llm_profile()["device"] == "");   // ahora mide la CPU
  answer(7.0);
  r = t.step();
  CHECK_EQ(jstr(r["accel"], "choice"), std::string("gpu"));
  CHECK_EQ(jstr(s->llm->llm_profile(), "device"), std::string("Vulkan0"));
  CHECK_EQ(jstr(r, "llama_variant"), std::string("gpu:Vulkan0"));
  CHECK(truthy(r["exploring"]) && s->llm->llm_profile()["n_threads"] == 3);
  // los hilos: 3 -> 4 -> 6 y se queda con el mas rapido
  answer(6.0);
  t.step();
  CHECK(s->llm->llm_profile()["n_threads"] == 4);
  answer(9.0);
  t.step();
  CHECK(s->llm->llm_profile()["n_threads"] == 6);
  answer(7.0);
  r = t.step();
  CHECK(s->llm->llm_profile()["n_threads"] == 4 && r["best"] == 4 && !truthy(r["exploring"]));
  CHECK(r["tps"] == json({{"3", 6.0}, {"4", 9.0}, {"6", 7.0}}));
  // calor: menos hilos sin medir
  write_file(d / "device_state.json", cat(R"({"t": )", (long long)now(), R"(, "thermal": 3})"));
  r = t.step();
  CHECK(s->llm->llm_profile()["n_threads"] == 2 && contains(jstr(r, "throttled"), "caliente"));
  t.stop();
  s->shutdown();
  for (auto* k : {"NEXUS_DEVICE_STATE", "NEXUS_RUNTIME_LLAMA_CPU", "NEXUS_RUNTIME_LLAMA_GPU"}) set_env(k, "");
}
