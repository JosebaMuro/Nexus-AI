// Grafo de conocimiento multicapa de un dominio rastreado entero (rag/domain_kg.py).
//
// Mientras el rastreo de «todo el dominio» avanza por lotes, cada pagina y documento leido alimenta un grafo propio
// del dominio, guardado en disco (data/rag/crawls/<trabajo>.kg.sqlite): la memoria no crece con el tamano del sitio.
// Al terminar (o cuando se pida) se razona y se empaqueta para darselo a un agente.
//
// Capas: 1 fuentes (paginas y documentos con su URL), 2 estructura (dominio -> secciones -> paginas y enlaces),
// 3 conceptos (con su frecuencia), 4 afirmaciones (agrupadas: «contrastada», «fuente única» o «discrepante», y las
// mas importantes comprobadas contra el resto de la biblioteca), 5 relaciones (co-ocurrencia PMI y, con un embedder
// semantico, significado parecido) y 6 sintesis (temas razonados por el LLM, o extractivos sin el, y un resumen).
//
// El paquete (.zip) lleva graph.json (formato nexus-kg), conocimiento.md y manifest.json.
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <variant>

#include "sqlite3.h"

#include "agents/agents.h"
#include "core/services.h"
#include "memory/kg.h"
#include "memory/memory.h"
#include "rag/rag.h"
#include "rag/web.h"
#include "util/archive.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag::domain_kg {

namespace {

auto L = log::get("domain_kg");
constexpr size_t CLAIMS_PER_PAGE = 14, CONCEPTS_PER_PAGE = 12;
const Rx SENT(R"((?<=[.!?])\s+(?=[A-ZÁÉÍÓÚÑ¿¡0-9"«(])|\n{1,})");
const Rx FACTUAL(R"(\b(es|son|era|fue|fueron|tiene|tienen|permite|permiten|incluye|consiste|se\s+\w+|puede|pueden|)"
                 R"(debe|deben|significa|define|sirve|usa|utiliza|contiene|ofrece|is|are|was|were|has|have|can|)"
                 R"(must|allows?|provides?|includes?|means|uses?|contains?|supports?|requires?|returns?)\b)",
                 Rx::I);
const Rx NUM(R"(\d+(?:[.,]\d+)*)");
std::mutex g_locks_mu;
std::map<std::string, std::shared_ptr<std::mutex>> g_locks;

std::shared_ptr<std::mutex> lock_for(const fs::path& p) {
  std::lock_guard<std::mutex> lk(g_locks_mu);
  auto& m = g_locks[S(p)];
  if (!m) m = std::make_shared<std::mutex>();
  return m;
}

std::set<std::string> stems(const std::string& text) {
  std::set<std::string> out;
  for (auto& w : tokens(text))
    if (ulen(w) >= 4 && !mem::is_stop(w) && !is_digit_str(w)) out.insert(uleft(w, 7));
  return out;
}

std::string short_(const std::string& text, size_t n = 58) {
  static const Rx WS(R"(\s+)");
  std::string t = trim(WS.sub(text, " "));
  return ulen(t) <= n ? t : rtrim(uleft(t, n - 1)) + "…";
}

// conexion SQLite propia del grafo del dominio (no es la base principal)
class Lite {
 public:
  explicit Lite(const fs::path& p) {
    if (sqlite3_open(S(p).c_str(), &db_) != SQLITE_OK) throw std::runtime_error("no se pudo abrir " + S(p));
    sqlite3_busy_timeout(db_, 60000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
  }
  ~Lite() { sqlite3_close(db_); }
  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string e = err ? err : "error";
      sqlite3_free(err);
      throw std::runtime_error(e);
    }
  }
  using Val = std::variant<std::nullptr_t, long long, double, std::string>;
  // ejecuta y devuelve las filas (cada columna como Val)
  std::vector<std::vector<Val>> q(const std::string& sql, const std::vector<Val>& args = {}, int* changes = nullptr) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    for (size_t i = 0; i < args.size(); ++i) {
      int k = (int)i + 1;
      std::visit(
          [&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) sqlite3_bind_null(st, k);
            else if constexpr (std::is_same_v<T, long long>) sqlite3_bind_int64(st, k, v);
            else if constexpr (std::is_same_v<T, double>) sqlite3_bind_double(st, k, v);
            else sqlite3_bind_text(st, k, v.data(), (int)v.size(), SQLITE_TRANSIENT);
          },
          args[i]);
    }
    std::vector<std::vector<Val>> rows;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      std::vector<Val> row;
      for (int c = 0; c < sqlite3_column_count(st); ++c) {
        switch (sqlite3_column_type(st, c)) {
          case SQLITE_INTEGER: row.emplace_back((long long)sqlite3_column_int64(st, c)); break;
          case SQLITE_FLOAT: row.emplace_back(sqlite3_column_double(st, c)); break;
          case SQLITE_NULL: row.emplace_back(nullptr); break;
          default: row.emplace_back(std::string((const char*)sqlite3_column_text(st, c), sqlite3_column_bytes(st, c)));
        }
      }
      rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    if (changes) *changes = sqlite3_changes(db_);
    return rows;
  }
  long long last_id() { return sqlite3_last_insert_rowid(db_); }
  static long long I(const Val& v) { return std::holds_alternative<long long>(v) ? std::get<long long>(v) : 0; }
  static std::string T(const Val& v) { return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : ""; }

 private:
  sqlite3* db_ = nullptr;
};

