// Embeddings con degradacion elegante (memory/embeddings.py): modelo GGUF en llama-server > Ollama > hashing.
// El hashing da exactamente los mismos vectores que la version Python (zlib.crc32 de los mismos rasgos).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include "memory/memory.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("embeddings");

// ---------------------------------------------------------------- vectores
float dot(const Vec& a, const Vec& b) {
  size_t n = std::min(a.size(), b.size());
  double s = 0;
  for (size_t i = 0; i < n; ++i) s += (double)a[i] * b[i];
  return (float)s;
}

std::vector<float> matvec(const Mat& m, const Vec& v) {
  std::vector<float> out(m.size());
  for (size_t i = 0; i < m.size(); ++i) out[i] = dot(m[i], v);
  return out;
}

size_t cols(const Mat& m) { return m.empty() ? 0 : m[0].size(); }

std::vector<size_t> argsort_desc(const std::vector<float>& v) {
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a] > v[b]; });
  return idx;
}

static void normalize_rows(Mat& m) {
  for (auto& r : m) {
    double n = 0;
    for (float x : r) n += (double)x * x;
    n = std::max(std::sqrt(n), 1e-9);
    for (float& x : r) x = (float)(x / n);
  }
}

// ---------------------------------------------------------------- tareas
const std::map<std::string, std::string> TASKS = {
    {"search", "Given a question, retrieve passages from technical documentation, manuals and project files that answer it"},
    {"memory", "Given a user message, retrieve stored facts, preferences and past conversation notes relevant to it"},
    {"classify", "Identify the field of expertise or knowledge area this text belongs to"},
    {"organize", "Identify the main topic of this document to organize a documentation library"},
    {"dedupe", "Retrieve names of concepts that refer to the same thing as this concept"},
    {"graph", "Given a query, retrieve knowledge graph entities and facts related to it"},
    {"tools", "Given a user request in any language, retrieve the software tool (function) that should be used to fulfil it"},
    {"route", "Given a user request in any language, retrieve the specialist assistant whose field of expertise fits it best"},
    {"claim", "Given a statement, retrieve passages that state the same fact or contradict it"},
};

std::string instruct(const std::string& text, const std::string& task) {
  auto it = TASKS.find(task);
  return "Instruct: " + (it == TASKS.end() ? task : it->second) + "\nQuery:" + text;
}

json Embedder::info() {
  std::string kind = split(split(id, ":", 1)[0], "-", 1)[0];
  return {{"id", id}, {"slow", slow}, {"instructions", instructions}, {"kind", kind}};
}

// ---------------------------------------------------------------- hashing
HashingEmbedder::HashingEmbedder(int d) : dims(d) { id = "hash-" + std::to_string(d); }

Vec HashingEmbedder::one(const std::string& text) const {
  Vec out(dims, 0.0f);
  auto add = [&](const std::string& feat, double w) {
    uint32_t h = crc32(feat);
    out[h % (uint32_t)dims] += (float)(((h >> 31) & 1) ? w : -w);
  };
  std::vector<std::string> words = tokens(text);
  for (const auto& w : words) {
    add(w, 1.0);
    std::u32string cw = u32(w);
    if (cw.size() > 4) {
      std::u32string padded = U"<" + cw + U">";
      for (size_t i = 0; i + 3 < padded.size(); ++i) add("#" + u8(padded.substr(i, 4)), 0.35);
    }
  }
  for (size_t i = 0; i + 1 < words.size(); ++i) add(words[i] + "_" + words[i + 1], 0.6);
  double n = 0;
  for (float x : out) n += (double)x * x;
  n = std::sqrt(n);
  if (n > 0)
    for (float& x : out) x = (float)(x / n);
  return out;
}

