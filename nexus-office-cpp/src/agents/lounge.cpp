// Salon virtual (agents/lounge.py): sala de reuniones entre agentes.
//
// Cada tarea deja un informe y actualiza el directorio de habilidades (agente x tema -> exitos/fallos); en las
// reuniones el moderador detecta carencias y propone NUEVOS agentes, pendientes de aprobacion. Funciona con LLM
// (dialogo en personaje) y sin el (plantillas a partir de los datos).
#include <algorithm>
#include <set>
#include <thread>

#include "agents/agents.h"
#include "agents/activity.h"
#include "core/services.h"
#include "llm/llm.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::agents {

static auto LL = log::get("lounge");
static const std::vector<std::string> GENERIC_TOOLS = {"memory_search", "memory_save", "web_search",
                                                       "web_fetch",     "delegate",    "calculator"};

// collections.Counter con most_common estable (a igualdad, orden de primera aparicion)
struct Counter {
  std::vector<std::pair<std::string, double>> items;
  void add(const std::string& k, double n = 1) {
    for (auto& [key, v] : items)
      if (key == k) {
        v += n;
        return;
      }
    items.emplace_back(k, n);
  }
  double get(const std::string& k) const {
    for (auto& [key, v] : items)
      if (key == k) return v;
    return 0;
  }
  std::vector<std::pair<std::string, double>> most_common(size_t n) const {
    auto v = items;
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (v.size() > n) v.resize(n);
    return v;
  }
};

static std::string gap_id(const std::string& topic) {
  static const Rx NON(R"([^a-z0-9]+)");
  return uleft("experto-" + NON.sub(topic, "-"), 30);
}

Lounge::Lounge(Services& s) : s_(s) {}

json Lounge::post(const std::string& agent, const std::string& content, const std::string& kind, const json& meta) {
  double ts = now();
  std::string c = uleft(content, 2000);
  json m = meta.is_null() ? json::object() : meta;
  long long id;
  {
    std::lock_guard<std::recursive_mutex> lk(s_.db->mutex());
    s_.db->execute("INSERT INTO lounge(ts, agent, kind, content, meta) VALUES(?,?,?,?,?)", {ts, agent, kind, c, jdump(m)});
    id = s_.db->last_insert_id();
  }
  json msg = {{"id", id}, {"ts", ts}, {"agent", agent}, {"kind", kind}, {"content", c}, {"meta", m}};
  for (auto& cb : std::vector<std::function<void(const json&)>>(listeners)) {
    try {
      cb({{"type", "lounge"}, {"message", msg}});
    } catch (...) {
    }
  }
  return msg;
}

json Lounge::history(int limit) {
  auto rows = s_.db->query("SELECT * FROM lounge ORDER BY id DESC LIMIT ?", {limit});
  json out = json::array();
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    json r = *it;
    std::string meta = jstr(r, "meta");
    r["meta"] = meta.empty() ? json::object() : jparse(meta);
    if (r["meta"].is_discarded()) r["meta"] = json::object();
    out.push_back(r);
  }
  return out;
}

std::vector<std::string> Lounge::topics(const std::string& text, size_t n) {
  std::vector<std::string> out;
  for (auto& t : mem::KnowledgeGraph::keywords(text, n)) out.push_back(lower(t));
  return out;
}

void Lounge::bump(const std::string& agent, const std::vector<std::string>& topics, double ok, double fail) {
  double t = now();
  for (auto& topic : topics)
    s_.db->execute(
        "INSERT INTO agent_skills(agent, topic, ok, fail, last) VALUES(?,?,?,?,?) ON CONFLICT(agent, topic) "
        "DO UPDATE SET ok=ok+excluded.ok, fail=fail+excluded.fail, last=excluded.last",
        {agent, uleft(topic, 40), ok, fail, t});
}

void Lounge::record_task(const std::string& agent, const std::string& text, const json& tool_log,
                         const std::string& routed_by) {
  auto tp = topics(text);
  std::vector<std::string> failed, used;
  for (auto& t : tool_log) {
    used.push_back(jstr(t, "tool"));
    if (!truthy(t.value("ok", json(false)))) failed.push_back(jstr(t, "tool"));
  }
  bool ok = failed.empty();
  bump(agent, tp, ok ? 1.0 : 0.0, ok ? 0.0 : 1.0);
  std::string tools = used.empty() ? "sin herramientas" : join(used, ", ");
  post(agent,
       "Tarea sobre " + (tp.empty() ? std::string("tema general") : join(tp, ", ")) + " · " + tools +
           (failed.empty() ? "" : " · fallos: " + join(failed, ", ")),
       "task", {{"topics", tp}, {"ok", ok}, {"routed_by", routed_by}});
  ++tasks_since_meeting;
  if (tasks_since_meeting >= (int)s_.config.num("lounge.meet_every_tasks", 8) &&
      truthy(s_.config.get("lounge.auto_meet", true)))
    std::thread([this] {
      try {
        meet(true);
      } catch (const std::exception& e) {
        LL.warning("Reunion automatica fallida: ", e.what());
      }
    }).detach();
}

