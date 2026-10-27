// Gestor de LLM (llm/manager.py): elige backend y modelo, presupuesto de razonamiento y medicion de rendimiento.
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/models.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::llm {

static const auto L = log::get("llm");
const char* REASONING_STOP = "\n\nYa he pensado bastante: respondo ahora con lo que tengo.\n";

static double msg_tokens(const json& m) {
  return ulen(jstr(m, "content")) / 3.2 + 8 + 400.0 * m.value("images", json::array()).size();
}

json fit_context(json msgs, int n_ctx, int reply_tokens) {
  double budget = n_ctx - reply_tokens - 48;
  auto total = [&] {
    double s = 0;
    for (auto& m : msgs) s += msg_tokens(m);
    return s;
  };
  while (total() > budget && msgs.size() > 2) msgs.erase(msgs.begin() + 1);
  double over = total() - budget;
  if (over > 0 && !msgs.empty() && msgs[0]["role"] == "system") {
    size_t keep = (size_t)std::max(400.0, (msg_tokens(msgs[0]) - over) * 3.2);
    msgs[0]["content"] = uleft(jstr(msgs[0], "content"), keep);
  }
  return msgs;
}

LLMManager::LLMManager(Config& config, models::Catalog& catalog, json& profile, hw::Monitor* monitor,
                       runtimes::RuntimeManager* rt)
    : config_(config), catalog_(catalog), profile_(profile), monitor_(monitor), rt_(rt) {
  rebuild();
}

void LLMManager::rebuild() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  double timeout = config_.num("llm.request_timeout", 180);
  for (auto& [n, b] : backends_)
    if (n == "llamaserver") std::static_pointer_cast<LlamaServerBackend>(b)->stop();
  backends_.clear();
  backends_.push_back({"llamaserver", std::make_shared<LlamaServerBackend>(
                                          rt_, resolver("gguf"), [this] { return profile_; },
                                          [this] { return config_.str("hardware.llama_variant"); })});
  std::static_pointer_cast<LlamaServerBackend>(backends_.back().second)->set_llm_opts = [this](const json& o) {
    update_llm_profile(o);
  };
  backends_.push_back({"llamacpp", std::make_shared<UnavailableBackend>("llamacpp")});
  backends_.push_back({"ollama", std::make_shared<OllamaBackend>(config_.str("llm.ollama_url"), timeout)});
  backends_.push_back({"openai", std::make_shared<OpenAICompatBackend>(config_.str("llm.openai_base_url"),
                                                                       config_.str("llm.openai_api_key"),
                                                                       config_.str("llm.openai_model"), timeout)});
  backends_.push_back({"transformers", std::make_shared<UnavailableBackend>("transformers")});
  backends_.push_back({"nube", std::make_shared<CloudBackend>(config_, timeout)});
  backends_.push_back({"builtin", std::make_shared<BuiltinBackend>()});
  avail_.clear();
}

std::shared_ptr<Backend> LLMManager::backend(const std::string& name) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& [n, b] : backends_)
    if (n == name) return b;
  return nullptr;
}

std::vector<std::string> LLMManager::backend_names() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::vector<std::string> v;
  for (auto& [n, b] : backends_) v.push_back(n);
  return v;
}

LlamaServerBackend::Resolver LLMManager::resolver(const std::string& fmt) {
  return [this, fmt](const std::string& model_id) -> std::optional<std::pair<std::string, json>> {
    json entry = model_id.empty() ? json() : catalog_.get(model_id);
    if (entry.is_null()) {
      std::string def = config_.str("llm.default_model");
      if (!def.empty()) entry = catalog_.get(def.substr(def.find(':') == std::string::npos ? 0 : def.find(':') + 1));
    }
    if (entry.is_null())
      for (auto& m : catalog_.list())
        if (jstr(m, "format") == fmt && jstr(m, "kind", "llm") == "llm") {
          entry = m;
          break;
        }
    if (!entry.is_null() && jstr(entry, "format") == fmt) return std::make_pair(jstr(entry, "path"), entry);
    return std::nullopt;
  };
}