Mat HashingEmbedder::embed(const std::vector<std::string>& texts) {
  Mat out(texts.size());
  // lotes grandes (rastreos, ZIP): un trozo por nucleo
  size_t workers = std::max(1u, std::thread::hardware_concurrency());
  if (texts.size() >= 256 && workers > 1) {
    std::vector<std::thread> th;
    size_t per = (texts.size() + workers - 1) / workers;
    for (size_t w = 0; w < workers; ++w) {
      size_t a = w * per, b = std::min(texts.size(), a + per);
      if (a >= b) break;
      th.emplace_back([&, a, b] {
        for (size_t i = a; i < b; ++i) out[i] = one(texts[i]);
      });
    }
    for (auto& t : th) t.join();
    return out;
  }
  for (size_t i = 0; i < texts.size(); ++i) out[i] = one(texts[i]);
  return out;
}

// ---------------------------------------------------------------- Ollama
OllamaEmbedder::OllamaEmbedder(std::string url, std::string model) : url_(rtrim(url, "/")), model_(std::move(model)) {
  id = "ollama:" + model_;
  instructions = contains(lower(model_), "qwen3-embedding");
}

Mat OllamaEmbedder::embed(const std::vector<std::string>& texts) {
  Mat out;
  for (size_t i = 0; i < texts.size(); i += 128) {   // lotes acotados: documentos enormes no agotan el tiempo
    json input = json::array();
    for (size_t j = i; j < std::min(texts.size(), i + 128); ++j) input.push_back(texts[j]);
    auto r = http::post_json(url_ + "/api/embed", {{"model", model_}, {"input", input}}, {}, 120);
    if (!r.ok()) throw http::HttpError(r.status, "Ollama embeddings: " + (r.status ? std::to_string(r.status) : r.error));
    json body = r.json_body();
    for (auto& e : body.value("embeddings", json::array())) {
      Vec v;
      v.reserve(e.size());
      for (auto& x : e) v.push_back(x.get<float>());
      out.push_back(std::move(v));
    }
  }
  normalize_rows(out);
  return out;
}

Mat OllamaEmbedder::embed_query(const std::vector<std::string>& texts, const std::string& task) {
  if (!instructions) return embed(texts);
  std::vector<std::string> q;
  for (auto& t : texts) q.push_back(instruct(t, task));
  return embed(q);
}

// ---------------------------------------------------------------- GGUF (llama-server --embedding)
GGUFEmbedder::GGUFEmbedder(json entry, ExeGetter exe, int threads, int dims, double idle_s)
    : entry_(std::move(entry)), exe_(std::move(exe)), threads_(threads), dims_(dims), idle_s_(idle_s) {
  slow = true;
  path_ = jstr(entry_, "path");
  json meta = entry_.value("meta", json::object());
  if (!meta.is_object()) meta = json::object();
  std::string name = lower(jstr(entry_, "id") + " " + S(P(path_).filename()) + " " + jstr(meta, "name"));
  instructions = truthy(entry_.value("instructions", json(false))) || (contains(name, "qwen3") && contains(name, "embed"));
  std::string arch = jstr(meta, "architecture");
  static const std::vector<std::string> last_arch = {"qwen2", "qwen3", "llama", "mistral", "gemma3", "gemma2"};
  if (meta.contains("pooling") && !meta["pooling"].is_null())
    pooling_ = "";
  else
    pooling_ = std::find(last_arch.begin(), last_arch.end(), arch) != last_arch.end() ? "last" : "mean";
  // Qwen3-Embedding agrupa por el ultimo token: debe ser <|endoftext|> si el GGUF no lo anade solo
  eos_ = instructions && !(meta.contains("add_eos") && meta["add_eos"] == true) ? "<|endoftext|>" : "";
  std::string mid = jstr(entry_, "id");
  id = "gguf:" + (mid.empty() ? stem(P(path_)) : mid) + (dims_ ? "@" + std::to_string(dims_) : "");
}

GGUFEmbedder::~GGUFEmbedder() { close(); }

bool GGUFEmbedder::running() {
  std::lock_guard<std::mutex> lk(mu_);
  return proc_ && proc_->running();
}

