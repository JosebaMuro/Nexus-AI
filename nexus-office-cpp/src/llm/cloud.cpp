// IA online para el agente Profesor (nexus/llm/cloud.py). Claude por la Messages API de Anthropic (HTTP: no hay SDK
// oficial para C++) y el resto por su API compatible con OpenAI, siempre en streaming. Las claves son secretos de la
// configuracion (nube.clave_<id>) y nunca salen por la API.
#include "llm/llm.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/text.h"

namespace nx::llm {

const std::vector<CloudProvider>& cloud_providers() {
  static const std::vector<CloudProvider> P = {
      {"gemini", "Google Gemini", "openai", "https://generativelanguage.googleapis.com/v1beta/openai",
       "gemini-2.5-flash", "https://aistudio.google.com/apikey"},
      {"openai", "ChatGPT (OpenAI)", "openai", "https://api.openai.com/v1", "gpt-4.1-mini",
       "https://platform.openai.com/api-keys"},
      {"perplexity", "Perplexity", "openai", "https://api.perplexity.ai", "sonar", "https://www.perplexity.ai/settings/api"},
      {"claude", "Claude (Anthropic)", "anthropic", "https://api.anthropic.com", "claude-opus-5-5",
       "https://platform.claude.com/settings/keys"},
      {"mistral", "Mistral", "openai", "https://api.mistral.ai/v1", "mistral-small-latest",
       "https://console.mistral.ai/api-keys"},
      {"deepseek", "DeepSeek", "openai", "https://api.deepseek.com/v1", "deepseek-chat",
       "https://platform.deepseek.com/api_keys"},
      {"groq", "Groq", "openai", "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile", "https://console.groq.com/keys"},
      {"openrouter", "OpenRouter", "openai", "https://openrouter.ai/api/v1", "openrouter/auto", "https://openrouter.ai/keys"},
      {"compatible", "Otro compatible con OpenAI", "openai", "", "", ""},
  };
  return P;
}

const std::vector<std::string> CLOUD_EFFORTS = {"low", "medium", "high", "xhigh", "max"};

namespace {

const CloudProvider* find_provider(const std::string& id) {
  for (auto& p : cloud_providers())
    if (p.id == id) return &p;
  return nullptr;
}

std::string mime_of(const std::string& b64) {
  std::string head = b64decode(b64.substr(0, std::min<size_t>(16, b64.size())));
  if (starts_with(head, "\xff\xd8")) return "image/jpeg";
  if (head.size() >= 12 && head.substr(8, 4) == "WEBP") return "image/webp";
  if (starts_with(head, "GIF")) return "image/gif";
  return "image/png";
}

std::string error_message(const std::string& name, const http::Response& r) {
  if (r.status == 0) return "No se puede conectar con " + name + " (¿hay internet?)" + (r.error.empty() ? "" : ": " + r.error);
  if (r.status == 401 || r.status == 403) return name + ": la clave no es válida (" + std::to_string(r.status) + ")";
  if (r.status == 429) return name + ": límite de uso alcanzado, prueba en un rato";
  json e = jparse(r.body);
  if (e.is_array() && !e.empty()) e = e[0];
  std::string msg;
  if (e.is_object()) {
    if (e.contains("error") && e["error"].is_object()) msg = jstr(e["error"], "message");
    else if (e.contains("error") && e["error"].is_string()) msg = e["error"].get<std::string>();
    if (msg.empty()) msg = jstr(e, "message");
  }
  if (msg.empty()) msg = r.body;
  return name + ": error " + std::to_string(r.status) + ": " + uleft(msg, 300);
}

}  // namespace

std::map<std::string, std::string>& CloudBackend::base_override() {
  static std::map<std::string, std::string> m;
  return m;
}

std::string CloudBackend::key(const std::string& prov) const { return config_.str("nube.clave_" + prov); }

std::pair<std::string, std::string> CloudBackend::resolve(const std::string& spec) const {
  std::string prov = spec, model;
  if (auto c = spec.find(':'); c != std::string::npos) prov = spec.substr(0, c), model = spec.substr(c + 1);
  std::string chosen = config_.str("nube.proveedor");
  if (prov.empty()) prov = chosen;
  if (model.empty() && prov == chosen) model = config_.str("nube.modelo");
  const CloudProvider* p = find_provider(prov);
  return {prov, !model.empty() ? model : p ? p->modelo : ""};
}

std::string CloudBackend::base_url(const std::string& prov) const {
  if (auto it = base_override().find(prov); it != base_override().end()) return it->second;
  if (prov == "compatible") return rtrim(config_.str("nube.base_url"), "/");
  const CloudProvider* p = find_provider(prov);
  return p ? p->base_url : "";
}

bool CloudBackend::ready(const std::string& spec) const {
  auto [prov, model] = resolve(spec);
  return find_provider(prov) && (!key(prov).empty() || prov == "compatible") && !model.empty() &&
         !base_url(prov).empty();
}

std::vector<std::string> CloudBackend::models() {
  auto [prov, model] = resolve();
  if (!ready()) return {};
  return {prov + ":" + model};
}

void CloudBackend::stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) {
  auto [prov, mdl] = resolve(model);
  if (!ready(model))
    throw CloudError("IA online sin configurar (" + (prov.empty() ? std::string("ningun proveedor") : prov) +
                     "): pon la clave en Ajustes > Profesor");
  Pipe pipe(*this, "tags", out, opts.on_reasoning);
  if (find_provider(prov)->estilo == "anthropic") anthropic(messages, mdl, opts, pipe);
  else openai(prov, messages, mdl, opts, pipe);
  pipe.finish((long)pipe.sp.raw);
}

