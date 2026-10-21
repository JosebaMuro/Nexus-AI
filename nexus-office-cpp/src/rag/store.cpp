// Biblioteca RAG (rag/store.py): documentos troceados con embeddings y busqueda hibrida (semantica + palabras clave).
#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <set>
#include <thread>

#include "rag/rag.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

static auto L = log::get("rag");
static const size_t EMBED_BLOCK = 2048;
static const std::set<std::string> STOP = {"de", "la", "el",  "los", "las", "y",   "en",   "un",  "una",
                                           "que", "a", "por", "con", "para", "del", "se",  "es",  "the",
                                           "of", "and", "to", "in",  "is",  "for", "on",   "with"};

static std::u32string ustrip(const std::u32string& s) { return u32(trim(u8(s))); }

std::vector<std::string> chunk_text(const std::string& text, size_t size, size_t overlap) {
  static const Rx split_rx(R"(\n\s*\n|(?<=[.!?])\s{2,})");
  std::vector<std::u32string> paras;
  for (auto& p : split_rx.split(text)) {
    std::string t = trim(p);
    if (!t.empty()) paras.push_back(u32(t));
  }
  std::vector<std::string> chunks;
  std::u32string cur;
  for (auto p : paras) {
    while (p.size() > size) {   // parrafos enormes: corte por frases/longitud
      size_t cut = std::u32string::npos;
      if (size >= 2) {
        // rfind(". ", 0, size): la coincidencia debe acabar dentro de [0, size)
        std::u32string head = p.substr(0, size);
        cut = head.rfind(U". ");
      }
      size_t c = (cut != std::u32string::npos && cut > size / 2) ? cut + 1 : size;
      if (!cur.empty()) {
        chunks.push_back(u8(cur));
        cur.clear();
      }
      chunks.push_back(trim(u8(p.substr(0, c))));
      p = ustrip(p.substr(c));
    }
    if (cur.size() + p.size() + 2 > size && !cur.empty()) {
      chunks.push_back(u8(cur));
      cur = overlap ? (cur.size() > overlap ? cur.substr(cur.size() - overlap) : cur) + U" " : U"";
    }
    bool has = !trim(u8(cur)).empty();
    cur += (has ? U"\n\n" : U"") + p;
  }
  std::string last = trim(u8(cur));
  if (!last.empty()) chunks.push_back(last);
  return chunks;
}

RAGStore::RAGStore(DB& d, mem::EmbedderGetter emb) : db(d), embedder(std::move(emb)), files(paths::sub({"rag", "files"})) {}

void RAGStore::invalidate() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  cache_.reset();
}

void RAGStore::link(const std::string& agent, const std::string& title, const std::string& text) {
  if (!graph) return;
  try {
    graph->for_agent(agent).link_document(title, text);
  } catch (const std::exception& e) {
    L.warning("RAG: no se pudo enlazar '", title, "' en el grafo: ", e.what());
  }
}

void RAGStore::unlink(const json& d) {
  if (graph && !db.one("SELECT 1 FROM rag_docs WHERE agent=? AND title=? AND id!=?", {d["agent"], d["title"], d["id"]}))
    graph->for_agent(jstr(d, "agent")).unlink_document(jstr(d, "title"));
}

json RAGStore::ingest_pages(const std::string& title, const std::vector<std::string>& pages, const std::string& source,
                            const std::string& kind, const std::string& path, const json& meta,
                            const std::vector<std::string>& labels, const std::string& agent, bool do_link) {
  std::vector<std::pair<int, std::string>> pieces;
  size_t chars = 0;
  for (size_t i = 0; i < pages.size(); ++i) {
    chars += ulen(pages[i]);
    for (auto& c : chunk_text(pages[i]))
      pieces.emplace_back((int)i + 1, i < labels.size() && !labels[i].empty() ? labels[i] + "\n" + c : c);
  }
  if (pieces.empty()) throw std::invalid_argument("el documento no contiene texto extraible");
  long long doc_id = 0;
  db.transaction([&] {
    db.execute("INSERT INTO rag_docs(title, source, path, kind, pages, chars, added, meta, agent) VALUES(?,?,?,?,?,?,?,?,?)",
               {uleft(title, 200), source, path, kind, (long long)pages.size(), (long long)chars, now(),
                jdump(meta.is_object() ? meta : json::object()), agent});
    doc_id = db.last_insert_id();
  });
  auto active = embedder();
  bool slow = active->slow;
  for (size_t b = 0; b < pieces.size(); b += EMBED_BLOCK) {   // por bloques: memoria acotada en documentos enormes
    size_t e = std::min(pieces.size(), b + EMBED_BLOCK);
    std::vector<std::string> texts;
    for (size_t i = b; i < e; ++i) texts.push_back(title + "\n" + pieces[i].second);
    mem::Mat vecs = slow ? fast.embed(texts) : active->embed(texts);
    std::string eid = slow ? fast.id : active->id;
    std::vector<Params> rows;
    for (size_t i = b; i < e; ++i)
      rows.push_back({doc_id, (long long)i, pieces[i].first, pieces[i].second, blob(vecs[i - b]), eid});
    db.executemany("INSERT INTO rag_chunks(doc_id, idx, page, text, embedding, embedder) VALUES(?,?,?,?,?,?)", rows);
  }
  invalidate();
  if (slow && on_pending) on_pending();
  if (do_link) {
    std::vector<std::string> sample;
    for (size_t i = 0; i < pieces.size() && i < 8; ++i) sample.push_back(pieces[i].second);
    link(agent, title, join(sample, "\n"));
  }
  L.info("RAG: '", title, "' indexado (", pieces.size(), " fragmentos) en el grafo ", agent.empty() ? "general" : agent);
  return get(doc_id);
}