const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS pages(id INTEGER PRIMARY KEY, url TEXT UNIQUE, title TEXT, section TEXT,
    words INT, document TEXT, summary TEXT);
CREATE TABLE IF NOT EXISTS concepts(id INTEGER PRIMARY KEY, key TEXT UNIQUE, name TEXT, df INT DEFAULT 0,
    tf INT DEFAULT 0);
CREATE TABLE IF NOT EXISTS page_concepts(page INT, concept INT, n INT, PRIMARY KEY(page, concept));
CREATE TABLE IF NOT EXISTS cooc(a INT, b INT, n INT, PRIMARY KEY(a, b));
CREATE TABLE IF NOT EXISTS links(src INT, dst TEXT, PRIMARY KEY(src, dst));
CREATE TABLE IF NOT EXISTS claims(id INTEGER PRIMARY KEY, page INT, text TEXT, stems TEXT, nums TEXT,
    concepts TEXT);
CREATE INDEX IF NOT EXISTS claims_page ON claims(page);
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
)";

std::string as_text(const json& v) { return v.is_string() ? v.get<std::string>() : v.is_null() ? "" : v.dump(); }

size_t count_sub(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
  return n;
}

}  // namespace

std::string section_of(const std::string& url) {
  static const Rx R(R"(^[A-Za-z][A-Za-z0-9+.-]*://([^/?#]*)([^?#]*))");
  auto m = R.search(url);
  std::string netloc = m ? lower(m->str(1)) : "", path = m ? m->str(2) : "";
  std::vector<std::string> seg;
  for (auto& s : split(path, "/"))
    if (!s.empty()) {
      seg.push_back(s);
      break;
    }
  std::string host = starts_with(netloc, "www.") ? netloc.substr(4) : netloc;
  if (!seg.empty() && contains(seg[0], ".")) seg.clear();
  return host + (seg.empty() ? "" : "/" + seg[0]);
}

std::vector<std::string> claims_of(const std::string& text, size_t limit) {
  static const Rx WS(R"(\s+)");
  struct C {
    double score;
    size_t idx;
    std::string s;
  };
  std::vector<C> out;
  std::set<std::string> seen;
  for (auto& raw : SENT.split(text)) {
    std::string s = strip_u(WS.sub(raw, " "), U" -•*#>|");
    if (!(ulen(s) >= 45 && ulen(s) <= 360) || std::count(s.begin(), s.end(), ' ') < 6 || ends_with(s, ":")) continue;
    bool fact = FACTUAL.test(s), num = NUM.test(s);
    if (!(fact || num)) continue;
    std::string key = uleft(normalize(s), 120);
    if (!seen.insert(key).second) continue;
    double score = (double)stems(s).size() + 2 * num + (fact ? 1.5 : 0);
    out.push_back({score, out.size(), s});
  }
  std::stable_sort(out.begin(), out.end(), [](const C& a, const C& b) { return a.score != b.score ? a.score > b.score : a.idx < b.idx; });
  if (out.size() > limit) out.resize(limit);
  std::sort(out.begin(), out.end(), [](const C& a, const C& b) { return a.idx < b.idx; });
  std::vector<std::string> r;
  for (auto& c : out) r.push_back(c.s);
  return r;
}

DomainGraph::DomainGraph(const fs::path& path) : path_(path) {
  auto lk = lock_for(path_);
  std::lock_guard<std::mutex> g(*lk);
  Lite db(path_);
  db.exec(SCHEMA);
}

int DomainGraph::add_pages(const json& pages) {
  auto lk = lock_for(path_);
  std::lock_guard<std::mutex> g(*lk);
  Lite db(path_);
  int added = 0;
  db.exec("BEGIN");
  try {
    for (auto& p : pages) {
      std::string text = jstr(p, "text"), url = jstr(p, "url");
      int ch = 0;
      std::string title = jstr(p, "title").empty() ? url : jstr(p, "title");
      db.q("INSERT OR IGNORE INTO pages(url, title, section, words, document, summary) VALUES(?,?,?,?,?,?)",
           {url, uleft(title, 300), section_of(url), (long long)p.value("words", 0LL), jstr(p, "document"),
            uleft(jstr(p, "description"), 400)},
           &ch);
      if (!ch) continue;
      ++added;
      long long pid = db.last_id();
      std::vector<std::string> heads;
      for (auto& h : p.value("headings", json::array())) heads.push_back(h.get<std::string>());
      auto found = mem::KnowledgeGraph::keywords(uleft(join(heads, " "), 2000) + "\n" + uleft(text, 60000), CONCEPTS_PER_PAGE);
      std::string low = normalize(text);
      std::vector<std::pair<long long, std::string>> cids;
      for (auto& name : found) {
        std::string key = trim(normalize(name));
        if (ulen(key) < 3 || mem::is_stop(key)) continue;
        long long n = std::max<long long>(1, (long long)count_sub(low, key));
        db.q("INSERT INTO concepts(key, name, df, tf) VALUES(?,?,1,?) ON CONFLICT(key) DO UPDATE SET df=df+1, tf=tf+excluded.tf",
             {key, uleft(name, 60), n});
        long long cid = Lite::I(db.q("SELECT id FROM concepts WHERE key=?", {key})[0][0]);
        db.q("INSERT OR IGNORE INTO page_concepts(page, concept, n) VALUES(?,?,?)", {pid, cid, n});
        cids.push_back({cid, key});
      }
      std::set<long long> idset;
      for (auto& [c, _] : cids) idset.insert(c);
      std::vector<long long> ids(idset.begin(), idset.end());
      for (size_t i = 0; i < ids.size() && i < 10; ++i)
        for (size_t j = i + 1; j < ids.size() && j < 10; ++j)
          db.q("INSERT INTO cooc(a, b, n) VALUES(?,?,1) ON CONFLICT(a, b) DO UPDATE SET n=n+1", {ids[i], ids[j]});
      const json& links = p.value("links", json::array());
      for (size_t i = 0; i < links.size() && i < 200; ++i)
        db.q("INSERT OR IGNORE INTO links(src, dst) VALUES(?,?)", {pid, links[i].get<std::string>()});
      for (auto& c : claims_of(text)) {
        std::string lc = normalize(c);
        std::vector<std::string> about;
        for (auto& [cid, key] : cids)
          if (contains(lc, key)) about.push_back(std::to_string(cid));
        auto st = stems(c);
        std::vector<std::string> sv(st.begin(), st.end());
        db.q("INSERT INTO claims(page, text, stems, nums, concepts) VALUES(?,?,?,?,?)",
             {pid, c, join(sv, " "), join(NUM.findall(c), " "), join(about, " ")});
      }
    }
    db.exec("COMMIT");
  } catch (...) {
    db.exec("ROLLBACK");
    throw;
  }
  return added;
}

