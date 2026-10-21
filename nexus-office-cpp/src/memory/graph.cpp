// Grafo de conocimiento (memory/graph.py): grafo general comun y un subgrafo aislado por agente.
#include <algorithm>
#include <cmath>
#include <set>

#include "memory/memory.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::mem {

const char* SELF = "Nexus-Office";
const char* USER = "Usuario";
const char* AGENT = "agente";
const char* GENERAL = "";
static const char* LINK_REL = "subgrafo";
#define NX_CAP "[A-ZÁÉÍÓÚÑ][\\wáéíóúñü]+"

const json GENERAL_CATEGORIES = {{"perfil", "Datos del usuario"},
                                 {"preferencia", "Gustos y preferencias"},
                                 {"comportamiento", "Pautas de comportamiento"},
                                 {"relacion", "Relaciones entre personas y entidades"},
                                 {"interaccion", "Interacciones entre el usuario y los agentes"},
                                 {"comun", "Conocimiento general para la inferencia"}};
const json AGENT_CATEGORIES = {{"especialidad", "Saber hacer de su especialidad"},
                               {"procedimiento", "Procedimientos y técnicas"},
                               {"documento", "Documentos y fuentes"},
                               {"experiencia", "Experiencia propia"}};
const json CATEGORIES = [] {
  json c = GENERAL_CATEGORIES;
  for (auto& [k, v] : AGENT_CATEGORIES.items()) c[k] = v;
  return c;
}();

static const std::set<std::string>& stopset() {
  static const std::set<std::string> s = [] {
    std::string words = R"(
el la los las un una unos unas de del al a en y o u que se por para con sin sobre como mas pero sus su mi mis tu tus
es son fue era ser estar esta este estos estas eso esto ese esa aqui alli hay muy tambien ya no si le les lo me te nos
yo tu el ella ellos nosotros vosotros usted ustedes cual cuales quien donde cuando porque puedes puede quiero quieres
hola gracias favor algo nada todo todos cosa cosas hacer tiene tengo tienes
hablame dime cuentame explica explicame compon componer calcula crea genera dibuja escribe busca quiero
necesito ayuda ayudame sabes puedo podrias haz dame muestra
the a an of to in on and or for with is are was were be been this that these those it its i you he she we they my your
what which who when where why how can could would should will just about from into than then there here please thanks
)";
    std::set<std::string> out;
    for (auto& w : split_ws(normalize(words))) out.insert(w);
    return out;
  }();
  return s;
}

bool is_stop(const std::string& key) { return stopset().count(key) > 0; }

std::string gkey(const std::string& name) { return join(tokens(name), " "); }

std::string clean_obj(const std::string& obj) {
  static const Rx cut(R"(\b(?:y|and|pero|but|porque|because|que|that)\b)");
  std::string o = cut.split(trim(obj), 1)[0];
  return trim(trim(o, " .,'\""));
}

struct Pattern {
  Rx rx;
  std::string rel, otype;
};

static const std::vector<Pattern>& patterns() {
  static const std::vector<Pattern> P = [] {
    std::vector<Pattern> v;
    auto add = [&](const std::string& p, const char* rel, const char* t) { v.push_back({Rx(p, Rx::I), rel, t}); };
    add(R"(\b(?:me llamo|mi nombre es|my name is|i am called|llamame)\s+()" NX_CAP R"((?:\s+)" NX_CAP R"()?))",
        "se_llama", "persona");
    add(R"rx(\b(?:no me gusta(?:n)?|odio|detesto|i hate|i dislike|i don'?t like)\s+(?:el |la |los |las |the )?([^.,;!?\n]{2,40}))rx",
        "no_le_gusta", "preferencia");
    add(R"rx(\b(?<!no )(?:me gusta(?:n)?|me encanta(?:n)?|adoro|i like|i love|i enjoy)\s+(?:el |la |los |las |the )?([^.,;!?\n]{2,40}))rx",
        "le_gusta", "preferencia");
    add(R"(\b(?:vivo en|soy de|resido en|i live in|i'?m from)\s+()" NX_CAP R"((?:\s+)" NX_CAP R"()*))", "vive_en",
        "lugar");
    add(R"rx(\b(?:trabajo (?:en|para)|i work (?:at|for))\s+([^.,;!?\n]{2,40}))rx", "trabaja_en", "organizacion");
    add(R"rx(\b(?:trabajo como|soy (?:un |una )?(?=programador|ingenier|disenador|diseñador|medic|profesor|estudiante|artista|music|desarrollador)|i work as (?:an? )?|i'?m an? (?=developer|engineer|designer|doctor|teacher|student|artist|musician))([^.,;!?\n]{2,40}))rx",
        "es", "rol");
    add(R"rx(\b(?:tengo un|tengo una|i have an?)\s+([^.,;!?\n]{2,30}))rx", "tiene", "objeto");
    add(R"rx(\b(?:estoy aprendiendo|aprendo|i'?m learning|i am learning)\s+([^.,;!?\n]{2,40}))rx", "aprende",
        "concepto");
    return v;
  }();
  return P;
}

