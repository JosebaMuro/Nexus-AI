// IA de apoyo (memory/assist.py): el embed multilingue selecciona herramientas, corrige nombres y valores,
// enruta agentes, condensa resultados largos y reordena listados. Solo actua con un embedder semantico.
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

#include "memory/memory.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("assist");
static const std::set<std::string> CORE_TOOLS = {"memory_search", "memory_save", "delegate",
                                                 "calculator",    "datetime_now", "rag_search"};

static std::string ckey(const std::string& task, const std::string& text) {
  return task + ":" + sha256_hex(text).substr(0, 24);
}

std::vector<std::string> chunks(const std::string& text, size_t size) {
  static const Rx paras(R"(\n\s*\n)");
  static const Rx sentences(R"((?<=[.!?])\s+)");
  std::vector<std::string> out;
  std::string cur;
  for (auto& para : paras.split(text)) {
    std::vector<std::string> pieces =
        ulen(para) <= size * 1.5 ? std::vector<std::string>{para} : sentences.split(para);
    for (auto p : pieces) {
      if (!cur.empty() && ulen(cur) + ulen(p) > size) {
        out.push_back(cur);
        cur.clear();
      }
      while (ulen(p) > size * 1.5) {   // sin puntos: corte duro
        out.push_back(uleft(p, size));
        p = usub(p, (long)size);
      }
      cur = cur.empty() ? p : cur + "\n\n" + p;
    }
  }
  if (!trim(cur).empty()) out.push_back(cur);
  std::vector<std::string> res;
  for (auto& c : out)
    if (!trim(c).empty()) res.push_back(c);
  return res;
}

SemanticAssist::SemanticAssist(EmbedderGetter emb, Config* config, size_t max_cache)
    : emb_(std::move(emb)), config_(config), max_(max_cache) {}

json SemanticAssist::cfg(const std::string& key, const json& def) {
  try {
    return config_ ? config_->get(key, def) : def;
  } catch (...) {
    return def;
  }
}

bool SemanticAssist::semantic() {
  auto e = emb_();
  if (!e || starts_with(e->id, "hash")) return false;
  return truthy(cfg("assist.enabled", true));
}

json SemanticAssist::info() {
  std::lock_guard<std::mutex> lk(mu_);
  auto e = emb_();
  return {{"embedder", e ? e->id : ""}, {"semantic", e && !starts_with(e->id, "hash")}, {"last", last}};
}

std::optional<Vec> SemanticAssist::cget(const std::string& k) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(k);
  if (it == cache_.end()) return std::nullopt;
  return it->second->second;
}

void SemanticAssist::cput(const std::string& k, Vec v) {
  std::lock_guard<std::mutex> lk(mu_);
  if (cache_.count(k)) return;
  order_.emplace_back(k, std::move(v));
  cache_[k] = std::prev(order_.end());
  while (cache_.size() > max_) {
    cache_.erase(order_.front().first);
    order_.pop_front();
  }
}

Mat SemanticAssist::docs(const std::vector<std::string>& texts, const std::string& task) {
  auto e = emb_();
  std::string tag = e->id + "|" + task;
  std::vector<std::string> keys;
  std::vector<size_t> missing;
  for (size_t i = 0; i < texts.size(); ++i) {
    keys.push_back(ckey(tag, texts[i]));
    if (!cget(keys.back())) missing.push_back(i);
  }
  if (!missing.empty()) {
    std::vector<std::string> m;
    for (size_t i : missing) m.push_back(uleft(texts[i], 2000));
    Mat vecs = e->embed(m);
    for (size_t k = 0; k < missing.size() && k < vecs.size(); ++k) cput(keys[missing[k]], vecs[k]);
  }
  Mat out;
  for (size_t i = 0; i < keys.size(); ++i) {
    auto v = cget(keys[i]);
    if (!v) v = e->embed({uleft(texts[i], 2000)})[0];   // expulsado de la cache mientras tanto
    out.push_back(*v);
  }
  return out;
}