json DomainGraph::stats() {
  Lite db(path_);
  auto q = [&](const char* sql) { return Lite::I(db.q(sql)[0][0]); };
  return {{"pages", q("SELECT COUNT(*) FROM pages")},
          {"documents", q("SELECT COUNT(*) FROM pages WHERE document!=''")},
          {"concepts", q("SELECT COUNT(*) FROM concepts")},
          {"claims", q("SELECT COUNT(*) FROM claims")},
          {"links", q("SELECT COUNT(*) FROM links")},
          {"sections", q("SELECT COUNT(DISTINCT section) FROM pages")}};
}

json DomainGraph::build(const std::string& title, const std::string& site,
                        const std::function<std::string(const std::string&)>& llm, Services* s,
                        const std::vector<long long>& exclude_docs, const std::function<void(const std::string&)>& step_in,
                        int max_llm_topics, long long max_claims) {
  auto step = [&](const std::string& t) {
    if (step_in) step_in(t);
  };
  double t0 = now();
  struct Page {
    std::string url, title, section, document;
    long long words;
  };
  struct Concept {
    std::string key, name;
    long long df, tf;
  };
  struct Claim {
    long long page;
    std::string text;
    std::set<std::string> stems;
    std::vector<std::string> nums;
    std::vector<long long> about;
  };
  struct Fact {
    long long id;
    std::string text, status;
    std::vector<long long> sources, about;
    int independent;
    double weight;
    std::vector<std::string> variants;
    json external;
  };
  std::map<long long, Page> pages;
  std::map<long long, Concept> concepts;
  std::map<long long, Claim> claims;
  std::vector<long long> claim_order;
  std::map<long long, std::vector<long long>> by_concept;
  std::vector<Fact> facts;
  std::vector<long long> top;
  std::vector<std::vector<long long>> topics;
  std::map<long long, std::vector<std::pair<long long, long long>>> page_concepts;
  std::vector<std::pair<long long, std::string>> links;
  std::vector<std::tuple<long long, long long, std::string, double>> rel_edges;
  long long compared = 0, external = 0, semantic = 0;
  {
    auto lk = lock_for(path_);
    std::lock_guard<std::mutex> g(*lk);
    Lite db(path_);
    for (auto& r : db.q("SELECT id, url, title, section, words, document FROM pages"))
      pages[Lite::I(r[0])] = {Lite::T(r[1]), Lite::T(r[2]), Lite::T(r[3]), Lite::T(r[5]), Lite::I(r[4])};
    if (pages.empty()) throw std::invalid_argument("el dominio aun no tiene paginas leidas");
    long long ndocs = (long long)pages.size();
    for (auto& r : db.q("SELECT id, key, name, df, tf FROM concepts WHERE df >= ?", {(long long)(ndocs < 30 ? 1 : 2)}))
      concepts[Lite::I(r[0])] = {Lite::T(r[1]), Lite::T(r[2]), Lite::I(r[3]), Lite::I(r[4])};
    step("Comprobando afirmaciones entre fuentes");
    // dominios enormes: se razona sobre las afirmaciones mas conectadas (todo el texto sigue en la biblioteca)
    for (auto& r : db.q("SELECT id, page, text, stems, nums, concepts FROM claims ORDER BY length(concepts) DESC, id LIMIT ?",
                        {max_claims})) {
      long long cid = Lite::I(r[0]);
      Claim c{Lite::I(r[1]), Lite::T(r[2]), {}, split_ws(Lite::T(r[4])), {}};
      for (auto& st : split_ws(Lite::T(r[3]))) c.stems.insert(st);
      for (auto& a : split_ws(Lite::T(r[5]))) {
        long long id = std::stoll(a);
        if (concepts.count(id)) c.about.push_back(id);
      }
      for (long long a : c.about) by_concept[a].push_back(cid);
      claims[cid] = std::move(c);
      claim_order.push_back(cid);
    }
    // afirmaciones equivalentes de paginas distintas (union-find por concepto compartido)
    std::map<long long, long long> parent;
    for (long long c : claim_order) parent[c] = c;
    std::function<long long(long long)> find = [&](long long x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };
    std::vector<std::pair<long long, std::vector<long long>*>> bc;
    for (auto& [k, v] : by_concept) bc.push_back({k, &v});
    std::stable_sort(bc.begin(), bc.end(), [](auto& a, auto& b) { return a.second->size() > b.second->size(); });
    for (size_t bi = 0; bi < bc.size() && bi < 4000; ++bi) {
      auto& members = *bc[bi].second;
      std::vector<long long> block(members.end() - std::min<size_t>(120, members.size()), members.end());
      std::set<long long> pg;
      for (long long c : block) pg.insert(claims[c].page);
      if (pg.size() < 2) continue;
      for (size_t i = 0; i < block.size(); ++i) {
        auto& sa = claims[block[i]].stems;
        long long pa = claims[block[i]].page;
        for (size_t j = i + 1; j < block.size(); ++j) {
          if (claims[block[j]].page == pa) continue;
          auto& sb = claims[block[j]].stems;
          size_t inter = 0;
          for (auto& x : sa) inter += sb.count(x);
          size_t uni = sa.size() + sb.size() - inter;
          ++compared;
          if (inter >= 3 && (double)inter / std::max<size_t>(1, uni) >= 0.5) parent[find(block[i])] = find(block[j]);
        }
      }
    }
    std::map<long long, std::vector<long long>> groups;
    std::vector<long long> group_order;
    for (long long c : claim_order) {
      long long root = find(c);
      if (!groups.count(root)) group_order.push_back(root);
      groups[root].push_back(c);
    }
    // una afirmacion por grupo: la mas completa, con todas sus fuentes
    for (long long gr : group_order) {
      auto& members = groups[gr];
      std::set<long long> srcs;
      for (long long m : members) srcs.insert(claims[m].page);
      long long main = members[0];
      for (long long m : members)
        if (claims[m].stems.size() > claims[main].stems.size() ||
            (claims[m].stems.size() == claims[main].stems.size() && m < main))
          main = m;
      std::set<std::vector<std::string>> nums;
      for (long long m : members)
        if (!claims[m].nums.empty()) nums.insert(claims[m].nums);
      if (nums.size() >= 3 && nums.size() + 1 >= srcs.size()) continue;   // plantilla repetida con numeracion
      std::set<std::string> sections;
      for (long long sp : srcs) sections.insert(pages[sp].section);
      std::string status = nums.size() > 1 ? "discrepante" : srcs.size() >= 2 ? "contrastada" : "fuente única";
      std::set<long long> about;
      for (long long m : members)
        for (long long a : claims[m].about) about.insert(a);
      double w = (double)srcs.size() * 2 + (double)sections.size();
      double lg = 0;
      int k = 0;
      for (long long a : about) {
        if (k++ >= 5) break;
        lg += std::log(1 + (double)concepts[a].df);
      }
      w += lg / 3;
      Fact f{main, claims[main].text, status, std::vector<long long>(srcs.begin(), srcs.end()),
             std::vector<long long>(about.begin(), about.end()), (int)sections.size(), w, {}, nullptr};
      for (long long m : members)
        if (m != main && f.variants.size() < 3) f.variants.push_back(claims[m].text);
      facts.push_back(std::move(f));
    }
    std::stable_sort(facts.begin(), facts.end(), [](const Fact& a, const Fact& b) { return a.weight > b.weight; });
    // comprobacion con otras fuentes de la biblioteca (lo que no es este dominio)
    if (s && s->rag) {
      step("Contrastando con el resto de la biblioteca");
      std::set<long long> skip(exclude_docs.begin(), exclude_docs.end());
      for (size_t i = 0; i < facts.size() && i < 300; ++i) {
        json hits;
        try {
          hits = s->rag->search(facts[i].text, 4, 0.62);
        } catch (...) {
          break;
        }
        json keep = json::array();
        for (auto& h : hits)
          if (!skip.count(h.value("doc_id", 0LL)) && keep.size() < 2)
            keep.push_back({{"title", jstr(h, "title")}, {"score", round_to(h.value("score", 0.0), 2)}});
        if (!keep.empty()) {
          facts[i].external = keep;
          ++external;
          if (facts[i].status == "fuente única") facts[i].status = "contrastada";
        }
      }
    }
    // relaciones entre conceptos: co-ocurrencia (PMI) y significado
    step("Relacionando conceptos");
    for (auto& [c, _] : concepts) top.push_back(c);
    std::stable_sort(top.begin(), top.end(), [&](long long a, long long b) { return concepts[a].df > concepts[b].df; });
    if (top.size() > 5000) top.resize(5000);
    std::set<long long> topset(top.begin(), top.end());
    for (auto& r : db.q("SELECT a, b, n FROM cooc WHERE n >= 2 ORDER BY n DESC LIMIT 200000")) {
      long long a = Lite::I(r[0]), b = Lite::I(r[1]), n = Lite::I(r[2]);
      if (topset.count(a) && topset.count(b)) {
        double pmi = std::log((double)(n * ndocs) / (double)(concepts[a].df * concepts[b].df));
        if (pmi > 0.3) rel_edges.push_back({a, b, "aparece_con", round_to(std::min(5.0, n * pmi / 2), 3)});
      }
    }
    if (s && top.size() > 3) {
      try {
        auto emb = s->embedder();
        if (emb && !starts_with(emb->id, "hash")) {
          std::vector<long long> sel(top.begin(), top.begin() + std::min<size_t>(3000, top.size()));
          std::vector<std::string> names;
          for (long long c : sel) names.push_back(concepts[c].name);
          mem::Mat vecs = emb->embed(names);
          for (auto& v : vecs) {
            double nn = 0;
            for (float x : v) nn += (double)x * x;
            nn = std::sqrt(nn) + 1e-9;
            for (float& x : v) x = (float)(x / nn);
          }
          for (size_t ci = 0; ci < sel.size(); ++ci) {
            std::vector<float> row = mem::matvec(vecs, vecs[ci]);
            row[ci] = -1;
            auto order = mem::argsort_desc(row);
            for (size_t t = 0; t < 3 && t < order.size(); ++t) {
              size_t j = order[t];
              if (row[j] >= 0.62 && ci < j) {
                rel_edges.push_back({sel[ci], sel[j], "relacionado_con", round_to(row[j], 3)});
                ++semantic;
              }
            }
          }
        }
      } catch (const std::exception& e) {
        L.info("Relaciones semanticas omitidas: ", e.what());
      }
    }
    // temas: comunidades de conceptos por propagacion de etiquetas sobre la co-ocurrencia (determinista)
    step("Agrupando por temas");
    std::vector<long long> ctop(top.begin(), top.begin() + std::min<size_t>(2500, top.size()));
    std::map<long long, long long> label;
    if (!ctop.empty()) {
      std::set<long long> ok(ctop.begin(), ctop.end());
      std::map<long long, std::vector<std::pair<long long, long long>>> neigh;
      std::vector<std::string> idlist;
      for (long long c : ctop) idlist.push_back(std::to_string(c));
      for (auto& r : db.q("SELECT a, b, n FROM cooc WHERE n >= 2 AND a IN (" + join(idlist, ",") + ")")) {
        long long a = Lite::I(r[0]), b = Lite::I(r[1]), n = Lite::I(r[2]);
        if (ok.count(b)) {
          neigh[a].push_back({b, n});
          neigh[b].push_back({a, n});
        }
      }
      for (long long c : ctop) label[c] = c;
      for (int it = 0; it < 12; ++it) {
        int changed = 0;
        for (long long c : ctop) {
          if (neigh[c].empty()) continue;
          std::map<long long, long long> score;
          for (auto& [o2, n] : neigh[c]) score[label[o2]] += n;
          long long best = 0, bestv = -1;
          for (auto& [lab, v] : score)
            if (v > bestv || (v == bestv && lab < best)) {
              best = lab;
              bestv = v;
            }
          long long curv = score.count(label[c]) ? score[label[c]] : 0;
          if (best != label[c] && bestv > curv) {
            label[c] = best;
            ++changed;
          }
        }
        if (!changed) break;
      }
    }
    std::map<long long, std::vector<long long>> comm;
    std::vector<long long> comm_order;
    for (long long c : ctop) {
      long long lab = label[c];
      if (!comm.count(lab)) comm_order.push_back(lab);
      comm[lab].push_back(c);
    }
    for (long long lab : comm_order) {
      auto m = comm[lab];
      if (m.size() < 3) continue;
      std::stable_sort(m.begin(), m.end(), [&](long long a, long long b) { return concepts[a].df > concepts[b].df; });
      topics.push_back(m);
    }
    auto dfsum = [&](const std::vector<long long>& m) {
      long long t = 0;
      for (long long c : m) t += concepts[c].df;
      return t;
    };
    std::stable_sort(topics.begin(), topics.end(), [&](auto& a, auto& b) { return dfsum(a) > dfsum(b); });
    if (topics.size() > 80) topics.resize(80);
    for (auto& r : db.q("SELECT page, concept, n FROM page_concepts")) {
      long long cpt = Lite::I(r[1]);
      if (concepts.count(cpt)) page_concepts[Lite::I(r[0])].push_back({Lite::I(r[2]), cpt});
    }
    for (auto& r : db.q("SELECT src, dst FROM links")) links.push_back({Lite::I(r[0]), Lite::T(r[1])});
  }
  std::map<std::string, long long> url_id;
  for (auto& [pid, p] : pages) url_id[p.url] = pid;

  // sintesis razonada por tema
  struct Topic {
    std::string name, synthesis;
    std::vector<long long> concepts;
    std::vector<const Fact*> facts;
    std::vector<std::string> conclusions;
    bool reasoned;
  };
  std::vector<Topic> tdata;
  for (size_t i = 0; i < topics.size(); ++i) {
    auto& members = topics[i];
    std::set<long long> mset(members.begin(), members.end());
    std::vector<const Fact*> tf;
    for (auto& f : facts) {
      if (tf.size() >= 12) break;
      for (long long a : f.about)
        if (mset.count(a)) {
          tf.push_back(&f);
          break;
        }
    }
    std::vector<std::string> n3, n8;
    for (size_t j = 0; j < members.size(); ++j) {
      if (j < 3) n3.push_back(concepts[members[j]].name);
      if (j < 8) n8.push_back(concepts[members[j]].name);
    }
    std::string name = "Tema: " + join(n3, ", ");
    std::string synth;
    std::vector<std::string> concl;
    if (llm && (int)i < max_llm_topics && !tf.empty()) {
      step("Razonando el tema " + std::to_string(i + 1) + "/" + std::to_string(std::min<size_t>(topics.size(), max_llm_topics)));
      std::string lines;
      for (size_t j = 0; j < tf.size() && j < 10; ++j)
        lines += (j ? "\n" : "") + std::string("- (") + tf[j]->status + ", " + std::to_string(tf[j]->sources.size()) +
                 " fuentes) " + tf[j]->text;
      std::string prompt = "Eres un analista que asimila el conocimiento del sitio " + (site.empty() ? title : site) +
                           ". Tema: " + join(n8, ", ") + ".\nAfirmaciones extraidas del sitio:\n" + lines +
                           "\n\nRazona sobre ellas: relacionalas, deduce lo que implican juntas y senala lo dudoso "
                           "(fuente unica o discrepante). Devuelve SOLO JSON {\"sintesis\": \"3-5 frases\", "
                           "\"conclusiones\": [\"...\", \"...\", \"...\"]} en espanol.";
      try {
        std::string raw = llm(prompt);
        size_t a = raw.find('{'), b = raw.rfind('}');
        json data = a != std::string::npos && b != std::string::npos && b > a ? jparse(raw.substr(a, b - a + 1)) : json();
        if (data.is_object()) {
          synth = uleft(trim(as_text(data.value("sintesis", json("")))), 1500);
          for (auto& x : data.value("conclusiones", json::array())) {
            std::string c = trim(as_text(x));
            if (!c.empty() && concl.size() < 5) concl.push_back(uleft(c, 300));
          }
        }
      } catch (...) {
        synth.clear();
      }
    }
    if (synth.empty()) {
      std::vector<const Fact*> ok;
      for (auto* f : tf)
        if (f->status == "contrastada") ok.push_back(f);
      if (ok.empty()) ok = tf;
      std::vector<std::string> parts;
      for (size_t j = 0; j < ok.size() && j < 3; ++j) parts.push_back(ok[j]->text);
      synth = join(parts, " ");
      concl.clear();
      for (size_t j = 3; j < tf.size() && j < 6; ++j) concl.push_back(tf[j]->text);
    }
    bool reasoned = llm && (int)i < max_llm_topics && !synth.empty() && !tf.empty() && !concl.empty();
    tdata.push_back({name, synth, members, tf, concl, reasoned});
  }
  std::set<std::string> secset;
  for (auto& [_, p] : pages) secset.insert(p.section);
  std::vector<std::string> tnames;
  for (size_t i = 0; i < tdata.size() && i < 8; ++i) tnames.push_back(usub(tdata[i].name, 6));
  std::string overview = (title.empty() ? site : title) + ": " + std::to_string(pages.size()) + " páginas y documentos en " +
                         std::to_string(secset.size()) + " secciones. Temas principales: " + join(tnames, "; ") + ".";
  if (llm && !tdata.empty()) {
    try {
      std::string list;
      for (size_t i = 0; i < tdata.size() && i < 15; ++i)
        list += (i ? "\n" : "") + std::string("- ") + tdata[i].name + ": " + uleft(tdata[i].synthesis, 400);
      std::string raw = trim(llm("Resume en 4-6 frases, en espanol, de que trata y que ensena este sitio a partir de las "
                                 "sintesis de sus temas:\n" + list));
      if (!raw.empty()) overview = uleft(raw, 2000);
    } catch (...) {
    }
  }

  // ------------------------------------------------------------ grafo nexus-kg
  step("Construyendo el grafo");
  json nodes = json::array(), edges = json::array();
  std::string domain_name = !site.empty() ? site : !title.empty() ? title : "dominio";
  nodes.push_back({{"id", "d0"}, {"name", short_(domain_name)}, {"type", "dominio"}, {"category", ""},
                   {"note", uleft(overview, 500)}, {"weight", 5}});
  std::map<std::string, std::string> sec_ids;
  for (auto& [pid, p] : pages)
    if (!sec_ids.count(p.section)) {
      std::string sid = "s" + std::to_string(sec_ids.size());
      sec_ids[p.section] = sid;
      nodes.push_back({{"id", sid}, {"name", short_(p.section)}, {"type", "seccion"}, {"category", ""}, {"note", ""}, {"weight", 1}});
      edges.push_back({{"src", "d0"}, {"dst", sid}, {"rel", "contiene"}, {"weight", 1}});
    }
  std::vector<long long> by_words;
  for (auto& [pid, _] : pages) by_words.push_back(pid);
  std::stable_sort(by_words.begin(), by_words.end(), [&](long long a, long long b) { return pages[a].words > pages[b].words; });
  if (by_words.size() > 50000) by_words.resize(50000);
  std::set<long long> shown(by_words.begin(), by_words.end());
  for (long long pid : shown) {
    auto& p = pages[pid];
    std::string id = "p" + std::to_string(pid);
    nodes.push_back({{"id", id}, {"name", short_(p.title)}, {"type", p.document.empty() ? "pagina" : "documento"},
                     {"category", ""}, {"note", p.url}, {"weight", 1}});
    edges.push_back({{"src", sec_ids[p.section]}, {"dst", id}, {"rel", "contiene"}, {"weight", 1}});
    auto pc = page_concepts[pid];
    std::sort(pc.begin(), pc.end(), std::greater<>());
    for (size_t i = 0; i < pc.size() && i < 5; ++i)
      edges.push_back({{"src", id}, {"dst", "c" + std::to_string(pc[i].second)}, {"rel", "menciona"}, {"weight", 1}});
  }
  for (size_t i = 0; i < links.size() && i < 300000; ++i) {
    auto it = url_id.find(links[i].second);
    if (it == url_id.end()) continue;
    long long d = it->second, src = links[i].first;
    if (d && d != src && shown.count(src) && shown.count(d))
      edges.push_back({{"src", "p" + std::to_string(src)}, {"dst", "p" + std::to_string(d)}, {"rel", "enlaza"}, {"weight", 0.5}});
  }
  std::set<long long> used(top.begin(), top.end());
  for (auto& t : topics) used.insert(t.begin(), t.end());
  std::map<long long, const Fact*> best_fact;
  for (auto& f : facts)
    for (long long a : f.about)
      if (!best_fact.count(a)) best_fact[a] = &f;
  for (long long c : used) {
    auto it = best_fact.find(c);
    nodes.push_back({{"id", "c" + std::to_string(c)}, {"name", concepts[c].name}, {"type", "concepto"}, {"category", ""},
                     {"note", it != best_fact.end() ? uleft(it->second->text, 480) : ""},
                     {"weight", round_to(1 + std::log(1 + (double)concepts[c].df), 3)}});
  }
  for (auto& [a, b2, rel, w] : rel_edges)
    edges.push_back({{"src", "c" + std::to_string(a)}, {"dst", "c" + std::to_string(b2)}, {"rel", rel}, {"weight", w}});
  const size_t fact_cap = 60000;
  std::set<long long> kept;
  for (size_t i = 0; i < facts.size() && i < fact_cap; ++i) {
    auto& f = facts[i];
    kept.insert(f.id);
    std::vector<std::string> su;
    for (size_t j = 0; j < f.sources.size() && j < 3; ++j) su.push_back(pages[f.sources[j]].url);
    std::string note = f.text + " [" + f.status + "; " + std::to_string(f.sources.size()) + " fuente" +
                       (f.sources.size() != 1 ? "s" : "") + ": " + join(su, ", ") + "]";
    std::string fid = "f" + std::to_string(f.id);
    nodes.push_back({{"id", fid}, {"name", short_(f.text)}, {"type", "afirmacion"}, {"category", ""},
                     {"note", uleft(note, 500)}, {"weight", round_to(1 + f.sources.size() * 0.5, 3)}});
    for (size_t j = 0; j < f.about.size() && j < 6; ++j)
      if (used.count(f.about[j]))
        edges.push_back({{"src", fid}, {"dst", "c" + std::to_string(f.about[j])}, {"rel", "sobre"}, {"weight", 1}});
    for (size_t j = 0; j < f.sources.size() && j < 10; ++j)
      if (shown.count(f.sources[j]))
        edges.push_back({{"src", fid}, {"dst", "p" + std::to_string(f.sources[j])}, {"rel", "fuente"}, {"weight", 1}});
  }
  for (size_t i = 0; i < tdata.size(); ++i) {
    auto& t = tdata[i];
    std::string tid = "t" + std::to_string(i);
    nodes.push_back({{"id", tid}, {"name", short_(t.name)}, {"type", "tema"}, {"category", ""},
                     {"note", uleft(t.synthesis, 500)}, {"weight", 3}});
    edges.push_back({{"src", "d0"}, {"dst", tid}, {"rel", "trata_de"}, {"weight", 2}});
    for (size_t j = 0; j < t.concepts.size() && j < 40; ++j)
      edges.push_back({{"src", tid}, {"dst", "c" + std::to_string(t.concepts[j])}, {"rel", "incluye"}, {"weight", 1}});
    for (size_t j = 0; j < t.facts.size() && j < 8; ++j)
      if (kept.count(t.facts[j]->id))
        edges.push_back({{"src", tid}, {"dst", "f" + std::to_string(t.facts[j]->id)}, {"rel", "se_apoya_en"}, {"weight", 1}});
    for (size_t j = 0; j < t.conclusions.size(); ++j) {
      std::string cid = tid + "k" + std::to_string(j);
      nodes.push_back({{"id", cid}, {"name", short_(t.conclusions[j])}, {"type", "conclusion"}, {"category", ""},
                       {"note", uleft(t.conclusions[j], 500)}, {"weight", 2}});
      edges.push_back({{"src", tid}, {"dst", cid}, {"rel", "concluye"}, {"weight", 1.5}});
    }
  }
  json layers = {"fuentes", "estructura", "conceptos", "afirmaciones", "relaciones", "sintesis"};
  json graph = {{"format", "nexus-kg"}, {"version", 1}, {"layers", layers}, {"nodes", nodes}, {"edges", edges}};

  // ------------------------------------------------------------ conocimiento legible y citado
  std::map<std::string, long long> counts;
  for (auto& f : facts) ++counts[f.status];
  std::vector<std::string> md = {
      "# Conocimiento del dominio: " + domain_name, "", overview, "",
      "_" + std::to_string(pages.size()) + " páginas y documentos · " + std::to_string(sec_ids.size()) + " secciones · " +
          std::to_string(used.size()) + " conceptos · " + std::to_string(facts.size()) + " afirmaciones (" +
          std::to_string(counts["contrastada"]) + " contrastadas, " + std::to_string(counts["fuente única"]) +
          " de fuente única, " + std::to_string(counts["discrepante"]) + " discrepantes) · " + std::to_string(tdata.size()) +
          " temas._",
      ""};
  auto srcs_of = [&](const Fact& f, size_t n) {
    std::vector<std::string> v;
    for (size_t j = 0; j < f.sources.size() && j < n; ++j) v.push_back(pages[f.sources[j]].url);
    return join(v, ", ");
  };
  for (auto& t : tdata) {
    md.push_back("## " + usub(t.name, 6));
    md.push_back("");
    md.push_back(t.synthesis);
    md.push_back("");
    if (!t.conclusions.empty()) {
      md.push_back("**Conclusiones**");
      md.push_back("");
      for (auto& c : t.conclusions) md.push_back("- " + c);
      md.push_back("");
    }
    std::vector<std::string> cn;
    for (size_t j = 0; j < t.concepts.size() && j < 20; ++j) cn.push_back(concepts[t.concepts[j]].name);
    md.push_back("**Conceptos:** " + join(cn, ", "));
    md.push_back("");
    if (!t.facts.empty()) {
      md.push_back("**Afirmaciones**");
      md.push_back("");
      for (size_t j = 0; j < t.facts.size() && j < 12; ++j)
        md.push_back("- " + t.facts[j]->text + " _(" + t.facts[j]->status + "; " + srcs_of(*t.facts[j], 3) + ")_");
      md.push_back("");
    }
  }
  std::vector<const Fact*> rest;
  for (auto& f : facts) {
    if (rest.size() >= 400) break;
    bool in_topic = false;
    for (auto& t : tdata) {
      std::set<long long> tc(t.concepts.begin(), t.concepts.end());
      for (long long a : f.about)
        if (tc.count(a)) in_topic = true;
      if (in_topic) break;
    }
    if (!in_topic) rest.push_back(&f);
  }
  if (!rest.empty()) {
    md.push_back("## Otras afirmaciones");
    md.push_back("");
    for (auto* f : rest) md.push_back("- " + f->text + " _(" + f->status + "; " + pages[f->sources[0]].url + ")_");
    md.push_back("");
  }
  md.push_back("## Mapa del sitio");
  md.push_back("");
  std::map<std::string, std::vector<const Page*>> by_sec;
  for (auto& [pid, p] : pages) by_sec[p.section].push_back(&p);
  for (auto& [sec, list] : by_sec) {
    md.push_back("### " + sec);
    md.push_back("");
    for (size_t j = 0; j < list.size() && j < 500; ++j) md.push_back("- [" + list[j]->title + "](" + list[j]->url + ")");
    md.push_back("");
  }
  long long ndoc = 0, reasoned = 0;
  for (auto& [_, p] : pages) ndoc += !p.document.empty();
  for (auto& t : tdata) reasoned += t.reasoned;
  json by_status = json::object();
  for (auto& [k, v] : counts)
    if (v) by_status[k] = v;
  json manifest = {{"format", "nexus-domain-knowledge"},
                   {"version", 1},
                   {"domain", domain_name},
                   {"title", title},
                   {"created", strftime_local("%Y-%m-%d %H:%M")},
                   {"pages", pages.size()},
                   {"documents", ndoc},
                   {"sections", sec_ids.size()},
                   {"concepts", used.size()},
                   {"facts", facts.size()},
                   {"facts_by_status", by_status},
                   {"external_checked", external},
                   {"topics", tdata.size()},
                   {"reasoned_topics", reasoned},
                   {"semantic_relations", semantic},
                   {"relations", edges.size()},
                   {"nodes", nodes.size()},
                   {"layers", layers},
                   {"seconds", round_to(now() - t0, 1)},
                   {"compared", compared}};
  return {{"graph", graph}, {"markdown", join(md, "\n") + "\n"}, {"manifest", manifest}};
}