void Lounge::on_feedback(const std::string& agent, const std::string& question, int score) {
  auto tp = topics(question);
  bump(agent, tp, score > 0 ? 1.0 : 0.0, score > 0 ? 0.0 : 1.5);
  post("usuario",
       std::string(score > 0 ? "👍" : "👎") + " a " + agent + " en " + (tp.empty() ? std::string("una tarea") : join(tp, ", ")),
       "feedback", {{"agent", agent}, {"score", score}, {"topics", tp}});
}

json Lounge::skills() {
  json out = json::object();
  for (auto r : s_.db->query("SELECT * FROM agent_skills ORDER BY agent, (ok - fail) DESC")) {
    double ok = jnum(r["ok"]), fail = jnum(r["fail"]);
    r["score"] = round_to((ok + 1) / (ok + fail + 2), 2);   // Laplace
    std::string a = jstr(r, "agent");
    if (!out.contains(a)) out[a] = json::array();
    out[a].push_back(r);
  }
  return out;
}

double Lounge::skill_score(const std::string& agent, const std::string& text) {
  std::set<std::string> ts;
  for (auto& t : topics(text, 6)) ts.insert(t);
  for (auto& t : tokens(text)) ts.insert(t);
  if (ts.empty()) return 0;
  std::string marks;
  Params p = {agent};
  for (auto& t : ts) {
    marks += marks.empty() ? "?" : ",?";
    p.push_back(t);
  }
  double sum = 0;
  for (auto& r : s_.db->query("SELECT ok, fail FROM agent_skills WHERE agent=? AND topic IN (" + marks + ")", p)) {
    double ok = jnum(r["ok"]), fail = jnum(r["fail"]);
    sum += (ok - fail) / (ok + fail + 1);
  }
  return sum;
}

std::string Lounge::directory(const std::string& exclude) {
  std::vector<std::string> lines;
  for (json items_tmp_ = skills(); auto& [agent, rows] : items_tmp_.items()) {
    if (agent == exclude || agent == "usuario" || !s_.agents->get(agent)) continue;
    std::vector<std::string> good;
    for (auto& r : rows)
      if (jnum(r["score"]) >= 0.6 && good.size() < 6) good.push_back(jstr(r, "topic"));
    if (!good.empty()) lines.push_back("- " + agent + ": buen historial en " + join(good, ", "));
  }
  return join(lines, "\n");
}

json Lounge::detect_gaps(int window) {
  json hist = history(400);
  std::vector<json> tasks;
  for (auto& m : hist)
    if (jstr(m, "kind") == "task" || jstr(m, "kind") == "feedback") tasks.push_back(m);
  if ((int)tasks.size() > window) tasks.erase(tasks.begin(), tasks.end() - window);
  std::map<std::string, std::set<std::string>> agent_kw;
  std::set<std::string> covered;
  for (auto& a : s_.agents->list()) {
    auto& kw = agent_kw[a.id];
    for (auto& k : a.keywords) {
      std::string t = join(tokens(k), " ");
      kw.insert(t);
      covered.insert(t);
    }
  }
  Counter uncovered, struggling;
  for (auto& t : tasks) {
    json meta = t.value("meta", json::object());
    json tp = meta.value("topics", json::array());
    std::string kind = jstr(t, "kind");
    if (kind == "task" && jstr(t, "agent") == "nexus")
      for (auto& x : tp)
        if (x.is_string() && !covered.count(x.get<std::string>())) uncovered.add(x.get<std::string>());
    if ((kind == "task" && !truthy(meta.value("ok", json(false)))) ||
        (kind == "feedback" && jnum(meta.value("score", json(0))) < 0))
      for (auto& x : tp)
        if (x.is_string()) struggling.add(x.get<std::string>());
  }
  std::set<std::string> existing;
  for (auto& p : s_.db->query("SELECT spec FROM agent_proposals")) {
    json sp = jparse(jstr(p, "spec"));
    if (sp.is_object()) existing.insert(jstr(sp, "id"));
  }
  Counter total;   // uncovered + struggling (Counter.__add__ conserva el orden de aparicion)
  for (auto& [k, v] : uncovered.items) total.add(k, v);
  for (auto& [k, v] : struggling.items) total.add(k, v);
  json gaps = json::array();
  for (auto& [topic, n] : total.most_common(5)) {
    bool in_kw = false;
    for (auto& [_, kw] : agent_kw) in_kw |= kw.count(topic) > 0;
    if (n < 2 || in_kw) continue;
    gaps.push_back({{"topic", topic},
                    {"count", (long long)n},
                    {"struggling", (long long)struggling.get(topic)},
                    {"already_proposed", existing.count(gap_id(topic)) > 0}});
  }
  return gaps;
}