Vec SemanticAssist::query(const std::string& text, const std::string& task) {
  auto e = emb_();
  std::string k = ckey(e->id + "|q|" + task, text);
  if (auto v = cget(k)) return *v;
  Vec v = e->embed_query({uleft(text, 2000)}, task)[0];
  cput(k, v);
  return v;
}

std::vector<float> SemanticAssist::scores(const std::string& q, const std::vector<std::string>& candidates,
                                          const std::string& task) {
  if (candidates.empty() || trim(q).empty()) return std::vector<float>(candidates.size(), 0.0f);
  return matvec(docs(candidates), query(q, task));
}

std::string SemanticAssist::tool_text(const ToolView& t) {
  std::vector<std::string> args;
  json props = t.parameters.is_object() ? t.parameters.value("properties", json::object()) : json::object();
  for (auto& [k, v] : props.items()) args.push_back(uleft(k + ": " + jstr(v, "description"), 120));
  return replace_all(t.name, "_", " ") + ": " + t.description + " (" + join(args, ", ") + ")";
}

std::vector<std::string> SemanticAssist::select_tools(const std::string& text, const std::vector<ToolView>& tools, int k) {
  std::vector<std::string> all;
  for (auto& t : tools) all.push_back(t.name);
  size_t limit = k ? (size_t)k : (size_t)jnum(cfg("assist.max_tools", 10), 10);
  if (tools.size() <= limit || !semantic() || trim(text).empty()) return all;
  std::vector<float> sims;
  try {
    std::vector<std::string> texts;
    for (auto& t : tools) texts.push_back(tool_text(t));
    sims = scores(text, texts, "tools");
  } catch (const std::exception& e) {
    L.warning("Seleccion semantica de herramientas no disponible: ", e.what());
    return all;
  }
  std::string low = lower(text);
  std::set<std::string> keep;
  for (auto& t : tools)
    if (CORE_TOOLS.count(t.name) || contains(low, t.name)) keep.insert(t.name);
  auto order = argsort_desc(sims);
  for (size_t i : order) {
    size_t core = 0;
    for (auto& n : keep) core += CORE_TOOLS.count(n);
    if (keep.size() >= limit + core) break;
    keep.insert(tools[i].name);
  }
  std::vector<std::string> chosen;
  for (auto& t : tools)
    if (keep.count(t.name)) chosen.push_back(t.name);
  json top = json::array();
  for (size_t i = 0; i < order.size() && i < 5; ++i) top.push_back({tools[order[i]].name, round_to(sims[order[i]], 3)});
  std::lock_guard<std::mutex> lk(mu_);
  last = {{"tools", chosen}, {"of", tools.size()}, {"top", top}};
  return chosen;
}

std::optional<std::string> SemanticAssist::fix_tool_name(const std::string& name, const std::vector<ToolView>& available) {
  std::vector<std::string> names;
  for (auto& t : available) names.push_back(t.name);
  if (names.empty()) return std::nullopt;
  if (std::find(names.begin(), names.end(), name) != names.end()) return name;
  static const Rx nonalnum("[^a-z0-9]");
  std::string norm = nonalnum.sub(lower(name), "");
  for (auto& n : names)
    if (nonalnum.sub(lower(n), "") == norm) return n;
  if (!semantic()) return std::nullopt;
  std::vector<float> sims;
  try {
    std::vector<std::string> texts;
    for (auto& t : available) texts.push_back(tool_text(t));
    sims = scores(replace_all(name, "_", " "), texts, "tools");
  } catch (...) {
    return std::nullopt;
  }
  auto order = argsort_desc(sims);
  double best = sims[order[0]], second = order.size() > 1 ? sims[order[1]] : 0.0;
  if (best >= 0.45 && best - second >= 0.04) return names[order[0]];
  return std::nullopt;
}

std::optional<std::string> SemanticAssist::fix_enum(const std::string& value, const json& options) {
  std::vector<std::string> opts;
  for (auto& o : options) opts.push_back(o.is_string() ? o.get<std::string>() : o.dump());
  std::string low = lower(trim(value));
  for (auto& o : opts)
    if (lower(o) == low) return o;
  if (!semantic() || low.empty() || opts.empty()) return std::nullopt;
  std::vector<float> sims;
  try {
    sims = scores(low, opts, "dedupe");
  } catch (...) {
    return std::nullopt;
  }
  auto order = argsort_desc(sims);
  double best = sims[order[0]], second = order.size() > 1 ? sims[order[1]] : 0.0;
  if (best >= 0.5 && best - second >= 0.05) return opts[order[0]];
  return std::nullopt;
}

