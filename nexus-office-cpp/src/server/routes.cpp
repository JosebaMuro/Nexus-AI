// Rutas de la API (server/app.py): mismas URLs, mismos parametros y mismos JSON que la version Python, en el mismo
// orden. Lo que la edicion C++ aun no trae (red COOP, acceso remoto, VPN, mesa de bolsa, rastreo web, importar grafos)
// responde 501 con un mensaje claro en lugar de fallar a medias.
#include <httplib.h>

#include <algorithm>
#include <csignal>
#include <thread>

#include "agents/agents.h"
#include "core/service.h"
#include "core/services.h"
#include "core/system.h"
#include "gen/data.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "media/media.h"
#include "memory/memory.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "office/bolsa.h"
#include "memory/kg.h"
#include "memory/kg_jobs.h"
#include "util/rx.h"
#include "office/office.h"
#include "rag/rag.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

namespace nx::server {

namespace {

auto L = log::get("server");

[[noreturn]] void not_in_cpp(const std::string& what) {
  throw HttpErr(501, what + " todavia no esta disponible en la edicion C++ (2.0 beta C). Usa la edicion Python para esto.");
}

// str(KeyError("x")) de Python
std::string key_msg(const std::exception& e) { return "'" + std::string(e.what()) + "'"; }

json merged(json a, const json& b) {
  for (auto& [k, v] : b.items()) a[k] = v;
  return a;
}

json agents_json(const std::vector<agents::AgentSpec>& list) {
  json out = json::array();
  for (auto& a : list) out.push_back(a.to_json());
  return out;
}

std::string str_or(const json& v, const std::string& def) {
  if (!truthy(v)) return def;
  return Ctx::as_str(v);
}

// limite de intentos de PIN desde la red local (en el propio equipo no tiene sentido bloquear)
class Limiter {
 public:
  bool blocked(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = until_.find(key);
    return it != until_.end() && it->second > now();
  }
  void fail(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (++fails_[key] >= 5) {
      until_[key] = now() + 15 * 60;
      fails_[key] = 0;
    }
  }
  void reset(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    fails_.erase(key);
    until_.erase(key);
  }

 private:
  std::mutex mu_;
  std::map<std::string, int> fails_;
  std::map<std::string, double> until_;
};

// Trabajos del gestor de conocimiento (memory/kg_jobs.py): reorganizar y buscar duplicados en segundo plano
class KgJobs {
 public:
  explicit KgJobs(App& app) : app_(app) {}

  json list() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<json> all;
    for (auto& [_, j] : jobs_) all.push_back(j);
    std::stable_sort(all.begin(), all.end(), [](const json& a, const json& b) {
      return a["started"].get<double>() > b["started"].get<double>();
    });
    json out = json::array();
    for (size_t i = 0; i < all.size() && i < 30; ++i) out.push_back(all[i]);
    return out;
  }

  json get(const std::string& jid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(jid);
    return it == jobs_.end() ? json() : it->second;
  }

  using Fn = std::function<json(const std::string& jid)>;
  json start(const std::string& kind, const std::string& title, Fn fn) {
    std::string jid = uuid_hex().substr(0, 10);
    json job = {{"id", jid},  {"kind", kind}, {"title", title},   {"status", "en curso"}, {"done", 0},
                {"total", 0}, {"step", ""},   {"log", json::array()}, {"result", nullptr}, {"error", ""},
                {"started", now()}, {"finished", nullptr}};
    {
      std::lock_guard<std::mutex> lk(mu_);
      jobs_[jid] = job;
    }
    std::thread([this, jid, title, fn] {
      try {
        json res = fn(jid);
        update(jid, {{"result", res}, {"status", "completado"}});
      } catch (const std::exception& e) {
        L.error("Trabajo ", title, " fallido: ", e.what());
        update(jid, {{"status", "error"}, {"error", uleft(e.what(), 500)}});
      }
      update(jid, {{"finished", now()}});
      app_.broadcast({{"type", "graph_update"}});
    }).detach();
    emit(jid);
    return get(jid);
  }

  void step(const std::string& jid, const std::string& text, long done = -1, long total = -1) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      json& j = jobs_[jid];
      j["step"] = text;
      if (done >= 0) j["done"] = done;
      if (total >= 0) j["total"] = total;
      j["log"].push_back(text);
      if (j["log"].size() > 40) j["log"].erase(j["log"].begin());
    }
    emit(jid);
  }

 private:
  App& app_;
  std::mutex mu_;
  std::map<std::string, json> jobs_;
  void update(const std::string& jid, const json& patch) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& [k, v] : patch.items()) jobs_[jid][k] = v;
    }
    emit(jid);
  }
  void emit(const std::string& jid) { app_.broadcast({{"type", "kg_job"}, {"job", get(jid)}}); }
};

const std::map<std::string, std::string> SOURCE_URLS = {{"huggingface", "https://huggingface.co"},
                                                        {"ollama", "https://ollama.com"},
                                                        {"modelscope", "https://www.modelscope.cn"},
                                                        {"civitai", "https://civitai.com"},
                                                        {"kaggle", "https://www.kaggle.com"}};

fs::path temp_folder() {
  fs::path d = paths::sub({"cache"}) / ("up_" + token_hex(6));
  mkdirs(d);
  return d;
}

std::string base_name(const std::string& filename, const std::string& def) {
  std::string n = replace_all(filename, "\\", "/");
  auto slash = n.rfind('/');
  if (slash != std::string::npos) n = n.substr(slash + 1);
  return n.empty() ? def : n;
}

}  // namespace

