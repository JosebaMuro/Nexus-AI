// Organizador del conocimiento (memory/organizer.py): que es comun y que es saber hacer de cada agente.
#include <algorithm>
#include <cmath>
#include <set>

#include "memory/memory.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("organizer");

static const std::string V_ =
    R"((?:respond|contest|habl|escrib|explic|us|utilic|trat|llam|dig|pong|hag|mand|formate|resum|corrij|pregunt|repit|)"
    R"(invent|tute|ignor|traduzc|cit|alarg|extiend|incluy|emple|met|dej|insist|enroll|interrump|repas|ofrezc|propong|)"
    R"(sugier|recomiend|pid)\w*?(?:as|es))";
static const std::string KIN =
    R"(hermano|hermana|madre|padre|mam[aá]|pap[aá]|hijo|hija|mujer|marido|esposa|esposo|pareja|novio|novia|jefe|jefa|)"
    R"(amigo|amiga|compa[nñ]ero|compa[nñ]era|socio|socia|cliente|vecino|vecina|abuelo|abuela|t[ií]o|t[ií]a|primo|prima|)"
    R"(perro|perra|gato|gata|brother|sister|mother|father|son|daughter|wife|husband|partner|boss|friend|colleague)";
static const std::string CAPS = R"([A-ZÁÉÍÓÚÑ][\wáéíóúñü]+)";

static const std::vector<Rx>& behavior() {
  static const std::vector<Rx> B = [] {
    std::vector<Rx> v;
    auto add = [&](const std::string& p) { v.emplace_back(p, Rx::I); };
    add(R"(\b(?:prefiero|quiero|me gusta(?:r[ií]a)?|necesito|te pido) que (?:me |le |nos )?)" + V_ + R"(\b[^.!?\n]{0,140})");
    add(R"(\b(?:no|nunca|jam[aá]s) (?:me |le |nos |lo |la |los |las )?)" + V_ + R"(\b[^.!?\n]{0,140})");
    add(R"(\b(?:a partir de ahora|de ahora en adelante|en adelante|por defecto)\b[ ,:]+[^.!?\n]{5,140})");
    add(R"(\b(?:siempre) (?:me |le |nos )?(?:)" + V_ + R"(|\w+(?:me|nos))\b[^.!?\n]{0,120})");
    add(R"(\b(?:s[eé]|sed) (?:m[aá]s |menos )?(?:breve|conciso|concisa|directo|directa|detallado|detallada|formal|)"
        R"(informal|claro|clara|t[eé]cnico|t[eé]cnica|amable|sincero|sincera)\b[^.!?\n]{0,80})");
    add(R"(\b(?:h[aá]blame|tr[aá]tame|dir[ií]gete a m[ií]) de (?:t[uú]|usted)\b)");
    add(R"(\b\w+(?:me|nos)\s+(?:siempre|nunca)\b[^.!?\n]{3,120})");
    add(R"(\bll[aá]mame\s+[^.!?\n]{2,40})");
    add(R"(\bcuando (?:te pida|te diga|te pregunte|te hable|te mande)\b[^.!?\n]{5,140})");
    add(R"(\b(?:i prefer|i want|i'?d like) (?:you|that you) (?:to )?\w+[^.!?\n]{2,120})");
    add(R"(\b(?:always|never|from now on)\b[ ,]+(?:answer|reply|respond|use|write|call|explain|cite|include|)"
        R"(speak|talk|be|keep|give|show|ask)\b[^.!?\n]{0,120})");
    add(R"(\b(?:be|keep it) (?:more |less )?(?:brief|concise|short|detailed|formal|informal|direct)\b[^.!?\n]{0,80})");
    return v;
  }();
  return B;
}