std::pair<json, std::vector<std::string>> SemanticAssist::repair_args(const json& schema, const json& args) {
  std::vector<std::string> changes;
  json out = args.is_object() ? args : json::object();
  json props = schema.is_object() ? schema.value("properties", json::object()) : json::object();
  if (!props.is_object()) return {out, changes};
  for (auto& [key, spec] : props.items()) {
    if (!out.contains(key) || !spec.is_object() || !spec.contains("enum") || !out[key].is_string()) continue;
    bool in = false;
    for (auto& e : spec["enum"])
      if (e == out[key]) in = true;
    if (in) continue;
    if (auto fixed = fix_enum(out[key].get<std::string>(), spec["enum"])) {
      changes.push_back(key + ": " + out[key].get<std::string>() + " -> " + *fixed);
      out[key] = *fixed;
    }
  }
  return {out, changes};
}

std::optional<std::pair<std::string, json>> SemanticAssist::guess_tool(const std::string& text,
                                                                       const std::vector<ToolView>& tools) {
  std::vector<std::pair<const ToolView*, std::string>> single;
  for (auto& t : tools) {
    json props = t.parameters.value("properties", json::object());
    json req = t.parameters.value("required", json::array());
    if (req.size() == 1 && req[0].is_string()) {
      std::string r = req[0];
      std::string type = props.contains(r) && props[r].is_object() ? props[r].value("type", "string") : "string";
      if (type == "string" && t.risk == "low") single.emplace_back(&t, r);
    }
  }
  if (single.size() < 2 || !semantic()) return std::nullopt;
  std::vector<std::string> texts;
  for (auto& [t, _] : single) texts.push_back(tool_text(*t));
  auto sims = scores(text, texts, "tools");
  auto order = argsort_desc(sims);
  double best = sims[order[0]], second = sims[order[1]];
  if (best < 0.5 || best - second < 0.06) return std::nullopt;
  auto& [t, arg] = single[order[0]];
  return std::make_pair(t->name, json{{arg, text}});
}

std::string SemanticAssist::agent_text(const agents::AgentSpec& a) {
  std::vector<std::string> kw(a.keywords.begin(), a.keywords.begin() + std::min<size_t>(20, a.keywords.size()));
  return a.name + ": " + a.description + ". " + a.specialty + ". " + join(kw, ", ");
}

std::map<std::string, double> SemanticAssist::route_bonus(const std::string& text,
                                                          const std::vector<agents::AgentSpec>& agents) {
  if (agents.size() < 2 || !semantic() || trim(text).empty()) return {};
  std::vector<float> sims;
  try {
    std::vector<std::string> texts;
    for (auto& a : agents) texts.push_back(agent_text(a));
    sims = scores(text, texts, "route");
  } catch (const std::exception& e) {
    L.warning("Enrutado semantico no disponible: ", e.what());
    return {};
  }
  auto order = argsort_desc(sims);
  std::vector<float> sorted(sims);
  std::sort(sorted.begin(), sorted.end());
  size_t n = sorted.size();
  double med = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + (double)sorted[n / 2]) / 2.0;
  double top = sims[order[0]], second = sims[order[1]];
  if (top - med < 0.06) return {};   // nadie destaca sobre el resto
  return {{agents[order[0]].id, round_to(std::min(3.0, 40.0 * (top - second)), 3)}};
}