static const Rx& is_a() {
  static const Rx r(R"(\b()" NX_CAP R"((?:[ \t]+)" NX_CAP R"()*)\s+(?:es|is)\s+(?:un|una|el|la|an?|the)\s+([\wáéíóúñ ]{3,40}))");
  return r;
}

static const std::map<std::string, std::string> PERSONAL_CATEGORY = {{"le_gusta", "preferencia"},
                                                                     {"no_le_gusta", "preferencia"}};

static std::string name60(const std::string& n) { return uleft(trim(n), 60); }

// ---------------------------------------------------------------- construccion y ambitos
KnowledgeGraph::KnowledgeGraph(DB& d, std::string sc, std::string sn, KnowledgeGraph* root)
    : db(d), scope(std::move(sc)), self_name(std::move(sn)), root_(root ? root : this) {
  self_type = scope.empty() ? "self" : AGENT;
  db.transaction([&] {
    if (!scope.empty()) {   // agente renombrado: su ancla cambia de nombre en vez de duplicarse
      db.execute("UPDATE OR IGNORE entities SET key=?, name=? WHERE scope=? AND type=? AND key!=?",
                 {gkey(self_name), name60(self_name), scope, AGENT, gkey(self_name)});
      db.execute("UPDATE entities SET category='' WHERE scope=? AND key=? AND category!=''", {scope, gkey(USER)});
    }
    upsert_entity(self_name, self_type, 5);
    upsert_entity(USER, "persona", 4);
  });
}

KnowledgeGraph& KnowledgeGraph::for_agent(const std::string& agent_in) {
  KnowledgeGraph& root = *root_;
  std::string agent = trim(agent_in);
  if (agent.empty()) return root;
  std::lock_guard<std::mutex> lk(root.views_mu_);
  auto it = root.views_.find(agent);
  if (it != root.views_.end()) return *it->second;
  std::string name = root.namer ? root.namer(agent) : "";
  if (name.empty()) name = agent;
  auto v = std::make_unique<KnowledgeGraph>(db, agent, name, &root);
  auto& ref = *v;
  root.views_[agent] = std::move(v);
  return ref;
}

void KnowledgeGraph::forget_view(const std::string& agent) {
  // la proxima vista se rehace (si cambio el nombre, su ancla lo sigue); la antigua se conserva viva por si
  // alguien tiene aun una referencia
  KnowledgeGraph& root = *root_;
  std::lock_guard<std::mutex> lk(root.views_mu_);
  auto it = root.views_.find(agent);
  if (it == root.views_.end()) return;
  root.graveyard_.push_back(std::move(it->second));
  root.views_.erase(it);
}

std::vector<json> KnowledgeGraph::scopes() {
  std::map<std::string, json> rels;
  for (auto& r : db.query("SELECT scope, COUNT(*) n FROM relations GROUP BY scope")) rels[jstr(r, "scope")] = r["n"];
  std::vector<json> out;
  for (auto& r : db.query("SELECT scope, COUNT(*) n FROM entities GROUP BY scope")) {
    std::string sc = jstr(r, "scope");
    out.push_back({{"scope", sc}, {"entities", r["n"]}, {"relations", rels.count(sc) ? rels[sc] : json(0)}});
  }
  return out;
}

int KnowledgeGraph::drop_scope(const std::string& agent) {
  if (agent.empty()) throw std::invalid_argument("el grafo general no se puede borrar");
  int n = 0;
  db.transaction([&] {
    db.execute("DELETE FROM relations WHERE scope=?", {agent});
    n = db.execute("DELETE FROM entities WHERE scope=?", {agent});
  });
  forget_view(agent);
  return n;
}

// ---------------------------------------------------------------- escritura
std::string KnowledgeGraph::default_category(const std::string& etype) const {
  if (etype == "documento") return "documento";
  return scope.empty() ? "comun" : "especialidad";
}

