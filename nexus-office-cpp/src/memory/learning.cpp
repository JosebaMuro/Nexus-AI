// Aprendizaje continuo (memory/learning.py): hechos, pautas, retroalimentacion y consolidacion.
#include "memory/memory.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("learning");
static const std::map<std::string, std::string> REL_TEXT = {
    {"se_llama", "El usuario se llama {o}"},       {"le_gusta", "Al usuario le gusta {o}"},
    {"no_le_gusta", "Al usuario no le gusta {o}"}, {"vive_en", "El usuario vive en {o}"},
    {"trabaja_en", "El usuario trabaja en {o}"},   {"es", "El usuario es {o}"},
    {"tiene", "El usuario tiene {o}"},             {"aprende", "El usuario esta aprendiendo {o}"}};

Learner::Learner(DB& d, MemoryStore& m, KnowledgeGraph& g, Organizer& o) : db(d), memory(m), graph(g), organizer(o) {}

json Learner::on_user_message(const std::string& text, const std::string&, const std::string& agent, bool llm_extraction) {
  static const Rx EXPLICIT(R"(^\s*(?:recuerda(?: que)?|apunta(?: que)?|no olvides(?: que)?|remember(?: that)?)\s*[:,]?\s*(.+))",
                           Rx::I | Rx::S);
  json learned = json::array();
  if (auto m = EXPLICIT.match(text)) {
    std::string what = trim(m->str(1));
    memory.remember(what, "fact", 0.9, "explicito", "");
    learned.push_back({{"s", "Usuario"}, {"r", "pidio_recordar"}, {"o", uleft(what, 80)}, {"type", "nota"}});
  }
  for (auto& t : learn_text(text, agent, llm_extraction)) learned.push_back(t);
  return learned;
}

json Learner::learn_text(const std::string& text, const std::string& agent, bool llm_extraction) {
  json triples = json::array();
  std::string topics = Organizer::strip_behaviors(text);
  auto [scope, category] = organizer.route(topics, agent);
  KnowledgeGraph& dest = graph.for_agent(scope);
  db.transaction([&] {
    for (auto& t : graph.general().extract_personal(text)) triples.push_back(t);   // lo del usuario: para todos
    for (auto& t : organizer.extract_general(text)) triples.push_back(t);          // pautas y relaciones: idem
    for (auto& t : dest.extract_topics(topics, "user", category)) triples.push_back(t);
  });
  if (llm_extraction && llm_complete)
    for (auto& t : dest.extract_with_llm(text, llm_complete)) triples.push_back(t);
  for (auto& t : triples) {
    std::string r = jstr(t, "r"), o = jstr(t, "o");
    auto it = REL_TEXT.find(r);
    if (it != REL_TEXT.end()) {
      std::string kind = contains(r, "gusta") ? "preference" : "fact";
      memory.remember(replace_all(it->second, "{o}", o), kind, 0.8, "conversacion");
      if (r == "se_llama") db.set_profile("user_name", o);
    } else if (jstr(t, "category") == "comportamiento") {
      memory.remember(o, "behavior", 0.85, "conversacion");
    } else if (jstr(t, "category") == "relacion") {
      memory.remember(o + " es " + replace_all(r.size() > 3 ? r.substr(3) : "", "_", " ") + " del usuario", "fact", 0.75,
                      "conversacion");
    }
  }
  return triples;
}

void Learner::on_exchange(const std::string& session_id, const std::string& user, const std::string& assistant,
                          const std::string& agent, int consolidate_every) {
  auto [scope, category] = organizer.route(uleft(user, 600) + "\n" + uleft(assistant, 1500), agent);
  graph.for_agent(scope).extract_topics(uleft(assistant, 1500), "assistant", category);
  if (!agent.empty()) {
    try {   // interaccion usuario <-> agente: conocimiento comun
      std::string n = namer(agent);
      organizer.record_interaction(USER, "conversa_con", n.empty() ? agent : n);
    } catch (const std::exception& e) {
      L.warning("No se pudo anotar la interaccion: ", e.what());
    }
  }
  int n;
  {
    std::lock_guard<std::mutex> lk(mu_);
    n = ++since_[session_id];
  }
  if (n >= consolidate_every) {
    consolidate(session_id);
    try {
      graph.for_agent(agent).prune();
      graph.general().prune();
    } catch (const std::exception& e) {
      L.warning("Poda del grafo fallida: ", e.what());
    }
    std::lock_guard<std::mutex> lk(mu_);
    since_[session_id] = 0;
  }
}