static fs::path stored_name(const fs::path& dir, const std::string& name, const std::string& extra = "") {
  return dir / (std::to_string((long long)now()) + "_" + extra + safe_name(name, 80));
}

json RAGStore::ingest_file(const fs::path& src, const std::string& title, const std::string& source, const json& meta,
                           bool copy, const std::string& agent, bool do_link) {
  std::string fname = S(src.filename());
  if (!archive_suffix(source.empty() ? fname : source).empty() || !archive_suffix(fname).empty())
    return ingest_archive(*this, src, title, source, meta, agent);
  fs::path dest = src;
  if (copy) {
    dest = stored_name(files, fname);
    std::error_code ec;
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("no se pudo copiar el documento: " + ec.message());
  }
  Doc d = read_document(dest);
  std::string kind = lower(S(dest.extension()));
  if (!kind.empty() && kind[0] == '.') kind = kind.substr(1);
  return ingest_pages(title.empty() ? d.title : title, d.pages, source.empty() ? fname : source, kind, S(dest), meta, {},
                      agent, do_link);
}

json RAGStore::ingest_many(const std::vector<std::pair<fs::path, std::string>>& items,
                           const std::function<void(const json&)>& emit, const std::string& agent) {
  auto step = [&](const std::string& s) {
    if (emit) emit({{"type", "progress"}, {"text", s}});
  };
  std::vector<json> out(items.size());
  struct Loose {
    size_t i;
    fs::path dest;
    std::string name;
  };
  std::vector<Loose> stored;
  for (size_t i = 0; i < items.size(); ++i) {   // copia a la biblioteca antes de leer (biblioteca autocontenida)
    if (!archive_suffix(items[i].second).empty()) continue;
    fs::path dest = stored_name(files, items[i].second, std::to_string(i) + "_");
    std::error_code ec;
    fs::copy_file(items[i].first, dest, fs::copy_options::overwrite_existing, ec);
    stored.push_back({i, dest, items[i].second});
  }
  // lectura en paralelo: un fichero por nucleo
  std::vector<std::future<std::tuple<std::string, std::vector<std::string>, std::string>>> futs;
  for (auto& s : stored) futs.push_back(std::async(std::launch::async, [p = s.dest] { return read_safe(p); }));
  for (size_t k = 0; k < stored.size(); ++k) {
    auto [title, pages, err] = futs[k].get();
    auto& s = stored[k];
    step(cat("Indexando ", k + 1, "/", stored.size(), ": ", s.name));
    try {
      if (!err.empty()) throw std::invalid_argument(err);
      std::string kind = lower(S(s.dest.extension()));
      if (!kind.empty()) kind = kind.substr(1);
      out[s.i] = {{"name", s.name}, {"doc", ingest_pages(title, pages, s.name, kind, S(s.dest), json::object(), {}, agent)}};
    } catch (const std::exception& e) {
      remove_file(s.dest);
      out[s.i] = {{"name", s.name}, {"error", e.what()}};
    }
  }
  for (size_t i = 0; i < items.size(); ++i) {
    if (!out[i].is_null()) continue;
    step("Descomprimiendo " + items[i].second);
    try {
      out[i] = {{"name", items[i].second}, {"doc", ingest_file(items[i].first, "", items[i].second, json::object(), true, agent)}};
    } catch (const std::exception& e) {
      out[i] = {{"name", items[i].second}, {"error", e.what()}};
    }
  }
  return out;
}