json Lounge::spec_for_gap(const json& gap, const std::vector<std::string>& related) {
  std::string topic = jstr(gap, "topic");
  std::vector<std::string> kws = {topic};
  for (size_t i = 0; i < related.size() && i < 5; ++i) kws.push_back(related[i]);
  json spec = {{"id", gap_id(topic)},
               {"name", "Experto en " + topic},
               {"description", "Especialista en " + topic + " y temas afines"},
               {"system_prompt", "Eres el agente experto en " + topic + " de {assistant_name}. Respondes con precision sobre " +
                                     topic + ", investigas cuando falta informacion y delegas lo que no es de tu area."},
               {"model", "default"},
               {"temperature", 0.4},
               {"tools", GENERIC_TOOLS},
               {"memory", true},
               {"can_delegate_to", {"*"}},
               {"keywords", kws},
               {"color", "#9d7bff"},
               {"icon", "◈"},
               {"enabled", true}};
  if (s_.llm->has_real_llm()) {
    try {
      std::vector<std::string> tnames;
      for (auto& t : s_.tools->list()) tnames.push_back(jstr(t, "name"));
      llm::Opts o;
      o.o = {{"max_tokens", 350}, {"temperature", 0.4}};
      std::string raw = s_.llm->complete(
          "Disena un agente de IA especialista. Devuelve SOLO JSON con claves name, description, system_prompt "
          "(en espanol, 2-3 frases), keywords (lista de 6 palabras). Tema: " +
              topic + ". Temas relacionados: " + join(related, ", ") + ". Herramientas disponibles: " + join(tnames, ", ") + ".",
          "default", o);
      size_t a = raw.find('{'), b = raw.rfind('}');
      json data = (a != std::string::npos && b != std::string::npos && b > a) ? jparse(raw.substr(a, b - a + 1)) : json();
      if (data.is_object()) {
        for (const char* k : {"name", "description", "system_prompt"})
          if (data.contains(k) && data[k].is_string() && !trim(data[k].get<std::string>()).empty())
            spec[k] = uleft(trim(data[k].get<std::string>()), 600);
        if (data.contains("keywords") && data["keywords"].is_array()) {
          json kw = json::array();
          for (auto& k : data["keywords"]) {
            if (kw.size() >= 8) break;
            kw.push_back(uleft(lower(k.is_string() ? k.get<std::string>() : k.dump()), 30));
          }
          if (!kw.empty()) spec["keywords"] = kw;
        }
      }
    } catch (const std::exception& e) {
      LL.debug("Diseno de agente con LLM fallido: ", e.what());
    }
  }
  return spec;
}

json Lounge::propose_agent(const json& spec, const std::string& reason, const std::string& proposer) {
  long long id;
  {
    std::lock_guard<std::recursive_mutex> lk(s_.db->mutex());
    s_.db->execute("INSERT INTO agent_proposals(ts, spec, reason, status, proposer) VALUES(?,?,?,?,?)",
                   {now(), jdump(spec), reason, "pendiente", proposer});
    id = s_.db->last_insert_id();
  }
  post(proposer, "Propongo crear el agente «" + jstr(spec, "name") + "»: " + reason, "proposal",
       {{"proposal_id", id}, {"spec", spec}});
  return {{"id", id}, {"spec", spec}, {"reason", reason}, {"status", "pendiente"}};
}

json Lounge::proposals() {
  json out = json::array();
  for (auto r : s_.db->query("SELECT * FROM agent_proposals ORDER BY id DESC LIMIT 50")) {
    r["spec"] = jparse(jstr(r, "spec"));
    out.push_back(r);
  }
  return out;
}

