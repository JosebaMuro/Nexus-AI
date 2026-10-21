// Memoria (nexus/memory): embeddings, memoria a largo plazo, grafo de conocimiento, organizador del conocimiento,
// aprendizaje continuo, IA de apoyo semantica e indexacion semantica en segundo plano.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agents/spec.h"
#include "core/config.h"
#include "core/db.h"
#include "util/json.h"
#include "util/proc.h"

namespace nx::mem {

// ---------------------------------------------------------------- vectores
using Vec = std::vector<float>;
using Mat = std::vector<Vec>;   // filas normalizadas
float dot(const Vec& a, const Vec& b);
std::vector<float> matvec(const Mat& m, const Vec& v);
size_t cols(const Mat& m);
std::vector<size_t> argsort_desc(const std::vector<float>& v);   // estable

// ---------------------------------------------------------------- embeddings
extern const std::map<std::string, std::string> TASKS;
std::string instruct(const std::string& text, const std::string& task);

class Embedder {
 public:
  virtual ~Embedder() = default;
  std::string id = "base";
  bool slow = false;
  bool instructions = false;
  std::atomic<int> waiting{0};   // consultas esperando: la indexacion de fondo cede el turno
  virtual Mat embed(const std::vector<std::string>& texts) = 0;
  virtual Mat embed_query(const std::vector<std::string>& texts, const std::string& task = "search") {
    (void)task;
    return embed(texts);
  }
  virtual json info();
  virtual void close() {}
};
using EmbedderPtr = std::shared_ptr<Embedder>;
using EmbedderGetter = std::function<EmbedderPtr()>;

class HashingEmbedder : public Embedder {
 public:
  explicit HashingEmbedder(int dims = 768);
  Mat embed(const std::vector<std::string>& texts) override;
  Vec one(const std::string& text) const;
  int dims;
};

class OllamaEmbedder : public Embedder {
 public:
  OllamaEmbedder(std::string url, std::string model);
  Mat embed(const std::vector<std::string>& texts) override;
  Mat embed_query(const std::vector<std::string>& texts, const std::string& task = "search") override;

 private:
  std::string url_, model_;
};

// modelo GGUF de embeddings (Qwen3-Embedding...) en un llama-server aparte con --embedding
class GGUFEmbedder : public Embedder {
 public:
  using ExeGetter = std::function<std::optional<std::pair<fs::path, std::string>>()>;
  GGUFEmbedder(json entry, ExeGetter exe, int threads = 0, int dims = 0, double idle_s = 600);
  ~GGUFEmbedder() override;
  Mat embed(const std::vector<std::string>& texts) override;
  Mat embed_query(const std::vector<std::string>& texts, const std::string& task = "search") override;
  json info() override;
  void close() override;
  bool running();
  static constexpr size_t BATCH = 16;
  static constexpr size_t MAX_CHARS = 6000;