static const std::vector<Rx>& relations() {
  static const std::vector<Rx> R = [] {
    std::vector<Rx> v;
    v.emplace_back(R"(\b(?i:mis?) ((?i:)" + KIN + R"())\s+(?:se llama\s+|es\s+|,\s*)?()" + CAPS + R"((?:\s+)" + CAPS + R"()?))");
    v.emplace_back(R"(\b()" + CAPS + R"((?:\s+)" + CAPS + R"()?) es (?i:mi) ((?i:)" + KIN + R"())\b)");
    v.emplace_back(R"(\b(?i:my) ((?i:)" + KIN + R"())(?:'s name is|\s+is called|\s+is)?\s+()" + CAPS + R"((?:\s+)" + CAPS +
                   R"()?))");
    return v;
  }();
  return R;
}

static const std::vector<std::string> GENERAL_PROTOTYPES = {
    "Conversación cotidiana, cultura general, actualidad, ocio, viajes, comida, salud, deportes y temas comunes",
    "Personas, familia, amigos, lugares, fechas y acontecimientos de la vida diaria del usuario",
    "Gustos, preferencias, opiniones, costumbres y forma de ser del usuario",
    "Cómo debe comportarse y responder el asistente: tono, estilo, longitud, idioma y normas de trato",
    "Conocimiento del mundo y sentido común útil para razonar sobre cualquier tema",
};
static const std::set<std::string> GENERALIST = {"nexus"};

static std::string clip(const std::string& text, size_t n = 140) {
  static const Rx ws(R"(\s+)");
  return uleft(trim(ws.sub(text, " "), " .,;:"), n);
}

Organizer::Organizer(KnowledgeGraph& g, EmbedderGetter emb, AgentsGetter agents, Config* c)
    : graph(g), embedder(std::move(emb)), agents_getter(std::move(agents)), config(c) {}

json Organizer::cfg(const std::string& key, const json& def) {
  return config ? config->get("memory.organizer." + key, def) : def;
}

bool Organizer::semantic() { return !starts_with(embedder()->id, "hash"); }

std::vector<agents::AgentSpec> Organizer::specialists() {
  std::vector<agents::AgentSpec> out;
  if (!agents_getter) return out;
  for (auto& a : agents_getter()) {
    if (GENERALIST.count(a.id) || !a.enabled) continue;
    if (!a.specialty.empty() || !a.keywords.empty() || !a.description.empty()) out.push_back(a);
  }
  return out;
}

std::string Organizer::profile_text(const agents::AgentSpec& a) {
  return trim(a.name + ". " + a.description + ". " + a.specialty + ". Palabras clave: " + join(a.keywords, ", "));
}

double Organizer::keyword_hits(const agents::AgentSpec& a, const std::string& text) {
  auto toks = tokens(text);
  std::set<std::string> words(toks.begin(), toks.end());
  std::string low = join(toks, " ");
  std::vector<std::string> terms = a.keywords;
  static const Rx sep(R"([,;\n])");
  for (auto& t : sep.split(a.specialty)) {
    std::string s = trim(t);
    if (!s.empty() && ulen(s) <= 30) terms.push_back(s);
  }
  std::vector<std::string> uniq;
  for (auto& t : terms)
    if (std::find(uniq.begin(), uniq.end(), t) == uniq.end()) uniq.push_back(t);
  double score = 0;
  for (auto& k : uniq) {
    std::string kt = join(tokens(k), " ");
    if (kt.empty()) continue;
    if (contains(kt, " ") && contains(low, kt)) {
      score += 2;
    } else if (words.count(kt)) {
      score += 1;
    } else if (ulen(kt) >= 6) {
      for (auto& w : words)
        if (starts_with(w, kt)) {
          score += 1;
          break;
        }
    }
  }
  return score;
}