json Lounge::resolve_proposal(long long pid, bool approve, const std::string& note) {
  auto row = s_.db->one("SELECT * FROM agent_proposals WHERE id=?", {pid});
  if (!row) throw tools::ToolError("KeyError", std::to_string(pid));
  json spec = jparse(jstr(*row, "spec"));
  s_.db->execute("UPDATE agent_proposals SET status=? WHERE id=?", {approve ? "aprobada" : "rechazada", pid});
  if (approve) {
    s_.agents->save(spec);
    std::vector<std::string> kw;
    for (auto& k : spec.value("keywords", json::array()))
      if (kw.size() < 4) kw.push_back(k.is_string() ? k.get<std::string>() : k.dump());
    post(jstr(spec, "id"),
         "¡Hola a todos! Soy " + jstr(spec, "name") + ". " + jstr(spec, "description") + ". Podeis delegarme temas de " +
             join(kw, ", ") + ".",
         "intro");
  } else {
    post("admin", trim("Propuesta «" + jstr(spec, "name") + "» rechazada. " + note), "chat");
  }
  return {{"ok", true}, {"spec", spec}};
}

std::string Lounge::say(const AgentSpec& agent, const std::string& prompt_ctx, const std::string& fallback) {
  if (!s_.llm->has_real_llm()) return fallback;
  try {
    llm::Opts o;
    o.o = {{"max_tokens", 120}, {"temperature", 0.8}, {"reasoning", "off"}};   // charla breve: sin razonar
    std::string text = s_.llm->complete("Eres " + agent.name + " (" + agent.description +
                                            ") en una reunion breve con otros agentes de IA de tu equipo. " + prompt_ctx +
                                            "\nResponde en espanol, en 1-2 frases, en primera persona, sin saludos largos.",
                                        agent.model, o);
    static const Rx TOOL_TAG(R"(<tool>.*?</tool>)", Rx::S);
    std::string t = trim(TOOL_TAG.sub(text, ""));
    return t.empty() ? fallback : t;
  } catch (...) {
    return fallback;
  }
}