RAGStore::Cache& RAGStore::matrix() {
  auto emb = embedder();
  if (cache_ && cache_->emb_id == emb->id) return *cache_;
  std::string cur = emb->id;
  bool slow = emb->slow;
  std::set<std::string> ok = {cur};
  if (slow) ok.insert(fast.id);
  auto rows = db.query("SELECT c.id, c.embedding, c.embedder, d.agent FROM rag_chunks c "
                       "JOIN rag_docs d ON d.id=c.doc_id ORDER BY c.id");
  std::vector<size_t> stale;
  for (size_t i = 0; i < rows.size(); ++i)
    if (!ok.count(jstr(rows[i], "embedder")) || rows[i]["embedding"].is_null()) stale.push_back(i);
  if (!stale.empty()) {   // otro embedder: rapido -> se recalcula ya; lento -> hashing y el resto en segundo plano
    std::string marks = slow ? "?,?" : "?";
    Params p(ok.begin(), ok.end());
    std::map<long long, std::string> texts;
    for (auto& r : db.query("SELECT id, text FROM rag_chunks WHERE embedder IS NULL OR embedder NOT IN (" + marks + ")", p))
      texts[r["id"].get<long long>()] = jstr(r, "text");
    std::string eid = slow ? fast.id : emb->id;
    for (size_t b = 0; b < stale.size(); b += EMBED_BLOCK) {
      size_t e = std::min(stale.size(), b + EMBED_BLOCK);
      std::vector<std::string> t;
      for (size_t i = b; i < e; ++i) t.push_back(texts[rows[stale[i]]["id"].get<long long>()]);
      mem::Mat vecs = slow ? fast.embed(t) : emb->embed(t);
      std::vector<Params> up;
      for (size_t i = b; i < e; ++i) {
        auto& r = rows[stale[i]];
        r["embedding"] = blob(vecs[i - b]);
        r["embedder"] = eid;
        up.push_back({r["embedding"], eid, r["id"]});
      }
      db.executemany("UPDATE rag_chunks SET embedding=?, embedder=? WHERE id=?", up);
    }
    if (slow && on_pending) on_pending();
  }
  Cache c;
  c.emb_id = cur;
  for (auto& r : rows) {
    Index& ix = jstr(r, "embedder") == cur ? c.sem : c.pend;
    ix.mat.push_back(floats(r["embedding"]));
    ix.ids.push_back(r["id"].get<long long>());
    ix.owners.push_back(jstr(r, "agent"));
  }
  cache_ = std::move(c);
  return *cache_;
}

json RAGStore::search(const std::string& query, int k, double min_score, long long doc_id,
                      const std::optional<std::string>& agent) {
  std::set<long long> allowed;
  if (doc_id)
    for (auto& r : db.query("SELECT id FROM rag_chunks WHERE doc_id=?", {doc_id})) allowed.insert(r["id"].get<long long>());
  std::vector<std::pair<long long, double>> sims_by_id;
  std::map<long long, size_t> pos;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto emb = embedder();
    Cache& c = matrix();
    for (int pass = 0; pass < 2; ++pass) {
      Index& ix = pass == 0 ? c.sem : c.pend;
      if (ix.ids.empty()) continue;
      mem::Vec q = pass == 0 ? emb->embed_query({query}, "search")[0] : fast.one(query);
      if (mem::cols(ix.mat) != q.size()) {
        cache_.reset();
        break;
      }
      const double NEG = -std::numeric_limits<double>::infinity();
      std::vector<double> sims(ix.ids.size());
      for (size_t i = 0; i < ix.ids.size(); ++i) {
        double s = mem::dot(ix.mat[i], q);
        if (agent && !ix.owners[i].empty() && ix.owners[i] != *agent) s = NEG;   // nunca lo de otro agente
        if (doc_id && !allowed.count(ix.ids[i])) s = NEG;
        sims[i] = s;
      }
      size_t n = std::min(ix.ids.size(), (size_t)std::max(k * 6, 30));
      std::vector<size_t> idx(ix.ids.size());
      for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
      if (n < idx.size())
        std::partial_sort(idx.begin(), idx.begin() + n, idx.end(), [&](size_t a, size_t b) { return sims[a] > sims[b]; });
      for (size_t t = 0; t < n; ++t) {
        size_t i = idx[t];
        if (!std::isfinite(sims[i])) continue;
        auto it = pos.find(ix.ids[i]);
        if (it == pos.end()) {
          pos[ix.ids[i]] = sims_by_id.size();
          sims_by_id.emplace_back(ix.ids[i], sims[i]);
        } else {
          sims_by_id[it->second].second = sims[i];
        }
      }
    }
  }
  if (sims_by_id.empty()) return json::array();
  std::set<std::string> qterms;
  for (auto& t : tokens(query))
    if (!STOP.count(t) && ulen(t) > 2) qterms.insert(t);
  std::string ids;
  for (size_t i = 0; i < sims_by_id.size(); ++i) ids += (i ? "," : "") + std::to_string(sims_by_id[i].first);
  std::map<long long, Row> rows;
  for (auto& r : db.query("SELECT c.id, c.doc_id, c.page, c.text, d.title, d.source, d.agent FROM rag_chunks c "
                          "JOIN rag_docs d ON d.id=c.doc_id WHERE c.id IN (" + ids + ")"))
    rows[r["id"].get<long long>()] = r;
  std::vector<std::pair<double, json>> out;
  for (auto& [cid, sim] : sims_by_id) {
    auto it = rows.find(cid);
    if (it == rows.end()) continue;
    auto tw = tokens(jstr(it->second, "text"));
    std::set<std::string> words(tw.begin(), tw.end());
    size_t hit = 0;
    for (auto& q : qterms) hit += words.count(q);
    double kw = qterms.empty() ? 0.0 : (double)hit / qterms.size();
    double score = 0.7 * sim + 0.3 * kw;   // hibrida: semantica + coincidencia de terminos
    if (score >= min_score) {
      json r = it->second;
      r["score"] = round_to(score, 3);
      out.emplace_back(round_to(score, 3), r);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first > b.first; });
  json res = json::array();
  for (size_t i = 0; i < out.size() && (int)i < k; ++i) res.push_back(out[i].second);
  return res;
}

