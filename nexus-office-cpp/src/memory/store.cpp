// Memoria a largo plazo (memory/store.py): similitud semantica x importancia x recencia.
#include <cmath>

#include "memory/memory.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("memory");
const std::vector<std::string> KINDS = {"fact", "preference", "behavior", "episode", "summary",
                                        "example", "lesson", "note", "skill"};

MemoryStore::MemoryStore(DB& d, EmbedderGetter emb, double decay) : db(d), embedder(std::move(emb)), decay_days(decay) {}

std::string MemoryStore::new_session(const std::string& title) {
  std::string sid = uuid_hex().substr(0, 12);
  double t = now();
  db.execute("INSERT INTO sessions(id, title, created, updated) VALUES(?,?,?,?)", {sid, title, t, t});
  return sid;
}

std::string MemoryStore::ensure_session(const std::string& sid, const std::string& title) {
  if (!sid.empty() && db.one("SELECT id FROM sessions WHERE id=?", {sid})) return sid;
  std::string t = uleft(title, 60);
  return new_session(t.empty() ? "Nueva conversacion" : t);
}

long long MemoryStore::add_message(const std::string& session_id, const std::string& role, const std::string& content,
                                   const std::string& agent, const std::string& meta) {
  double t = now();
  long long id = 0;
  db.transaction([&] {
    db.execute("INSERT INTO messages(session_id, role, content, agent, ts, meta) VALUES(?,?,?,?,?,?)",
               {session_id, role, content, agent, t, meta});
    id = db.last_insert_id();
    db.execute("UPDATE sessions SET updated=? WHERE id=?", {t, session_id});
  });
  return id;
}

std::vector<Row> MemoryStore::history(const std::string& session_id, int limit) {
  auto rows = db.query("SELECT * FROM messages WHERE session_id=? ORDER BY id DESC LIMIT ?", {session_id, limit});
  std::reverse(rows.begin(), rows.end());
  return rows;
}

std::vector<Row> MemoryStore::sessions(int limit) {
  return db.query("SELECT s.*, (SELECT COUNT(*) FROM messages m WHERE m.session_id=s.id) AS n "
                  "FROM sessions s ORDER BY updated DESC LIMIT ?",
                  {limit});
}

void MemoryStore::delete_session(const std::string& sid) {
  db.execute("DELETE FROM messages WHERE session_id=?", {sid});
  db.execute("DELETE FROM sessions WHERE id=?", {sid});
}

void MemoryStore::invalidate() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  cache_.reset();
}

MemoryStore::Cache& MemoryStore::matrix() {
  auto emb = embedder();
  if (cache_ && cache_->emb_id == emb->id) return *cache_;
  std::string cur = emb->id;
  bool slow = emb->slow;
  auto rows = db.query("SELECT id, embedding, embedder, content FROM memories");
  std::vector<size_t> stale;
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string e = rows[i]["embedder"].is_string() ? rows[i]["embedder"].get<std::string>() : "";
    bool ok = e == cur || (slow && e == fast.id);
    if (!ok || rows[i]["embedding"].is_null()) stale.push_back(i);
  }
  if (!stale.empty()) {   // el embedder cambio: re-indexar (con hashing si el modelo nuevo es lento)
    std::vector<std::string> texts;
    for (size_t i : stale) texts.push_back(jstr(rows[i], "content"));
    Mat vecs = slow ? fast.embed(texts) : emb->embed(texts);
    std::string eid = slow ? fast.id : emb->id;
    std::vector<Params> up;
    for (size_t k = 0; k < stale.size() && k < vecs.size(); ++k) {
      auto& r = rows[stale[k]];
      up.push_back({blob(vecs[k]), eid, r["id"]});
      r["embedding"] = blob(vecs[k]);
      r["embedder"] = eid;
    }
    db.executemany("UPDATE memories SET embedding=?, embedder=? WHERE id=?", up);
    if (slow && on_pending) on_pending();
  }
  Cache c;
  c.emb_id = cur;
  for (auto& r : rows) {
    Index& ix = jstr(r, "embedder") == cur ? c.sem : c.pend;
    ix.mat.push_back(floats(r["embedding"]));
    ix.ids.push_back(r["id"].get<long long>());
  }
  cache_ = std::move(c);
  return *cache_;
}

long long MemoryStore::remember(std::string content, const std::string& kind, double importance,
                                const std::string& source, const std::string& agent, double dedupe) {
  content = trim(content);
  if (content.empty()) return 0;
  auto emb = embedder();
  Vec vec = emb->embed({content})[0];
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto& sem = matrix().sem;
    if (!sem.ids.empty() && cols(sem.mat) == vec.size()) {
      auto sims = matvec(sem.mat, vec);
      size_t j = std::max_element(sims.begin(), sims.end()) - sims.begin();
      if (sims[j] >= dedupe) {
        db.execute("UPDATE memories SET importance=MIN(1.0, importance+0.1), last_access=?, "
                   "access_count=access_count+1 WHERE id=?",
                   {now(), sem.ids[j]});
        return sem.ids[j];
      }
    }
  }
  double t = now();
  long long id = 0;
  db.transaction([&] {
    db.execute(
        "INSERT INTO memories(kind, content, importance, embedding, embedder, created, last_access, source, agent)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        {kind, content, importance, blob(vec), emb->id, t, t, source, agent});
    id = db.last_insert_id();
  });
  invalidate();
  return id;
}

