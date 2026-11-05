// 2.10: video a partir de imagenes (AVI Motion JPEG), sd.cpp dentro de la APK de Android y TAESD.
#include <chrono>
#include <cstring>

#include "check.h"
#include "media/media.h"
#include "media/video.h"
#include "models/runtimes.h"
#include "stb_image_write.h"
#include "util/fs.h"
#include "util/sysinfo.h"
#include "util/text.h"

using namespace nx;

namespace {

fs::path tmpdir(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() /
           ("nexus-vid-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(d);
  return d;
}

void png(const fs::path& p, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> px((size_t)w * h * 3);
  for (size_t i = 0; i < px.size(); i += 3) px[i] = r, px[i + 1] = g, px[i + 2] = b;
  stbi_write_png(S(p).c_str(), w, h, 3, px.data(), w * 3);
}

uint32_t le32(const std::string& s, size_t at) {
  uint32_t v;
  std::memcpy(&v, s.data() + at, 4);
  return v;
}

}  // namespace

TEST("video: AVI Motion JPEG valido con fotogramas, indice y tamanos coherentes") {
  fs::path d = tmpdir("avi");
  png(d / "a.png", 320, 200, 255, 0, 0);
  png(d / "b.png", 200, 320, 0, 0, 255);
  video::Opts o;
  o.w = 160;
  o.h = 90;
  o.fps = 10;
  o.segundos = 1.0;
  o.transicion = 0.3;
  json r = video::render_avi({d / "a.png", d / "no-existe.png", d / "b.png"}, o, d / "v.avi");
  CHECK_EQ(r["images"].get<int>(), 2);
  CHECK_EQ(r["frames"].get<uint32_t>(), 20u);
  CHECK_EQ(r["width"].get<int>(), 160);
  std::string f = read_text(d / "v.avi");
  CHECK_EQ(f.substr(0, 4), std::string("RIFF"));
  CHECK_EQ(f.substr(8, 4), std::string("AVI "));
  CHECK_EQ((size_t)le32(f, 4) + 8, f.size());
  CHECK_EQ(f.substr(108, 4), std::string("vids"));
  CHECK_EQ(f.substr(112, 4), std::string("MJPG"));
  CHECK_EQ(le32(f, 48), 20u);   // dwTotalFrames
  CHECK_EQ(f.substr(220, 4), std::string("movi"));
  // primer fotograma: un JPEG justo detras de «00dc»
  CHECK_EQ(f.substr(224, 4), std::string("00dc"));
  CHECK((uint8_t)f[232] == 0xFF && (uint8_t)f[233] == 0xD8);
  size_t idx = f.rfind("idx1");
  CHECK(idx != std::string::npos);
  CHECK_EQ(le32(f, idx + 4), 20u * 16);
  // la primera entrada del indice apunta a la cabecera del primer fotograma (relativa a «movi»)
  CHECK_EQ(le32(f, idx + 16), 4u);
  bool threw = false;
  try {
    video::render_avi({d / "no-existe.png"}, o, d / "x.avi");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  rm_rf(d);
}

TEST("video: formatos por nombre") {
  video::Opts o;
  video::formato(o, "9:16");
  CHECK(o.w == 720 && o.h == 1280);
  video::formato(o, "1:1");
  CHECK(o.w == 1080 && o.h == 1080);
  video::formato(o, "");
  CHECK(o.w == 1280 && o.h == 720);
}

TEST("sd.cpp en Android: viene en la APK, no se descarga y cae a la CPU si la GPU falla") {
  fs::path d = tmpdir("sd-android");
  fs::path libs = d / "lib";
  mkdirs(libs);
  for (auto n : {"libsd-cli.so", "libsd-server.so", "libsd-cli-vulkan.so", "libsd-server-vulkan.so"}) write_file(libs / n, "x");
  std::string old_android = env("NEXUS_ANDROID"), old_libs = env("NEXUS_ANDROID_LIBS"), old_data = env("NEXUS_DATA_DIR");
  set_env("NEXUS_ANDROID", "1");
  set_env("NEXUS_ANDROID_LIBS", S(libs));
  set_env("NEXUS_DATA_DIR", S(d / "data"));
  if (sys::is_android()) {
    auto vs = runtimes::android_sd_variants();
    CHECK(std::find(vs.begin(), vs.end(), "vulkan") != vs.end());
    CHECK(runtimes::bundled_variants("sd") == vs);
    runtimes::RuntimeManager rm;
    auto f = rm.exe("sd", json{{"accelerator", "opencl"}, {"id", "android-qualcomm"}});
    CHECK(f.has_value());
    CHECK_EQ(f->variant, std::string("vulkan"));
    CHECK_EQ(S(runtimes::sd_server_for(f->exe).filename()), std::string("libsd-server-vulkan.so"));
    // «instalar» en el movil no descarga nada: usa el de la APK
    CHECK_EQ(S(rm.install("sd", "vulkan").filename()), std::string("libsd-cli-vulkan.so"));
    CHECK_EQ(jstr(rm.progress()["sd:vulkan"], "status"), std::string("instalado"));
    // la GPU fallo al generar: la siguiente imagen va a la CPU
    runtimes::mark_sd_broken("vulkan", "vk::DeviceLostError");
    CHECK(!runtimes::sd_broken("vulkan").empty());
    auto g = rm.exe("sd", json{{"accelerator", "opencl"}, {"id", "android-qualcomm"}});
    CHECK(g.has_value());
    if (g) CHECK_EQ(S(g->exe.filename()), std::string(contains(read_text("/proc/cpuinfo"), "asimddp") || read_text("/proc/cpuinfo").empty() ? "libsd-cli.so" : "libsd-cli-vulkan.so"));
  }
  set_env("NEXUS_ANDROID", old_android);
  set_env("NEXUS_ANDROID_LIBS", old_libs);
  set_env("NEXUS_DATA_DIR", old_data);
  rm_rf(d);
}

TEST("sd.cpp: sd-server junto a sd-cli y Windows ARM usa el de x64") {
  CHECK_EQ(S(runtimes::sd_server_for(P("/x/sd-cli")).filename()), std::string("sd-server"));
  CHECK_EQ(S(runtimes::sd_server_for(P("/x/sd-cli.exe")).filename()), std::string("sd-server.exe"));
  CHECK_EQ(S(runtimes::sd_server_for(P("/x/libsd-cli.so")).filename()), std::string("libsd-server.so"));
}

TEST("imagen: TAESD solo cuando la VAE completa seria lenta (CPU, movil, poca VRAM)") {
  fs::path d = tmpdir("taesd");
  write_file(d / "m.gguf", "x");
  write_file(d / "taesd.safetensors", "x");
  json m = {{"id", "sdxs"}, {"path", S(d / "m.gguf")}, {"format", "gguf"},
            {"difusion", {{"arch", "sd1"}, {"solo_difusion", false}, {"necesita", json::array()}}},
            {"componentes", {{"taesd", S(d / "taesd.safetensors")}}}};
  auto has_taesd = [](const media::SdPlan& p) { return std::find(p.model_args.begin(), p.model_args.end(), "--taesd") != p.model_args.end(); };
  CHECK(has_taesd(media::sd_plan(m, json{{"vram_gb", 0}, {"ram_gb", 8}}, "cpu", {"--taesd"})));
  CHECK(!has_taesd(media::sd_plan(m, json{{"vram_gb", 16}, {"ram_gb", 32}}, "cuda", {"--taesd"})));
  CHECK(!has_taesd(media::sd_plan(m, json{{"vram_gb", 0}, {"ram_gb", 8}}, "cpu", {"--vae-tiling"})));   // sd-cli antiguo
  m["gen"] = {{"taesd", false}};
  CHECK(!has_taesd(media::sd_plan(m, json{{"vram_gb", 0}, {"ram_gb", 8}}, "cpu", {"--taesd"})));
  rm_rf(d);
}