void register_routes(App& app) {
  auto limiter = std::make_shared<Limiter>();
  auto kg_jobs = std::make_shared<KgJobs>(app);
  App* A = &app;

  // ------------------------------------------------------------------ comunes
  auto rag_target = [A](const std::string& agent_in) -> std::string {
    std::string agent = trim(agent_in);
    if (agent.empty() || agent == "general") return "";
    if (!A->S().agents->get(agent)) throw HttpErr(400, "Agente desconocido: " + agent);
    return agent;
  };
  // nullopt o "*": todo; "" o "general": el grafo general; un id: el subgrafo de ese agente
  auto kg_scope = [A](const std::optional<std::string>& scope) -> std::optional<std::string> {
    if (!scope || *scope == "*") return std::nullopt;
    if (scope->empty() || *scope == "general") return std::string();
    if (!A->S().agents->get(*scope)) throw HttpErr(400, "Agente desconocido: " + *scope);
    return scope;
  };
  auto json_scope = [](const json& v) -> std::optional<std::string> {
    if (v.is_null()) return std::nullopt;
    return Ctx::as_str(v);
  };

  app.get("/api/ping", [](Ctx& c) -> Out {
    std::string name = c.app.ready() ? c.app.S().config.str("user.assistant_name") : "Nexus-Office";
    return json{{"nexus", true}, {"version", NEXUS_VERSION}, {"name", name}, {"edition", "cpp"}};
  });

  app.get("/api/lan", [](Ctx& c) -> Out {
    if (!c.local()) throw HttpErr(403, "Solo desde el propio equipo");
    Services& s = c.app.S();
    int port = (int)s.config.num("server.port", 8775);
    std::string bound = s.config.str("server.host", "127.0.0.1");
    std::string key = c.app.lan_key();
    json urls = json::array();
    for (auto& ip : net::lan_ips())
      if (!starts_with(ip, "169.254.")) urls.push_back("http://" + ip + ":" + std::to_string(port) + "/?key=" + key);
    return json{{"key", key}, {"listening_lan", bound != "127.0.0.1" && bound != "localhost"}, {"port", port}, {"urls", urls}};
  });

  app.post("/api/lan/enable", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.config.set("server.host", truthy(c.b("enabled")) ? "0.0.0.0" : "127.0.0.1");
    c.app.lan_key();
    return json{{"host", s.config.get("server.host")}, {"restart_required", true}};
  }, true);

  app.get("/api/package/termux", [](Ctx&) -> Out {
    not_in_cpp("El paquete para Termux (Android) se instala con la APK de Nexus-Office;");
  });

  // ------------------------------------------------------------------ estaticos
  app.get("/", [](Ctx& c) -> Out {
    c.res.set_header("Cache-Control", "no-cache");
    return FileOut{paths::web() / "index.html", "", "text/html; charset=utf-8"};
  });
  app.get("/sw.js", [](Ctx& c) -> Out {   // en la raiz para que su ambito cubra toda la app (PWA)
    c.res.set_header("Cache-Control", "no-cache");
    return FileOut{paths::web() / "sw.js", "", "text/javascript"};
  });

  // ------------------------------------------------------------------ estado general
  app.get("/api/status", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json prof = json::object();
    for (auto* k : {"id", "tier", "accelerator", "device_name", "ram_gb", "vram_gb"}) prof[k] = s.profile.value(k, json());
    return json{{"version", NEXUS_VERSION},
                {"name", s.config.get("user.assistant_name")},
                {"admin", c.admin},
                {"has_pin", s.config.has_pin()},
                {"profile", prof},
                {"npu", s.profile.value("npu", json())},
                {"llm", s.llm->status()},
                {"image", s.images->status()},
                {"voice", s.voice->info()},
                {"memory", s.memory->stats()},
                {"graph", s.graph->stats()},
                {"rag", s.rag->stats()},
                {"tuning", s.tuning_reasons},
                {"assist", s.assist->info()},
                {"coop", {{"running", false}, {"peers", 0}, {"vip", ""}}},
                {"pending_approvals", s.supervisor->list_pending().size()},
                {"pending_proposals", s.proposals->list("pendiente").size()},
                {"suggest_smaller_model", s.profile.value("llm", json::object()).value("suggest_smaller_model", false)},
                {"edition", "cpp"}};
  });

  // ------------------------------------------------------------------ admin
  app.post("/api/admin/setup", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string pin = c.bs("pin");
    if (s.config.has_pin()) throw HttpErr(400, "Ya existe un PIN; inicia sesion");
    s.config.set_pin(pin);
    return json{{"token", c.app.new_admin_token()}};
  });

  app.post("/api/admin/login", [limiter](Ctx& c) -> Out {
    std::string pin = c.bs("pin");
    std::string key = "pin:" + c.host;
    bool lim = !c.local();
    if (lim && limiter->blocked(key)) throw HttpErr(429, "Demasiados intentos fallidos: espera 15 minutos");
    if (!c.app.S().config.check_pin(pin)) {
      if (lim) limiter->fail(key);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      throw HttpErr(401, "PIN incorrecto");
    }
    if (lim) limiter->reset(key);
    return json{{"token", c.app.new_admin_token()}};
  });

  app.post("/api/admin/logout", [](Ctx& c) -> Out {
    c.app.drop_admin_token(c.header("X-Admin-Token"));
    return json{{"ok", true}};
  });

  app.post("/api/admin/pin", [](Ctx& c) -> Out {
    c.app.S().config.set_pin(c.bs("pin"));
    return json{{"ok", true}};
  }, true);

  // apagado limpio: guarda el resumen de rendimiento para el autoajuste del siguiente arranque
  app.post("/api/admin/shutdown", [](Ctx& c) -> Out {
    App* a = &c.app;
    std::thread([a] {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (a->on_shutdown_request) a->on_shutdown_request();
      else std::raise(SIGINT);
    }).detach();
    return json{{"ok", true}};
  }, true);

  app.get("/api/admin/logs", [](Ctx& c) -> Out { return json{{"lines", log::recent((size_t)std::max(0LL, c.qi("n", 200)))}}; },
          true);

  // ------------------------------------------------------------------ configuracion
  app.get("/api/config", [](Ctx& c) -> Out { return c.app.S().config.view(c.admin); });

  app.put("/api/config", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    if (!c.body.is_object()) throw HttpErr(422, "Se esperaba un objeto JSON");
    json applied = s.config.update(c.body, c.admin);
    bool llm = false, tools_ = false;
    for (auto& k : applied) {
      std::string key = k.get<std::string>();
      llm |= starts_with(key, "llm.") || starts_with(key, "hardware.");
      tools_ |= starts_with(key, "tools.");
    }
    if (llm) s.llm->rebuild();
    if (tools_) s.tools->reload();
    return json{{"applied", applied}};
  });

  // ------------------------------------------------------------------ hardware / rendimiento
  app.get("/api/hardware", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json hist = hw::load_history();
    json last = json::array();
    if (hist.is_array())
      for (size_t i = hist.size() > 15 ? hist.size() - 15 : 0; i < hist.size(); ++i) last.push_back(hist[i]);
    return json{{"hardware", s.hw.to_json()}, {"profile", s.profile}, {"tuning", s.tuning_reasons},
                {"tuning_state", hw::load_state()}, {"history", last}};
  });

  app.get("/api/monitor", [](Ctx& c) -> Out { return c.app.S().monitor->live(); });

  for (auto* p : {"/api/android/autoajuste"}) {
    app.get(p, [](Ctx&) -> Out { throw HttpErr(404, "El autoajuste en vivo solo funciona en la app Android autonoma"); });
  }
  app.post("/api/android/autoajuste/medir",
           [](Ctx&) -> Out { throw HttpErr(404, "El autoajuste en vivo solo funciona en la app Android autonoma"); });
  app.post("/api/android/autoajuste/modelo/{mid}",
           [](Ctx&) -> Out { throw HttpErr(404, "El autoajuste en vivo solo funciona en la app Android autonoma"); });

  app.post("/api/hardware/redetect", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.hw = hw::detect(s.config.str("llm.ollama_url", "http://127.0.0.1:11434"));
    hw::save(s.hw);
    s.profile = hw::build_profile(s.hw, s.config.str("hardware.profile_override"));
    s.llm->rebuild();
    return json{{"hardware", s.hw.to_json()}, {"profile", s.profile}};
  }, true);

  // selector de inicio: motor llama y acelerador (CPU/GPU/NPU) con su disponibilidad y lo que se usa de verdad
  app.get("/api/accel", [](Ctx& c) -> Out { return sys2::accel_options(c.app.S()); });

  app.post("/api/accel", [](Ctx& c) -> Out {
    json res;
    try {
      res = sys2::accel_apply(c.app.S(), c.bs("engine", "auto"), c.bs("accel", "auto"), c.bb("remember", true));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "status_changed"}});
    return res;
  });

  // carga el modelo con la eleccion actual, genera unas palabras y dice donde se ha ejecutado y a que velocidad
  app.post("/api/accel/test", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string backend = s.llm->select().first;
    double t0 = now();
    std::string text;
    try {
      llm::Opts o;
      o.o = {{"max_tokens", 16}, {"temperature", 0}, {"reasoning", "off"}};
      text = s.llm->complete("Responde solo: hola", "default", o);
    } catch (const std::exception& e) {
      return merged({{"ok", false}, {"backend", backend}, {"error", uleft(e.what(), 400)}}, sys2::accel_options(s));
    }
    json tps = nullptr;
    if (auto b = s.llm->backend(backend); b && b->last_stats.seconds > 0)
      tps = round_to(b->last_stats.tokens / b->last_stats.seconds, 1);
    return merged({{"ok", true}, {"backend", backend}, {"text", uleft(trim(text), 80)},
                   {"seconds", round_to(now() - t0, 2)}, {"tokens_s", tps}},
                  sys2::accel_options(s));
  });

  app.get("/api/service", [](Ctx& c) -> Out {
    if (hw::android()) return json{{"kind", "android"}, {"registered", ""}, {"supported", false}};
    return json{{"kind", service::kind()}, {"registered", service::registered()}, {"supported", true},
                {"admin_os", service::is_admin()}, {"port", c.app.S().config.get("server.port", 8775)}};
  });

  // instala o quita el arranque automatico; en un servidor ya en marcha el servicio toma el relevo al reiniciar
  app.post("/api/service", [](Ctx& c) -> Out {
    Config& cfg = c.app.S().config;
    std::string action = c.bs("action");
    try {
      if (action == "instalar") {
        std::string host = c.bb("red", false) ? "0.0.0.0" : cfg.str("server.host", "127.0.0.1");
        return json{{"ok", true}, {"detail", service::install(host, (int)cfg.num("server.port", 8775))}};
      }
      if (action == "quitar") {
        std::string done = join(service::uninstall(), ", ");
        return json{{"ok", true}, {"detail", done.empty() ? "no habia servicio" : done}};
      }
    } catch (const HttpErr&) {
      throw;
    } catch (const std::exception& e) {
      throw HttpErr(400, e.what());
    }
    throw HttpErr(400, "accion: instalar | quitar");
  }, true);

  app.get("/api/downloads", [](Ctx&) -> Out { return sys2::downloads_listing(); });

  app.get("/descargas/{rel:path}", [](Ctx& c) -> Out {
    auto f = sys2::downloads_resolve(c.p("rel"));
    if (!f) throw HttpErr(404, "no existe");
    return FileOut{*f, S(f->filename()), suffix_lower(*f) == ".apk" ? "application/vnd.android.package-archive" : ""};
  });

  app.get("/api/drivers", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    return sys2::drivers_report(jstr(s.profile, "accelerator"), s);
  });

  app.post("/api/drivers/update", [](Ctx& c) -> Out {
    try {
      return sys2::drivers_launch(c.bs("option"));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  }, true);

  app.get("/api/runtimes", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json bundled = json::object(), recommended = json::object();
    for (auto* rt : {"llama", "sd"}) {
      bundled[rt] = runtimes::bundled_variants(rt);
      recommended[rt] = runtimes::candidates(s.profile, rt);
    }
    std::string variant;
    if (auto b = std::dynamic_pointer_cast<llm::LlamaServerBackend>(s.llm->backend("llamaserver"))) variant = b->variant;
    return json{{"installed", s.runtimes->installed()},
                {"progress", s.runtimes->progress()},
                {"bundled", bundled},
                {"recommended", recommended},
                {"descriptions", {{"llama", "LLM / multimodal (llama-server)"}, {"sd", "Generacion de imagen (sd.cpp)"}}},
                {"active", {{"llama", variant}}}};
  });

  app.post("/api/runtimes/install", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string runtime = c.bs("runtime"), variant = c.bs("variant");
    auto rec = runtimes::candidates(s.profile, runtime);
    if (!c.admin && (rec.empty() || variant != rec[0]))
      throw HttpErr(403, "En modo usuario solo se instala la variante recomendada");
    Services* sp = &s;
    std::thread([sp, runtime, variant] {
      try {
        sp->runtimes->install(runtime, variant);
        sp->llm->rebuild();
      } catch (const std::exception& e) {
        L.error("Instalacion de runtime fallida: ", e.what());
      }
    }).detach();
    return json{{"started", true}};
  });

  // ------------------------------------------------------------------ sesiones / memoria
  app.get("/api/sessions", [](Ctx& c) -> Out { return json(c.app.S().memory->sessions()); });
  app.get("/api/sessions/{sid}", [](Ctx& c) -> Out { return json(c.app.S().memory->history(c.p("sid"), 200)); });
  app.del("/api/sessions/{sid}", [](Ctx& c) -> Out {
    c.app.S().memory->delete_session(c.p("sid"));
    return json{{"ok", true}};
  });

  app.post("/api/feedback", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    long long mid = (long long)jnum(c.b("message_id"));
    int score = (int)jnum(c.b("score"));
    json res = s.learner->feedback(mid, score, c.bs("comment", ""));
    if (auto msg = s.db->one("SELECT session_id, agent FROM messages WHERE id=?", {mid})) {
      auto q = s.db->one(
          "SELECT content FROM messages WHERE session_id=? AND id<? AND role='user' ORDER BY id DESC LIMIT 1",
          {(*msg)["session_id"], mid});
      s.lounge->on_feedback(jstr(*msg, "agent"), q ? jstr(*q, "content") : "", score);
    }
    return res;
  });

  // ------------------------------------------------------------------ salon virtual
  app.get("/api/lounge", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    return json{{"messages", s.lounge->history(150)}, {"skills", s.lounge->skills()}, {"proposals", s.lounge->proposals()},
                {"gaps", s.lounge->detect_gaps()}, {"agents", agents_json(s.agents->list())}, {"busy", s.lounge->busy()}};
  });

  app.post("/api/lounge/meet", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    if (s.lounge->busy()) return json{{"started", false}, {"busy", true}};
    std::string topic = c.bs("topic", "");
    Services* sp = &s;
    std::thread([sp, topic] {
      try {
        sp->lounge->meet(false, topic);
      } catch (const std::exception& e) {
        L.error("Reunion del salon fallida: ", e.what());
      }
    }).detach();
    return json{{"started", true}};
  });

  app.post("/api/lounge/say", [](Ctx& c) -> Out {
    c.app.S().lounge->user_says(c.bs("text"));
    return json{{"ok", true}};
  });

  app.post("/api/lounge/proposals/{pid}", [](Ctx& c) -> Out {
    long long pid = c.pi("pid");
    bool approve = truthy(c.b("approve"));
    try {
      return c.app.S().lounge->resolve_proposal(pid, approve, c.bs("note", ""));
    } catch (const tools::ToolError& e) {
      throw HttpErr(400, "'" + std::string(e.what()) + "'");
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    } catch (const std::out_of_range& e) {
      throw HttpErr(400, key_msg(e));
    }
  }, true);

  app.get("/api/memory", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    return json{{"items", s.memory->list(c.q("kind"), (int)c.qi("limit", 100), (int)c.qi("offset", 0))},
                {"stats", s.memory->stats()}};
  });

  app.get("/api/memory/search", [](Ctx& c) -> Out {
    if (!c.has_q("q")) throw missing_field("query", "q");
    return json(c.app.S().memory->recall(c.q("q"), (int)c.qi("k", 10)));
  });

  app.post("/api/memory", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string content = c.bs("content");
    long long mid = s.memory->remember(content, c.bs("kind", "note"), 0.8, "manual");
    s.learner->learn_text(content);   // lo comun al general; el saber hacer, al especialista que corresponda
    c.app.broadcast({{"type", "graph_update"}});
    return json{{"id", mid}};
  });

  app.del("/api/memory/{mid}", [](Ctx& c) -> Out {
    c.app.S().memory->forget(c.pi("mid"));
    return json{{"ok", true}};
  });

  // scope="*": el general con el subgrafo de cada agente dentro; "": solo el general; id: solo ese agente
  app.get("/api/graph", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string scope = c.q("scope", "*");
    if (scope != "*" && !scope.empty() && !s.agents->get(scope)) throw HttpErr(404, "Agente desconocido");
    return s.graph->to_json((int)c.qi("limit", 220), scope);
  });

  // grafo general y subgrafo de cada agente, con sus tamanos y documentos RAG propios
  auto graph_scopes = [](Services& s) {
    std::map<std::string, json> sizes;
    for (auto& r : s.graph->scopes()) sizes[jstr(r, "scope")] = r;
    json docs = s.rag->stats().value("by_scope", json::object());
    auto sz = [&](const std::string& id, json o) {
      auto it = sizes.find(id);
      o["entities"] = it != sizes.end() ? it->second["entities"] : json(0);
      o["relations"] = it != sizes.end() ? it->second["relations"] : json(0);
      return o;
    };
    json out = json::array();
    json g = sz("", {{"id", ""}, {"name", "General"}, {"icon", "◎"}, {"color", ""}});
    g["documents"] = docs.value("general", json(0));
    out.push_back(g);
    for (auto& a : s.agents->list()) {
      json o = sz(a.id, {{"id", a.id}, {"name", a.name}, {"icon", a.icon}, {"color", a.color}});
      o["documents"] = docs.value(a.id, json(0));
      out.push_back(o);
    }
    return out;
  };
  app.get("/api/graph/scopes", [graph_scopes](Ctx& c) -> Out { return graph_scopes(c.app.S()); });

  app.del("/api/graph/{eid}", [](Ctx& c) -> Out {
    c.app.S().graph->delete_entity(c.pi("eid"));
    return json{{"ok", true}};
  }, true);

  // ------------------------------------------------------------------ gestor de conocimiento (Ajustes)
  app.get("/api/kg/overview", [graph_scopes, kg_jobs](Ctx& c) -> Out {
    Services& s = c.app.S();
    json embed_models = json::array();
    for (auto& m : s.models->catalog.list())
      if (jstr(m, "kind") == "embed") embed_models.push_back(m);
    return merged(s.organizer->overview(), {{"stats", s.graph->stats()},
                                            {"scopes", graph_scopes(s)},
                                            {"memory", s.memory->stats()},
                                            {"embeddings", s.embed_worker->status()},
                                            {"jobs", kg_jobs->list()},
                                            {"embed_models", embed_models},
                                            {"embed_active", s.config.get("memory.embed_model", "")}});
  });

  app.get("/api/kg/nodes", [kg_scope](Ctx& c) -> Out {
    Services& s = c.app.S();
    auto sc = kg_scope(c.qopt("scope"));
    std::string q = c.q("q");
    long long limit = c.qi("limit", 60);
    if (c.qb("semantic", false) && !trim(q).empty()) {
      json items;
      try {
        items = s.organizer->semantic_search(q, sc, (int)std::min(limit, 200LL));
      } catch (const std::exception& e) {
        throw HttpErr(400, std::string("Búsqueda semántica no disponible: ") + e.what());
      }
      return json{{"total", items.size()}, {"items", items}, {"semantic", true}};
    }
    return s.graph->search_nodes(sc, q, c.q("category"), c.q("type"), (int)std::clamp(limit, 1LL, 500LL),
                                 (int)std::max(c.qi("offset", 0), 0LL));
  });

  app.get("/api/kg/nodes/{eid}", [](Ctx& c) -> Out {
    json n = c.app.S().graph->node(c.pi("eid"));
    if (!truthy(n)) throw HttpErr(404, "Nodo no encontrado");
    return n;
  });

  app.post("/api/kg/nodes", [kg_scope, json_scope](Ctx& c) -> Out {
    Services& s = c.app.S();
    const json& d = c.body.is_object() ? c.body : json::object();
    json n;
    try {
      n = s.graph->add_entity(kg_scope(json_scope(d.value("scope", json("")))).value_or(""),
                              Ctx::as_str(d.value("name", json(""))), str_or(d.value("type", json()), "concepto"),
                              str_or(d.value("category", json()), ""), str_or(d.value("note", json()), ""));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return n;
  }, true);

  app.patch("/api/kg/nodes/{eid}", [kg_scope, json_scope](Ctx& c) -> Out {
    Services& s = c.app.S();
    long long eid = c.pi("eid");
    const json& d = c.body.is_object() ? c.body : json::object();
    json n;
    try {
      json changes = json::object();
      for (auto* k : {"name", "type", "category", "note", "weight"})
        if (d.contains(k)) changes[k] = d[k];
      n = s.graph->update_entity(eid, changes);
      if (d.contains("scope") && !d["scope"].is_null())
        n = s.graph->move_entity(n["id"].get<long long>(), kg_scope(json_scope(d["scope"])).value_or(""),
                                 str_or(d.value("category", json()), ""));
    } catch (const std::out_of_range&) {
      throw HttpErr(404, "Nodo no encontrado");
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return n;
  }, true);

  app.del("/api/kg/nodes/{eid}", [](Ctx& c) -> Out {
    c.app.S().graph->delete_entity(c.pi("eid"));
    c.app.broadcast({{"type", "graph_update"}});
    return json{{"ok", true}};
  }, true);

  app.post("/api/kg/merge", [](Ctx& c) -> Out {
    json n;
    try {
      n = c.app.S().graph->merge_entities((long long)jnum(c.b("src")), (long long)jnum(c.b("dst")));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return n;
  }, true);

  app.post("/api/kg/relations", [](Ctx& c) -> Out {
    json r;
    try {
      r = c.app.S().graph->add_relation((long long)jnum(c.b("src")), (long long)jnum(c.b("dst")),
                                        c.bs("rel", "relacionado_con"), c.bn("weight", 1.0));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return r;
  }, true);

  app.del("/api/kg/relations/{rid}", [](Ctx& c) -> Out {
    c.app.S().graph->delete_relation(c.pi("rid"));
    c.app.broadcast({{"type", "graph_update"}});
    return json{{"ok", true}};
  }, true);

  app.get("/api/kg/formats", [](Ctx&) -> Out { return kg::formats_json(); });

  // otro grafo en cualquier formato (se detecta por el contenido); varios ficheros (files) se unen en uno solo
  app.post("/api/kg/import", [kg_jobs](Ctx& c) -> Out {
    std::vector<Upload> ups;
    if (auto f = c.file("file")) ups.push_back(*f);
    for (auto& f : c.files("files")) ups.push_back(f);
    if (ups.empty()) throw HttpErr(400, "Falta el fichero del grafo");
    kg::Files items;
    size_t total = 0;
    for (size_t i = 0; i < ups.size() && i < 50; ++i) {
      total += ups[i].content.size();
      if (total > 300u * 1024 * 1024) throw HttpErr(400, "El grafo supera 300 MB");
      items.emplace_back(base_name(ups[i].filename, "grafo.json"), std::move(ups[i].content));
    }
    Services* s = &c.app.S();
    std::string target, name;
    json parsed;
    try {
      target = kg::resolve_target(*s, c.form("target"));
      if (items.size() == 1) {
        name = items[0].first;
        parsed = kg::parse_bytes(std::move(items[0].second), name);
      } else {
        parsed = kg::parse_many(items);
        for (size_t i = 0; i < items.size() && i < 3; ++i) name += (i ? ", " : "") + items[i].first;
        if (items.size() > 3) name += "…";
      }
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    } catch (const std::exception& e) {
      throw HttpErr(400, std::string("No se pudo leer el grafo: ") + e.what());
    }
    KgJobs* jobs = kg_jobs.get();
    auto p = std::make_shared<json>(std::move(parsed));
    return kg_jobs->start("import", "Importar grafo «" + name + "»", [s, p, name, target, jobs](const std::string& jid) {
      return kg::run_import(*s, *p, name, target,
                            [jobs, jid](const std::string& t, long d, long n) { jobs->step(jid, t, d, n); });
    });
  }, true);

  // ZIP (o RAR, 7z, tar) razonado con el LLM y procesado en segundo plano
  app.post("/api/kg/archive", [kg_jobs](Ctx& c) -> Out {
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    std::string name = base_name(f->filename, "archivo.zip");
    std::string safe = re_sub("[^A-Za-z0-9._-]+", "_", name);
    fs::path tmp = paths::sub({"cache"}) / P("tmp" + token_hex(4) + "_" + uright(safe, 60));
    if (!write_file(tmp, f->content)) throw HttpErr(500, "No se pudo guardar " + name);
    Services* s = &c.app.S();
    std::string target;
    try {
      target = kg::resolve_target(*s, c.form("target", "auto"));
    } catch (const std::invalid_argument& e) {
      remove_file(tmp);
      throw HttpErr(400, e.what());
    }
    bool reason = c.form_bool("reason", true), rag = c.form_bool("rag", true);
    long long mr = -1;
    try {
      mr = std::stoll(c.form("max_reason", "-1"));
    } catch (...) {
      remove_file(tmp);
      throw HttpErr(422, "max_reason: Input should be a valid integer");
    }
    int max_reason = mr < 0 ? (int)s->config.num("memory.zip_reason_docs", 12) : (int)mr;
    KgJobs* jobs = kg_jobs.get();
    return kg_jobs->start("zip", "Procesar «" + name + "»", [s, tmp, name, target, reason, rag, max_reason, jobs](const std::string& jid) {
      return kg::run_archive(*s, tmp, name, target, reason, rag, max_reason,
                             [jobs, jid](const std::string& t, long d, long n) { jobs->step(jid, t, d, n); });
    });
  }, true);

  app.get("/api/kg/export", [kg_scope](Ctx& c) -> Out {
    std::string fmt = c.q("format", "json");
    if (fmt != "json" && fmt != "graphml" && fmt != "csv") throw HttpErr(400, "format: json, graphml o csv");
    auto sc = kg_scope(c.qopt("scope"));
    auto [data, mime, ext] = kg::export_graph(*c.app.S().graph, sc, fmt);
    std::string fname = "nexus-conocimiento-" + (!sc ? std::string("todo") : sc->empty() ? std::string("general") : *sc) + "." + ext;
    return Raw{200, data, mime, {{"Content-Disposition", "attachment; filename=" + fname}}};
  });

  app.post("/api/kg/reorganize", [kg_jobs](Ctx& c) -> Out {
    Services* s = &c.app.S();
    bool apply = c.bb("apply", false);
    KgJobs* jobs = kg_jobs.get();
    return kg_jobs->start("reorganize", apply ? "Reorganizar la memoria" : "Revisar la organizacion",
                          [s, apply, jobs](const std::string& jid) {
                            jobs->step(jid, "Clasificando lo aprendido (comun o de cada especialidad)");
                            json moves = s->organizer->plan([jobs, jid](size_t d, size_t t) {
                              jobs->step(jid, "Clasificando " + std::to_string(d) + "/" + std::to_string(t), (long)d, (long)t);
                            });
                            json listed = json::array();
                            for (size_t i = 0; i < moves.size() && i < 500; ++i) {
                              json m = json::object();
                              for (auto* k : {"id", "name", "type", "scope", "category", "to", "to_category", "reason", "score"})
                                m[k] = moves[i].value(k, json());
                              listed.push_back(m);
                            }
                            json res = {{"moves", listed}, {"total", moves.size()}};
                            if (apply) {
                              jobs->step(jid, "Moviendo " + std::to_string(moves.size()) + " nodos");
                              res = merged(res, s->organizer->apply(moves, true, s->memory.get()));
                            }
                            return res;
                          });
  }, true);

  app.post("/api/kg/duplicates", [kg_jobs, kg_scope, json_scope](Ctx& c) -> Out {
    Services* s = &c.app.S();
    auto sc = kg_scope(json_scope(c.b("scope", json())));
    KgJobs* jobs = kg_jobs.get();
    return kg_jobs->start("duplicates", "Buscar duplicados", [s, sc, jobs](const std::string& jid) {
      jobs->step(jid, "Comparando nodos con el modelo de embeddings");
      json pairs = s->organizer->duplicates(sc);
      return json{{"pairs", pairs}, {"total", pairs.size()}};
    });
  }, true);

  app.get("/api/kg/jobs", [kg_jobs](Ctx&) -> Out { return kg_jobs->list(); });
  app.get("/api/kg/jobs/{jid}", [kg_jobs](Ctx& c) -> Out {
    json j = kg_jobs->get(c.p("jid"));
    if (j.is_null()) throw HttpErr(404, "Trabajo no encontrado");
    return j;
  });

  app.get("/api/kg/memories", [kg_scope](Ctx& c) -> Out {
    return json(c.app.S().memory->list(c.q("kind"), (int)std::min(c.qi("limit", 100), 500LL), (int)c.qi("offset", 0),
                                       kg_scope(c.qopt("scope"))));
  });

  app.patch("/api/kg/memories/{mid}", [kg_scope](Ctx& c) -> Out {
    c.app.S().memory->move(c.pi("mid"), kg_scope(c.bs("scope", "")).value_or(""));
    return json{{"ok", true}};
  }, true);

  app.get("/api/kg/embeddings", [](Ctx& c) -> Out { return c.app.S().embed_worker->status(); });

  // activa un modelo de embeddings del catalogo ("" = sin modelo: hashing); se recalcula en segundo plano
  app.post("/api/kg/embeddings/model", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::string model = c.bs("model", "");
    if (!model.empty()) {
      json entry = s.models->catalog.get(model);
      if (entry.is_null() || jstr(entry, "kind") != "embed") throw HttpErr(400, "No es un modelo de embeddings instalado");
    }
    s.config.set("memory.embed_model", model);
    s.set_embedder();
    return s.embed_worker->status();
  }, true);

  app.post("/api/kg/embeddings/reindex", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.memory->invalidate();
    s.rag->invalidate();
    s.embed_worker->start();
    return s.embed_worker->status();
  }, true);

  // ------------------------------------------------------------------ biblioteca RAG
  app.get("/api/rag", [rag_target](Ctx& c) -> Out {
    Services& s = c.app.S();
    auto agent = c.qopt("agent");
    std::optional<std::string> target;
    if (agent) target = rag_target(*agent);
    return json{{"documents", s.rag->list(target)}, {"stats", s.rag->stats()}};
  });

  app.get("/api/rag/search", [rag_target](Ctx& c) -> Out {
    if (!c.has_q("q")) throw missing_field("query", "q");
    auto agent = c.qopt("agent");
    std::optional<std::string> target;
    if (agent) target = rag_target(*agent);
    return c.app.S().rag->search(c.q("q"), (int)c.qi("k", 6), 0.15, 0, target);
  });

  static const std::string RAG_FORMATS =
      "Formatos admitidos: PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, texto, código fuente y archivos "
      "ZIP, RAR, 7z o tar.gz";
  auto save_upload = [](const Upload& f, const fs::path& folder, size_t index) {
    std::string name = base_name(f.filename, "documento.txt");
    if (!rag::supported(name)) throw HttpErr(400, name + ": formato no admitido. " + RAG_FORMATS);
    fs::path tmp = folder / P(std::to_string(index) + "_" + name);
    if (!write_file(tmp, f.content)) throw HttpErr(500, "No se pudo guardar " + name);
    return std::make_pair(tmp, name);
  };

  app.get("/api/rag/formats", [](Ctx&) -> Out { return rag::formats_info(); });

  app.post("/api/rag/upload", [rag_target, save_upload](Ctx& c) -> Out {
    std::string target = rag_target(c.form("agent"));
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    fs::path folder = temp_folder();
    try {
      auto [tmp, name] = save_upload(*f, folder, 0);
      json doc = c.app.S().rag->ingest_file(tmp, "", name, json::object(), true, target);
      rm_rf(folder);
      return doc;
    } catch (const HttpErr&) {
      rm_rf(folder);
      throw;
    } catch (const std::exception& e) {
      rm_rf(folder);
      throw HttpErr(400, std::string("No se pudo indexar: ") + e.what());
    }
  });

  // varios documentos y/o comprimidos de una vez; agent: "" = grafo general, un id = solo ese agente
  app.post("/api/rag/import", [rag_target, save_upload](Ctx& c) -> Out {
    std::string target = rag_target(c.form("agent"));
    auto files = c.files("files");
    if (files.empty()) throw missing_field("body", "files");
    fs::path folder = temp_folder();
    json res;
    double t0 = now();
    try {
      std::vector<std::pair<fs::path, std::string>> items;
      for (size_t i = 0; i < files.size() && i < 200; ++i) items.push_back(save_upload(files[i], folder, i));
      App* a = &c.app;
      res = c.app.S().rag->ingest_many(items, [a](const json& e) { a->broadcast(e); }, target);
    } catch (...) {
      rm_rf(folder);
      throw;
    }
    rm_rf(folder);
    c.app.broadcast({{"type", "graph_update"}});
    int ok = 0, failed = 0;
    for (auto& r : res) {
      ok += r.contains("doc");
      failed += r.contains("error");
    }
    return json{{"results", res}, {"ok", ok}, {"failed", failed}, {"seconds", round_to(now() - t0, 2)}, {"agent", target}};
  });

  app.post("/api/rag/url", [](Ctx&) -> Out { not_in_cpp("Guardar una URL como PDF en la biblioteca"); });
  app.post("/api/rag/crawl", [](Ctx&) -> Out { not_in_cpp("Rastrear sitios de documentacion"); });
  app.get("/api/rag/crawls", [](Ctx&) -> Out { return json::array(); });
  app.post("/api/rag/crawls/{job_id}/resume", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });
  app.post("/api/rag/crawls/{job_id}/knowledge", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });
  app.get("/api/rag/crawls/{job_id}/knowledge", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });
  app.post("/api/rag/crawls/{job_id}/knowledge/import", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });
  app.post("/api/rag/domain-knowledge/import", [](Ctx&) -> Out { not_in_cpp("Importar paquetes de conocimiento de dominio"); });
  app.post("/api/rag/crawls/{job_id}/pause", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });
  app.del("/api/rag/crawls/{job_id}", [](Ctx&) -> Out { throw HttpErr(404, "Rastreo no encontrado"); });

  app.get("/api/rag/{doc_id}/file", [](Ctx& c) -> Out {
    json d = c.app.S().rag->get(c.pi("doc_id"));
    std::string path = d.is_object() ? jstr(d, "path") : "";
    if (path.empty() || !path_exists(P(path))) throw HttpErr(404, "Documento no disponible");
    std::string name = S(P(path).filename());
    auto us = name.find('_');
    return FileOut{P(path), us == std::string::npos ? name : name.substr(us + 1), ""};
  });

  app.patch("/api/rag/{doc_id}", [rag_target](Ctx& c) -> Out {
    json doc;
    try {
      doc = c.app.S().rag->assign(c.pi("doc_id"), rag_target(c.bs("agent", "")));
    } catch (const std::out_of_range&) {
      throw HttpErr(404, "Documento no encontrado");
    }
    c.app.broadcast({{"type", "graph_update"}});
    return doc;
  });

  app.del("/api/rag/{doc_id}", [](Ctx& c) -> Out {
    c.app.S().rag->remove(c.pi("doc_id"));
    c.app.broadcast({{"type", "graph_update"}});
    return json{{"ok", true}};
  });

  app.post("/api/memory/consolidate", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::function<std::string(const std::string&)> summarize;
    if (s.llm->has_real_llm())
      summarize = [&s](const std::string& p) {
        llm::Opts o;
        o.o = {{"max_tokens", 200}};
        return s.llm->complete(p, "default", o);
      };
    return json{{"summary", s.learner->consolidate(c.bs("session_id"), summarize)}};
  });

  // ------------------------------------------------------------------ multimedia
  app.post("/api/image/generate", [](Ctx& c) -> Out {
    std::string prompt = c.bs("prompt");
    std::optional<long long> seed;
    if (c.has_b("seed") && !c.body["seed"].is_null()) seed = (long long)jnum(c.body["seed"]);
    return c.app.S().images->generate(prompt, c.bs("negative", ""), std::nullopt, std::nullopt, seed);
  });

  app.post("/api/image/analyze", [](Ctx& c) -> Out {
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    return c.app.S().images->analyze(f->content, c.form("question"));
  });

  app.post("/api/music/generate", [](Ctx& c) -> Out {
    json res = c.app.S().music->generate(c.bs("prompt"), c.bn("seconds", 20));
    res.erase("path");
    return res;
  });

  app.get("/api/outputs", [](Ctx& c) -> Out {
    long long limit = c.qi("limit", 60);
    json out = json::object();
    for (auto* kind : {"images", "music", "docs"}) {
      auto files = glob(paths::sub({"outputs", kind}), "*.*");
      std::stable_sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return mtime(a) > mtime(b); });
      json list = json::array();
      for (auto& f : files) {
        std::string ext = f.extension().string();
        if (ext != ".png" && ext != ".jpg" && ext != ".wav" && ext != ".mid" && ext != ".pdf") continue;
        if ((long long)list.size() >= limit) break;
        list.push_back(std::string("/outputs/") + kind + "/" + S(f.filename()));
      }
      out[kind] = list;
    }
    return out;
  });

  app.post("/api/prompt/enhance", [](Ctx& c) -> Out {
    return prompt::enhance(c.app.S().llm.get(), c.bs("text"), c.bs("target", "image"), c.bs("style", ""),
                          c.bb("use_llm", true));
  });

  app.get("/api/prompt/styles", [](Ctx&) -> Out {
    json out = json::object();
    for (auto& [k, v] : prompt::STYLES)
      if (!k.empty()) out[k] = v;
    return out;
  });

  app.post("/api/avatar/upload", [](Ctx& c) -> Out {
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    std::string name = base_name(f->filename, "avatar.glb");
    std::string low = lower(name);
    if (!ends_with(low, ".glb") && !ends_with(low, ".gltf") && !ends_with(low, ".vrm"))
      throw HttpErr(400, "Formatos admitidos: GLB, GLTF (embebido) o VRM");
    if (f->content.size() > 200000000) throw HttpErr(400, "Modelo demasiado grande (max. 200 MB)");
    std::string fname = std::to_string((long long)now()) + "_" + safe_name(name, 1000);
    write_file(paths::sub({"outputs", "avatars"}) / P(fname), f->content);
    return json{{"url", "/outputs/avatars/" + fname}, {"size", f->content.size()}};
  });

  app.get("/api/voice", [](Ctx& c) -> Out { return c.app.S().voice->info(); });

  app.post("/api/voice/tts", [](Ctx& c) -> Out {
    auto res = c.app.S().voice->tts(c.bs("text"), c.bn("rate", 1.0));
    if (!res) return Raw{204, "", "", {{"X-TTS-Engine", "browser"}}};   // sin motor local: la voz del navegador
    return Raw{200, res->first, "audio/wav", {{"X-TTS-Engine", res->second}}};
  });

  app.post("/api/voice/stt", [](Ctx& c) -> Out {
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    std::string fn = f->filename.empty() ? "a.webm" : f->filename;
    auto dot = fn.rfind('.');
    std::string suffix = "." + (dot == std::string::npos ? fn : fn.substr(dot + 1));
    return c.app.S().voice->stt(f->content, suffix);
  });

  // ------------------------------------------------------------------ agentes
  app.get("/api/agents", [](Ctx& c) -> Out { return agents_json(c.app.S().agents->all()); });

  app.put("/api/agents", [](Ctx& c) -> Out {
    try {
      return c.app.S().agents->save(c.body).to_json();
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    } catch (const json::exception& e) {
      throw HttpErr(400, e.what());
    }
  }, true);

  app.del("/api/agents/{aid}", [](Ctx& c) -> Out {
    try {
      c.app.S().agents->remove(c.p("aid"));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    return json{{"ok", true}};
  }, true);

  // ------------------------------------------------------------------ oficina (perfiles de pyme)
  app.get("/api/office/profiles", [](Ctx& c) -> Out { return office::Office(c.app.S().config).list(); });

  app.post("/api/office/profile", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    office::Office o(s.config);
    const json& d = c.body.is_object() ? c.body : json::object();
    json prof = nullptr;
    try {
      if (truthy(d.value("id", json())))
        prof = o.apply(Ctx::as_str(d["id"]), truthy(d.value("reset_agents", json(false))));
      else
        o.clear();
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    s.agents->reload();
    for (auto& a : s.agents->all()) s.graph->forget_view(a.id);   // nombres nuevos: el ancla de cada subgrafo los sigue
    json seeded = o.seed_graphs(*s.graph, s.agents->ids());
    json packs = office::install_packs(s);
    return json{{"profile", prof}, {"seeded", seeded}, {"packs", packs}, {"agents", s.agents->ids()}};
  }, true);

  // ------------------------------------------------------------------ mesa de bolsa (oficina bolsa-inversiones)
  auto B = [](auto fn) -> Out {
    try {
      return fn(bolsa::get());
    } catch (const bolsa::PermissionDenied& e) {
      throw HttpErr(403, e.what());
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  };
  app.get("/api/bolsa", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      long long cid = c.qi("cartera", 0);
      json res = b.resumen(cid ? json(cid) : json(nullptr));
      return json{{"aviso", bolsa::AVISO},       {"confirmacion", bolsa::CONFIRMACION}, {"mercados", b.mercados()},
                  {"carteras", b.carteras()},    {"tarifas", bolsa::tarifas_json()},   {"tipos", bolsa::ORDER_TYPES},
                  {"resumen", res},              {"ordenes", b.ordenes(res["cartera"])},
                  {"operaciones", b.operaciones(res["cartera"], 50)}};
    });
  });
  app.get("/api/bolsa/instrumentos", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return b.instrumentos(c.q("q")); });
  });
  app.get("/api/bolsa/cotizacion/{simbolo}", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return merged(b.precio(c.p("simbolo")), {{"ticks", b.ticks(c.p("simbolo"))}}); });
  });
  app.post("/api/bolsa/costes", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      json q = b.precio(c.bs("simbolo", ""));
      std::string lado = c.bs("lado", "compra");
      json pin = c.b("precio", nullptr);
      double px = truthy(pin) ? bolsa::num(pin) : jnum(lado == "compra" ? q["venta"] : q["compra"]);
      json cart = c.b("cartera", nullptr);
      json cant = c.b("cantidad", nullptr);
      return b.costes(truthy(cart) ? cart : json(nullptr), jstr(q, "simbolo"), lado, truthy(cant) ? bolsa::num(cant) : 0.0, px);
    });
  });
  app.post("/api/bolsa/orden", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      json cart = c.b("cartera", nullptr);
      return b.ordenar(c.bs("simbolo", ""), c.bs("lado", ""), c.b("cantidad", 0), c.bs("tipo", "mercado"),
                       c.b("limite", nullptr), c.b("stop", nullptr), c.bs("validez", "dia"),
                       truthy(cart) ? cart : json(nullptr), "usuario");
    });
  });
  app.post("/api/bolsa/orden/{oid}/cancelar", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return b.cancelar(c.pi("oid")); });
  });
  app.post("/api/bolsa/orden/{oid}/confirmar", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return b.confirmar(c.pi("oid"), c.bs("confirmacion", "")); });
  }, true);
  app.post("/api/bolsa/orden/{oid}/ejecucion", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      json com = c.b("comision_eur", nullptr), fx = c.b("fx", nullptr);
      return b.registrar_ejecucion(c.pi("oid"), c.b("precio", 0), truthy(com) ? com : json(0),
                                   c.b("impuestos_eur", nullptr), truthy(fx) ? fx : json(nullptr));
    });
  }, true);
  app.post("/api/bolsa/cartera", [B](Ctx& c) -> Out {
    if (c.bs("modo", "") == "real" && !c.admin) throw HttpErr(403, "Una cartera real requiere modo administrador");
    return B([&](bolsa::Bolsa& b) {
      return b.crear_cartera(str_or(c.b("nombre", nullptr), "Cartera simulada"), c.b("capital", 10000),
                             str_or(c.b("tarifa", nullptr), "broker-online"), str_or(c.b("modo", nullptr), "simulado"));
    });
  });
  app.post("/api/bolsa/cartera/{cid}/reiniciar", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return b.reiniciar(c.pi("cid"), c.b("capital", nullptr)); });
  });
  app.post("/api/bolsa/instrumento", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      auto orv = [&](const char* k, json d) { json v = c.b(k, nullptr); return truthy(v) ? v : d; };
      return b.anadir_instrumento(c.bs("simbolo", ""), c.bs("nombre", ""), c.bs("mercado", ""), c.b("precio", 0),
                                  c.bs("divisa", ""), str_or(c.b("tipo", nullptr), "acción"), orv("vol", 0.25),
                                  orv("spread_bps", 5), orv("adv", 1e6), orv("impuesto_pct", 0));
    });
  });
  app.post("/api/bolsa/importar", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) { return b.importar_csv(c.bs("simbolo", ""), c.bs("csv", "")); });
  });
  app.get("/api/bolsa/fiscal", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      long long cid = c.qi("cartera", 0), anio = c.qi("anio", 0);
      return b.informe_fiscal(cid ? json(cid) : json(nullptr), anio ? json(anio) : json(nullptr));
    });
  });

  // ------------------------------------------------------------------ herramientas
  app.get("/api/tools", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    return json{{"tools", s.tools->list()}, {"policy", s.config.get("tools.policy")}, {"pending", s.supervisor->list_pending()}};
  });

  app.post("/api/tools/{name}/enabled", [](Ctx& c) -> Out {
    c.app.S().tools->set_enabled(c.p("name"), truthy(c.b("enabled")));
    return json{{"ok", true}};
  }, true);

  app.post("/api/approvals/{pid}", [](Ctx& c) -> Out {
    std::string pid = c.p("pid");
    bool approve = truthy(c.b("approve"));
    bool ok = c.app.S().supervisor->resolve(pid, approve, c.admin);
    c.app.broadcast({{"type", "approval_resolved"}, {"id", pid}, {"approved", approve}});
    return json{{"ok", ok}};
  });

  app.get("/api/tools/audit", [](Ctx& c) -> Out { return c.app.S().supervisor->audit_log((int)c.qi("limit", 100)); }, true);

  app.get("/api/proposals", [](Ctx& c) -> Out {
    json items = c.app.S().proposals->list(c.q("status"));
    if (!c.admin)
      for (auto& p : items) p.erase("code");
    return items;
  });

  app.post("/api/proposals", [](Ctx& c) -> Out {
    const json& d = c.body.is_object() ? c.body : json::object();
    try {
      for (auto* k : {"name", "code"})
        if (!d.contains(k)) throw std::out_of_range(k);
      return c.app.S().proposals->create(Ctx::as_str(d["name"]), Ctx::as_str(d.value("description", json(""))),
                                         Ctx::as_str(d["code"]), d.value("parameters", json()),
                                         Ctx::as_str(d.value("risk", json("medium"))),
                                         Ctx::as_str(d.value("rationale", json(""))), "admin");
    } catch (const std::out_of_range& e) {
      throw HttpErr(400, key_msg(e));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  }, true);

  app.put("/api/proposals/{pid}", [](Ctx& c) -> Out { return c.app.S().proposals->update_code(c.p("pid"), c.bs("code")); },
          true);

  app.post("/api/proposals/{pid}/test", [](Ctx& c) -> Out {
    return c.app.S().proposals->test(c.p("pid"), c.b("args", json::object()));
  }, true);

  app.post("/api/proposals/{pid}/approve", [](Ctx& c) -> Out {
    try {
      return c.app.S().proposals->approve(c.p("pid"), "admin", c.bs("note", ""));
    } catch (const std::out_of_range& e) {
      throw HttpErr(400, key_msg(e));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  }, true);

  app.post("/api/proposals/{pid}/reject", [](Ctx& c) -> Out { return c.app.S().proposals->reject(c.p("pid"), c.bs("reason")); },
           true);

  app.del("/api/tools/community/{name}", [](Ctx& c) -> Out { return json{{"ok", c.app.S().proposals->uninstall(c.p("name"))}}; },
          true);

  // ------------------------------------------------------------------ modelos
  app.get("/api/models", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json jobs = json::array();
    for (json items_tmp_ = s.models->jobs(); auto& [_, j] : items_tmp_.items()) jobs.push_back(j);
    json sources = json::array();
    for (auto* id : {"huggingface", "ollama", "modelscope", "civitai", "kaggle"}) {   // orden de la version Python
      auto src = s.models->sources.find(id);
      if (src != s.models->sources.end())
        sources.push_back({{"id", id}, {"name", src->second->name()}, {"url", SOURCE_URLS.at(id)}});
    }
    json ollama = json::array();
    if (s.llm->is_available("ollama"))
      if (auto b = s.llm->backend("ollama")) ollama = b->models();
    return json{{"catalog", s.models->catalog.list()}, {"active", s.models->active()}, {"jobs", jobs},
                {"sources", sources}, {"stt_models", data::get("stt_models")}, {"ollama", ollama}};
  });

  app.get("/api/models/recommended", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    return json{{"items", s.models->recommended()},
                {"profile", merged({{"tier", s.profile.value("tier", json())}}, s.profile.value("llm", json::object()))}};
  });

  app.get("/api/models/search", [](Ctx& c) -> Out {
    std::string source = c.q("source");
    for (auto* k : {"source", "q"})
      if (!c.has_q(k)) throw missing_field("query", k);
    try {
      return c.app.S().models->search(source, c.q("q"), c.q("kind"));
    } catch (const std::out_of_range& e) {
      throw HttpErr(502, source + ": " + key_msg(e));
    } catch (const std::exception& e) {
      throw HttpErr(502, source + ": " + e.what());
    }
  });

  app.get("/api/models/files", [](Ctx& c) -> Out {
    std::string source = c.q("source");
    for (auto* k : {"source", "repo"})
      if (!c.has_q(k)) throw missing_field("query", k);
    try {
      return c.app.S().models->files(source, c.q("repo"));
    } catch (const std::out_of_range& e) {
      throw HttpErr(502, source + ": " + key_msg(e));
    } catch (const std::exception& e) {
      throw HttpErr(502, source + ": " + e.what());
    }
  });

  // cualquier modelo de cualquier repositorio (las recomendaciones son orientativas)
  app.post("/api/models/download", [](Ctx& c) -> Out {
    return json{{"job", c.app.S().models->download(c.bs("source"), c.bs("repo"), c.bs("file", ""), c.bs("kind", ""),
                                                   c.bb("extras", true))}};
  });

  app.post("/api/models/download-repo", [](Ctx& c) -> Out {
    std::string source = c.bs("source"), repo = c.bs("repo");
    if (source != "huggingface" && source != "modelscope")
      throw HttpErr(400, "La descarga de repositorios completos esta disponible para Hugging Face y ModelScope");
    return json{{"job", c.app.S().models->download_repo(source, repo, c.bs("kind", ""))}};
  });

  app.post("/api/models/recommended/{rid}", [](Ctx& c) -> Out {
    return json{{"job", c.app.S().models->download_recommended(c.p("rid"))}};
  });

  app.post("/api/models/cancel/{jid}", [](Ctx& c) -> Out {
    c.app.S().models->cancel(c.p("jid"));
    return json{{"ok", true}};
  });

  app.post("/api/models/import", [](Ctx& c) -> Out {
    std::string path = c.bs("path");
    try {
      return c.app.S().models->import_local(path, c.bb("copy_file", false), c.bs("kind", ""));
    } catch (const std::runtime_error& e) {
      if (starts_with(e.what(), "No existe")) throw HttpErr(404, e.what());
      throw;
    }
  }, true);

  app.post("/api/models/activate/{mid}", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json entry;
    try {
      entry = s.models->activate(c.p("mid"));
    } catch (const std::out_of_range& e) {
      throw HttpErr(400, key_msg(e));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    s.llm->rebuild();
    return entry;
  });

  // activa un modelo externo: 'ollama:qwen3-vl:2b', 'openai:gpt-4o-mini'
  app.post("/api/models/activate-spec", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.config.set("llm.default_model", c.bs("spec"));
    s.llm->rebuild();
    return s.llm->status();
  }, true);

  app.del("/api/models/{mid}", [](Ctx& c) -> Out {
    c.app.S().models->remove(c.p("mid"), c.qb("files", false));
    return json{{"ok", true}};
  }, true);

  // ------------------------------------------------------------------ cooperativo (pendiente en C++)
  app.get("/api/coop", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json addresses = json::array();
    for (auto& ip : net::lan_ips()) addresses.push_back({{"iface", ""}, {"ip", ip}, {"kind", "lan"}});
    return json{{"running", false}, {"node_id", ""}, {"name", s.config.get("coop.node_name", "")}, {"vip", ""},
                {"network_id", ""}, {"configured", false}, {"listen", false}, {"port", s.config.get("coop.listen_port", 8766)},
                {"links", json::array()}, {"outbound", json::object()}, {"peers", json::array()}, {"chat", json::array()},
                {"addresses", addresses}, {"available", false},
                {"detail", "La red COOP todavia no esta disponible en la edicion C++ (2.0 beta C)."}};
  });
  for (auto* p : {"/api/coop/join", "/api/coop/start", "/api/coop/stop", "/api/coop/chat", "/api/coop/ping/{node}",
                  "/api/coop/agent"})
    app.post(p, [](Ctx&) -> Out { not_in_cpp("La red COOP"); });
  for (auto* p : {"/api/coop/create", "/api/coop/share-proposal/{pid}", "/api/coop/share-facts"})
    app.post(p, [](Ctx&) -> Out { not_in_cpp("La red COOP"); }, true);
  app.get("/api/coop/invite", [](Ctx&) -> Out { not_in_cpp("La red COOP"); }, true);

  // ------------------------------------------------------------------ acceso remoto y VPN (pendiente en C++)
  app.post("/api/remote/pair", [](Ctx&) -> Out { not_in_cpp("El acceso remoto por internet"); });
  for (auto* p : {"/api/remote", "/api/remote/router"})
    app.get(p, [](Ctx&) -> Out { not_in_cpp("El acceso remoto por internet"); }, true);
  for (auto* p : {"/api/remote/enable", "/api/remote/pairing", "/api/remote/upnp", "/api/remote/public-ip",
                  "/api/remote/check", "/api/remote/certificate"})
    app.post(p, [](Ctx&) -> Out { not_in_cpp("El acceso remoto por internet"); }, true);
  app.del("/api/remote/devices/{dev_id}", [](Ctx&) -> Out { not_in_cpp("El acceso remoto por internet"); }, true);
  for (auto* p : {"/api/vpn", "/api/vpn/server.conf"}) app.get(p, [](Ctx&) -> Out { not_in_cpp("La VPN cifrada"); }, true);
  for (auto* p : {"/api/vpn/enable", "/api/vpn/peers", "/api/vpn/apply", "/api/vpn/upnp"})
    app.post(p, [](Ctx&) -> Out { not_in_cpp("La VPN cifrada"); }, true);
  app.del("/api/vpn/peers/{peer_id}", [](Ctx&) -> Out { not_in_cpp("La VPN cifrada"); }, true);
}

}  // namespace nx::server
