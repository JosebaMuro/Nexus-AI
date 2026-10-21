// Backends Ollama y compatible con OpenAI (llm/backends.py).
#include "llm/llm.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::llm {

static const auto L = log::get("llm");

static std::string http_error(const http::Response& r) {
  if (r.status == 0) return r.error.empty() ? "sin conexion" : r.error;
  return cat("HTTP ", r.status, ": ", uleft(r.body, 300));
}

// ---------------------------------------------------------------- Ollama
OllamaBackend::OllamaBackend(std::string url, double timeout) : url_(rtrim(url, "/")), timeout_(timeout) {}

bool OllamaBackend::available() { return http::get(url_ + "/api/version", {}, 1.5).status == 200; }

std::vector<std::string> OllamaBackend::models() {
  std::vector<std::string> out;
  auto r = http::get(url_ + "/api/tags", {}, 3);
  if (!r.ok()) return out;
  json j = r.json_body();
  if (!j.is_object()) return out;
  for (auto& m : j.value("models", json::array())) out.push_back(jstr(m, "name"));
  return out;
}

void OllamaBackend::stream_chat(const json& messages, const std::string& model_in, const Opts& opts, const Sink& out) {
  std::string model = model_in;
  if (model.empty()) {
    auto ms = models();
    model = ms.empty() ? "" : ms[0];
  }
  json msgs = json::array();
  for (auto& m : merge_roles(messages)) {
    json x = {{"role", m["role"]}, {"content", m["content"]}};
    if (truthy(m.value("images", json()))) x["images"] = m["images"];
    msgs.push_back(x);
  }
  const json& o = opts.o;
  json options = {{"temperature", o.value("temperature", json(0.7))}, {"num_predict", o.value("max_tokens", json(1024))}};
  if (truthy(o.value("n_ctx", json()))) options["num_ctx"] = o["n_ctx"];
  if (truthy(o.value("n_threads", json()))) options["num_thread"] = o["n_threads"];
  if (jnum(o.value("gpu_layers", json(-1)), -1) >= 0) options["num_gpu"] = o["gpu_layers"];
  if (options["temperature"].is_null()) options["temperature"] = 0.7;
  if (options["num_predict"].is_null()) options["num_predict"] = 1024;
  Pipe pipe(*this, jstr(o, "think_mode", "tags"), out, opts.on_reasoning);
  long eval_count = 0;
  std::string err;
  http::Request req;
  req.method = "POST";
  req.url = url_ + "/api/chat";
  req.body = jdump({{"model", model}, {"messages", msgs}, {"stream", true}, {"options", options}});
  req.content_type = "application/json";
  req.timeout = timeout_;
  auto r = http::stream_lines(req, [&](const std::string& line) {
    if (line.empty()) return true;
    json d = jparse(line);
    if (!d.is_object()) return true;
    if (truthy(d.value("error", json()))) {
      err = jstr(d, "error");
      return false;
    }
    json msg = d.value("message", json::object());
    std::string thinking = jstr(msg, "thinking");
    if (!thinking.empty()) {
      pipe.sp.server_split();
      pipe.sp.add_thought(thinking);
      if (opts.on_reasoning) opts.on_reasoning(thinking);
    }
    std::string piece = jstr(msg, "content");
    if (!piece.empty()) pipe.piece(piece);
    if (truthy(d.value("done", json(false)))) eval_count = (long)jnum(d.value("eval_count", json(0)));
    return true;
  });
  if (!err.empty()) throw std::runtime_error(err);
  if (!r.ok()) throw std::runtime_error("Ollama: " + http_error(r));
  pipe.finish(eval_count);
}

void OllamaBackend::pull(const std::string& name, const std::function<bool(const json&)>& on_event) {
  http::Request req;
  req.method = "POST";
  req.url = url_ + "/api/pull";
  req.body = jdump({{"name", name}, {"stream", true}});
  req.content_type = "application/json";
  req.timeout = 30;
  req.read_timeout = 3600;
  auto r = http::stream_lines(req, [&](const std::string& line) {
    if (line.empty()) return true;
    json ev = jparse(line);
    return ev.is_object() ? on_event(ev) : true;
  });
  if (r.status == 0) throw std::runtime_error(http_error(r));
}

