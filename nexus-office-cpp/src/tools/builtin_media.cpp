// Biblioteca RAG y creacion multimedia para los agentes (tools/builtin/rag_tools.py y media_tools.py).
#include "core/services.h"
#include "llm/llm.h"
#include "media/media.h"
#include "rag/rag.h"
#include "tools/tools.h"
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

Register r1("rag_search", rag_search), r2("rag_list", rag_list), r3("generate_image", generate_image),
    r4("compose_music", compose_music), r5("enhance_prompt", enhance_prompt);

}  // namespace

void link_builtin_media() {}

}  // namespace nx::tools