json Organizer::affinities(const std::vector<std::string>& texts, const std::string& current) {
  auto agents = specialists();
  json out = json::array();
  if (agents.empty() || texts.empty()) {
    for (size_t i = 0; i < texts.size(); ++i)
      out.push_back({{"scope", ""}, {"category", "comun"}, {"score", 0.0}, {"scores", json::object()}});
    return out;
  }
  std::vector<std::vector<double>> kws;
  for (auto& t : texts) {
    std::vector<double> row;
    for (auto& a : agents) row.push_back(keyword_hits(a, t));
    kws.push_back(row);
  }
  if (semantic()) {
    auto emb = embedder();
    Mat prof, gen;
    {
      std::lock_guard<std::mutex> lk(mu_);
      std::string key = emb->id;
      for (auto& a : agents) key += "\x1f" + a.id + "\x1e" + profile_text(a);
      if (prof_key_ != key) {
        std::vector<std::string> all;
        for (auto& a : agents) all.push_back(profile_text(a));
        all.insert(all.end(), GENERAL_PROTOTYPES.begin(), GENERAL_PROTOTYPES.end());
        prof_mat_ = emb->embed(all);
        prof_key_ = key;
      }
      prof.assign(prof_mat_.begin(), prof_mat_.begin() + agents.size());
      gen.assign(prof_mat_.begin() + agents.size(), prof_mat_.end());
    }
    std::vector<std::string> q;
    for (auto& t : texts) q.push_back(uleft(t, 2000));
    Mat qv = emb->embed_query(q, "classify");
    double min_aff = jnum(cfg("min_affinity", 0.35)), margin = jnum(cfg("margin", 0.03));
    for (size_t i = 0; i < texts.size(); ++i) {
      auto sims = matvec(prof, qv[i]);
      auto gs = matvec(gen, qv[i]);
      double gsim = gs.empty() ? 0 : *std::max_element(gs.begin(), gs.end());
      json scores = json::object();
      std::string best;
      double bestv = -1e9;
      size_t bj = 0;
      for (size_t j = 0; j < agents.size(); ++j) {
        double v = sims[j] + 0.04 * std::min(kws[i][j], 3.0) + (agents[j].id == current ? 0.03 : 0);
        scores[agents[j].id] = v;
        if (v > bestv) {
          bestv = v;
          best = agents[j].id;
          bj = j;
        }
      }
      bool ok = sims[bj] >= min_aff && bestv - gsim >= margin;
      json rounded = json::object();
      for (auto& [k, v] : scores.items()) rounded[k] = round_to(v.get<double>(), 3);
      out.push_back({{"scope", ok ? best : ""},
                     {"category", ok ? "especialidad" : "comun"},
                     {"score", round_to(bestv, 3)},
                     {"general", round_to(gsim, 3)},
                     {"scores", rounded}});
    }
    return out;
  }
  for (size_t i = 0; i < texts.size(); ++i) {   // sin modelo semantico: palabras clave de cada especialidad
    json scores = json::object();
    std::string best;
    double bestv = -1e9;
    for (size_t j = 0; j < agents.size(); ++j) {
      double v = kws[i][j] + (agents[j].id == current && kws[i][j] ? 0.5 : 0);
      scores[agents[j].id] = v;
      if (v > bestv) {
        bestv = v;
        best = agents[j].id;
      }
    }
    bool ok = bestv >= jnum(cfg("min_keyword_hits", 1));
    out.push_back({{"scope", ok ? best : ""}, {"category", ok ? "especialidad" : "comun"}, {"score", bestv},
                   {"scores", scores}});
  }
  return out;
}

std::pair<std::string, std::string> Organizer::route(const std::string& text, const std::string& current) {
  auto sp = specialists();
  if (sp.empty()) return {current, current.empty() ? "comun" : "especialidad"};
  json r;
  try {
    r = affinities({text}, current)[0];
  } catch (const std::exception& e) {
    L.warning("Clasificacion semantica no disponible (", e.what(), "): se usan palabras clave");
    std::string best;
    double bv = -1;
    for (auto& a : sp) {
      double h = keyword_hits(a, text);
      if (h > bv) {
        bv = h;
        best = a.id;
      }
    }
    return bv >= 1 ? std::make_pair(best, std::string("especialidad")) : std::make_pair(std::string(), std::string("comun"));
  }
  static const Rx proc(R"(\b(paso \d|primero|despu[eé]s|luego|finalmente|c[oó]mo se hace|procedimiento|)"
                       R"(instala|configura|ejecuta|step \d|how to)\b)",
                       Rx::I);
  std::string sc = jstr(r, "scope");
  if (!sc.empty() && proc.test(text)) return {sc, "procedimiento"};
  return {sc, jstr(r, "category")};
}