long long KnowledgeGraph::upsert_entity(const std::string& name, const std::string& etype, double weight,
                                        std::string category, const std::string& note, const std::string& source) {
  std::string key = gkey(name);
  if (key.empty()) return 0;
  double t = now();
  if (etype == "self" || etype == AGENT || key == gkey(USER)) {
    // el Usuario solo es "perfil" en el general; en un subgrafo de agente es un ancla, como el propio agente
    category = !scope.empty() ? "" : (!category.empty() ? category : (key == gkey(USER) ? "perfil" : ""));
  } else if (category.empty()) {
    category = default_category(etype);
  }
  auto row = db.returning(
      "INSERT INTO entities(scope, key, name, type, weight, first_seen, last_seen, category, note, source) "
      "VALUES(?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(scope, key) DO UPDATE SET mentions=mentions+1, weight=weight+excluded.weight*0.5, "
      "last_seen=excluded.last_seen, type=CASE WHEN type IN ('concepto','') AND excluded.type!='concepto' "
      "THEN excluded.type ELSE type END, "
      "category=CASE WHEN category IN ('', 'comun', 'especialidad') AND excluded.category NOT IN ('', 'comun', "
      "'especialidad') THEN excluded.category WHEN category='' THEN excluded.category ELSE category END, "
      "note=CASE WHEN note='' THEN excluded.note ELSE note END, "
      "source=CASE WHEN source='' THEN excluded.source ELSE source END RETURNING id",
      {scope, key, name60(name), etype, weight, t, t, category, uleft(note, 500), uleft(source, 120)});
  return row ? (*row)["id"].get<long long>() : 0;
}

std::optional<std::pair<long long, long long>> KnowledgeGraph::relate(const std::string& src, const std::string& rel,
                                                                      const std::string& dst,
                                                                      const std::string& src_type,
                                                                      const std::string& dst_type, double weight,
                                                                      const std::string& category,
                                                                      const std::string& src_category) {
  std::optional<std::pair<long long, long long>> out;
  db.transaction([&] {
    long long a = upsert_entity(src, src_type, 1.0, src_category);
    long long b = upsert_entity(dst, dst_type, 1.0, category);
    if (!a || !b || a == b) return;
    db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
               "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
               "last_seen=excluded.last_seen",
               {a, b, rel, weight, now(), scope});
    out = std::make_pair(a, b);
  });
  return out;
}

// ---------------------------------------------------------------- extraccion
json KnowledgeGraph::extract(const std::string& text, const std::string& speaker) {
  json learned = json::array();
  db.transaction([&] {
    if (speaker == "user") learned = extract_personal(text);
    for (auto& t : extract_topics(text, speaker)) learned.push_back(t);
  });
  return learned;
}

json KnowledgeGraph::extract_personal(const std::string& text) {
  json learned = json::array();
  db.transaction([&] {
    for (auto& p : patterns()) {
      for (auto& m : p.rx.finditer(text)) {
        std::string obj = clean_obj(m.str(1));
        if (ulen(obj) < 2 || is_stop(gkey(obj))) continue;
        auto it = PERSONAL_CATEGORY.find(p.rel);
        std::string cat = it == PERSONAL_CATEGORY.end() ? "perfil" : it->second;
        if (relate(USER, p.rel, obj, "persona", p.otype, 1.5, cat))
          learned.push_back({{"s", USER}, {"r", p.rel}, {"o", obj}, {"type", p.otype}, {"category", cat}});
      }
    }
  });
  return learned;
}

json KnowledgeGraph::extract_topics(const std::string& text, const std::string& speaker, const std::string& category) {
  json learned = json::array();
  std::string cat = category.empty() ? default_category() : category;
  db.transaction([&] {
    for (auto& m : is_a().finditer(text)) {
      std::string subj = trim(m.str(1)), obj = clean_obj(m.str(2));
      if (!is_stop(gkey(subj)) && ulen(obj) > 2 && relate(subj, "es_un", obj, "concepto", "concepto", 1.0, cat, cat))
        learned.push_back({{"s", subj}, {"r", "es_un"}, {"o", obj}, {"type", "concepto"}, {"scope", scope},
                           {"category", cat}});
    }
    auto concepts = keywords(text, 4);
    std::string anchor = speaker == "user" ? USER : self_name;
    std::string atype = speaker == "user" ? "persona" : self_type;
    for (auto& c : concepts) relate(anchor, "menciona", c, atype, "concepto", 0.3, cat);
    for (size_t i = 0; i < concepts.size(); ++i)
      for (size_t j = i + 1; j < concepts.size(); ++j)
        relate(concepts[i], "relacionado_con", concepts[j], "concepto", "concepto", 0.2, cat, cat);
  });
  return learned;
}

