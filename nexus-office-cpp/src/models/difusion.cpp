// Modelos de imagen en GGUF para stable-diffusion.cpp (ver models.h): que arquitectura son, si traen todo (SD 1.x,
// SDXL convertidos con sd-cli) o solo el modelo de difusion (Flux, Z-Image, Qwen-Image, SD3, Chroma...), y donde
// estan las piezas que les faltan (VAE y codificadores de texto).
#include <set>

#include "models/models.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::models {

namespace {

// piezas que necesita cada arquitectura cuando el GGUF es solo el modelo de difusion
const std::map<std::string, std::vector<std::string>> NEEDS = {
    {"sd1", {"vae", "clip_l"}},
    {"sdxl", {"vae", "clip_l", "clip_g"}},
    {"sd3", {"vae", "clip_l", "clip_g", "t5xxl"}},
    {"flux", {"vae", "clip_l", "t5xxl"}},
    {"chroma", {"vae", "t5xxl"}},
    {"z_image", {"vae", "llm"}},
    {"qwen_image", {"vae", "llm"}},
    {"flux2", {"vae", "llm"}},
    {"ovis_image", {"vae", "llm"}},
};

// general.architecture de los GGUF de imagen (city96/ComfyUI-GGUF, unsloth, leejet...)
const std::map<std::string, std::string> ARCHS = {
    {"flux", "flux"},          {"chroma", "chroma"},   {"sd3", "sd3"},           {"sd1", "sd1"},
    {"sdxl", "sdxl"},          {"lumina2", "z_image"}, {"z_image", "z_image"},   {"zimage", "z_image"},
    {"qwen_image", "qwen_image"}, {"qwenimage", "qwen_image"}, {"flux2", "flux2"}, {"aura", "aura"},
    {"hidream", "hidream"},    {"hyvid", "video"},     {"ltxv", "video"},        {"wan", "video"},
    {"cosmos", "video"},       {"ovis_image", "ovis_image"}};

std::string arch_from_name(const std::string& n) {
  static const std::vector<std::pair<Rx, std::string>> R = [] {
    std::vector<std::pair<Rx, std::string>> v;
    v.emplace_back(Rx(R"(z[_-]?image)", Rx::I), "z_image");
    v.emplace_back(Rx(R"(qwen[_-]?image)", Rx::I), "qwen_image");
    v.emplace_back(Rx(R"(flux[_.-]?2)", Rx::I), "flux2");
    v.emplace_back(Rx(R"(chroma)", Rx::I), "chroma");
    v.emplace_back(Rx(R"(flux)", Rx::I), "flux");
    v.emplace_back(Rx(R"(sd[_-]?3|stable[_-]diffusion[_-]3)", Rx::I), "sd3");
    v.emplace_back(Rx(R"(sdxl|sd_xl|xl[_-]turbo)", Rx::I), "sdxl");
    v.emplace_back(Rx(R"(\bwan\d|hunyuan[_-]?video|ltx)", Rx::I), "video");
    return v;
  }();
  for (auto& [rx, a] : R)
    if (rx.test(n)) return a;
  return "";
}

std::string arch_from_tensors(const std::vector<std::string>& t) {
  auto any = [&](const char* s) {
    return std::any_of(t.begin(), t.end(), [&](const std::string& x) { return contains(x, s); });
  };
  if (any("distilled_guidance_layer")) return "chroma";
  if (any("double_blocks.")) return "flux";
  if (any("joint_blocks.")) return "sd3";
  if (any("cap_embedder") || any("noise_refiner")) return "z_image";
  if (any("transformer_blocks.") && any("img_mod")) return "qwen_image";
  if (any("input_blocks.")) return any("label_emb") || any("conditioner.embedders.1") ? "sdxl" : "sd1";
  return "";
}

}  // namespace