std::string Organizer::strip_behaviors(std::string text) {
  for (auto& p : behavior()) text = p.sub(text, " ");
  return text;
}

json Organizer::extract_general(const std::string& text) {
  KnowledgeGraph& g = graph.general();
  json learned = json::array();
  g.db.transaction([&] {
    std::vector<std::string> rules;
    for (auto& p : behavior())
      for (auto& m : p.finditer(text)) {
        std::string c = clip(m.str());
        if (std::find(rules.begin(), rules.end(), c) == rules.end()) rules.push_back(c);
      }
    std::vector<std::string> kept;
    for (auto& r : rules) {
      if (ulen(r) < 6) continue;
      bool inside = false;
      for (auto& o : rules)
        if (r != o && contains(lower(o), lower(r))) inside = true;
      if (!inside) kept.push_back(r);
    }
    for (auto& rule : kept)
      if (g.relate(USER, "pide", rule, "persona", "pauta", 1.2, "comportamiento"))
        learned.push_back({{"s", USER}, {"r", "pide"}, {"o", rule}, {"type", "pauta"}, {"category", "comportamiento"}});
    static const Rx kin("(?:" + KIN + ")", Rx::I);
    for (auto& p : relations()) {
      for (auto& m : p.finditer(text)) {
        std::string a = m.str(1), b = m.str(2);
        bool a_kin = kin.fullmatch(a).has_value();
        std::string k = a_kin ? a : b, name = a_kin ? b : a;
        std::string rel = "su_" + replace_all(normalize(k), " ", "_");
        if (!gkey(name).empty() && g.relate(USER, rel, name, "persona", "persona", 1.3, "relacion"))
          learned.push_back({{"s", USER}, {"r", rel}, {"o", name}, {"type", "persona"}, {"category", "relacion"}});
      }
    }
  });
  return learned;
}

void Organizer::record_interaction(const std::string& who, const std::string& rel, const std::string& whom,
                                   double weight) {
  if (who.empty() || whom.empty() || gkey(who) == gkey(whom)) return;
  KnowledgeGraph& g = graph.general();
  std::string wtype = gkey(who) == gkey(USER) ? "persona" : "interlocutor";
  g.relate(who, rel, whom, wtype, "interlocutor", weight, "interaccion", wtype == "persona" ? "perfil" : "interaccion");
}

Mat Organizer::node_vectors(const std::vector<Row>& rows) {
  auto emb = embedder();
  DB& db = graph.db;
  if (rows.empty()) return {};
  std::string ids;
  for (size_t i = 0; i < rows.size(); ++i) ids += (i ? "," : "") + std::to_string(rows[i]["id"].get<long long>());
  std::map<long long, std::pair<std::string, Vec>> have;
  for (auto& r : db.query("SELECT entity_id, key, vec FROM entity_vecs WHERE embedder=? AND entity_id IN (" + ids + ")",
                          {emb->id}))
    have[r["entity_id"].get<long long>()] = {jstr(r, "key"), floats(r["vec"])};
  std::vector<size_t> todo;
  for (size_t i = 0; i < rows.size(); ++i) {
    long long id = rows[i]["id"].get<long long>();
    auto it = have.find(id);
    if (it == have.end() || it->second.first != jstr(rows[i], "key")) todo.push_back(i);
  }
  if (!todo.empty()) {
    std::vector<std::string> texts;
    for (size_t i : todo) texts.push_back(trim(jstr(rows[i], "name") + ". " + jstr(rows[i], "note"), " ."));
    Mat vecs = emb->embed(texts);
    std::vector<Params> up;
    for (size_t k = 0; k < todo.size(); ++k) {
      auto& r = rows[todo[k]];
      up.push_back({r["id"], emb->id, r["key"], blob(vecs[k])});
      have[r["id"].get<long long>()] = {jstr(r, "key"), vecs[k]};
    }
    db.executemany("INSERT OR REPLACE INTO entity_vecs(entity_id, embedder, key, vec) VALUES(?,?,?,?)", up);
  }
  Mat out;
  for (auto& r : rows) out.push_back(have[r["id"].get<long long>()].second);
  return out;
}