int KnowledgeGraph::link_document(const std::string& title_in, const std::string& text, const std::string&) {
  std::string title = name60(title_in);
  if (gkey(title).empty()) return 0;
  int n = 0;
  db.transaction([&] {
    upsert_entity(title, "documento", 2, "documento");
    relate(self_name, "ha_leido", title, self_type, "documento", 1.0, "documento");
    for (auto& kw : keywords(title + ". " + uleft(text, 6000), 8))
      if (gkey(kw) != gkey(title) && relate(title, "trata_de", kw, "documento", "concepto", 0.6, "", "documento")) ++n;
  });
  return n;
}

void KnowledgeGraph::unlink_document(const std::string& title) {
  auto row = db.one("SELECT id FROM entities WHERE scope=? AND key=? AND type='documento'", {scope, gkey(name60(title))});
  if (!row) return;
  db.transaction([&] {
    std::vector<long long> ids = {(*row)["id"].get<long long>()};
    for (auto& r : db.query(
             "SELECT r.dst FROM relations r JOIN entities e ON e.id=r.dst WHERE r.src=? AND r.rel='trata_de' "
             "AND e.mentions<=1 AND (SELECT COUNT(*) FROM relations x WHERE x.src=r.dst OR x.dst=r.dst)=1",
             {(*row)["id"]}))
      ids.push_back(r["dst"].get<long long>());
    for (long long eid : ids) delete_entity(eid);
  });
}