std::string SemanticAssist::condense(const std::string& q, const std::string& text, size_t budget) {
  if (ulen(text) <= budget || !semantic() || trim(q).empty()) return text;
  auto parts = chunks(text, std::max<size_t>(150, std::min<size_t>(700, budget / 4)));
  if (parts.size() < 3) return text;
  std::vector<float> sims;
  try {
    sims = scores(q, parts, "search");
  } catch (...) {
    return text;
  }
  std::set<size_t> chosen = {0};   // el principio da contexto (titulo, cabecera)
  size_t used = ulen(parts[0]);
  for (size_t i : argsort_desc(sims)) {
    size_t l = ulen(parts[i]);
    if (used + l > budget) continue;
    chosen.insert(i);
    used += l;
  }
  std::vector<std::string> out;
  long prev = -1;
  for (size_t i : chosen) {
    if (prev >= 0 && (long)i != prev + 1) out.push_back("[…]");
    out.push_back(parts[i]);
    prev = (long)i;
  }
  return join(out, "\n\n") + cat("\n\n[resumen semántico: ", chosen.size(), " de ", parts.size(), " fragmentos]");
}

json SemanticAssist::condense_result(const std::string& q, const json& result, size_t budget) {
  if (result.is_string()) return condense(q, result.get<std::string>(), budget);
  if (result.is_object()) {
    json out = json::object();
    for (auto& [k, v] : result.items())
      out[k] = v.is_string() && ulen(v.get_ref<const std::string&>()) > budget ? json(condense(q, v.get<std::string>(), budget))
                                                                                : v;
    return out;
  }
  return result;
}

std::vector<size_t> SemanticAssist::rerank(const std::string& q, const std::vector<std::string>& items) {
  std::vector<size_t> idx(items.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  if (items.size() < 2 || !semantic() || trim(q).empty()) return idx;
  try {
    return argsort_desc(scores(q, items, "search"));
  } catch (...) {
    return idx;
  }
}

void SemanticAssist::prewarm(const std::vector<ToolView>& tools, const std::vector<agents::AgentSpec>& agents) {
  if (!semantic()) return;
  std::vector<std::string> tt, at;
  for (auto& t : tools) tt.push_back(tool_text(t));
  for (auto& a : agents) at.push_back(agent_text(a));
  std::thread([this, tt, at] {
    try {
      docs(tt);
      docs(at);
    } catch (const std::exception& e) {
      L.debug("Precalculo de la IA de apoyo fallido: ", e.what());
    }
  }).detach();
}

// ---------------------------------------------------------------- indexacion semantica en segundo plano
static const int BATCH = 4;            // fragmentos por peticion: el chat nunca espera mas de un lote
static const int REFRESH_EVERY = 256;  // fragmentos convertidos entre recargas de la matriz de busqueda
static auto LE = log::get("embeddings");

EmbedWorker::EmbedWorker(DB& db, EmbedderGetter emb, MemoryStore& memory, std::function<bool()> busy,
                         std::function<void()> invalidate_rag)
    : db_(db), emb_(std::move(emb)), memory_(memory), busy_(std::move(busy)), invalidate_rag_(std::move(invalidate_rag)) {}

EmbedWorker::~EmbedWorker() {
  stop();
  if (th_.joinable()) th_.join();
}

void EmbedWorker::start() {
  if (th_.joinable() && !stop_) {
    wake();
    return;
  }
  if (th_.joinable()) th_.join();
  stop_ = false;
  th_ = std::thread([this] { loop(); });
}

void EmbedWorker::wake() {
  std::lock_guard<std::mutex> lk(mu_);
  wake_ = true;
  cv_.notify_all();
}

void EmbedWorker::stop() {
  stop_ = true;
  wake();
}

void EmbedWorker::wait_for(double seconds) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait_for(lk, std::chrono::milliseconds((long)(seconds * 1000)), [this] { return wake_ || stop_; });
  wake_ = false;
}

json EmbedWorker::pending() {
  std::string id = emb_()->id;
  json mem = db_.scalar("SELECT COUNT(*) n FROM memories WHERE embedder IS NOT ?", {id});
  json rag = db_.scalar("SELECT COUNT(*) n FROM rag_chunks WHERE embedder IS NOT ?", {id});
  return {{"memories", mem}, {"chunks", rag}, {"total", mem.get<long long>() + rag.get<long long>()}};
}

json EmbedWorker::status() {
  json s;
  {
    std::lock_guard<std::mutex> lk(mu_);
    s = state_;
  }
  for (auto& [k, v] : pending().items()) s[k] = v;
  s["embedder"] = emb_()->info();
  return s;
}