 private:
  json entry_;
  std::string path_;
  ExeGetter exe_;
  int threads_, dims_;
  double idle_s_;
  std::string pooling_, eos_;
  std::unique_ptr<proc::Process> proc_;
  int port_ = 0;
  std::mutex mu_;
  double last_ = 0;
  std::thread idle_;
  std::atomic<bool> closing_{false};
  std::mutex qmu_;
  std::list<std::pair<std::string, Vec>> qorder_;
  std::unordered_map<std::string, std::list<std::pair<std::string, Vec>>::iterator> qcache_;
  void start_server();
  void stop_server();
  Mat raw(const std::vector<std::string>& texts);
};

EmbedderPtr get_embedder(const std::string& kind, const std::string& ollama_url, const std::string& ollama_model,
                         const json& gguf_entry, GGUFEmbedder::ExeGetter exe, int threads, int dims);

// ---------------------------------------------------------------- memoria a largo plazo
extern const std::vector<std::string> KINDS;

class MemoryStore {
 public:
  MemoryStore(DB& db, EmbedderGetter emb, double decay_days = 30);
  std::string new_session(const std::string& title = "Nueva conversacion");
  std::string ensure_session(const std::string& sid, const std::string& title = "");
  long long add_message(const std::string& session_id, const std::string& role, const std::string& content,
                        const std::string& agent = "", const std::string& meta = "");
  std::vector<Row> history(const std::string& session_id, int limit = 20);
  std::vector<Row> sessions(int limit = 50);
  void delete_session(const std::string& sid);
  long long remember(std::string content, const std::string& kind = "note", double importance = 0.5,
                     const std::string& source = "", const std::string& agent = "", double dedupe = 0.93);
  // kinds vacio: todos; agent nullopt: sin filtro
  std::vector<Row> recall(const std::string& query, int k = 6, const std::vector<std::string>& kinds = {},
                          const std::optional<std::string>& agent = std::nullopt, double min_score = 0.12);
  std::vector<Row> list(const std::string& kind = "", int limit = 100, int offset = 0,
                        const std::optional<std::string>& agent = std::nullopt);
  void move(long long mid, const std::string& agent);
  void forget(long long mid);
  void adjust(long long mid, double delta);
  int decay();
  json stats();
  void invalidate();
  std::function<void()> on_pending;
  DB& db;
  EmbedderGetter embedder;
  double decay_days;
  HashingEmbedder fast;

 private:
  struct Index {
    Mat mat;
    std::vector<long long> ids;
  };
  struct Cache {
    std::string emb_id;
    Index sem, pend;
  };
  std::recursive_mutex mu_;
  std::optional<Cache> cache_;
  Cache& matrix();
};

// ---------------------------------------------------------------- grafo de conocimiento
extern const char* SELF;
extern const char* USER;
extern const char* AGENT;
extern const char* GENERAL;
extern const json GENERAL_CATEGORIES;
extern const json AGENT_CATEGORIES;
extern const json CATEGORIES;
std::string gkey(const std::string& name);   // _key: tokens normalizados
std::string clean_obj(const std::string& obj);
bool is_stop(const std::string& key);

struct Triple {
  json t;   // {"s", "r", "o", "type", "category", "scope"?}
};

class KnowledgeGraph {
 public:
  KnowledgeGraph(DB& db, std::string scope = "", std::string self_name = SELF, KnowledgeGraph* root = nullptr);
  KnowledgeGraph& for_agent(const std::string& agent);
  void forget_view(const std::string& agent);
  KnowledgeGraph& general() { return *root_; }
  std::vector<json> scopes();
  int drop_scope(const std::string& agent);

  std::string default_category(const std::string& etype = "concepto") const;
  long long upsert_entity(const std::string& name, const std::string& etype = "concepto", double weight = 1.0,
                          std::string category = "", const std::string& note = "", const std::string& source = "");
  std::optional<std::pair<long long, long long>> relate(const std::string& src, const std::string& rel,
                                                        const std::string& dst, const std::string& src_type = "concepto",
                                                        const std::string& dst_type = "concepto", double weight = 1.0,
                                                        const std::string& category = "",
                                                        const std::string& src_category = "");
  json extract(const std::string& text, const std::string& speaker = "user");
  json extract_personal(const std::string& text);
  json extract_topics(const std::string& text, const std::string& speaker = "user", const std::string& category = "");
  int link_document(const std::string& title, const std::string& text = "", const std::string& kind = "documento");
  void unlink_document(const std::string& title);
  static std::vector<std::string> keywords(const std::string& text, size_t n = 4);
  json extract_with_llm(const std::string& text, const std::function<std::string(const std::string&)>& complete);

  std::vector<std::string> facts_about(const std::vector<std::string>& names, int limit = 12);
  std::vector<std::string> notes_about(const std::vector<std::string>& names, int limit = 6);
  std::vector<std::string> user_facts(int limit = 15) { return facts_about({USER}, limit); }
  json to_json(int limit = 220, const std::optional<std::string>& scope = std::nullopt);
  json stats();
  void delete_entity(long long eid);