void GGUFEmbedder::start_server() {
  if (proc_ && proc_->running()) return;
  auto found = exe_ ? exe_() : std::nullopt;
  if (!found) throw std::runtime_error("Falta el runtime llama.cpp (Sistema > Runtimes) para el modelo de embeddings");
  fs::path exe = found->first;
  port_ = net::free_port(0);
  std::vector<std::string> cmd = {S(exe), "-m", path_, "--host", "127.0.0.1", "--port", std::to_string(port_),
                                  "--embedding", "-c", "2048", "-b", "2048", "-ub", "2048", "-np", "1", "-ngl", "0",
                                  "--no-webui"};
  if (!pooling_.empty()) {
    cmd.push_back("--pooling");
    cmd.push_back(pooling_);
  }
  if (threads_) {
    cmd.push_back("-t");
    cmd.push_back(std::to_string(threads_));
  }
  fs::path logp = paths::logs_dir() / "llama-embed.log";
  proc::Options po;
  po.cwd = exe.parent_path();
  po.output_file = logp;
  po.env = runtimes::lib_env(exe);
  std::vector<std::string> shown(cmd.begin() + 1, cmd.end());
  L.info("Arrancando el servidor de embeddings: ", join(shown, " "));
  proc_ = std::make_unique<proc::Process>();
  if (!proc_->start(cmd, po)) {
    std::string e = proc_->error();
    proc_.reset();
    throw std::runtime_error("no se pudo arrancar el servidor de embeddings: " + e);
  }
  double t0 = now();
  while (now() - t0 < 180) {
    if (!proc_->running()) {
      std::string tail = read_text(logp);
      tail = valid_utf8(tail.size() > 800 ? tail.substr(tail.size() - 800) : tail);
      auto it = std::find(cmd.begin(), cmd.end(), "--no-webui");
      if (it != cmd.end() && (contains(tail, "--no-webui") || contains(tail, "invalid argument"))) {
        cmd.erase(it);   // versiones antiguas de llama.cpp
        proc_ = std::make_unique<proc::Process>();
        if (proc_->start(cmd, po)) continue;
      }
      proc_.reset();
      throw std::runtime_error("el servidor de embeddings termino al arrancar: " + uright(tail, 400));
    }
    if (http::get("http://127.0.0.1:" + std::to_string(port_) + "/health", {}, 1).status == 200) {
      L.info("Servidor de embeddings listo en ", fnum(now() - t0, 1), "s");
      if (!idle_.joinable() && idle_s_ > 0) {
        idle_ = std::thread([this] {
          while (!closing_) {
            for (int i = 0; i < 20 && !closing_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard<std::mutex> lk(mu_);
            if (proc_ && last_ > 0 && now() - last_ >= idle_s_ - 1) stop_server();
          }
        });
      }
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  throw std::runtime_error("el servidor de embeddings no respondio a tiempo");
}

void GGUFEmbedder::stop_server() {
  if (proc_ && proc_->running()) {
    proc_->terminate();
    if (!proc_->wait(10)) proc_->kill();
  }
  proc_.reset();
}

void GGUFEmbedder::close() {
  closing_ = true;
  if (idle_.joinable() && idle_.get_id() != std::this_thread::get_id()) idle_.join();
  std::lock_guard<std::mutex> lk(mu_);
  stop_server();
}

Mat GGUFEmbedder::raw(const std::vector<std::string>& texts) {
  json data;
  {
    std::lock_guard<std::mutex> lk(mu_);
    start_server();
    last_ = now();
    json input = json::array();
    for (auto& t : texts) input.push_back(t);
    auto r = http::post_json("http://127.0.0.1:" + std::to_string(port_) + "/v1/embeddings",
                             {{"input", input}, {"model", "embed"}}, {}, 600);
    if (r.status >= 400 || r.status == 0)
      throw std::runtime_error(cat("servidor de embeddings: ", r.status, " ", uleft(r.status ? r.body : r.error, 200)));
    data = r.json_body().value("data", json::array());
    last_ = now();
  }
  std::vector<json> items(data.begin(), data.end());
  std::stable_sort(items.begin(), items.end(),
                   [](const json& a, const json& b) { return a.value("index", 0) < b.value("index", 0); });
  Mat out;
  for (auto& d : items) {
    Vec v;
    for (auto& x : d["embedding"]) v.push_back(x.get<float>());
    if (dims_ && (int)v.size() > dims_) v.resize(dims_);   // Matryoshka: Qwen3-Embedding admite recortar
    out.push_back(std::move(v));
  }
  normalize_rows(out);
  return out;
}

Mat GGUFEmbedder::embed(const std::vector<std::string>& texts) {
  std::vector<std::string> prep;
  for (auto& t : texts) prep.push_back(uleft(t.empty() ? " " : t, MAX_CHARS) + eos_);
  Mat out;
  for (size_t i = 0; i < prep.size(); i += BATCH) {
    std::vector<std::string> part(prep.begin() + i, prep.begin() + std::min(prep.size(), i + BATCH));
    for (auto& v : raw(part)) out.push_back(std::move(v));
  }
  return out;
}

Mat GGUFEmbedder::embed_query(const std::vector<std::string>& texts, const std::string& task) {
  // consultas: con la instruccion de la tarea, prioridad sobre la indexacion y cache
  std::vector<std::string> keys, missing;
  for (auto& t : texts) keys.push_back(task + "\x1f" + t);
  {
    std::lock_guard<std::mutex> lk(qmu_);
    for (auto& k : keys)
      if (!qcache_.count(k) && std::find(missing.begin(), missing.end(), k) == missing.end()) missing.push_back(k);
  }
  if (!missing.empty()) {
    std::vector<std::string> q;
    for (auto& k : missing) {
      std::string t = k.substr(task.size() + 1);
      q.push_back(instructions ? instruct(t, task) : t);
    }
    ++waiting;
    Mat vecs;
    try {
      vecs = embed(q);
    } catch (...) {
      --waiting;
      throw;
    }
    --waiting;
    std::lock_guard<std::mutex> lk(qmu_);
    for (size_t i = 0; i < missing.size() && i < vecs.size(); ++i) {
      qorder_.emplace_back(missing[i], vecs[i]);
      qcache_[missing[i]] = std::prev(qorder_.end());
    }
    while (qcache_.size() > 256) {
      qcache_.erase(qorder_.front().first);
      qorder_.pop_front();
    }
  }
  std::lock_guard<std::mutex> lk(qmu_);
  Mat out;
  for (auto& k : keys) {
    auto it = qcache_.find(k);
    out.push_back(it == qcache_.end() ? Vec{} : it->second->second);
  }
  return out;
}

json GGUFEmbedder::info() {
  json i = Embedder::info();
  i["kind"] = "gguf";
  std::string name = jstr(entry_, "name");
  i["model"] = name.empty() ? jstr(entry_, "id") : name;
  i["path"] = path_;
  i["running"] = running();
  i["engine"] = "llama-server";
  return i;
}

EmbedderPtr get_embedder(const std::string& kind, const std::string& ollama_url, const std::string& ollama_model,
                         const json& gguf_entry, GGUFEmbedder::ExeGetter exe, int threads, int dims) {
  if (gguf_entry.is_object() && (kind == "auto" || kind == "gguf") && path_exists(P(jstr(gguf_entry, "path"))))
    return std::make_shared<GGUFEmbedder>(gguf_entry, std::move(exe), threads, dims);
  if ((kind == "auto" || kind == "ollama") && !ollama_url.empty()) {
    try {
      auto emb = std::make_shared<OllamaEmbedder>(ollama_url, ollama_model.empty() ? "nomic-embed-text" : ollama_model);
      emb->embed({"ping"});
      return emb;
    } catch (const std::exception&) {
      if (kind == "ollama") L.warning("Ollama embeddings no disponible; usando hashing");
    }
  }
  return std::make_shared<HashingEmbedder>();
}

}  // namespace nx::mem