std::vector<std::string> KnowledgeGraph::keywords(const std::string& text, size_t n) {
  static const Rx proper(R"((?<![.!?]\s)(?<!^)(?<!\n)\b)" NX_CAP R"((?:[ \t]+)" NX_CAP R"()*)");
  std::vector<std::string> words;
  for (auto& w : tokens(text))
    if (ulen(w) >= 5 && !is_stop(w) && !is_digit_str(w)) words.push_back(w);
  // Counter.most_common: por frecuencia y, a igualdad, por orden de aparicion
  std::vector<std::pair<std::string, int>> counts;
  std::map<std::string, size_t> pos;
  for (auto& w : words) {
    auto it = pos.find(w);
    if (it == pos.end()) {
      pos[w] = counts.size();
      counts.emplace_back(w, 1);
    } else {
      counts[it->second].second++;
    }
  }
  std::stable_sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::vector<std::string> out;
  std::string kself = gkey(SELF), kuser = gkey(USER);
  for (auto& m : proper.finditer(text)) {
    std::string p = m.str();
    std::string k = gkey(p);
    if (!is_stop(k) && k != kself && k != kuser && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  }
  for (size_t i = 0; i < counts.size() && i < n; ++i) {
    const std::string& w = counts[i].first;
    bool clash = false;
    for (auto& o : out)
      if (gkey(o) == w) clash = true;
    if (!clash) out.push_back(w);
  }
  if (out.size() > n) out.resize(n);
  return out;
}

json KnowledgeGraph::extract_with_llm(const std::string& text,
                                      const std::function<std::string(const std::string&)>& complete) {
  std::string prompt =
      "Extrae hechos como JSON: lista de objetos {\"s\":sujeto,\"r\":relacion,\"o\":objeto,\"type\":tipo}. "
      "Usa 'Usuario' para el usuario. Solo JSON.\nTexto: " +
      uleft(text, 2000);
  json data;
  try {
    std::string raw = complete(prompt);
    size_t a = raw.find('['), b = raw.rfind(']');
    if (a == std::string::npos || b == std::string::npos || b < a) return json::array();
    data = jparse(raw.substr(a, b - a + 1));
  } catch (const std::exception&) {
    return json::array();
  }
  if (!data.is_array()) return json::array();
  json out = json::array();
  db.transaction([&] {
    for (size_t i = 0; i < data.size() && i < 15; ++i) {
      json& t = data[i];
      if (!t.is_object() || !truthy(t.value("s", json())) || !truthy(t.value("o", json())) ||
          !truthy(t.value("r", json())))
        continue;
      std::string c = jstr(t, "category");
      std::string cat = CATEGORIES.contains(c) ? c : "";
      std::string type = t.contains("type") ? jstr(t, "type") : "concepto";
      relate(jstr(t, "s"), uleft(jstr(t, "r"), 30), jstr(t, "o"), "concepto", type, 1.0, cat, cat);
      out.push_back(t);
    }
  });
  return out;
}

// ---------------------------------------------------------------- lectura
static std::vector<std::string> unique_keys(const std::vector<std::string>& names) {
  std::vector<std::string> keys;
  for (auto& n : names) {
    std::string k = gkey(n);
    if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  return keys;
}

static std::string marks(size_t n) {
  std::string m;
  for (size_t i = 0; i < n; ++i) m += i ? ",?" : "?";
  return m;
}

std::vector<std::string> KnowledgeGraph::facts_about(const std::vector<std::string>& names, int limit) {
  auto keys = unique_keys(names);
  if (keys.empty()) return {};
  Params p = {scope};
  for (auto& k : keys) p.push_back(k);
  p.push_back(scope);
  p.push_back(limit);
  auto rows = db.query(
      "WITH ids AS (SELECT id FROM entities WHERE scope=? AND key IN (" + marks(keys.size()) +
          ")) SELECT a.name AS s, r.rel, b.name AS o, r.weight FROM relations r JOIN entities a ON a.id=r.src "
          "JOIN entities b ON b.id=r.dst WHERE (r.src IN ids OR r.dst IN ids) AND r.scope=? AND r.rel NOT IN "
          "('menciona','relacionado_con','conversa_con','delega_en','pide') ORDER BY r.weight DESC LIMIT ?",
      p);
  std::vector<std::string> out;
  for (auto& r : rows) out.push_back(jstr(r, "s") + " " + replace_all(jstr(r, "rel"), "_", " ") + " " + jstr(r, "o"));
  return out;
}

std::vector<std::string> KnowledgeGraph::notes_about(const std::vector<std::string>& names, int limit) {
  auto keys = unique_keys(names);
  if (keys.empty()) return {};
  Params p = {scope};
  for (auto& k : keys) p.push_back(k);
  p.push_back(limit);
  auto rows = db.query("SELECT name, note FROM entities WHERE scope=? AND key IN (" + marks(keys.size()) +
                           ") AND note!='' ORDER BY weight DESC LIMIT ?",
                       p);
  std::vector<std::string> out;
  for (auto& r : rows) out.push_back(jstr(r, "name") + ": " + uleft(jstr(r, "note"), 300));
  return out;
}

json KnowledgeGraph::to_json(int limit, const std::optional<std::string>& sc_in) {
  if (sc_in && *sc_in == "*") return to_json_all(limit);
  std::string sc = sc_in ? *sc_in : scope;
  auto nodes = db.query("SELECT id, name, type, weight, mentions, last_seen, scope, category FROM entities "
                        "WHERE scope=? ORDER BY weight DESC LIMIT ?",
                        {sc, limit});
  if (!sc.empty())   // vista aislada de un agente: su ancla es el nucleo
    for (auto& n : nodes)
      if (jstr(n, "type") == AGENT) {
        n["type"] = "self";
        n["agent_core"] = true;
      }
  std::vector<long long> ids;
  for (auto& n : nodes) ids.push_back(n["id"].get<long long>());
  return {{"nodes", nodes}, {"links", links(ids, limit * 3)}};
}

json KnowledgeGraph::to_json_all(int limit) {
  std::vector<std::pair<std::string, long long>> counts;
  long long total = 0;
  for (auto& r : db.query("SELECT scope, COUNT(*) n FROM entities GROUP BY scope")) {
    counts.emplace_back(jstr(r, "scope"), r["n"].get<long long>());
    total += r["n"].get<long long>();
  }
  if (!total) total = 1;
  json nodes = json::array();
  for (auto& [sc, n] : counts) {   // reparto proporcional (minimo 12 por agente) para que todos se vean
    long long share = (long long)std::nearbyint((double)limit * n / total);
    long long quota = !sc.empty() ? std::max(12LL, share) : std::max((long long)(limit / 2), share);
    for (auto& r : db.query("SELECT id, name, type, weight, mentions, last_seen, scope, category FROM entities "
                            "WHERE scope=? ORDER BY weight DESC LIMIT ?",
                            {sc, quota}))
      nodes.push_back(r);
  }
  std::vector<long long> ids;
  for (auto& n : nodes) ids.push_back(n["id"].get<long long>());
  json lk = links(ids, limit * 3);
  json core;
  for (auto& n : nodes)
    if (jstr(n, "scope").empty() && jstr(n, "type") == "self") {
      core = n["id"];
      break;
    }
  if (!core.is_null())
    for (auto& n : nodes)
      if (jstr(n, "type") == AGENT)
        lk.push_back({{"source", core}, {"target", n["id"]}, {"rel", LINK_REL}, {"weight", 3.0}});
  return {{"nodes", nodes}, {"links", lk}};
}

json KnowledgeGraph::links(const std::vector<long long>& ids, int limit) {
  if (ids.empty()) return json::array();
  std::string m;
  for (size_t i = 0; i < ids.size(); ++i) m += (i ? "," : "") + std::to_string(ids[i]);
  json out = json::array();
  for (auto& r : db.query("SELECT src AS source, dst AS target, rel, weight FROM relations WHERE src IN (" + m +
                              ") AND dst IN (" + m + ") ORDER BY weight DESC LIMIT ?",
                          {limit}))
    out.push_back(r);
  return out;
}

json KnowledgeGraph::stats() {
  auto by = scopes();
  long long e = 0, r = 0;
  json sc = json::object();
  for (auto& s : by) {
    e += s["entities"].get<long long>();
    r += s["relations"].get<long long>();
    std::string k = jstr(s, "scope");
    sc[k.empty() ? "general" : k] = {{"entities", s["entities"]}, {"relations", s["relations"]}};
  }
  return {{"entities", e}, {"relations", r}, {"scopes", sc}};
}

void KnowledgeGraph::delete_entity(long long eid) {
  auto row = db.one("SELECT key, type FROM entities WHERE id=?", {eid});
  if (!row || jstr(*row, "type") == "self" || jstr(*row, "type") == AGENT || jstr(*row, "key") == gkey(USER)) return;
  db.transaction([&] {
    db.execute("DELETE FROM relations WHERE src=? OR dst=?", {eid, eid});
    db.execute("DELETE FROM entities WHERE id=?", {eid});
  });
}

// ---------------------------------------------------------------- gestion
bool KnowledgeGraph::is_protected(const json& row) const {
  std::string t = jstr(row, "type");
  return t == "self" || t == AGENT || jstr(row, "key") == gkey(USER);
}

std::optional<Row> KnowledgeGraph::entity(long long eid) { return db.one("SELECT * FROM entities WHERE id=?", {eid}); }

json KnowledgeGraph::node(long long eid) {
  auto row = entity(eid);
  if (!row) return nullptr;
  auto rels = db.query(
      "SELECT r.id, r.src, r.dst, r.rel, r.weight, r.scope, a.name AS src_name, b.name AS dst_name, "
      "a.scope AS src_scope, b.scope AS dst_scope FROM relations r JOIN entities a ON a.id=r.src "
      "JOIN entities b ON b.id=r.dst WHERE r.src=? OR r.dst=? ORDER BY r.weight DESC LIMIT 300",
      {eid, eid});
  json out = *row;
  out["protected"] = is_protected(*row);
  out["relations"] = rels;
  return out;
}

json KnowledgeGraph::search_nodes(const std::optional<std::string>& sc, const std::string& q, const std::string& category,
                                  const std::string& etype, int limit, int offset) {
  std::vector<std::string> where;
  Params p;
  if (sc) {
    where.push_back("scope=?");
    p.push_back(*sc);
  }
  if (!category.empty()) {
    where.push_back("category=?");
    p.push_back(category);
  }
  if (!etype.empty()) {
    where.push_back("type=?");
    p.push_back(etype);
  }
  std::string qs = trim(q);
  if (!qs.empty()) {
    where.push_back("(key LIKE ? OR name LIKE ? OR note LIKE ?)");
    std::string k = gkey(q);
    p.push_back("%" + (k.empty() ? qs : k) + "%");
    p.push_back("%" + qs + "%");
    p.push_back("%" + qs + "%");
  }
  std::string sql = where.empty() ? "" : " WHERE " + join(where, " AND ");
  json total = db.scalar("SELECT COUNT(*) n FROM entities" + sql, p);
  p.push_back(limit);
  p.push_back(offset);
  auto rows = db.query("SELECT e.*, (SELECT COUNT(*) FROM relations r WHERE r.src=e.id OR r.dst=e.id) AS degree "
                       "FROM entities e" +
                           sql + " ORDER BY weight DESC, id LIMIT ? OFFSET ?",
                       p);
  return {{"total", total}, {"items", rows}};
}

json KnowledgeGraph::update_entity(long long eid, const json& ch) {
  auto row = entity(eid);
  if (!row) throw std::out_of_range(std::to_string(eid));
  auto has = [&](const char* k) { return ch.contains(k) && !ch[k].is_null(); };
  if (has("category")) {
    std::string c = jstr(ch, "category");
    if (!c.empty() && !CATEGORIES.contains(c)) throw std::invalid_argument("categoria desconocida: " + c);
  }
  db.transaction([&] {
    if (has("name") && !trim(jstr(ch, "name")).empty() && gkey(jstr(ch, "name")) != jstr(*row, "key")) {
      std::string name = jstr(ch, "name");
      if (is_protected(*row)) throw std::invalid_argument("los nucleos y el usuario no se renombran aqui");
      auto other = db.one("SELECT id FROM entities WHERE scope=? AND key=?", {(*row)["scope"], gkey(name)});
      if (other) {   // ya existe ese nombre en el mismo ambito: se fusionan
        merge_entities(eid, (*other)["id"].get<long long>());
        eid = (*other)["id"].get<long long>();
      } else {
        db.execute("UPDATE entities SET key=?, name=? WHERE id=?", {gkey(name), name60(name), eid});
      }
    } else if (has("name") && !trim(jstr(ch, "name")).empty()) {
      db.execute("UPDATE entities SET name=? WHERE id=?", {name60(jstr(ch, "name")), eid});
    }
    for (const char* col : {"type", "category", "note"}) {
      if (has(col) && !(std::string(col) == "type" && is_protected(*row)))
        db.execute(std::string("UPDATE entities SET ") + col + "=? WHERE id=?", {uleft(jstr(ch, col), 500), eid});
    }
    if (has("weight")) db.execute("UPDATE entities SET weight=? WHERE id=?", {std::max(0.0, jnum(ch["weight"])), eid});
  });
  auto e = entity(eid);
  return e ? *e : json(nullptr);
}

json KnowledgeGraph::add_entity(const std::string& sc, const std::string& name, const std::string& etype,
                                const std::string& category, const std::string& note, const std::string& source) {
  if (!category.empty() && !CATEGORIES.contains(category))
    throw std::invalid_argument("categoria desconocida: " + category);
  long long eid = for_agent(sc).upsert_entity(name, etype.empty() ? "concepto" : etype, 1.0, category, note, source);
  if (!eid) throw std::invalid_argument("nombre vacio");
  return *entity(eid);
}

std::string KnowledgeGraph::relation_scope(const std::string& a, const std::string& b) {
  if (a == b) return a;
  if (a.empty() || b.empty()) return a.empty() ? b : a;
  throw std::invalid_argument("no se pueden relacionar los subgrafos de dos agentes distintos (estan aislados)");
}

json KnowledgeGraph::add_relation(long long src, long long dst, const std::string& rel_in, double weight) {
  auto a = entity(src), b = entity(dst);
  if (!a || !b || src == dst) throw std::invalid_argument("relacion invalida");
  static const Rx ws(R"(\s+)");
  std::string rel = uleft(ws.sub(lower(trim(rel_in)), "_"), 30);
  if (rel.empty()) rel = "relacionado_con";
  std::string sc = relation_scope(jstr(*a, "scope"), jstr(*b, "scope"));
  db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
             "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
             "last_seen=excluded.last_seen",
             {src, dst, rel, weight, now(), sc});
  auto r = db.one("SELECT * FROM relations WHERE src=? AND dst=? AND rel=?", {src, dst, rel});
  return r ? *r : json(nullptr);
}

void KnowledgeGraph::delete_relation(long long rid) { db.execute("DELETE FROM relations WHERE id=?", {rid}); }

json KnowledgeGraph::merge_entities(long long src, long long dst) {
  auto a = entity(src), b = entity(dst);
  if (!a || !b || src == dst) throw std::invalid_argument("fusion invalida");
  if (is_protected(*a)) throw std::invalid_argument("los nucleos y el usuario no se pueden fusionar en otro nodo");
  db.transaction([&] {
    for (auto& r : db.query("SELECT * FROM relations WHERE src=? OR dst=?", {src, src})) {
      long long rs = r["src"].get<long long>(), rd = r["dst"].get<long long>();
      long long s2 = rs == src ? dst : rs, d2 = rd == src ? dst : rd;
      db.execute("DELETE FROM relations WHERE id=?", {r["id"]});
      if (s2 == d2) continue;
      std::map<long long, std::string> ends;
      for (auto& e : db.query("SELECT id, scope FROM entities WHERE id IN (?,?)", {s2, d2}))
        ends[e["id"].get<long long>()] = jstr(e, "scope");
      if (!ends.count(s2) || !ends.count(d2)) continue;
      std::string sc;
      try {
        sc = relation_scope(ends[s2], ends[d2]);
      } catch (const std::invalid_argument&) {
        continue;
      }
      db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                 "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
                 "last_seen=MAX(last_seen, excluded.last_seen)",
                 {s2, d2, r["rel"], r["weight"], r["last_seen"], sc});
    }
    double fs = jnum((*a)["first_seen"]), ls = jnum((*a)["last_seen"]);
    db.execute("UPDATE entities SET weight=weight+?, mentions=mentions+?, first_seen=MIN(first_seen, ?), "
               "last_seen=MAX(last_seen, ?), note=CASE WHEN note='' THEN ? ELSE note END, "
               "category=CASE WHEN category IN ('', 'comun', 'especialidad') AND ? NOT IN ('', 'comun', 'especialidad') "
               "THEN ? ELSE category END WHERE id=?",
               {(*a)["weight"], (*a)["mentions"], fs ? fs : now(), ls ? ls : now(), (*a)["note"], (*a)["category"],
                (*a)["category"], dst});
    db.execute("DELETE FROM entities WHERE id=?", {src});
  });
  return *entity(dst);
}

