// Biblioteca RAG y creacion multimedia para los agentes (tools/builtin/rag_tools.py y media_tools.py).
#include <algorithm>

#include "core/services.h"
#include "llm/llm.h"
#include "media/media.h"
#include "media/video.h"
#include "rag/rag.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::tools {

namespace {

json rag_search(const json& args, ToolContext& ctx) {
  // general + propios
  json hits = ctx.services->rag->search(jstr(args, "query"), (int)jnum(args.value("k", json(5)), 5), 0.15, 0, ctx.agent);
  json out = json::array();
  for (auto& h : hits)
    out.push_back({{"document", h["title"]}, {"source", h["source"]}, {"page", h["page"]}, {"score", h["score"]},
                   {"text", uleft(jstr(h, "text"), 700)}});
  return {{"results", out}};
}

json rag_list(const json&, ToolContext& ctx) {
  json out = json::array();
  for (auto& d : ctx.services->rag->list(ctx.agent)) {
    if (out.size() >= 50) break;
    out.push_back({{"id", d["id"]}, {"title", d["title"]}, {"source", d["source"]}, {"pages", d["pages"]}});
  }
  return {{"documents", out}};
}

std::string english_prompt(Services& s, const std::string& prompt) {
  // los modelos de difusion entienden mejor el ingles: se traduce con el LLM si lo hay
  static const Rx NON_EN(R"(\b(de|del|una|un|con|el|la|los|las|y|en|sobre|muy)\b|[áéíóúñ])", Rx::I);
  if (!NON_EN.test(prompt) || !s.llm->has_real_llm()) return prompt;
  try {
    llm::Opts o;
    o.o = {{"max_tokens", 80}, {"temperature", 0.2}};
    std::string out = s.llm->complete(
        "Translate this image description into a concise English prompt for an image generator. Reply with the "
        "prompt only.\n\n" + prompt, "default", o);
    if (!trim(out).empty()) {
      auto lines = splitlines(trim(trim(out), "\""));
      out = lines.empty() ? "" : trim(lines[0]);
    } else {
      out = "";
    }
    size_t n = ulen(out);
    return n > 3 && n < 400 ? out : prompt;
  } catch (...) {
    return prompt;
  }
}

json without_path(json res) {
  res.erase("path");   // el modelo no necesita rutas locales
  return res;
}

json generate_image(const json& args, ToolContext& ctx) {
  Services& s = *ctx.services;
  std::string negative = jstr(args, "negative"), prompt;
  std::string idea = jstr(args, "prompt");
  if (truthy(s.config.get("media.auto_enhance_prompts", true))) {
    if (ctx.emit) ctx.emit({{"type", "progress"}, {"text", "Mejorando el prompt"}});
    json enh = prompt::enhance(s.llm.get(), idea, "image", jstr(args, "style"));
    prompt = jstr(enh, "prompt");
    if (negative.empty()) negative = jstr(enh, "negative");
  } else {
    prompt = english_prompt(s, idea);
  }
  std::optional<long long> seed;
  if (args.contains("seed") && args["seed"].is_number()) seed = (long long)args["seed"].get<double>();
  json res = s.images->generate(prompt, negative, std::nullopt, std::nullopt, seed, idea);
  if (!res.contains("prompt")) res["prompt"] = prompt;
  if (ctx.emit) ctx.emit({{"type", "media"}, {"kind", "image"}, {"url", res["url"]}, {"engine", res["engine"]}});
  return without_path(res);
}

json compose_music(const json& args, ToolContext& ctx) {
  json res = ctx.services->music->generate(jstr(args, "prompt"), jnum(args.value("seconds", json(20)), 20));
  if (ctx.emit) ctx.emit({{"type", "media"}, {"kind", "audio"}, {"url", res["url"]}, {"engine", res["engine"]}});
  return without_path(res);
}

json enhance_prompt(const json& args, ToolContext& ctx) {
  return prompt::enhance(ctx.services->llm.get(), jstr(args, "text"), jstr(args, "target", "image"), jstr(args, "style"));
}

}  // namespace

