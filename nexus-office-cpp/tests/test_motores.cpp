// Motores de inferencia (LiteRT-LM, ONNX Runtime GenAI), runtimes por fabricante y modelos de imagen en GGUF.
#include <chrono>
#include <cstring>

#include "check.h"
#include "core/config.h"
#include "llm/motores.h"
#include "media/media.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "util/archive.h"
#include "util/dynlib.h"
#include "util/fs.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

using namespace nx;
namespace mo = nx::motores;

namespace {

fs::path tmpdir(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() /
           ("nexus-mot-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(d);
  return d;
}

// GGUF v3 minimo: claves de texto y nombres de tensores (sin datos)
void gguf(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& kv,
          const std::vector<std::string>& tensors) {
  std::string b = "GGUF";
  auto u32 = [&](uint32_t v) { b.append((const char*)&v, 4); };
  auto u64 = [&](uint64_t v) { b.append((const char*)&v, 8); };
  auto str = [&](const std::string& s) {
    u64(s.size());
    b += s;
  };
  u32(3);
  u64(tensors.size());
  u64(kv.size());
  for (auto& [k, v] : kv) {
    str(k);
    u32(8);
    str(v);
  }
  for (auto& t : tensors) {
    str(t);
    u32(2);
    u64(64);
    u64(64);
    u32(1);
    u64(0);
  }
  write_file(p, b);
}

bool has_seq(const std::vector<std::string>& v, const std::string& a, const std::string& b) {
  for (size_t i = 0; i + 1 < v.size(); ++i)
    if (v[i] == a && v[i + 1] == b) return true;
  return false;
}
bool has(const std::vector<std::string>& v, const std::string& a) { return std::find(v.begin(), v.end(), a) != v.end(); }

}  // namespace

TEST("imagen gguf: arquitectura, piezas que faltan y ajustes de los destilados") {
  json flux = models::diffusion_info({{"general.architecture", "flux"}}, {"double_blocks.0.img_attn.qkv.weight"},
                                     "flux1-schnell-Q4_K_S.gguf");
  CHECK_EQ(jstr(flux, "arch"), std::string("flux"));
  CHECK(flux["solo_difusion"] == true);
  CHECK_EQ(flux["necesita"], json::array({"vae", "clip_l", "t5xxl"}));
  CHECK(flux["gen"]["steps"] == 4 && flux["gen"]["cfg"] == 1.0 && flux["gen"]["sampler"] == "euler");

  // checkpoint SD 1.x convertido con sd-cli: trae VAE y codificador, no necesita nada
  json sd1 = models::diffusion_info(json::object(),
                                    {"first_stage_model.decoder.conv_in.weight", "cond_stage_model.transformer.x",
                                     "model.diffusion_model.input_blocks.0.0.weight"},
                                    "v1-5-pruned-emaonly-Q8_0.gguf");
  CHECK_EQ(jstr(sd1, "arch"), std::string("sd1"));
  CHECK(sd1["solo_difusion"] == false && sd1["necesita"].empty());

  json z = models::diffusion_info({{"general.architecture", "lumina2"}}, {"cap_embedder.0.weight"}, "z_image_turbo-Q4_K.gguf");
  CHECK_EQ(jstr(z, "arch"), std::string("z_image"));
  CHECK_EQ(z["necesita"], json::array({"vae", "llm"}));
  CHECK(z["gen"]["steps"] == 8);

  // sin general.architecture, por los tensores
  CHECK_EQ(jstr(models::diffusion_info(json::object(), {"joint_blocks.0.x"}, "modelo.gguf"), "arch"), std::string("sd3"));
  CHECK_EQ(jstr(models::diffusion_info(json::object(), {"double_blocks.0.x", "distilled_guidance_layer.in"}, "m.gguf"), "arch"),
           std::string("chroma"));
  // un LLM no es un modelo de imagen
  CHECK(models::diffusion_info({{"general.architecture", "llama"}}, {"blk.0.attn_q.weight"}, "flux-chat.gguf").is_null());
  CHECK(models::diffusion_info({{"general.architecture", "wan"}}, {}, "wan2.1.gguf")["video"] == true);
}

TEST("imagen gguf: el catalogo lo reconoce y sd-cli recibe el modelo con sus piezas") {
  auto d = tmpdir("img");
  set_env("NEXUS_DATA_DIR", S(d / "datos"));
  reset_config();
  fs::path m = d / "flux1-schnell-Q4_K_S.gguf";
  gguf(m, {{"general.architecture", "flux"}}, {"double_blocks.0.img_attn.qkv.weight", "final_layer.linear.weight"});
  write_file(d / "ae.safetensors", "x");
  write_file(d / "clip_l.safetensors", "x");

  json info = models::classify(m);
  CHECK_EQ(jstr(info, "kind"), std::string("image"));
  CHECK_EQ(jstr(info["difusion"], "arch"), std::string("flux"));

  json model = {{"id", "flux"}, {"path", S(m)}, {"format", "gguf"}, {"difusion", info["difusion"]}, {"gen", info["difusion"]["gen"]}};
  // falta el T5-XXL: se dice cual
  media::SdPlan p = media::sd_plan(model, json::object(), "cpu", {});
  CHECK(contains(p.error, "T5-XXL"));
  write_file(d / "t5xxl_fp16.safetensors", "x");
  p = media::sd_plan(model, {{"llm", {{"n_threads", 6}}}}, "cpu", {});
  CHECK(p.error.empty());
  CHECK(has_seq(p.model_args, "--diffusion-model", S(m)));
  CHECK(has_seq(p.model_args, "--vae", S(d / "ae.safetensors")));
  CHECK(has_seq(p.model_args, "--clip_l", S(d / "clip_l.safetensors")));
  CHECK(has_seq(p.model_args, "--t5xxl", S(d / "t5xxl_fp16.safetensors")));
  CHECK(has_seq(p.perf_args, "-t", "6") && has_seq(p.perf_args, "--sampling-method", "euler"));

  // la ruta elegida en Modelos manda sobre la que se encuentra
  write_file(d / "otra" / "mi-vae.safetensors", "x");
  model["componentes"] = {{"vae", S(d / "otra" / "mi-vae.safetensors")}};
  p = media::sd_plan(model, json::object(), "cpu", {});
  CHECK(has_seq(p.model_args, "--vae", S(d / "otra" / "mi-vae.safetensors")));

  // GPU pequena con un modelo que no cabe (5 GB en 4 GB de VRAM): codificadores a la CPU, pesos por partes y VAE por
  // teselas; las versiones nuevas de sd.cpp lo hacen solas (--flash-attn auto, --auto-fit)
  std::filesystem::resize_file(m, 5ull << 30);
  std::set<std::string> viejas = {"--diffusion-fa", "--clip-on-cpu", "--offload-to-cpu", "--vae-tiling", "--sampling-method"};
  p = media::sd_plan(model, {{"vram_gb", 4}}, "vulkan", viejas);
  CHECK(has(p.perf_args, "--diffusion-fa") && has(p.perf_args, "--clip-on-cpu") && has(p.perf_args, "--offload-to-cpu"));
  CHECK(has(p.perf_args, "--vae-tiling"));
  std::set<std::string> nuevas = viejas;
  nuevas.insert({"--flash-attn", "--auto-fit"});
  p = media::sd_plan(model, {{"vram_gb", 4}}, "vulkan", nuevas);
  CHECK(!has(p.perf_args, "--diffusion-fa") && !has(p.perf_args, "--offload-to-cpu"));
  // GPU grande: nada de eso
  p = media::sd_plan(model, {{"vram_gb", 24}}, "cuda", viejas);
  CHECK(!has(p.perf_args, "--offload-to-cpu") && !has(p.perf_args, "--vae-tiling") && !has(p.perf_args, "-t"));

  // Z-Image: el codificador LLM; las versiones antiguas de sd.cpp lo llamaban --qwen2vl
  fs::path z = d / "z" / "z_image_turbo-Q4_K.gguf";
  gguf(z, {{"general.architecture", "lumina2"}}, {"cap_embedder.0.weight"});
  write_file(d / "z" / "ae.safetensors", "x");
  write_file(d / "z" / "Qwen3-4B-Instruct-2507-Q4_K_M.gguf", "x");
  json zm = {{"id", "z"}, {"path", S(z)}, {"format", "gguf"}};
  p = media::sd_plan(zm, json::object(), "cpu", {"--qwen2vl"});
  CHECK_EQ(p.arch, std::string("z_image"));
  CHECK(has_seq(p.model_args, "--qwen2vl", S(d / "z" / "Qwen3-4B-Instruct-2507-Q4_K_M.gguf")));
  CHECK(has_seq(p.model_args, "--vae", S(d / "z" / "ae.safetensors")));
  p = media::sd_plan(zm, json::object(), "cpu", {"--llm", "--qwen2vl"});
  CHECK(has(p.model_args, "--llm"));

  CHECK_EQ(media::sd_flags("  --vae-tiling   x\n  --flash-attn on|off\n -t N").size(), size_t(2));
}

TEST("runtimes: paquetes oficiales y orden por fabricante") {
  if (lower(sys::arch()) == "x86_64" && !sys::is_windows() && !sys::is_macos()) {
    auto ok = [](const std::string& rt, const std::string& v, const std::string& name) {
      return Rx(runtimes::asset_pattern(rt, v)).test(name);
    };
    CHECK(ok("llama", "cpu", "llama-b11982-bin-ubuntu-x64.tar.gz"));
    CHECK(!ok("llama", "cpu", "llama-b11982-bin-ubuntu-vulkan-x64.tar.gz"));
    CHECK(ok("llama", "vulkan", "llama-b11982-bin-ubuntu-vulkan-x64.tar.gz"));
    CHECK(ok("llama", "cuda", "llama-b11982-bin-ubuntu-cuda-12.8-x64.tar.gz"));
    CHECK(ok("llama", "cuda13", "llama-b11982-bin-ubuntu-cuda-13.4-x64.tar.gz"));
    CHECK(!ok("llama", "cuda", "llama-b11982-bin-ubuntu-cuda-13.4-x64.tar.gz"));
    CHECK(ok("llama", "rocm", "llama-b11982-bin-ubuntu-rocm-10.0-x64.tar.gz"));
    CHECK(ok("llama", "openvino", "llama-b11982-bin-ubuntu-openvino-2026.4.1-x64.tar.gz"));
    CHECK(ok("llama", "sycl", "llama-b11982-bin-ubuntu-sycl-fp16-x64.tar.gz"));
    CHECK(ok("sd", "vulkan", "sd-master-1a2b3c4-bin-Linux-Ubuntu-24.04-x86_64-vulkan.zip"));
    CHECK(ok("sd", "cpu", "sd-master-1a2b3c4-bin-Linux-Ubuntu-24.04-x86_64.zip"));
  }
  if (!sys::is_macos()) {
    json rtx50 = {{"accelerator", "cuda"}, {"id", "cuda-nvidia-dgpu"}, {"compute_capability", "12.0"}};
    CHECK_EQ(runtimes::candidates(rtx50, "llama")[0], std::string("cuda13"));
    CHECK_EQ(runtimes::candidates(rtx50, "sd")[0], std::string("cuda"));
    json rtx30 = {{"accelerator", "cuda"}, {"id", "cuda-nvidia-dgpu"}, {"compute_capability", "8.6"}};
    CHECK_EQ(runtimes::candidates(rtx30, "llama")[0], std::string("cuda"));
    json amd = {{"accelerator", "vulkan"}, {"id", "vulkan-amd-dgpu"}};
    CHECK(has(runtimes::candidates(amd, "llama"), "rocm") && has(runtimes::candidates(amd, "llama"), "vulkan"));
    json apu = {{"accelerator", "vulkan"}, {"id", "vulkan-amd-igpu"}};
    CHECK_EQ(runtimes::candidates(apu, "llama")[0], std::string("vulkan"));
    json arc = {{"accelerator", "vulkan"}, {"id", "vulkan-intel-dgpu"}};
    CHECK(has(runtimes::candidates(arc, "llama"), "sycl"));
    CHECK_EQ(json(runtimes::candidates({{"accelerator", "cpu"}, {"id", "cpu-cpu-only"}}, "llama")), json::array({"cpu"}));
  }
}

TEST("motores: formatos, orden de aceleradores y mensajes") {
  CHECK_EQ(mo::motor_de_formato("litertlm"), std::string("litert"));
  CHECK_EQ(mo::motor_de_formato("onnx-genai"), std::string("onnx"));
  CHECK(mo::motor_de_formato("gguf").empty());

  json sd8 = {{"accelerator", "cpu"}, {"npu", {{"present", true}, {"soc", "SM8750"}, {"name", "Qualcomm Hexagon NPU"}}}};
  json para_npu = {{"path", "/m/gemma3-1b-it_q4_ekv1280_sm8750.litertlm"}};
  json generico = {{"path", "/m/gemma3-1b-it-int4.litertlm"}};
  CHECK_EQ(json(mo::orden("litert", sd8, para_npu)), json(std::vector<std::string>{"npu", "cpu"}));
  CHECK_EQ(json(mo::orden("litert", sd8, generico)), json(std::vector<std::string>{"cpu"}));
  // modelo compilado para otro SoC: no se intenta la NPU
  CHECK(!has(mo::orden("litert", sd8, {{"path", "/m/gemma3_mt6991.litertlm"}}), "npu"));
  // Pixel (Tensor G4 = zumapro)
  json pixel = {{"accelerator", "vulkan"}, {"npu", {{"present", true}, {"soc", "Tensor G4"}}}};
  CHECK_EQ(json(mo::orden("litert", pixel, {{"path", "/m/gemma3_Google_Tensor_G4.litertlm"}})),
           json(std::vector<std::string>{"npu", "gpu", "cpu"}));
  json forzado = sd8;
  forzado["llm"] = {{"motor_acelerador", "gpu"}};
  CHECK_EQ(mo::orden("litert", forzado, para_npu)[0], std::string("gpu"));
  CHECK_EQ(json(mo::orden("onnx", sd8, generico)), json(std::vector<std::string>{"modelo", "cpu"}));

  json msgs = json::array({{{"role", "system"}, {"content", "Eres util"}},
                           {{"role", "user"}, {"content", json::array({{{"type", "text"}, {"text", "hola"}},
                                                                       {{"type", "image_url"}, {"image_url", "x"}}})}},
                           {{"role", "tool"}, {"content", "resultado"}},
                           {{"role", "assistant"}, {"content", "vale"}}});
  json s = mo::mensajes_simples(msgs);
  CHECK_EQ(s.size(), size_t(3));
  CHECK_EQ(jstr(s[1], "content"), std::string("hola\n\nresultado"));
  CHECK_EQ(jstr(s[2], "role"), std::string("assistant"));
}

TEST("motores: sin biblioteca no arrancan y el catalogo reconoce sus modelos") {
  auto d = tmpdir("lib");
  set_env("NEXUS_DATA_DIR", S(d / "datos"));
  set_env("NEXUS_RUNTIME_LITERT", S(d / "no-existe"));
  reset_config();
  CHECK(!mo::lib_path("litert"));
  write_file(d / "m.litertlm", "x");
  CHECK_EQ(mo::main_motor({"otro"}), 2);
  CHECK_EQ(mo::main_motor({"litert", "--modelo", S(d / "m.litertlm"), "--puerto", "1"}), 3);
  // biblioteca que no es una biblioteca: no se carga y el proceso acaba con error, sin caerse
  write_file(d / "falsa" / dynlib_name("litert-lm"), "no soy una biblioteca");
  CHECK_EQ(mo::main_motor({"litert", "--modelo", S(d / "m.litertlm"), "--puerto", "1", "--lib", S(d / "falsa" / dynlib_name("litert-lm"))}), 4);
  set_env("NEXUS_RUNTIME_LITERT", "");

  CHECK_EQ(jstr(models::classify(d / "m.litertlm"), "format"), std::string("litertlm"));
  write_file(d / "phi" / "genai_config.json", "{}");
  write_file(d / "phi" / "config.json", "{}");
  json g = models::classify(d / "phi");
  CHECK(jstr(g, "format") == "onnx-genai" && jstr(g, "kind") == "llm");
}

TEST("runtimes: paquete sin conexion junto al programa, una sola vez") {
  auto d = tmpdir("pack");
  set_env("NEXUS_DATA_DIR", S(d / "datos"));
  reset_config();
  fs::path zip = d / "datos" / "paquetes" / "Nexus-Office-runtime-cuda13-2.5.0-cpp-linux-x86_64.zip";
  std::filesystem::create_directories(zip.parent_path());
  {
    archive::ZipWriter z(zip);
    z.add_bytes("runtimes/llama/cuda13/llama-server", "#!/bin/sh\n");
    z.add_bytes("runtimes/llama/cuda13/libggml-cuda.so", "x");
    CHECK(z.close());
  }
  runtimes::RuntimeManager rm;
  json v = rm.import_found();
  CHECK_EQ(v, json::array({"llama/cuda13"}));
  CHECK(rm.find_exe("llama", "cuda13").has_value());
  CHECK(rm.import_found().empty());   // ya importado
  try {
    rm.import_pack(d / "no-existe.zip");
    CHECK(false);
  } catch (const std::runtime_error&) {
  }
}