  bool is_protected(const json& row) const;
  std::optional<Row> entity(long long eid);
  json node(long long eid);
  json search_nodes(const std::optional<std::string>& scope, const std::string& q = "", const std::string& category = "",
                    const std::string& etype = "", int limit = 100, int offset = 0);
  json update_entity(long long eid, const json& changes);   // name, type, category, note, weight
  json add_entity(const std::string& scope, const std::string& name, const std::string& etype = "concepto",
                  const std::string& category = "", const std::string& note = "", const std::string& source = "manual");
  static std::string relation_scope(const std::string& a, const std::string& b);
  json add_relation(long long src, long long dst, const std::string& rel, double weight = 1.0);
  void delete_relation(long long rid);
  json merge_entities(long long src, long long dst);
  json move_entity(long long eid, std::string scope, const std::string& category = "");
  json categories_by_scope();
  int prune(double max_age_days = 45, double min_weight = 0.5);

  DB& db;
  std::string scope;
  std::string self_name;
  std::string self_type;
  std::function<std::string(const std::string&)> namer;   // id de agente -> nombre visible

 private:
  KnowledgeGraph* root_;
  std::mutex views_mu_;
  std::map<std::string, std::unique_ptr<KnowledgeGraph>> views_;
  std::vector<std::unique_ptr<KnowledgeGraph>> graveyard_;
  json links(const std::vector<long long>& ids, int limit);
  json to_json_all(int limit);
};

// ---------------------------------------------------------------- organizador del conocimiento
class Organizer {
 public:
  using AgentsGetter = std::function<std::vector<agents::AgentSpec>()>;
  Organizer(KnowledgeGraph& graph, EmbedderGetter emb, AgentsGetter agents = nullptr, Config* config = nullptr);
  bool semantic();
  std::vector<agents::AgentSpec> specialists();
  static std::string profile_text(const agents::AgentSpec& a);
  static double keyword_hits(const agents::AgentSpec& a, const std::string& text);
  json affinities(const std::vector<std::string>& texts, const std::string& current = "");
  std::pair<std::string, std::string> route(const std::string& text, const std::string& current = "");
  static std::string strip_behaviors(std::string text);
  json extract_general(const std::string& text);
  void record_interaction(const std::string& who, const std::string& rel, const std::string& whom, double weight = 0.2);
  Mat node_vectors(const std::vector<Row>& rows);
  json semantic_search(const std::string& q, const std::optional<std::string>& scope, int limit = 30, int pool = 3000);
  json duplicates(const std::optional<std::string>& scope, std::optional<double> threshold = std::nullopt,
                  int pool = 2000, int limit = 60);
  json plan(const std::function<void(size_t, size_t)>& progress = nullptr, int limit = 20000);
  json apply(const json& moves, bool memories = true, MemoryStore* store = nullptr);
  json overview();
  KnowledgeGraph& graph;
  EmbedderGetter embedder;
  AgentsGetter agents_getter;
  Config* config;

 private:
  std::mutex mu_;
  std::string prof_key_;
  Mat prof_mat_;
  json cfg(const std::string& key, const json& def);
};

// ---------------------------------------------------------------- aprendizaje continuo
class Learner {
 public:
  Learner(DB& db, MemoryStore& memory, KnowledgeGraph& graph, Organizer& organizer);
  json on_user_message(const std::string& text, const std::string& session_id, const std::string& agent = "",
                       bool llm_extraction = false);
  json learn_text(const std::string& text, const std::string& agent = "", bool llm_extraction = false);
  void on_exchange(const std::string& session_id, const std::string& user, const std::string& assistant,
                   const std::string& agent, int consolidate_every = 12);
  json feedback(long long message_id, int score, const std::string& comment = "");
  std::string consolidate(const std::string& session_id,
                          const std::function<std::string(const std::string&)>& summarize = nullptr);
  std::vector<std::string> behaviors(int limit = 8);
  json build_context(const std::string& query, const std::string& agent = "", int k = 6);
  std::function<std::string(const std::string&)> llm_complete;
  std::function<std::string(const std::string&)> namer = [](const std::string& a) { return a; };
  DB& db;
  MemoryStore& memory;
  KnowledgeGraph& graph;
  Organizer& organizer;