// ---------------------------------------------------------------- OpenAI compatible
OpenAICompatBackend::OpenAICompatBackend(std::string base_url, std::string api_key, std::string default_model,
                                         double timeout)
    : base_url_(rtrim(base_url, "/")), api_key_(std::move(api_key)), default_model_(std::move(default_model)),
      timeout_(timeout) {}

std::vector<std::pair<std::string, std::string>> OpenAICompatBackend::headers() const {
  if (api_key_.empty()) return {};
  return {{"Authorization", "Bearer " + api_key_}};
}

bool OpenAICompatBackend::available() {
  if (base_url_.empty()) return false;
  return http::get(base_url_ + "/models", headers(), 1.5).status == 200;
}

std::vector<std::string> OpenAICompatBackend::models() {
  std::vector<std::string> out;
  auto r = http::get(base_url_ + "/models", headers(), 3);
  if (!r.ok()) return out;
  json j = r.json_body();
  if (j.is_object() && j.contains("data"))
    for (auto& m : j["data"]) out.push_back(jstr(m, "id"));
  return out;
}

void OpenAICompatBackend::stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) {
  const json& o = opts.o;
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
  std::string mdl = model.empty() ? default_model_ : model;
  if (mdl.empty()) {
    auto ms = models();
    mdl = ms.empty() ? "default" : ms[0];
  }
  json temp = o.value("temperature", json(0.7)), mt = o.value("max_tokens", json(1024));
  json body = {{"model", mdl},
               {"messages", msgs},
               {"stream", true},
               {"temperature", temp.is_null() ? json(0.7) : temp},
               {"max_tokens", mt.is_null() ? json(1024) : mt}};
  if (jstr(o, "reasoning") == "off") body["chat_template_kwargs"] = {{"enable_thinking", false}};
  for (auto& [k, v] : o.value("extra_body", json::object()).items()) body[k] = v;
  bool with_tools = truthy(o.value("tool_specs", json()));
  if (with_tools) {
    json tools = json::array();
    for (auto& t : o["tool_specs"]) {
      json params = t.value("parameters", json());
      if (!truthy(params)) params = {{"type", "object"}, {"properties", json::object()}};
      tools.push_back({{"type", "function"},
                       {"function", {{"name", t["name"]}, {"description", t["description"]}, {"parameters", params}}}});
    }
    body["tools"] = tools;
  }
  Pipe pipe(*this, jstr(o, "think_mode", "tags"), out, opts.on_reasoning);
  std::map<long, std::pair<std::string, std::string>> calls;
  http::Request req;
  req.method = "POST";
  req.url = base_url_ + "/chat/completions";
  req.headers = headers();
  req.body = jdump(body);
  req.content_type = "application/json";
  req.timeout = timeout_;
  bool done = false;
  auto r = http::stream_lines(req, [&](const std::string& line) {
    if (!starts_with(line, "data:")) return true;
    std::string payload = trim(line.substr(5));
    if (payload == "[DONE]") {
      done = true;
      return false;
    }
    json d = jparse(payload);
    if (!d.is_object() || !d.contains("choices") || !d["choices"].is_array() || d["choices"].empty()) return true;
    json delta = d["choices"][0].value("delta", json::object());
    if (!delta.is_object()) return true;
    std::string thought = jstr(delta, "reasoning_content");
    if (thought.empty()) thought = jstr(delta, "reasoning");
    if (!thought.empty()) {
      pipe.sp.server_split();
      ++pipe.sp.raw;
      pipe.sp.add_thought(thought);
      if (opts.on_reasoning) opts.on_reasoning(thought);
    }
    for (auto& tc : delta.value("tool_calls", json::array())) {
      auto& slot = calls[(long)jnum(tc.value("index", json(0)))];
      json fn = tc.value("function", json::object());
      slot.first += jstr(fn, "name");
      slot.second += jstr(fn, "arguments");
    }
    std::string content = jstr(delta, "content");
    if (!content.empty()) pipe.piece(content);
    return true;
  });
  if (r.status >= 400 && with_tools) throw ToolsUnsupported(uleft(r.body, 300));
  if (!r.ok() && !done) throw std::runtime_error(http_error(r));
  for (auto& [i, c] : calls) {
    json args = jparse(c.second.empty() ? "{}" : c.second);
    if (args.is_discarded()) args = json{{"input", c.second}};
    ++pipe.sp.visible;
    pipe.piece(tool_call_text(c.first, args));
    break;   // una herramienta por paso
  }
  pipe.finish((long)pipe.sp.raw);
}

}  // namespace nx::llm