void CloudBackend::openai(const std::string& prov, const json& messages, const std::string& model, const Opts& opts,
                          Pipe& pipe) {
  const CloudProvider& P = *find_provider(prov);
  json msgs = json::array();
  for (auto& m : merge_roles(messages)) {
    if (truthy(m.value("images", json()))) {
      json parts = json::array({{{"type", "text"}, {"text", m["content"]}}});
      for (auto& b : m["images"])
        parts.push_back({{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64," + b.get<std::string>()}}}});
      msgs.push_back({{"role", m["role"]}, {"content", parts}});
    } else {
      msgs.push_back({{"role", m["role"]}, {"content", m["content"]}});
    }
  }
  json temp = opts.o.value("temperature", json(0.7));
  long mt = (long)jnum(opts.o.value("max_tokens", json(0)));
  json body = {{"model", model}, {"messages", msgs}, {"stream", true},
               {"temperature", temp.is_null() ? json(0.7) : temp}, {"max_tokens", std::max(mt, 2048L)}};
  http::Request req;
  req.method = "POST";
  req.url = base_url(prov) + "/chat/completions";
  if (!key(prov).empty()) req.headers.push_back({"Authorization", "Bearer " + key(prov)});
  if (prov == "openrouter") {
    req.headers.push_back({"HTTP-Referer", "https://github.com/JosebaMuro/Nexus-AI"});
    req.headers.push_back({"X-Title", "Nexus-Office"});
  }
  req.body = jdump(body);
  req.content_type = "application/json";
  req.timeout = timeout_;
  json citations = json::array();
  bool done = false;
  auto r = http::stream_lines(req, [&](const std::string& line) {
    if (!starts_with(line, "data:")) return true;
    std::string payload = trim(line.substr(5));
    if (payload == "[DONE]") {
      done = true;
      return false;
    }
    json d = jparse(payload);
    if (!d.is_object()) return true;
    if (d.contains("citations") && d["citations"].is_array() && !d["citations"].empty()) citations = d["citations"];
    if (!d.contains("choices") || !d["choices"].is_array() || d["choices"].empty()) return true;
    json delta = d["choices"][0].value("delta", json::object());
    if (!delta.is_object()) return true;
    std::string thought = jstr(delta, "reasoning_content");
    if (thought.empty()) thought = jstr(delta, "reasoning");
    if (!thought.empty()) {
      pipe.sp.server_split();
      pipe.sp.add_thought(thought);
      if (opts.on_reasoning) opts.on_reasoning(thought);
    }
    std::string content = jstr(delta, "content");
    if (!content.empty()) pipe.piece(content);
    return true;
  });
  if (!r.ok() && !done) throw CloudError(error_message(P.nombre, r));
  if (!citations.empty()) {   // Perplexity: fuentes de la respuesta
    std::string f = "\n\nFuentes:";
    int i = 0;
    for (auto& u : citations) f += "\n[" + std::to_string(++i) + "] " + (u.is_string() ? u.get<std::string>() : u.dump());
    pipe.piece(f);
  }
}

void CloudBackend::anthropic(const json& messages, const std::string& model, const Opts& opts, Pipe& pipe) {
  std::vector<std::string> sys;
  json rest = json::array();
  for (auto& m : messages) {
    if (jstr(m, "role") == "system") sys.push_back(jstr(m, "content"));
    else rest.push_back(m);
  }
  json msgs = json::array();
  for (auto& m : merge_roles(rest)) {
    json content = json::array();
    for (auto& b : m.value("images", json::array())) {
      std::string d = b.get<std::string>();
      content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", mime_of(d)}, {"data", d}}}});
    }
    std::string text = jstr(m, "content");
    content.push_back({{"type", "text"}, {"text", text.empty() ? "." : text}});
    msgs.push_back({{"role", m["role"]}, {"content", content}});
  }
  if (msgs.empty() || jstr(msgs[0], "role") != "user")
    msgs.insert(msgs.begin(), json{{"role", "user"}, {"content", json::array({{{"type", "text"}, {"text", "."}}})}});
  std::string effort = config_.str("nube.esfuerzo", "medium");
  if (std::find(CLOUD_EFFORTS.begin(), CLOUD_EFFORTS.end(), effort) == CLOUD_EFFORTS.end()) effort = "medium";
  long mt = (long)jnum(opts.o.value("max_tokens", json(0)));
  json body = {{"model", model},
               {"max_tokens", std::max(mt, 16000L)},
               {"stream", true},
               {"messages", msgs},
               {"thinking", {{"type", "adaptive"}, {"display", "summarized"}}},
               {"output_config", {{"effort", effort}}},
               // si el modelo declina por sus filtros de seguridad, Anthropic reintenta con su modelo recomendado
               {"fallbacks", "default"}};
  if (!sys.empty()) body["system"] = join(sys, "\n\n");
  http::Request req;
  req.method = "POST";
  req.url = base_url("claude") + "/v1/messages";
  req.headers = {{"x-api-key", key("claude")},
                 {"anthropic-version", "2023-06-01"},
                 {"anthropic-beta", "server-side-fallback-2026-07-01"}};
  req.body = jdump(body);
  req.content_type = "application/json";
  req.timeout = timeout_;
  std::string stop, err;
  auto r = http::stream_lines(req, [&](const std::string& line) {
    if (!starts_with(line, "data:")) return true;
    json d = jparse(trim(line.substr(5)));
    if (!d.is_object()) return true;
    std::string type = jstr(d, "type");
    if (type == "content_block_delta") {
      json delta = d.value("delta", json::object());
      std::string dt = jstr(delta, "type");
      if (dt == "thinking_delta") {
        std::string t = jstr(delta, "thinking");
        pipe.sp.server_split();
        pipe.sp.add_thought(t);
        if (opts.on_reasoning && !t.empty()) opts.on_reasoning(t);
      } else if (dt == "text_delta") {
        std::string t = jstr(delta, "text");
        if (!t.empty()) pipe.piece(t);
      }
    } else if (type == "message_delta") {
      std::string s = jstr(d.value("delta", json::object()), "stop_reason");
      if (!s.empty()) stop = s;
    } else if (type == "error") {
      err = jstr(d.value("error", json::object()), "message", "error del servidor");
      return false;
    }
    return true;
  });
  if (!r.ok()) {
    if (r.status == 404) throw CloudError("Claude: el modelo " + model + " no existe");
    throw CloudError(error_message("Claude", r));
  }
  if (!err.empty()) throw CloudError("Claude: " + uleft(err, 300));
  if (stop == "refusal") pipe.piece("\n\n_(Claude no ha querido responder a esta petición.)_");
  else if (stop == "max_tokens") pipe.piece("\n\n_(Respuesta cortada por el límite de tokens.)_");
}

json CloudBackend::status() const {
  auto [prov, model] = resolve();
  json provs = json::array();
  for (auto& p : cloud_providers())
    provs.push_back({{"id", p.id}, {"nombre", p.nombre}, {"modelo", p.modelo}, {"clave_en", p.clave_en},
                     {"con_clave", !key(p.id).empty()}});
  return {{"proveedor", prov}, {"modelo", model}, {"listo", ready()}, {"esfuerzo", config_.get("nube.esfuerzo", "medium")},
          {"base_url", config_.get("nube.base_url", "")}, {"proveedores", provs}};
}

json CloudBackend::test(const std::string& spec) {
  double t0 = now();
  try {
    Opts o;
    o.o = {{"temperature", 0}, {"max_tokens", 64}};
    std::string out;
    stream_chat(json::array({{{"role", "user"}, {"content", "Responde solo: OK"}}}), spec, o,
                [&](const std::string& p) { out += p; });
    return {{"ok", true}, {"respuesta", uleft(trim(out), 200)}, {"segundos", round_to(now() - t0, 1)}};
  } catch (const CloudError& e) {
    return {{"ok", false}, {"error", e.what()}};
  }
}

}  // namespace nx::llm