json Organizer::semantic_search(const std::string& q, const std::optional<std::string>& scope, int limit, int pool) {
  std::string where = scope ? "WHERE scope=?" : "";
  Params p;
  if (scope) p.push_back(*scope);
  p.push_back(pool);
  auto rows = graph.db.query("SELECT id, key, name, type, scope, category, weight, note FROM entities " + where +
                                 " ORDER BY weight DESC LIMIT ?",
                             p);
  if (rows.empty() || trim(q).empty()) return json::array();
  Mat mat = node_vectors(rows);
  Vec qv = embedder()->embed_query({q}, "graph")[0];
  if (cols(mat) != qv.size()) return json::array();
  auto sims = matvec(mat, qv);
  auto order = argsort_desc(sims);
  json out = json::array();
  for (size_t i = 0; i < order.size() && (int)i < limit; ++i) {
    json r = rows[order[i]];
    r["similarity"] = round_to(sims[order[i]], 3);
    out.push_back(r);
  }
  return out;
}

json Organizer::duplicates(const std::optional<std::string>& scope, std::optional<double> threshold, int pool, int limit) {
  double thr = threshold ? *threshold : (semantic() ? 0.9 : 0.82);
  std::vector<std::string> scopes;
  if (scope)
    scopes.push_back(*scope);
  else
    for (auto& r : graph.db.query("SELECT DISTINCT scope FROM entities")) scopes.push_back(jstr(r, "scope"));
  std::vector<std::pair<double, json>> out;
  for (auto& sc : scopes) {
    std::vector<Row> rows;
    for (auto& r : graph.db.query("SELECT id, key, name, type, scope, category, weight, note FROM entities WHERE scope=? "
                                  "ORDER BY weight DESC LIMIT ?",
                                  {sc, pool}))
      if (jstr(r, "type") != "self" && jstr(r, "type") != AGENT && jstr(r, "key") != gkey(USER)) rows.push_back(r);
    if (rows.size() < 2) continue;
    Mat mat = node_vectors(rows);
    for (size_t i = 0; i < rows.size(); ++i)
      for (size_t j = i + 1; j < rows.size(); ++j) {
        double s = dot(mat[i], mat[j]);
        if (s >= thr) {
          auto& a = rows[i];
          auto& b = rows[j];
          bool ka = jnum(a["weight"]) >= jnum(b["weight"]);
          out.emplace_back(s, json{{"keep", ka ? a : b}, {"drop", ka ? b : a}, {"similarity", round_to(s, 3)}});
        }
      }
  }
  std::stable_sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first > y.first; });
  json res = json::array();
  for (size_t i = 0; i < out.size() && (int)i < limit; ++i) res.push_back(out[i].second);
  return res;
}