json Learner::feedback(long long message_id, int score_in, const std::string& comment) {
  int score = score_in > 0 ? 1 : -1;
  db.execute("INSERT INTO feedback(message_id, score, comment, ts) VALUES(?,?,?,?)", {message_id, score, comment, now()});
  auto msg = db.one("SELECT * FROM messages WHERE id=?", {message_id});
  if (!msg) return {{"ok", false}};
  auto prev = db.one("SELECT content FROM messages WHERE session_id=? AND id<? AND role='user' ORDER BY id DESC LIMIT 1",
                     {(*msg)["session_id"], message_id});
  std::string question = prev ? jstr(*prev, "content") : "";
  json meta = jparse(jstr(*msg, "meta").empty() ? "{}" : jstr(*msg, "meta"));
  if (meta.is_object())
    for (auto& mid : meta.value("memories", json::array()))
      if (mid.is_number_integer()) memory.adjust(mid.get<long long>(), 0.1 * score);
  std::string content = jstr(*msg, "content"), agent = jstr(*msg, "agent");
  if (score > 0 && !question.empty()) {
    memory.remember("P: " + uleft(question, 400) + "\nR: " + uleft(content, 900), "example", 0.8, "feedback+", agent);
  } else if (score < 0) {
    std::string lesson = "Una respuesta a '" + uleft(question, 120) + "' no gusto al usuario";
    if (!comment.empty()) lesson += ": " + uleft(comment, 200);
    memory.remember(lesson, "lesson", 0.85, "feedback-", agent);
  }
  // preferencia de longitud: positivo en respuestas largas => le gustan largas, y viceversa
  bool long_answer = ulen(content) > 700;
  json prof = db.get_profile();
  double bias = jnum(prof.value("length_bias", json(0.0)));
  bias += 0.15 * score * (long_answer ? 1 : -1);
  db.set_profile("length_bias", std::max(-1.0, std::min(1.0, round_to(bias, 2))));
  return {{"ok", true}, {"length_bias", bias}};
}

std::string Learner::consolidate(const std::string& session_id,
                                 const std::function<std::string(const std::string&)>& summarize) {
  auto msgs = memory.history(session_id, 30);
  if (msgs.size() < 4) return "";
  std::vector<std::string> lines;
  for (auto& m : msgs) lines.push_back(jstr(m, "role") + ": " + uleft(jstr(m, "content"), 400));
  std::string convo = join(lines, "\n");
  std::string summary;
  if (summarize) {
    try {
      summary = summarize("Resume en 3 frases los temas y datos importantes de esta conversacion:\n" + convo);
    } catch (const std::exception& e) {
      L.warning("Resumen con LLM fallido: ", e.what());
    }
  }
  if (summary.empty()) {   // resumen extractivo
    std::vector<std::string> user_texts, firsts;
    for (auto& m : msgs)
      if (jstr(m, "role") == "user") {
        user_texts.push_back(jstr(m, "content"));
        if (firsts.size() < 3) firsts.push_back(uleft(jstr(m, "content"), 120));
      }
    auto topics = KnowledgeGraph::keywords(join(user_texts, " "), 6);
    summary = "Conversacion sobre: " + join(topics, ", ") + ". Peticiones: " + join(firsts, " | ");
  }
  memory.remember(summary, "summary", 0.6, "sesion:" + session_id);
  memory.decay();
  return summary;
}

std::vector<std::string> Learner::behaviors(int limit) {
  std::vector<std::string> out;
  for (auto& r : db.query("SELECT content FROM memories WHERE kind='behavior' AND agent='' ORDER BY importance DESC, "
                          "created DESC LIMIT ?",
                          {limit}))
    out.push_back(jstr(r, "content"));
  return out;
}

json Learner::build_context(const std::string& query, const std::string& agent, int k) {
  std::optional<std::string> ag = agent.empty() ? std::nullopt : std::optional<std::string>(agent);
  auto mems = memory.recall(query, k, {"fact", "preference", "summary", "note", "episode", "skill"}, ag);
  auto examples = memory.recall(query, 2, {"example"}, ag, 0.3);
  auto lessons = memory.recall(query, 2, {"lesson"}, std::nullopt, 0.25);
  json prof = db.get_profile();
  auto names = KnowledgeGraph::keywords(query, 4);
  KnowledgeGraph& general = graph.general();
  KnowledgeGraph* own = agent.empty() ? nullptr : &graph.for_agent(agent);
  std::vector<std::string> facts = general.user_facts(10);
  auto add = [&](const std::vector<std::string>& v) { facts.insert(facts.end(), v.begin(), v.end()); };
  if (own) {   // primero lo que el agente ha aprendido por su cuenta, despues lo comun
    auto n2 = names;
    n2.push_back(own->self_name);
    add(own->facts_about(n2, 6));
  }
  add(general.facts_about(names, 6));
  if (own) add(own->notes_about(names, 3));
  add(general.notes_about(names, 3));
  json uniq = json::array();
  std::set<std::string> seen;
  for (auto& f : facts)
    if (seen.insert(f).second) uniq.push_back(f);
  auto toks = tokens(query);
  if (toks.size() > 12) toks.resize(12);
  return {{"memories", mems}, {"examples", examples}, {"lessons", lessons}, {"facts", uniq},
          {"profile", prof},  {"query_terms", toks},   {"behaviors", behaviors()}};
}

}  // namespace nx::mem