bool LLMManager::is_available(const std::string& name) {
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = avail_.find(name);
    if (it != avail_.end() && now() - it->second.first < 15) return it->second.second;
  }
  auto b = backend(name);
  bool ok = b && b->available();
  if (ok && name == "ollama") ok = !b->models().empty();
  std::lock_guard<std::recursive_mutex> lk(mu_);
  avail_[name] = {now(), ok};
  return ok;
}

std::pair<std::string, std::string> LLMManager::select(const std::string& spec) {
  if (!spec.empty() && spec != "default" && contains(spec, ":")) {
    std::string b = spec.substr(0, spec.find(':')), m = spec.substr(spec.find(':') + 1);
    if (b == "nube") {   // IA online: solo con clave; si no, el modelo local de siempre
      if (std::static_pointer_cast<CloudBackend>(backend("nube"))->ready(m)) return {b, m};
    } else if (b == "peer" || is_available(b)) {
      return {b, m};
    }
  }
  std::string def = config_.str("llm.default_model");
  if (!def.empty() && contains(def, ":")) {
    std::string b = def.substr(0, def.find(':')), m = def.substr(def.find(':') + 1);
    if (is_available(b)) return {b, m};
  }
  json entry = def.empty() ? json() : catalog_.get(def);
  std::string engine = config_.str("llm.engine", "auto");
  if (!entry.is_null()) {
    std::string b = jstr(entry, "format") == "gguf" ? "llamacpp" : "transformers";
    if (jstr(entry, "format") == "gguf") {
      if (engine == "llamaserver" || engine == "llamacpp") b = engine;
      else if (is_available("llamaserver")) b = "llamaserver";
    }
    if (is_available(b)) return {b, jstr(entry, "id")};
  }
  std::vector<std::string> order;
  for (auto& x : config_.get("llm.backend_order", json::array())) order.push_back(x.get<std::string>());
  auto names = backend_names();
  if (engine != "auto" && std::find(names.begin(), names.end(), engine) != names.end()) {
    std::erase(order, engine);
    order.insert(order.begin(), engine);
  }
  for (auto& n : names)
    if (n != "nube" && std::find(order.begin(), order.end(), n) == order.end())
      order.insert(n == "llamaserver" ? order.begin() : order.end() - (order.empty() ? 0 : 1), n);
  for (auto& n : order)
    if (n != "nube" && is_available(n)) return {n, ""};
  return {"builtin", ""};
}

json LLMManager::status() {
  auto [b, model] = select();
  if (b == "llamacpp" && model.empty()) {
    auto r = resolver("gguf")("");
    model = r ? jstr(r->second, "id") : "";
  }
  if (b == "ollama" && model.empty()) {
    auto ms = backend("ollama")->models();
    model = ms.empty() ? "" : ms[0];
  }
  if (b == "builtin") model = "nexus-core";
  json avail = json::object();
  for (auto& n : backend_names()) avail[n] = is_available(n);
  return {{"backend", b}, {"model", model}, {"available", avail}};
}

json LLMManager::llm_profile() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return profile_.value("llm", json::object());
}

void LLMManager::update_llm_profile(const json& changes) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!profile_.contains("llm")) return;
  for (auto& [k, v] : changes.items()) profile_["llm"][k] = v;
}

json LLMManager::options(const json& agent_opts) {
  json llm = llm_profile();
  json o = {{"n_ctx", llm["n_ctx"]},
            {"n_threads", llm["n_threads"]},
            {"n_batch", llm["n_batch"]},
            {"gpu_layers", llm["gpu_layers"]},
            {"flash_attn", llm.value("flash_attn", json(false))},
            {"device", llm.value("device", json(""))},
            {"kv_ram", llm.value("kv_ram", json(false))},
            {"mlock", llm.value("mlock", json(false))},
            {"temperature", config_.get("llm.temperature", 0.7)},
            {"max_tokens", config_.get("llm.max_tokens", 1024)}};
  if (agent_opts.is_object())
    for (auto& [k, v] : agent_opts.items())
      if (!v.is_null()) o[k] = v;
  return o;
}

bool LLMManager::reasoning_model(const std::string& b, const std::string& model) {
  if (b == "llamaserver" || b == "llamacpp") {
    auto r = resolver("gguf")(model);
    return r && is_reasoning_model(r->second);
  }
  if (b == "ollama") {
    std::string m = model;
    if (m.empty()) {
      auto ms = backend("ollama")->models();
      m = ms.empty() ? "" : ms[0];
    }
    return is_reasoning_model(m);
  }
  if (b == "openai") return is_reasoning_model(model.empty() ? config_.str("llm.openai_model") : model);
  return false;
}