 private:
  std::mutex mu_;
  std::map<std::string, int> since_;
};

// ---------------------------------------------------------------- IA de apoyo
// herramienta vista por la IA de apoyo (nombre, descripcion, parametros, riesgo)
struct ToolView {
  std::string name, description, risk;
  json parameters;
};
std::vector<std::string> chunks(const std::string& text, size_t size = 700);

class SemanticAssist {
 public:
  SemanticAssist(EmbedderGetter emb, Config* config = nullptr, size_t max_cache = 4096);
  bool semantic();
  json info();
  std::vector<float> scores(const std::string& query, const std::vector<std::string>& candidates,
                            const std::string& task = "search");
  static std::string tool_text(const ToolView& t);
  std::vector<std::string> select_tools(const std::string& text, const std::vector<ToolView>& tools, int k = 0);
  std::optional<std::string> fix_tool_name(const std::string& name, const std::vector<ToolView>& available);
  std::optional<std::string> fix_enum(const std::string& value, const json& options);
  std::pair<json, std::vector<std::string>> repair_args(const json& schema, const json& args);
  std::optional<std::pair<std::string, json>> guess_tool(const std::string& text, const std::vector<ToolView>& tools);
  static std::string agent_text(const agents::AgentSpec& a);
  std::map<std::string, double> route_bonus(const std::string& text, const std::vector<agents::AgentSpec>& agents);
  std::string condense(const std::string& query, const std::string& text, size_t budget = 5000);
  json condense_result(const std::string& query, const json& result, size_t budget = 5000);
  std::vector<size_t> rerank(const std::string& query, const std::vector<std::string>& items);
  void prewarm(const std::vector<ToolView>& tools, const std::vector<agents::AgentSpec>& agents);
  json last = json::object();

 private:
  EmbedderGetter emb_;
  Config* config_;
  size_t max_;
  std::mutex mu_;
  std::list<std::pair<std::string, Vec>> order_;
  std::unordered_map<std::string, std::list<std::pair<std::string, Vec>>::iterator> cache_;
  json cfg(const std::string& key, const json& def);
  Mat docs(const std::vector<std::string>& texts, const std::string& task = "doc");
  Vec query(const std::string& text, const std::string& task);
  std::optional<Vec> cget(const std::string& k);
  void cput(const std::string& k, Vec v);
};

// ---------------------------------------------------------------- indexacion semantica en segundo plano
class EmbedWorker {
 public:
  // busy: el LLM esta respondiendo; invalidate_rag: recargar la matriz de la biblioteca
  EmbedWorker(DB& db, EmbedderGetter emb, MemoryStore& memory, std::function<bool()> busy,
              std::function<void()> invalidate_rag);
  ~EmbedWorker();
  void start();
  void wake();
  void stop();
  json pending();
  json status();

 private:
  DB& db_;
  EmbedderGetter emb_;
  MemoryStore& memory_;
  std::function<bool()> busy_;
  std::function<void()> invalidate_rag_;
  std::thread th_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool wake_ = false;
  std::atomic<bool> stop_{false};
  json state_ = {{"running", false}, {"done", 0}, {"pending", 0}, {"rate", 0.0}, {"error", ""}, {"phase", ""}};
  void loop();
  bool step(const EmbedderPtr& emb);
  void wait_for(double seconds);
};

}  // namespace nx::mem