std::vector<Row> MemoryStore::recall(const std::string& query, int k, const std::vector<std::string>& kinds,
                                     const std::optional<std::string>& agent, double min_score) {
  std::map<long long, float> sims;
  std::vector<long long> order;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto emb = embedder();
    Cache& c = matrix();
    for (int pass = 0; pass < 2; ++pass) {
      Index& ix = pass == 0 ? c.sem : c.pend;
      if (ix.ids.empty()) continue;
      Vec q = pass == 0 ? emb->embed_query({query}, "memory")[0] : fast.one(query);
      if (cols(ix.mat) != q.size()) {
        cache_.reset();
        break;
      }
      auto s = matvec(ix.mat, q);
      for (size_t i = 0; i < s.size(); ++i) {
        if (!sims.count(ix.ids[i])) order.push_back(ix.ids[i]);
        sims[ix.ids[i]] = s[i];
      }
    }
  }
  if (sims.empty()) return {};
  std::map<long long, Row> meta;
  for (auto& r : db.query("SELECT id, kind, content, importance, created, last_access, access_count, agent FROM memories"))
    meta[r["id"].get<long long>()] = r;
  double t = now();
  std::vector<std::pair<double, Row>> scored;
  for (long long mid : order) {
    double sim = sims[mid];
    auto it = meta.find(mid);
    if (it == meta.end()) continue;
    Row r = it->second;
    if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), jstr(r, "kind")) == kinds.end()) continue;
    std::string ra = jstr(r, "agent");
    if (agent && !agent->empty() && !ra.empty() && ra != *agent) continue;
    double la = jnum(r["last_access"]);
    double age_days = (t - (la ? la : jnum(r["created"]))) / 86400;
    double recency = std::exp(-age_days / std::max(decay_days, 1.0));
    double score = 0.75 * sim + 0.15 * jnum(r["importance"]) + 0.10 * recency;
    if (sim >= min_score) {
      r["similarity"] = round_to(sim, 3);
      r["score"] = round_to(score, 3);
      scored.emplace_back(round_to(score, 3), r);
    }
  }
  std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<Row> top;
  for (size_t i = 0; i < scored.size() && (int)i < k; ++i) top.push_back(scored[i].second);
  for (auto& r : top) db.execute("UPDATE memories SET last_access=?, access_count=access_count+1 WHERE id=?", {t, r["id"]});
  return top;
}

std::vector<Row> MemoryStore::list(const std::string& kind, int limit, int offset, const std::optional<std::string>& agent) {
  std::vector<std::string> where;
  Params p;
  if (!kind.empty()) {
    where.push_back("kind=?");
    p.push_back(kind);
  }
  if (agent) {
    where.push_back("agent=?");
    p.push_back(*agent);
  }
  std::string sql = where.empty() ? "" : " WHERE " + join(where, " AND ");
  p.push_back(limit);
  p.push_back(offset);
  return db.query("SELECT id, kind, content, importance, created, access_count, source, agent FROM memories" + sql +
                      " ORDER BY id DESC LIMIT ? OFFSET ?",
                  p);
}

void MemoryStore::move(long long mid, const std::string& agent) {
  db.execute("UPDATE memories SET agent=? WHERE id=?", {agent, mid});
}

void MemoryStore::forget(long long mid) {
  db.execute("DELETE FROM memories WHERE id=?", {mid});
  invalidate();
}

void MemoryStore::adjust(long long mid, double delta) {
  db.execute("UPDATE memories SET importance=MAX(0.0, MIN(1.0, importance+?)) WHERE id=?", {delta, mid});
}

int MemoryStore::decay() {
  double cutoff = now() - decay_days * 86400;
  db.execute("UPDATE memories SET importance=importance*0.95 WHERE last_access < ? AND kind IN "
             "('episode','note','summary')",
             {cutoff});
  int n = db.execute("DELETE FROM memories WHERE importance < 0.05 AND kind='episode'");
  if (n) invalidate();
  return n;
}

json MemoryStore::stats() {
  json by_kind = json::object(), by_scope = json::object();
  for (auto& r : db.query("SELECT kind, COUNT(*) AS n FROM memories GROUP BY kind")) by_kind[jstr(r, "kind")] = r["n"];
  for (auto& r : db.query("SELECT agent, COUNT(*) n FROM memories GROUP BY agent")) {
    std::string a = jstr(r, "agent");
    by_scope[a.empty() ? "general" : a] = r["n"];
  }
  return {{"by_kind", by_kind},
          {"messages", db.scalar("SELECT COUNT(*) AS n FROM messages")},
          {"embedder", embedder()->id},
          {"by_scope", by_scope}};
}

}  // namespace nx::mem