json RAGStore::get(long long doc_id) {
  auto d = db.one("SELECT * FROM rag_docs WHERE id=?", {doc_id});
  if (!d) return nullptr;
  json r = *d;
  json m = jparse(jstr(r, "meta").empty() ? "{}" : jstr(r, "meta"));
  r["meta"] = m.is_object() ? m : json::object();
  r["chunks"] = db.scalar("SELECT COUNT(*) n FROM rag_chunks WHERE doc_id=?", {doc_id});
  return r;
}

json RAGStore::list(const std::optional<std::string>& agent) {
  std::string where;
  Params p;
  if (agent) {
    where = "WHERE d.agent IN ('', ?) ";
    p.push_back(*agent);
  }
  json out = json::array();
  for (auto& r : db.query("SELECT d.*, (SELECT COUNT(*) FROM rag_chunks c WHERE c.doc_id=d.id) AS chunks "
                          "FROM rag_docs d " + where + "ORDER BY d.id DESC",
                          p)) {
    json m = jparse(jstr(r, "meta").empty() ? "{}" : jstr(r, "meta"));
    r["meta"] = m.is_object() ? m : json::object();
    out.push_back(r);
  }
  return out;
}

json RAGStore::assign(long long doc_id, const std::string& agent) {
  json d = get(doc_id);
  if (d.is_null()) throw std::out_of_range(std::to_string(doc_id));
  if (jstr(d, "agent") != agent) {
    db.execute("UPDATE rag_docs SET agent=? WHERE id=?", {agent, doc_id});
    invalidate();
    unlink(d);
    if (graph) {
      std::vector<std::string> sample;
      for (auto& r : db.query("SELECT text FROM rag_chunks WHERE doc_id=? ORDER BY idx LIMIT 8", {doc_id}))
        sample.push_back(jstr(r, "text"));
      link(agent, jstr(d, "title"), join(sample, "\n"));
    }
  }
  return get(doc_id);
}

int RAGStore::reassign_agent(const std::string& agent, const std::string& to) {
  if (agent.empty()) return 0;
  auto docs = db.query("SELECT id FROM rag_docs WHERE agent=?", {agent});
  for (auto& d : docs) assign(d["id"].get<long long>(), to);
  return (int)docs.size();
}

void RAGStore::remove(long long doc_id) {
  json d = get(doc_id);
  if (!d.is_null()) {
    unlink(d);
    for (auto& other : d["meta"].value("part_ids", json::array()))   // proyecto importado en varias partes
      if (other.is_number_integer() && other.get<long long>() != doc_id) remove(other.get<long long>());
  }
  db.execute("DELETE FROM rag_chunks WHERE doc_id=?", {doc_id});
  db.execute("DELETE FROM rag_docs WHERE id=?", {doc_id});
  if (!d.is_null()) {
    std::string p = jstr(d, "path");
    if (!p.empty() && contains(p, S(files))) remove_file(P(p));
    json m = d["meta"];
    if (truthy(m.value("sandbox", json())) && truthy(m.value("archive", json()))) {   // proyecto descomprimido
      fs::path root = resolve(paths::sandbox_dir());
      fs::path folder = resolve(root / P(jstr(m, "sandbox")));
      if (folder != root && is_relative_to(folder, root)) rm_rf(folder);
    }
  }
  invalidate();
}

json RAGStore::stats() {
  json by = json::object();
  long long total = 0;
  for (auto& r : db.query("SELECT agent, COUNT(*) n FROM rag_docs GROUP BY agent")) {
    std::string a = jstr(r, "agent");
    by[a.empty() ? "general" : a] = r["n"];
    total += r["n"].get<long long>();
  }
  return {{"documents", total}, {"chunks", db.scalar("SELECT COUNT(*) n FROM rag_chunks")}, {"by_scope", by}};
}

}  // namespace nx::rag
