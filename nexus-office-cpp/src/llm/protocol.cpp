// Protocolo de herramientas por texto (llm/protocol.py) y separacion del razonamiento (ThinkSplitter).
#include "llm/llm.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::llm {

static const Rx& TOOL_RE() {
  static const Rx r(R"(<tool>\s*(\{.*?\})\s*</tool>)", Rx::S);
  return r;
}
static const Rx& FENCE_RE() {
  static const Rx r(R"rx(```(?:json|tool)?\s*(\{\s*"(?:name|tool)".*?\})\s*```)rx", Rx::S);
  return r;
}

std::optional<ToolCall> parse_tool_call(const std::string& text) {
  for (const Rx* rx : {&TOOL_RE(), &FENCE_RE()}) {
    auto m = rx->search(text);
    if (!m) continue;
    std::string raw = m->str(1);
    json data = jparse(raw);
    if (data.is_discarded()) data = jparse(replace_all(raw, "'", "\""));
    if (data.is_discarded() || !data.is_object()) continue;
    json name = data.value("name", json());
    if (!truthy(name)) name = data.value("tool", json());
    if (!truthy(name)) continue;
    json args;
    for (const char* k : {"args", "arguments", "parameters"})
      if (truthy(data.value(k, json()))) {
        args = data[k];
        break;
      }
    if (args.is_null()) args = json::object();
    if (!args.is_object()) args = json{{"input", args}};
    std::string sname = name.is_string() ? name.get<std::string>() : name.dump();
    return ToolCall{sname, args, trim(text.substr(0, m->start()))};
  }
  return std::nullopt;
}

std::string format_result(const std::string& name, const json& result) {
  std::string body = result.is_string() ? result.get<std::string>() : pydump(result);
  if (ulen(body) > 6000) body = uleft(body, 6000) + "...[recortado]";
  return "<tool_result name=\"" + name + "\">" + body + "</tool_result>";
}

std::string tool_call_text(const std::string& name, const json& args) {
  return "<tool>" + pydump(json{{"name", name}, {"args", args}}) + "</tool>";
}

std::string tool_instructions(const json& tools) {
  if (!tools.is_array() || tools.empty()) return "";
  std::vector<std::string> lines = {"Tienes herramientas. Para usar una, responde SOLO con una linea asi:",
                                    "<tool>{\"name\": \"NOMBRE\", \"args\": {...}}</tool>",
                                    "Recibiras el resultado en <tool_result>. Despues responde al usuario con normalidad.",
                                    "Usa herramientas solo cuando aporten algo. Herramientas disponibles:"};
  for (auto& t : tools) {
    json props = t.value("parameters", json::object()).value("properties", json::object());
    std::vector<std::string> args;
    for (auto& [k, v] : props.items()) args.push_back(k + ": " + (v.is_object() ? jstr(v, "type", "string") : "string"));
    lines.push_back("- " + jstr(t, "name") + "(" + join(args, ", ") + "): " + jstr(t, "description"));
  }
  return join(lines, "\n");
}

// ---------------------------------------------------------------- razonamiento
bool is_reasoning_model(const json& e) {
  static const Rx R(R"(think|reason|\br1\b|-r1|qwq|magistral|deepseek-r|cogito|phi-4-reasoning)", Rx::I);
  if (e.is_object()) {
    if (e.contains("reasoning")) return truthy(e["reasoning"]);
    return R.test(jstr(e, "id") + " " + jstr(e, "name") + " " + jstr(e, "path"));
  }
  return e.is_string() && R.test(e.get<std::string>());
}

json merge_roles(const json& messages) {
  json out = json::array();
  for (auto& m : messages) {
    if (!out.empty() && out.back()["role"] == m["role"] && m["role"] != "system") {
      json& last = out.back();
      last["content"] = jstr(last, "content") + "\n\n" + jstr(m, "content");
      json imgs = last.value("images", json::array());
      if (!imgs.is_array()) imgs = json::array();
      for (auto& i : m.value("images", json::array())) imgs.push_back(i);
      last["images"] = imgs;
    } else {
      out.push_back(m);
    }
  }
  return out;
}

std::string reasoning_fallback(const std::string& reasoning) {
  std::string tail = uright(trim(reasoning), 1500);
  if (tail.empty()) return "";
  return "_(El modelo ha agotado el límite de tokens mientras razonaba y no ha llegado a escribir la respuesta. "
         "Sube «llm.reasoning_budget» en Ajustes o usa un modelo mayor o una GPU. Lo último que estaba pensando:)_\n\n> " +
         replace_all(tail, "\n", "\n> ");
}