bool LLMManager::thinking_only(const std::string& b, const std::string& model) {
  std::string ident = model;
  if (b == "llamaserver" || b == "llamacpp" || b == "transformers") {
    auto r = resolver(b == "transformers" ? "transformers" : "gguf")(model);
    if (r) ident = jstr(r->second, "id") + " " + jstr(r->second, "name") + " " + jstr(r->second, "path");
  }
  static const Rx R(R"(thinking|\br1\b|-r1|qwq|deepseek-r)", Rx::I);
  return R.test(ident);
}

void LLMManager::stream(json messages, const std::string& spec, Opts opts, const Sink& out) {
  auto [bname, model] = select(spec);
  if (bname == "peer") {
    if (!remote) throw std::runtime_error("Modo cooperativo no activo");
    opts.on_reasoning = nullptr;
    remote(model, messages, opts, out);
    return;
  }
  auto b = backend(bname);
  json full = options(opts.o);
  int budget = (int)jnum(full.value("reasoning_budget", json(0)));
  full.erase("reasoning_budget");
  if (!budget) budget = (int)config_.num("llm.reasoning_budget", 2048);
  bool thinking = reasoning_model(bname, model);
  bool off = config_.str("llm.reasoning", "auto") == "off" || jstr(full, "reasoning") == "off";
  if (thinking && off && thinking_only(bname, model)) {
    full.erase("reasoning");
    budget = std::min(budget ? budget : 256, 256);
    off = false;
  }
  if (thinking && off) {
    full["reasoning"] = "off";
  } else if (thinking) {
    full["think_mode"] = "detect";
    full["max_tokens"] = (truthy(full["max_tokens"]) ? (int)jnum(full["max_tokens"]) : 1024) + std::max(0, budget);
    if (bname == "llamaserver" && budget > 0)
      full["extra_body"] = {{"reasoning_budget_tokens", budget}, {"reasoning_budget_message", REASONING_STOP}};
  }
  if (bname == "llamaserver" || bname == "llamacpp" || bname == "ollama") {
    int n_ctx = (int)jnum(full["n_ctx"], 4096);
    int cap = thinking ? n_ctx / 2 : n_ctx / 3;
    int mt = truthy(full["max_tokens"]) ? (int)jnum(full["max_tokens"]) : 1024;
    full["max_tokens"] = std::min(mt, std::max(256, cap));
    messages = fit_context(messages, n_ctx, full["max_tokens"].get<int>());
  }
  Opts o2{full, opts.on_reasoning};
  ++inflight;
  try {
    b->stream_chat(messages, model, o2, out);
  } catch (const std::exception& e) {
    --inflight;
    static const Rx OOM(R"(out of memory|failed to allocate|cudaMalloc|insufficient memory|bad_alloc|OOM)", Rx::I);
    if (monitor_) monitor_->record_error(OOM.test(e.what()) ? "oom" : "llm", bname + ": " + e.what());
    throw;
  }
  --inflight;
  if (monitor_ && b->last_stats.tokens)
    monitor_->record_llm(bname, model, (int)b->last_stats.tokens, b->last_stats.seconds, b->last_stats.ttft);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  last_used = {bname, model};
}

std::string LLMManager::complete(const std::string& prompt, const std::string& spec, Opts opts) {
  if (!opts.o.contains("reasoning_budget")) opts.o["reasoning_budget"] = config_.get("llm.reasoning_budget_tasks", 768);
  std::string acc;
  stream(json::array({{{"role", "user"}, {"content", prompt}}}), spec, opts, [&](const std::string& p) { acc += p; });
  return starts_with(acc, "_(El modelo ha agotado") ? "" : acc;
}

bool LLMManager::native_tools(const std::string& spec) {
  std::string b = select(spec).first;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return (b == "llamaserver" || b == "openai") && !native_off.count(b);
}

bool LLMManager::has_real_llm() { return select().first != "builtin"; }

}  // namespace nx::llm