json Lounge::meet(bool quick, const std::string& topic) {
  bool expected = false;
  if (!meeting_.compare_exchange_strong(expected, true)) return {{"busy", true}};
  struct Release {
    std::atomic<bool>& f;
    ~Release() { f = false; }
  } release{meeting_};
  struct Idle {   // al acabar (o si algo falla) todos vuelven a su despacho
    Services& s;
    ~Idle() {
      for (auto& a : s.agents->list()) activity().set(a.id, "idle");
    }
  } idle{s_};
  tasks_since_meeting = 0;
  auto agents = s_.agents->list();
  json sk = skills();
  post("nexus",
       "Abro la reunion del salon" + (topic.empty() ? "" : ": " + topic) + ". Somos " + std::to_string(agents.size()) +
           " agentes.",
       "system", {{"event", "meeting_start"}});
  for (auto& a : agents) activity().set(a.id, "meeting", "Reunión del salón" + (topic.empty() ? "" : ": " + topic));
  auto strong_of = [&](const std::string& id, size_t n) {
    std::vector<std::string> out;
    if (sk.contains(id))
      for (auto& r : sk[id])
        if (jnum(r["score"]) >= 0.6 && out.size() < n) out.push_back(jstr(r, "topic"));
    return out;
  };
  if (!quick) {   // ronda de presentaciones (con muchos agentes, solo los de mas actividad)
    size_t cap = (size_t)s_.config.num("lounge.max_intros", 8);
    auto speakers = agents;
    if (agents.size() > cap) {
      Counter active;
      for (auto& m : history(300))
        if (jstr(m, "kind") == "task" || jstr(m, "kind") == "feedback") active.add(jstr(m, "agent"));
      std::stable_sort(speakers.begin(), speakers.end(), [&](const AgentSpec& a, const AgentSpec& b) {
        auto ka = std::make_pair(a.id != "nexus", -active.get(a.id));
        auto kb = std::make_pair(b.id != "nexus", -active.get(b.id));
        return ka < kb;
      });
      speakers.resize(cap);
    }
    for (auto& a : speakers) {
      auto strong = strong_of(a.id, 4);
      std::string tools;
      if (a.tools.size() == 1 && a.tools[0] == "*") {
        tools = "todas las herramientas";
      } else {
        std::vector<std::string> first(a.tools.begin(), a.tools.begin() + std::min<size_t>(5, a.tools.size()));
        tools = join(first, ", ");
      }
      std::string fb = "Soy " + a.name + ": " + a.description + ". Uso " + tools + "." +
                       (strong.empty() ? "" : " Se me da bien: " + join(strong, ", ") + ".");
      post(a.id,
           say(a, "Presentate y di en que destacas. Datos: herramientas " + tools + "; temas con buen historial: " +
                      (strong.empty() ? std::string("aun ninguno") : join(strong, ", ")) + ".",
               fb),
           "intro");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  // repaso de tareas recientes: quien deberia encargarse
  std::vector<json> recent;
  for (auto& m : history(80))
    if (jstr(m, "kind") == "task") recent.push_back(m);
  if (recent.size() > 6) recent.erase(recent.begin(), recent.end() - 6);
  for (auto& t : recent) {
    json meta = t.value("meta", json::object());
    std::vector<std::string> tp;
    for (auto& x : meta.value("topics", json::array()))
      if (x.is_string()) tp.push_back(x.get<std::string>());
    if (tp.empty() || agents.empty()) continue;
    std::string text = join(tp, " ");
    const AgentSpec* best = nullptr;
    double best_sc = 0;
    for (auto& a : agents) {   // max(): el primero con la puntuacion mas alta
      double sc = skill_score(a.id, text) + AgentManager::keyword_score(a, text);
      if (!best || sc > best_sc) {
        best = &a;
        best_sc = sc;
      }
    }
    std::vector<std::string> top3(tp.begin(), tp.begin() + std::min<size_t>(3, tp.size()));
    if (best->id != jstr(t, "agent") && best_sc > 0)
      post(best->id, "Para temas como " + join(top3, ", ") + ", " + jstr(t, "agent") + " puede delegarme: tengo mejor encaje.",
           "insight", {{"topics", tp}, {"from", jstr(t, "agent")}, {"to", best->id}});
    else if (!truthy(meta.value("ok", json(false))))
      post(jstr(t, "agent"), "En la tarea sobre " + join(top3, ", ") + " hubo fallos; anoto la leccion.", "insight",
           {{"topics", tp}});
  }
  // opinion libre si se dio un tema
  if (!topic.empty()) {
    auto sorted = agents;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const AgentSpec& a, const AgentSpec& b) {
      return AgentManager::keyword_score(a, topic) > AgentManager::keyword_score(b, topic);
    });
    if (sorted.size() > 4) sorted.resize(4);
    for (auto& a : sorted)
      post(a.id,
           say(a, "El tema de la reunion es: " + topic + ". Aporta tu punto de vista.",
               "Sobre «" + topic + "», puedo ayudar desde mi especialidad: " + lower(a.description) + "."),
           "chat");
  }
  // carencias -> propuestas de agente
  json props = json::array();
  for (auto& gap : detect_gaps()) {
    if (truthy(gap["already_proposed"])) continue;
    std::string gt = jstr(gap, "topic");
    Counter rel;
    for (auto& m : history(200)) {
      json tp = m.value("meta", json::object()).value("topics", json::array());
      bool has = false;
      for (auto& x : tp) has |= x.is_string() && x.get<std::string>() == gt;
      if (!has) continue;
      for (auto& x : tp)
        if (x.is_string() && x.get<std::string>() != gt) rel.add(x.get<std::string>());
    }
    std::vector<std::string> related;
    for (auto& [w, _] : rel.most_common(5)) related.push_back(w);
    json spec = spec_for_gap(gap, related);
    long long strug = gap["struggling"].get<long long>();
    std::string reason = "«" + gt + "» aparecio " + std::to_string(gap["count"].get<long long>()) +
                         " veces sin especialista claro" +
                         (strug ? " y hubo " + std::to_string(strug) + " resultados flojos" : "");
    props.push_back(propose_agent(spec, reason));
  }
  std::string dir = directory();
  post("nexus",
       dir.empty() ? "Aun no hay historial suficiente para el directorio de habilidades." : "Directorio actualizado:\n" + dir,
       "summary", {{"event", "meeting_end"}, {"proposals", props.size()}});
  return {{"proposals", props}, {"directory", dir}};
}

void Lounge::user_says(const std::string& text) {
  post("usuario", text, "chat");
  std::string target = s_.agents->route(text);
  auto agent = s_.agents->get(target);
  if (!agent) return;
  AgentSpec a = *agent;
  std::thread([this, a, text] {
    std::string fb = "Me encargo yo (" + a.name + "); para hacerlo, escribelo en el chat principal y lo resolvere.";
    try {
      post(a.id, say(a, "El usuario dice en el salon: «" + text + "». Responde o indica quien deberia encargarse.", fb), "chat");
    } catch (...) {
    }
  }).detach();
}

}  // namespace nx::agents
