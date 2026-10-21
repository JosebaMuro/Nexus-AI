// Sistema de agentes configurable (agents/manager.py).
//
// Cada agente es un YAML en data/agents (se siembran los de nexus/agents/defaults en el primer arranque). Campos: id,
// name, description, system_prompt, model ("default" o "backend:modelo"), temperature, max_tokens, tools (lista o
// ["*"]), memory, can_delegate_to, keywords, specialty, color, icon, enabled.
#include "agents/agents.h"

#include <set>

#include "core/services.h"
#include "llm/llm.h"
#include "memory/memory.h"
#include "rag/rag.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"
#include "util/yaml.h"

namespace nx::agents {

static auto L = log::get("agents");

static const std::map<std::string, std::string> STYLE = {
    {"breve", "Responde de forma breve y directa (1-3 frases salvo que se pida mas)."},
    {"equilibrado", "Responde con claridad y la extension justa."},
    {"detallado", "Responde de forma detallada, con ejemplos y estructura."}};
static const std::map<std::string, std::string> LANG = {
    {"es", "espanol"}, {"en", "ingles"},     {"ca", "catalan"}, {"eu", "euskera"},  {"gl", "gallego"},
    {"fr", "frances"}, {"pt", "portugues"}, {"de", "aleman"},  {"it", "italiano"}};

fs::path defaults_dir() { return paths::package() / "agents" / "defaults"; }

std::string yaml_dump_spec(const AgentSpec& spec) { return yaml::dump(spec.to_json()); }

// formato de imagen por su cabecera (lo que PIL reconoce en Image.open)
static std::string image_ext(const std::string& d) {
  if (d.size() >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF) return "jpg";
  if (d.size() >= 8 && d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "png";
  if (d.size() >= 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0) return "webp";
  if (d.size() >= 6 && (d.compare(0, 6, "GIF87a") == 0 || d.compare(0, 6, "GIF89a") == 0)) return "gif";
  if (d.size() >= 2 && d.compare(0, 2, "BM") == 0) return "bmp";
  if (d.size() >= 4 && (d.compare(0, 4, "II*\0", 4) == 0 || d.compare(0, 4, "MM\0*", 4) == 0)) return "png";   // TIFF
  return "";
}

std::vector<std::string> store_images(const std::vector<std::string>& images) {
  std::vector<std::string> out;
  fs::path folder = paths::sub({"sandbox", "adjuntos"});
  for (size_t i = 0; i < images.size(); ++i) {
    const std::string& b64 = images[i];
    size_t comma = b64.find(',');
    std::string data = b64decode(comma == std::string::npos ? b64 : b64.substr(comma + 1));
    std::string ext = image_ext(data);
    if (ext.empty()) {
      L.warning("No se pudo guardar la imagen adjunta ", i + 1, ": formato de imagen no reconocido");
      continue;
    }
    std::string name = strftime_local("%Y%m%d-%H%M%S") + "_" + std::to_string(i + 1) + "." + ext;
    if (!write_file(folder / P(name), data)) {
      L.warning("No se pudo guardar la imagen adjunta ", i + 1, ": error de escritura");
      continue;
    }
    out.push_back("adjuntos/" + name);
  }
  return out;
}

// ======================================================================== AgentManager
AgentManager::AgentManager(Services& s) : dir(paths::agents_dir()), s_(s) {
  seed();
  reload();
}

void AgentManager::seed() {
  for (auto& f : glob(defaults_dir(), "*.yaml")) {
    fs::path dst = dir / f.filename();
    if (!path_exists(dst)) {
      std::error_code ec;
      fs::copy_file(f, dst, ec);
    }
  }
}

void AgentManager::reload() {
  std::map<std::string, AgentSpec> agents;
  auto files = glob(dir, "*.yaml");
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    try {
      AgentSpec spec = AgentSpec::from_json(yaml::load(read_text(f)));
      fs::path def = defaults_dir() / f.filename();
      if (spec.specialty.empty() && path_exists(def)) {   // agentes sembrados antes de la 1.6
        json d = yaml::load_file(def);
        spec.specialty = d.is_object() ? jstr(d, "specialty") : "";
      }
      agents[spec.id] = spec;
    } catch (const std::exception& e) {
      L.error("Agente invalido ", S(f.filename()), ": ", e.what());
    }
  }
  std::lock_guard<std::recursive_mutex> lk(mu_);
  agents_ = std::move(agents);
}

AgentSpec AgentManager::save(json data) {
  static const Rx BAD(R"([^a-z0-9_\-])");
  std::string raw = data.contains("id") ? (data["id"].is_string() ? data["id"].get<std::string>() : data["id"].dump()) : "";
  std::string id = uleft(BAD.sub(lower(raw), ""), 30);
  if (id.empty()) throw std::invalid_argument("id invalido");
  data["id"] = id;
  AgentSpec spec = AgentSpec::from_json(data);
  write_file(dir / P(spec.id + ".yaml"), yaml_dump_spec(spec));
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    agents_[spec.id] = spec;
  }
  if (s_.graph) s_.graph->forget_view(spec.id);   // si cambia el nombre, el ancla de su subgrafo lo sigue
  return spec;
}