// imagen para el video: una URL de /outputs/... (lo que devuelve generate_image) o un fichero del sandbox
fs::path video_image_path(const std::string& ref) {
  std::string r = trim(ref);
  if (starts_with(r, "/outputs/")) {
    fs::path root = resolve(paths::outputs_dir());
    fs::path p = resolve(root / P(r.substr(9)));
    if (!is_relative_to(p, root)) throw ToolError("ValueError", "ruta fuera de las salidas: " + r);
    return p;
  }
  return sandbox_safe(r);
}

json crear_video(const json& args, ToolContext& ctx) {
  std::vector<fs::path> imgs;
  json refs = args.value("imagenes", json::array());
  if (refs.is_string()) refs = json::array({refs});
  if (refs.is_array())
    for (auto& r : refs)
      if (r.is_string()) imgs.push_back(video_image_path(r.get<std::string>()));
  if (imgs.empty()) {
    // sin lista: las ultimas imagenes generadas, en el orden en que se crearon
    int n = std::clamp((int)jnum(args.value("ultimas", json(6)), 6), 1, 60);
    auto files = glob(paths::sub({"outputs", "images"}), "*.png");
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return mtime(a) < mtime(b); });
    if ((int)files.size() > n) files.erase(files.begin(), files.end() - n);
    imgs = files;
  }
  if (imgs.empty()) throw ToolError("ValueError", "no hay imagenes: genera alguna o indica sus rutas en 'imagenes'");
  video::Opts o;
  video::formato(o, jstr(args, "formato"));
  o.segundos = jnum(args.value("segundos_por_imagen", json(3)), 3);
  o.transicion = jnum(args.value("transicion", json(0.8)), 0.8);
  o.fps = (int)jnum(args.value("fps", json(24)), 24);
  o.kenburns = truthy(args.value("movimiento", json(true)));
  std::string name = std::to_string((long long)now()) + "_" + media::slug_name(jstr(args, "titulo", "video"), 40, "video");
  fs::path out = paths::sub({"outputs", "videos"}) / P(name + ".avi");
  if (ctx.emit) ctx.emit({{"type", "progress"}, {"text", cat("Montando el video con ", imgs.size(), " imagenes")}});
  json r = video::render_avi(imgs, o, out);
  // proyecto para el editor web (WebM/MP4, titulos, musica): las imagenes se copian a una carpeta servida
  fs::path dir = paths::sub({"outputs", "videos", name});
  json urls = json::array();
  for (size_t i = 0; i < imgs.size(); ++i) {
    fs::path d = dir / P(cat(i, "_", S(imgs[i].filename())));
    std::error_code ec;
    fs::copy_file(imgs[i], d, fs::copy_options::overwrite_existing, ec);
    if (!ec) urls.push_back("/outputs/videos/" + name + "/" + S(d.filename()));
  }
  json proj = {{"titulo", jstr(args, "titulo")}, {"imagenes", urls},   {"segundos", o.segundos},
               {"transicion", o.transicion},     {"formato", jstr(args, "formato", "16:9")}, {"fps", o.fps}};
  write_file(dir / "proyecto.json", jdump(proj, 1));
  r["url"] = "/outputs/videos/" + S(out.filename());
  r["editor"] = "/static/video.html?proyecto=/outputs/videos/" + name + "/proyecto.json";
  r["note"] = "AVI (Motion JPEG): se abre con VLC, el reproductor de Windows o Android. En el editor se exporta a "
              "WebM/MP4 con titulos y musica.";
  if (ctx.emit) ctx.emit({{"type", "media"}, {"kind", "video"}, {"url", r["url"]}, {"editor", r["editor"]}, {"engine", "nexus-video"}});
  return r;
}

namespace {

Register r1("rag_search", rag_search), r2("rag_list", rag_list), r3("generate_image", generate_image),
    r4("compose_music", compose_music), r5("enhance_prompt", enhance_prompt), r6("crear_video", crear_video);

}  // namespace

void link_builtin_media() {}

}  // namespace nx::tools