void DomainGraph::package(const fs::path& out, const json& built) {
  fs::path tmp = out;
  tmp.replace_extension(".tmp");
  if (!archive::write_zip(tmp, {{"graph.json", built["graph"].dump()},
                                {"conocimiento.md", built["markdown"].get<std::string>()},
                                {"manifest.json", built["manifest"].dump(1)}}))
    throw std::runtime_error("no se pudo escribir el paquete");
  std::error_code ec;
  fs::rename(tmp, out, ec);
}

json read_package(const fs::path& path) {
  archive::Zip z(path);
  if (!z.ok()) throw std::invalid_argument("no es un ZIP valido");
  auto g = z.read("graph.json"), md = z.read("conocimiento.md"), m = z.read("manifest.json");
  if (!g || !md || !m) throw std::invalid_argument("faltan graph.json, conocimiento.md o manifest.json");
  json gj = jparse(*g), mj = jparse(*m);
  if (!gj.is_object() || !mj.is_object()) throw std::invalid_argument("graph.json o manifest.json no son JSON validos");
  return {{"graph", gj}, {"markdown", valid_utf8(*md)}, {"manifest", mj}};
}

json import_package(Services& s, const json& data, const std::string& agent, const std::string& source) {
  const json& m = data["manifest"];
  json parsed = kg::parse_json(data["graph"]);
  json res = kg::insert_graph(*s.graph, parsed, agent, nullptr, s.agents->ids(),
                              source.empty() ? "dominio:" + jstr(m, "domain") : source);
  std::string title = uleft("Conocimiento del dominio " + jstr(m, "domain"), 200);
  static const Rx PARTS(R"(\n(?=## ))");
  std::vector<std::string> parts;
  for (auto& p : PARTS.split(data["markdown"].get<std::string>()))
    if (!trim(p).empty()) parts.push_back(p);
  json meta = {{"origin", "domain-knowledge"}};
  for (auto* k : {"domain", "pages", "facts", "topics"})
    if (m.contains(k)) meta[k] = m[k];
  json doc = s.rag->ingest_pages(title, parts, source.empty() ? jstr(m, "domain") : source, "text", "", meta, {}, agent);
  return {{"graph", res}, {"rag_doc_id", doc["id"]}, {"rag_chunks", doc["chunks"]}, {"agent", agent}, {"manifest", m}};
}

}  // namespace nx::rag::domain_kg