json Organizer::plan(const std::function<void(size_t, size_t)>& progress, int limit) {
  DB& db = graph.db;
  auto rows = db.query("SELECT id, key, name, type, scope, category, note FROM entities ORDER BY weight DESC LIMIT ?",
                       {limit});
  json moves = json::array();
  std::vector<Row> topical;
  std::string ukey = gkey(USER);
  for (auto& r : rows) {
    std::string type = jstr(r, "type"), cat = jstr(r, "category"), sc = jstr(r, "scope");
    if (type == "self" || type == AGENT || jstr(r, "key") == ukey) continue;
    if (type == "documento" || cat == "documento") continue;   // los documentos siguen a su biblioteca
    bool personal = GENERAL_CATEGORIES.contains(cat) && cat != "comun";
    if (!personal && !sc.empty()) {   // hechos del usuario que acabaron en un agente
      auto tie = db.one("SELECT r.rel FROM relations r JOIN entities u ON u.id=r.src WHERE r.dst=? AND u.key=? "
                        "AND r.rel NOT IN ('menciona','relacionado_con') LIMIT 1",
                        {r["id"], ukey});
      personal = tie.has_value();
    }
    if (personal) {
      if (!sc.empty()) {
        json m = r;
        m["to"] = "";
        m["to_category"] = GENERAL_CATEGORIES.contains(cat) ? cat : "perfil";
        m["reason"] = "conocimiento del usuario (comun)";
        moves.push_back(m);
      }
      continue;
    }
    topical.push_back(r);
  }
  if (!specialists().empty()) {
    for (size_t b = 0; b < topical.size(); b += 64) {
      std::vector<Row> block(topical.begin() + b, topical.begin() + std::min(topical.size(), b + 64));
      std::vector<std::string> texts;
      for (auto& r : block) {
        auto nearby = db.query("SELECT e.name FROM relations x JOIN entities e ON e.id = CASE WHEN x.src=? THEN x.dst "
                             "ELSE x.src END WHERE (x.src=? OR x.dst=?) AND e.type NOT IN ('self','agente') "
                             "ORDER BY x.weight DESC LIMIT 8",
                             {r["id"], r["id"], r["id"]});
        std::vector<std::string> names;
        for (auto& n : nearby) names.push_back(jstr(n, "name"));
        texts.push_back(jstr(r, "name") + ". " + jstr(r, "note") + " " + join(names, ", "));
      }
      json aff = affinities(texts);
      for (size_t i = 0; i < block.size(); ++i) {
        if (jstr(aff[i], "scope") != jstr(block[i], "scope")) {
          json m = block[i];
          m["to"] = aff[i]["scope"];
          m["to_category"] = aff[i]["category"];
          m["score"] = aff[i]["score"];
          m["reason"] = jstr(aff[i], "scope").empty() ? "conocimiento comun" : "especialidad de un agente";
          moves.push_back(m);
        }
      }
      if (progress) progress(std::min(b + 64, topical.size()), topical.size());
    }
  }
  return moves;
}

json Organizer::apply(const json& moves, bool memories, MemoryStore* store) {
  int moved = 0;
  for (auto& m : moves) {
    try {
      graph.move_entity(m["id"].get<long long>(), jstr(m, "to"), jstr(m, "to_category"));
      ++moved;
    } catch (const std::exception& e) {
      L.info("No se movio ", jstr(m, "name"), ": ", e.what());
    }
  }
  int fixed = 0;
  if (memories && store) {   // recuerdos del usuario siempre comunes
    fixed = store->db.execute("UPDATE memories SET agent='' WHERE agent!='' AND kind IN ('fact','preference','behavior')");
    store->invalidate();
  }
  return {{"moved", moved}, {"memories_shared", fixed}, {"ts", now()}};
}

json Organizer::overview() {
  DB& db = graph.db;
  json sp = json::array();
  for (auto& a : specialists()) sp.push_back({{"id", a.id}, {"name", a.name}, {"profile", profile_text(a)}});
  return {{"general_categories", GENERAL_CATEGORIES},
          {"agent_categories", AGENT_CATEGORIES},
          {"by_scope", graph.categories_by_scope()},
          {"semantic", semantic()},
          {"embedder", embedder()->id},
          {"specialists", sp},
          {"relations", db.scalar("SELECT COUNT(*) n FROM relations")},
          {"bridges", db.scalar("SELECT COUNT(*) n FROM relations r JOIN entities a ON a.id=r.src JOIN entities b "
                                "ON b.id=r.dst WHERE a.scope!=b.scope")}};
}

}  // namespace nx::mem