void AgentManager::remove(const std::string& aid) {
  if (aid == "nexus") throw std::invalid_argument("El agente principal no se puede borrar");
  remove_file(dir / P(aid + ".yaml"));
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    agents_.erase(aid);
  }
  // su biblioteca pasa al grafo general (no se pierden documentos) y su subgrafo propio se elimina
  if (s_.rag) s_.rag->reassign_agent(aid, "");
  if (s_.graph && !aid.empty()) s_.graph->drop_scope(aid);
}

std::optional<AgentSpec> AgentManager::get(const std::string& aid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto it = agents_.find(aid);
  if (it == agents_.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> AgentManager::ids() {
  std::vector<std::string> out;
  for (auto& a : list()) out.push_back(a.id);
  return out;
}

// orden de insercion del dict de Python = orden alfabetico de ficheros y despues los guardados
std::vector<AgentSpec> AgentManager::list() {
  std::vector<AgentSpec> out;
  for (auto& a : all())
    if (a.enabled) out.push_back(a);
  return out;
}

std::vector<AgentSpec> AgentManager::all() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::vector<AgentSpec> out;
  for (auto& [_, a] : agents_) out.push_back(a);
  return out;
}

double AgentManager::keyword_score(const AgentSpec& agent, const std::string& text) {
  auto toks = tokens(text);
  std::set<std::string> words(toks.begin(), toks.end());
  std::string low = join(toks, " ");
  double score = 0;
  for (auto& k : agent.keywords) {
    std::string kt = join(tokens(k), " ");
    if (contains(kt, " ") && contains(low, kt))
      score += 2;
    else if (words.count(kt))
      score += 1;
  }
  return score;
}

std::string AgentManager::route(const std::string& text, bool has_images) {
  if (has_images) return "nexus";
  std::string best = "nexus";
  double score = 0.5;
  auto agents = list();
  std::map<std::string, double> semantic;
  if (s_.assist) {
    std::vector<AgentSpec> others;
    for (auto& a : agents)
      if (a.id != "nexus") others.push_back(a);
    semantic = s_.assist->route_bonus(text, others);
  }
  for (auto& a : agents) {
    double sc = keyword_score(a, text) + (s_.lounge && a.id != "nexus" ? 0.8 * s_.lounge->skill_score(a.id, text) : 0.0);
    auto it = semantic.find(a.id);
    if (it != semantic.end()) sc += it->second;
    if (sc > score) {
      best = a.id;
      score = sc;
    }
  }
  return best;
}

std::string AgentManager::build_system(const AgentSpec& spec, const json& info,
                                       const std::vector<std::shared_ptr<tools::ToolSpec>>& tools, bool text_tools) {
  Config& cfg = s_.config;
  std::string lc = cfg.str("user.language", "es");
  std::string lang = LANG.count(lc) ? LANG.at(lc) : lc;
  std::string sc = cfg.str("user.response_style", "equilibrado");
  std::string style = STYLE.count(sc) ? STYLE.at(sc) : "";
  json prof = info.value("profile", json::object());
  double bias = jnum(prof.value("length_bias", json(0)));
  if (bias <= -0.3)
    style += " El usuario prefiere respuestas cortas.";
  else if (bias >= 0.3)
    style += " El usuario valora respuestas completas.";
  // Orden pensado para la cache de prompt de llama.cpp: primero lo estable y al final lo variable.
  std::vector<std::string> parts = {
      replace_all(spec.system_prompt, "{assistant_name}", cfg.str("user.assistant_name", "Nexus-Office")),
      "Responde siempre en " + lang + ". " + style + " Usa Markdown sencillo (sin LaTeX)."};
  if (truthy(info.value("behaviors", json()))) {
    std::vector<std::string> b;
    for (auto& x : info["behaviors"]) b.push_back("- " + uleft(x.get<std::string>(), 200));
    parts.push_back("Pautas de comportamiento que el usuario te ha pedido (respetalas siempre):\n" + join(b, "\n"));
  }
  std::vector<std::string> dynamic;
  if (!tools.empty()) {
    if (text_tools) {
      json pub = json::array();
      for (auto& t : tools) pub.push_back(t->public_json());
      parts.push_back(llm::tool_instructions(pub));
    } else {
      parts.push_back(
          "Dispones de herramientas: usalas cuando aporten algo (crear imagenes o musica, calcular, buscar, "
          "recordar...). Nunca inventes resultados, enlaces ni imagenes: usa la herramienta.");
    }
    bool has_delegate = false;
    for (auto& t : tools) has_delegate |= t->name == "delegate";
    if (has_delegate) {
      auto& cd = spec.can_delegate_to;
      std::vector<std::string> others;
      for (auto& a : list()) {
        if (a.id == spec.id) continue;
        if (cd.empty() || std::find(cd.begin(), cd.end(), "*") != cd.end() ||
            std::find(cd.begin(), cd.end(), a.id) != cd.end())
          others.push_back(a.id + " (" + a.description + ")");
      }
      if (!others.empty()) {
        parts.push_back("Agentes en los que puedes delegar: " + join(others, "; "));
        std::string directory = s_.lounge ? s_.lounge->directory(spec.id) : "";
        if (!directory.empty()) dynamic.push_back("Lo aprendido en el salon sobre tus companeros:\n" + directory);
      }
    }
  }
  auto lines = [](const json& arr, size_t cap, const std::function<std::string(const json&)>& fn) {
    std::vector<std::string> o;
    for (size_t i = 0; i < arr.size() && i < cap; ++i) o.push_back(fn(arr[i]));
    return o;
  };
  json facts = info.value("facts", json::array()), mems = info.value("memories", json::array()),
       lessons = info.value("lessons", json::array()), examples = info.value("examples", json::array()),
       docs = info.value("documents", json::array());
  if (!facts.empty())
    dynamic.push_back("Datos que has aprendido (\"Usuario\" es la persona con la que hablas, no tu):\n" +
                      join(lines(facts, 12, [](const json& f) { return "- " + (f.is_string() ? f.get<std::string>() : f.dump()); }), "\n"));
  if (!mems.empty())
    dynamic.push_back("Recuerdos relevantes:\n" +
                      join(lines(mems, 1u << 30, [](const json& m) { return "- " + uleft(jstr(m, "content"), 300); }), "\n"));
  if (!lessons.empty())
    dynamic.push_back("Lecciones aprendidas (evita repetir estos errores):\n" +
                      join(lines(lessons, 1u << 30, [](const json& m) { return "- " + uleft(jstr(m, "content"), 250); }), "\n"));
  if (!examples.empty())
    dynamic.push_back("Ejemplos de respuestas que el usuario valoro:\n" +
                      join(lines(examples, 1u << 30, [](const json& m) { return uleft(jstr(m, "content"), 600); }), "\n---\n"));
  if (!docs.empty()) {
    std::vector<std::string> d;
    for (size_t i = 0; i < docs.size(); ++i)
      d.push_back("[" + std::to_string(i + 1) + "] «" + uleft(jstr(docs[i], "title"), 80) + "» p." + jstr(docs[i], "page") +
                  ": " + uleft(jstr(docs[i], "text"), 450));
    dynamic.push_back("Fragmentos de tu biblioteca de documentos (RAG). Usalos si son relevantes y cita la fuente como [n]:\n" +
                      join(d, "\n"));
  }
  dynamic.push_back("Fecha y hora actuales: " + strftime_local("%d/%m/%Y %H:%M") + ".");
  for (auto& x : dynamic) parts.push_back(x);
  return join(parts, "\n\n");
}

json AgentManager::run(std::string agent_id, const std::string& text, const std::string& session_id,
                       const std::vector<std::string>& images, tools::Emit emit, bool remote, int depth, bool save,
                       bool live) {
  Services& s = s_;
  if (!emit) emit = [](const json&) {};
  std::string routed_by = "usuario";
  if (agent_id.empty() || agent_id == "auto") {
    agent_id = route(text, !images.empty());
    routed_by = "auto";
  }
  std::optional<AgentSpec> found = get(agent_id);
  if (!found) found = get("nexus");
  if (!found) {
    auto a = all();
    if (a.empty()) throw std::runtime_error("No hay agentes configurados");
    found = a.front();
  }
  const AgentSpec spec = *found;
  emit({{"type", "agent"}, {"agent", spec.id}, {"name", spec.name}, {"color", spec.color}, {"icon", spec.icon}});
  bool use_memory = spec.memory && truthy(s.config.get("user.memory_enabled", true));
  if (save && use_memory && !remote) {
    json learned = s.learner->on_user_message(text, session_id, spec.id, truthy(s.config.get("memory.llm_graph_extraction", false)));
    if (!learned.empty()) {
      json items = json::array();
      for (size_t i = 0; i < learned.size() && i < 8; ++i) items.push_back(learned[i]);
      emit({{"type", "learned"}, {"items", items}});
    }
  }
  json info = use_memory ? s.learner->build_context(text, spec.id, (int)s.config.num("memory.top_k", 6))
                         : json{{"memories", json::array()}, {"examples", json::array()}, {"lessons", json::array()},
                                {"facts", json::array()}, {"profile", json::object()}};
  if (s.rag && use_memory) {
    try {
      info["documents"] = s.rag->search(text, 3, 0.3, 0, spec.id);   // general + los suyos
    } catch (const std::exception& e) {
      L.warning("Busqueda RAG fallida: ", e.what());
    }
  }
  auto all_tools = s.tools->for_agent(spec.tools);
  auto tools = all_tools;
  if (s.assist && !tools.empty() && images.empty()) {
    // IA de apoyo: solo las herramientas que vienen al caso (prompt mas corto y eleccion mas certera)
    std::vector<mem::ToolView> views;
    for (auto& t : tools) views.push_back(t->view());
    auto keep = s.assist->select_tools(text, views);
    std::vector<std::shared_ptr<tools::ToolSpec>> sel;
    for (auto& n : keep)
      for (auto& t : tools)
        if (t->name == n) {
          sel.push_back(t);
          break;
        }
    tools = sel;
    if (tools.size() < all_tools.size()) {
      json names = json::array();
      for (auto& t : tools) names.push_back(t->name);
      emit({{"type", "assist"}, {"tools", names}, {"of", all_tools.size()}});
    }
  }
  bool native = !tools.empty() && s.llm->native_tools(spec.model);
  json msgs = json::array();
  msgs.push_back({{"role", "system"}, {"content", build_system(spec, info, tools, !native)}});
  if (live)
    msgs[0]["content"] = msgs[0]["content"].get<std::string>() +
                         "\n\nConversacion EN VIVO por voz: lo que digas se leera en voz alta. Responde como al hablar: "
                         "1-3 frases cortas y naturales, sin Markdown, listas, tablas, enlaces ni emojis. Si hace falta "
                         "mas detalle, ofrece ampliarlo.";
  if (save) {
    for (auto& m : s.memory->history(session_id, 12)) {
      std::string role = jstr(m, "role"), content = jstr(m, "content");
      if ((role == "user" || role == "assistant") && !content.empty())
        msgs.push_back({{"role", role}, {"content", uleft(content, 3000)}});
    }
    s.memory->add_message(session_id, "user",
                          text + (images.empty() ? "" : "\n[" + std::to_string(images.size()) + " imagen(es) adjunta(s)]"),
                          spec.id);
  }
  auto stored = images.empty() ? std::vector<std::string>{} : store_images(images);
  std::string note = stored.empty() ? ""
                                    : "\n\n[Imagenes adjuntas guardadas en tu carpeta de trabajo: " + join(stored, ", ") +
                                          ". Para editarlas usa imagen_editar con esa ruta.]";
  msgs.push_back({{"role", "user"}, {"content", text + note}, {"images", images}});
  tools::ToolContext ctx{&s, spec.id, session_id, emit, remote, depth};
  int max_steps = (int)s.config.num("llm.max_tool_steps", 5);
  std::vector<std::string> answer_parts;
  std::vector<std::pair<std::string, std::string>> media;
  json tool_log = json::array();
  // via rapida: intenciones inequivocas se ejecutan sin esperar al LLM (fiable y rapido en CPU)
  std::optional<std::pair<std::string, json>> intent;
  if (!all_tools.empty() && images.empty()) {
    std::set<std::string> names;
    for (auto& t : all_tools) names.insert(t->name);
    intent = llm::detect_intent(text, names);
    if (!intent && s.assist && !s.llm->has_real_llm()) {   // modo basico: la herramienta que encaja, por significado
      std::vector<mem::ToolView> views;
      for (auto& t : all_tools) views.push_back(t->view());
      intent = s.assist->guess_tool(text, views);
    }
  }
  static const Rx TOOL_TAG(R"(<tool>.*?</tool>)", Rx::S);
  const std::string aid = spec.id;
  for (int step = 0; step <= max_steps; ++step) {
    emit({{"type", "state"}, {"state", "thinking"}});
    std::string buf;
    if (step == 0 && intent) {
      buf = "<tool>" + pydump(json{{"name", intent->first}, {"args", intent->second}}) + "</tool>";
    } else {
      llm::Opts opts;
      json tnames = json::array();
      for (auto& t : tools) tnames.push_back(t->name);
      if (spec.temperature) opts.o["temperature"] = *spec.temperature;
      if (spec.max_tokens) opts.o["max_tokens"] = *spec.max_tokens;
      opts.o["tools"] = tnames;
      opts.on_reasoning = [emit, aid](const std::string& t) { emit({{"type", "reasoning"}, {"text", t}, {"agent", aid}}); };
      if (live) {   // contestar ya: sin razonamiento (o con muy poco) y respuesta breve
        opts.o["max_tokens"] = std::min(spec.max_tokens.value_or(400), (int)s.config.num("live.max_tokens", 400));
        if (truthy(s.config.get("live.reasoning", false)))
          opts.o["reasoning_budget"] = (int)s.config.num("live.reasoning_budget", 256);
        else
          opts.o["reasoning"] = "off";
      }
      if (native) {
        json specs = json::array();
        for (auto& t : tools) specs.push_back(t->public_json());
        opts.o["tool_specs"] = specs;
      }
      auto sink = [&](const std::string& piece) {
        buf += piece;
        emit({{"type", "token"}, {"text", piece}, {"agent", aid}});
      };
      try {
        s.llm->stream(msgs, spec.model, opts, sink);
      } catch (const llm::ToolsUnsupported&) {
        native = false;   // el endpoint no acepta 'tools': protocolo de texto
        s.llm->native_off.insert(s.llm->select(spec.model).first);
        msgs[0]["content"] = build_system(spec, info, tools, true);
        opts.o.erase("tool_specs");
        s.llm->stream(msgs, spec.model, opts, sink);
      }
    }
    auto call = tools.empty() ? std::nullopt : llm::parse_tool_call(buf);
    if (!call || step == max_steps) {
      answer_parts.push_back(trim(TOOL_TAG.sub(buf, "")));
      break;
    }
    if (!call->pre.empty()) answer_parts.push_back(call->pre);
    const std::string name = call->name;
    emit({{"type", "tool_call"}, {"name", name}, {"args", call->args}});
    emit({{"type", "state"}, {"state", "tool"}});
    json result = s.supervisor->execute(name, call->args, ctx);
    bool is_err = result.is_object() && truthy(result.value("error", json()));
    if (result.is_object() && truthy(result.value("url", json())) &&
        (name == "generate_image" || name == "compose_music" || name == "imagen_editar" || name == "gimp_script"))
      media.emplace_back(name, jstr(result, "url"));
    tool_log.push_back({{"tool", name}, {"ok", !is_err}});
    emit({{"type", "tool_result"}, {"name", name}, {"result", uleft(pydump(result), 1500)}});
    if (s.assist)   // resultados largos: los fragmentos que responden a la peticion, no un corte a ciegas
      result = s.assist->condense_result(text, result, (size_t)s.config.num("assist.result_chars", 5000));
    if (step == 0 && intent && !is_err) {
      // via rapida: respuesta inmediata con plantilla (evita reprocesar todo el prompt en CPU)
      std::string final_text = llm::BuiltinBackend::after_tool(name, pydump(result));
      emit({{"type", "token"}, {"text", final_text}, {"agent", aid}});
      answer_parts.push_back(final_text);
      break;
    }
    msgs.push_back({{"role", "assistant"}, {"content", buf}});
    msgs.push_back({{"role", "user"}, {"content", llm::format_result(name, result)}});
  }
  std::vector<std::string> nonempty;
  for (auto& p : answer_parts)
    if (!p.empty()) nonempty.push_back(p);
  std::string answer = trim(join(nonempty, "\n\n"));
  if (answer.empty()) answer = "(sin respuesta)";
  std::set<std::string> produced;
  for (auto& [_, u] : media) produced.insert(u);
  // los modelos pequenos a veces "inventan" imagenes con URLs externas: se eliminan
  static const Rx IMG(R"(!\[[^\]]*\]\(([^)\s]+)\))");
  answer = trim(IMG.sub(answer, [&](const Rx::Match& m) {
    std::string u = m.str(1);
    return produced.count(u) || starts_with(u, "/outputs/") ? m.str(0) : std::string();
  }));
  if (answer.empty()) answer = "(sin respuesta)";
  for (auto& [name, url] : media)
    if (!contains(answer, url)) answer += name != "compose_music" ? "\n\n![imagen](" + url + ")" : "\n\n[audio](" + url + ")";
  json mid = nullptr;
  if (save) {
    json mem_ids = json::array();
    for (auto& m : info.value("memories", json::array())) mem_ids.push_back(m.value("id", json()));
    std::string meta = json{{"memories", mem_ids}, {"tools", tool_log}}.dump();
    mid = s.memory->add_message(session_id, "assistant", answer, spec.id, meta);
    if (use_memory && !remote) {
      s.learner->on_exchange(session_id, text, answer, spec.id, (int)s.config.num("memory.consolidate_every", 12));
      emit({{"type", "graph_update"}});
    }
  }
  if (depth == 0 && s.lounge) {
    try {
      s.lounge->record_task(spec.id, text, tool_log, routed_by);
    } catch (const std::exception& e) {
      L.warning("No se pudo registrar la tarea en el salon: ", e.what());
    }
  }
  emit({{"type", "done"}, {"message_id", mid}, {"agent", spec.id}, {"text", answer}, {"session_id", session_id}});
  emit({{"type", "state"}, {"state", "idle"}});
  return {{"answer", answer}, {"message_id", mid}, {"agent", spec.id}};
}

std::string AgentManager::run_subtask(const std::string& target, const std::string& task, tools::ToolContext& ctx) {
  tools::Emit parent = ctx.emit ? ctx.emit : [](const json&) {};
  tools::Emit sub_emit = [parent, target](const json& e) {
    static const std::set<std::string> PASS = {"tool_call", "tool_result", "approval_required", "media", "agent"};
    if (PASS.count(jstr(e, "type"))) {
      json c = e;
      c["sub"] = target;
      parent(c);
    }
  };
  if (s_.lounge)
    s_.lounge->post(ctx.agent, "→ " + target + ": " + uleft(task, 200), "delegation", {{"from", ctx.agent}, {"to", target}});
  if (s_.organizer) {   // interaccion entre agentes: conocimiento comun
    try {
      auto src = get(ctx.agent), dst = get(target);
      s_.organizer->record_interaction(src ? src->name : ctx.agent, "delega_en", dst ? dst->name : target, 0.5);
    } catch (const std::exception& e) {
      L.warning("No se pudo anotar la delegacion: ", e.what());
    }
  }
  json res = run(target, task, ctx.session_id, {}, sub_emit, ctx.remote, ctx.depth + 1, false);
  std::string answer = jstr(res, "answer");
  if (s_.lounge) {
    s_.lounge->bump(target, Lounge::topics(task), 1.0, 0.0);
    s_.lounge->post(target, "← " + ctx.agent + ": hecho. " + uleft(answer, 160), "delegation",
                    {{"from", target}, {"to", ctx.agent}});
  }
  return answer;
}

}  // namespace nx::agents