static const std::string OPEN = "<think>", CLOSE = "</think>";

ThinkSplitter::ThinkSplitter(const std::string& mode, size_t hold) : mode_(mode), hold_(hold), pending_(mode == "detect") {}

std::pair<std::string, std::string> ThinkSplitter::out(const std::string& v, const std::string& t) {
  if (!t.empty()) thought_.push_back(t);
  visible += trim(v).size();
  return {v, t};
}

std::pair<std::string, std::string> ThinkSplitter::feed(const std::string& piece) {
  ++raw;
  buf_ += piece;
  std::string vis, th;
  while (!buf_.empty()) {
    if (pending_) {
      std::string low = ltrim(buf_);
      if (starts_with(low, OPEN)) {
        buf_ = low.substr(OPEN.size());
        inside_ = true;
        pending_ = false;
        continue;
      }
      size_t i = buf_.find(CLOSE);
      if (i != std::string::npos) {
        th += buf_.substr(0, i);
        buf_ = ltrim(buf_.substr(i + CLOSE.size()), "\n");
        pending_ = false;
        continue;
      }
      if (ulen(buf_) > hold_) {
        pending_ = false;
        continue;
      }
      break;
    }
    if (inside_) {
      size_t i = buf_.find(CLOSE);
      if (i != std::string::npos) {
        th += buf_.substr(0, i);
        buf_ = ltrim(buf_.substr(i + CLOSE.size()), "\n");
        inside_ = false;
        continue;
      }
      size_t keep = CLOSE.size() - 1;
      if (buf_.size() > keep) {
        // no partir un caracter UTF-8: se retrocede hasta un inicio de caracter
        size_t cut = buf_.size() - keep;
        while (cut > 0 && ((unsigned char)buf_[cut] & 0xC0) == 0x80) --cut;
        th += buf_.substr(0, cut);
        buf_ = buf_.substr(cut);
      }
      break;
    }
    size_t i = buf_.find(OPEN);
    if (i != std::string::npos) {
      vis += buf_.substr(0, i);
      buf_ = buf_.substr(i + OPEN.size());
      inside_ = true;
      continue;
    }
    size_t j = buf_.rfind('<');
    if (j != std::string::npos && starts_with(OPEN, buf_.substr(j)) && buf_.size() - j < OPEN.size()) {
      vis += buf_.substr(0, j);
      buf_ = buf_.substr(j);
    } else {
      vis += buf_;
      buf_.clear();
    }
    break;
  }
  return out(vis, th);
}

std::pair<std::string, std::string> ThinkSplitter::flush() {
  std::string rest;
  rest.swap(buf_);
  if (inside_) return out("", rest);
  return out(rest, "");
}

std::string ThinkSplitter::reasoning() const {
  std::string s;
  for (auto& t : thought_) s += t;
  return s;
}

// ---------------------------------------------------------------- tuberia comun de los backends
Backend::Pipe::Pipe(Backend& be, const std::string& mode, const Sink& o, ReasonFn r)
    : b(be), sp(mode), out(o), on_reasoning(std::move(r)), t0(now()) {}

void Backend::Pipe::piece(const std::string& p) {
  auto [vis, th] = sp.feed(p);
  if (!th.empty() && on_reasoning) on_reasoning(th);
  if (!vis.empty()) {
    if (!first) first = now() - t0;
    ++n;
    out(vis);
  }
}

void Backend::Pipe::finish(long tokens) {
  auto [vis, th] = sp.flush();
  if (!th.empty() && on_reasoning) on_reasoning(th);
  if (!vis.empty()) {
    if (!first) first = now() - t0;
    ++n;
    out(vis);
  }
  b.last_stats = {tokens >= 0 ? tokens : n, now() - t0, first};
  b.last_reasoning = sp.reasoning();
  if (!sp.visible && !trim(b.last_reasoning).empty()) out(reasoning_fallback(b.last_reasoning));
}

std::string Backend::complete(const std::string& prompt, const std::string& model, const Opts& opts) {
  std::string acc;
  stream_chat(json::array({{{"role", "user"}, {"content", prompt}}}), model, opts, [&](const std::string& p) { acc += p; });
  return acc;
}

}  // namespace nx::llm