void EmbedWorker::loop() {
  while (!stop_) {
    auto emb = emb_();
    if (!emb->slow) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        state_["running"] = false;
        state_["phase"] = "";
      }
      wait_for(30);
      continue;
    }
    bool worked;
    try {
      worked = step(emb);
      std::lock_guard<std::mutex> lk(mu_);
      state_["error"] = "";
    } catch (const std::exception& e) {
      LE.warning("Indexacion semantica en pausa: ", e.what());
      {
        std::lock_guard<std::mutex> lk(mu_);
        state_["error"] = uleft(e.what(), 300);
        state_["running"] = false;
      }
      wait_for(60);
      continue;
    }
    if (!worked) {
      bool was;
      {
        std::lock_guard<std::mutex> lk(mu_);
        was = state_["running"].get<bool>();
      }
      if (was) {
        if (invalidate_rag_) invalidate_rag_();
        memory_.invalidate();
        LE.info("Indexacion semantica completa (", state_["done"].get<long long>(), " vectores)");
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        state_["running"] = false;
        state_["phase"] = "";
        state_["pending"] = 0;
      }
      wait_for(30);
    }
  }
}

bool EmbedWorker::step(const EmbedderPtr& emb) {
  while ((busy_ && busy_()) || emb->waiting > 0) {   // el chat tiene prioridad sobre la indexacion
    if (stop_) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  auto rows = db_.query("SELECT id, content AS text FROM memories WHERE embedder IS NOT ? ORDER BY last_access DESC "
                        "LIMIT ?",
                        {emb->id, BATCH});
  std::string table = "memories";
  if (rows.empty()) {
    rows = db_.query("SELECT id, text FROM rag_chunks WHERE embedder IS NOT ? ORDER BY doc_id DESC, idx LIMIT ?",
                     {emb->id, BATCH});
    table = "rag_chunks";
  }
  if (rows.empty()) return false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!state_["running"].get<bool>()) {
      state_["running"] = true;
      state_["done"] = 0;
    }
  }
  double t0 = now();
  std::vector<std::string> texts;
  if (table == "rag_chunks") {   // el titulo da contexto al fragmento, igual que al indexar
    std::string ids;
    for (size_t i = 0; i < rows.size(); ++i) ids += (i ? "," : "") + std::to_string(rows[i]["id"].get<long long>());
    std::map<long long, std::string> titles;
    for (auto& r : db_.query("SELECT c.id, d.title FROM rag_chunks c JOIN rag_docs d ON d.id=c.doc_id WHERE c.id IN (" +
                             ids + ")"))
      titles[r["id"].get<long long>()] = jstr(r, "title");
    for (auto& r : rows) texts.push_back(titles[r["id"].get<long long>()] + "\n" + jstr(r, "text"));
  } else {
    for (auto& r : rows) texts.push_back(jstr(r, "text"));
  }
  Mat vecs = emb->embed(texts);
  if (emb != emb_()) return true;   // el modelo cambio mientras calculaba: se descarta el lote
  std::vector<Params> up;
  for (size_t i = 0; i < rows.size() && i < vecs.size(); ++i) up.push_back({blob(vecs[i]), emb->id, rows[i]["id"]});
  db_.executemany("UPDATE " + table + " SET embedding=?, embedder=? WHERE id=?", up);
  long long n = (long long)rows.size();
  double dt = std::max(now() - t0, 1e-3);
  long long done;
  {
    std::lock_guard<std::mutex> lk(mu_);
    done = state_["done"].get<long long>() + n;
    state_["done"] = done;
    double rate = state_["rate"].get<double>();
    state_["rate"] = rate ? round_to(0.7 * rate + 0.3 * (n / dt), 2) : round_to(n / dt, 2);
    state_["phase"] = table == "memories" ? "recuerdos" : "biblioteca";
  }
  if (table == "memories")
    memory_.invalidate();
  else if (done % REFRESH_EVERY < n && invalidate_rag_)
    invalidate_rag_();
  return true;
}

}  // namespace nx::mem