json KnowledgeGraph::move_entity(long long eid, std::string sc, const std::string& category) {
  auto row = entity(eid);
  if (!row) throw std::out_of_range(std::to_string(eid));
  if (is_protected(*row)) throw std::invalid_argument("los nucleos y el usuario no se mueven");
  if (!category.empty() && !CATEGORIES.contains(category))
    throw std::invalid_argument("categoria desconocida: " + category);
  if (!sc.empty()) for_agent(sc);   // crea el ancla del agente si aun no existe
  if (jstr(*row, "scope") == sc) return category.empty() ? *row : update_entity(eid, {{"category", category}});
  std::string rc = jstr(*row, "category");
  std::string new_cat = !category.empty() ? category
                        : (GENERAL_CATEGORIES.contains(rc) == sc.empty()) ? rc
                        : (sc.empty() ? "comun" : "especialidad");
  long long target = eid;
  db.transaction([&] {
    auto other = db.one("SELECT id FROM entities WHERE scope=? AND key=?", {sc, (*row)["key"]});
    if (other) {   // mismo concepto ya presente en el destino: se fusiona alli
      db.execute("UPDATE entities SET scope=?, key=?, category=? WHERE id=?",
                 {sc, jstr(*row, "key") + "#" + std::to_string(eid), new_cat, eid});
      target = merge_entities(eid, (*other)["id"].get<long long>())["id"].get<long long>();
    } else {
      db.execute("UPDATE entities SET scope=?, category=? WHERE id=?", {sc, new_cat, eid});
    }
    for (auto& r : db.query("SELECT r.id, a.scope AS sa, b.scope AS sb FROM relations r JOIN entities a ON "
                            "a.id=r.src JOIN entities b ON b.id=r.dst WHERE r.src=? OR r.dst=?",
                            {target, target})) {
      try {
        db.execute("UPDATE relations SET scope=? WHERE id=?", {relation_scope(jstr(r, "sa"), jstr(r, "sb")), r["id"]});
      } catch (const std::invalid_argument&) {
        db.execute("DELETE FROM relations WHERE id=?", {r["id"]});
      }
    }
  });
  return *entity(target);
}

json KnowledgeGraph::categories_by_scope() {
  json out = json::object();
  for (auto& r : db.query("SELECT scope, category, COUNT(*) n FROM entities WHERE type NOT IN ('self', 'agente') "
                          "AND NOT (scope!='' AND key=?) GROUP BY scope, category",
                          {gkey(USER)})) {
    std::string sc = jstr(r, "scope"), c = jstr(r, "category");
    out[sc.empty() ? "general" : sc][c.empty() ? "sin_categoria" : c] = r["n"];
  }
  return out;
}

int KnowledgeGraph::prune(double max_age_days, double min_weight) {
  double cutoff = now() - max_age_days * 86400;
  int n = 0;
  db.transaction([&] {
    n = db.execute("DELETE FROM relations WHERE scope=? AND rel IN ('menciona', 'relacionado_con') AND weight<? AND "
                   "last_seen<?",
                   {scope, min_weight, cutoff});
    n += db.execute(
        "DELETE FROM entities WHERE scope=? AND type='concepto' AND mentions<=2 AND last_seen<? AND NOT EXISTS "
        "(SELECT 1 FROM relations r WHERE r.src=entities.id OR r.dst=entities.id)",
        {scope, cutoff});
  });
  return n;
}

}  // namespace nx::mem