json diffusion_info(const json& meta, const std::vector<std::string>& tensors, const std::string& name) {
  std::string ga = lower(jstr(meta, "general.architecture"));
  std::string arch;
  if (ARCHS.count(ga)) arch = ARCHS.at(ga);
  if (arch.empty() && !ga.empty()) return nullptr;   // otra arquitectura conocida por llama.cpp: es un LLM
  if (arch.empty()) arch = arch_from_tensors(tensors);
  if (arch.empty() || arch == "sd1") {
    std::string by_name = arch_from_name(name);
    if (!by_name.empty()) arch = by_name;
  }
  if (arch.empty()) return nullptr;
  auto any_prefix = [&](const char* p) {
    return std::any_of(tensors.begin(), tensors.end(), [&](const std::string& x) { return starts_with(x, p); });
  };
  // un checkpoint completo trae VAE (first_stage_model) y codificador de texto (cond_stage_model, conditioner,
  // text_encoders); los GGUF de solo difusion no
  bool vae = any_prefix("first_stage_model.") || any_prefix("vae.");
  bool te = any_prefix("cond_stage_model.") || any_prefix("conditioner.") || any_prefix("text_encoders.") ||
            any_prefix("te.");
  bool only = !tensors.empty() ? !(vae && te) : arch != "sd1" && arch != "sdxl";
  json needs = json::array();
  if (only && NEEDS.count(arch))
    for (auto& n : NEEDS.at(arch)) needs.push_back(n);
  // pasos y escala por defecto de los destilados (turbo, schnell): pocos pasos y sin CFG
  std::string ln = lower(name);
  json gen = json::object();
  if (arch == "flux" || arch == "chroma" || arch == "z_image" || arch == "flux2" || arch == "qwen_image") {
    gen["cfg"] = 1.0;
    gen["sampler"] = "euler";
  }
  if (contains(ln, "schnell")) gen["steps"] = 4;
  else if (arch == "z_image" && contains(ln, "turbo")) gen["steps"] = 8;
  else if (contains(ln, "turbo") || contains(ln, "lightning")) gen["steps"] = 4;
  else if (arch == "flux") gen["steps"] = 20;
  return {{"arch", arch}, {"solo_difusion", only}, {"necesita", needs}, {"gen", gen}, {"video", arch == "video"}};
}

json diffusion_info(const fs::path& p) {
  json meta;
  try {
    meta = gguf_metadata(p, 64);
  } catch (const std::exception&) {
    return nullptr;
  }
  // un LLM (llama, qwen3, gemma...) se descarta sin leer la lista de tensores
  std::string ga = lower(jstr(meta, "general.architecture"));
  if (!ga.empty() && !ARCHS.count(ga)) return nullptr;
  return diffusion_info(meta, gguf_tensor_names(p, 200000), S(p.filename()) + " " + S(p.parent_path().filename()));
}

json find_components(const fs::path& model, const json& dinfo, const json& configured) {
  json out = json::object();
  if (!dinfo.is_object()) return out;
  std::string arch = jstr(dinfo, "arch");
  static const std::set<std::string> EXT = {".safetensors", ".gguf", ".sft", ".ckpt", ".pt", ".bin"};
  std::vector<fs::path> cands;
  auto add_dir = [&](const fs::path& d, bool rec) {
    if (!is_dir(d)) return;
    for (auto& f : glob(d, "*", rec))
      if (is_file(f) && EXT.count(suffix_lower(f)) && f != model) cands.push_back(f);
  };
  add_dir(model.parent_path(), false);
  add_dir(paths::models_dir(), true);
  auto pattern = [&](const std::string& piece) -> Rx {
    if (piece == "vae")
      return arch == "qwen_image" ? Rx(R"(qwen.?image.?vae|qwen.*vae)", Rx::I)
             : arch == "flux2"    ? Rx(R"(flux.?2.*(vae|ae)|(^|[\W_])ae[\W_].*flux.?2)", Rx::I)
                                  : Rx(R"((^|[\W_])(ae|vae)([\W_]|$)|vae)", Rx::I);
    if (piece == "clip_l") return Rx(R"(clip[_-]?l([\W_]|$))", Rx::I);
    if (piece == "clip_g") return Rx(R"(clip[_-]?g([\W_]|$))", Rx::I);
    if (piece == "t5xxl") return Rx(R"(t5[_-]?xxl|t5[_-]v1[_.]1[_-]xxl|umt5)", Rx::I);
    // codificador de texto con un LLM
    if (arch == "z_image") return Rx(R"(qwen[_-]?3[_-]?4b|qwen3[_-]4b)", Rx::I);
    if (arch == "qwen_image") return Rx(R"(qwen[_-]?2[._]?5[_-]?vl)", Rx::I);
    return Rx(R"(mistral|qwen[_-]?3)", Rx::I);
  };
  for (auto& piece : dinfo.value("necesita", json::array())) {
    std::string pc = piece.get<std::string>();
    std::string set = configured.is_object() ? jstr(configured, pc) : "";
    if (!set.empty() && is_file(P(set))) {
      out[pc] = set;
      continue;
    }
    Rx rx = pattern(pc);
    const fs::path* best = nullptr;
    for (auto& c : cands) {
      std::string n = S(c.filename());
      if (!rx.test(n) || contains(lower(n), "mmproj")) continue;
      // la VAE no puede ser otro modelo de difusion; se prefiere la de la misma carpeta
      if (pc == "vae" && (contains(lower(n), "diffusion") || contains(lower(n), "unet"))) continue;
      if (!best || (c.parent_path() == model.parent_path() && best->parent_path() != model.parent_path())) best = &c;
    }
    if (best) out[pc] = S(*best);
  }
  return out;
}

}  // namespace nx::models
