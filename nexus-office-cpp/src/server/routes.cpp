// Rutas de la API (server/app.py): mismas URLs, mismos parametros y mismos JSON que la version Python, en el mismo
// orden.
#include <httplib.h>

#include <algorithm>
#include <csignal>
#include <thread>

#include "agents/agents.h"
#include "coop/coop.h"
#include "remote/remote.h"
#include "core/service.h"
#include "core/android_tune.h"
#include "core/services.h"
#include "core/system.h"
#include "core/explorador.h"
#include "gen/data.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "llm/motores.h"
#include "hardware/hw_profiles.h"
#include "media/avatares.h"
#include "rag/browser.h"
#include "media/media.h"
#include "memory/memory.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "office/bolsa.h"
#include "office/bolsa_agente.h"
#include "devices/devices.h"
#include "core/salud.h"
#include "devices/drivers.h"
#include "devices/firmware.h"
#include "agents/activity.h"
#include "memory/kg.h"
#include "memory/kg_jobs.h"
#include "rag/web.h"
#include "util/rx.h"
#include "office/office.h"
#include "rag/rag.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/archive.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

namespace nx::server {

namespace {

auto L = log::get("server");


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
    // paquete para instalar Nexus-Office en Android/Termux desde este equipo: el ejecutable Android de esta instalacion
    // (con sus bibliotecas) y los recursos; en un PC no hay binario Android que dar
#ifdef __ANDROID__
    std::vector<archive::TarFile> files;
    fs::path exe = paths::exe_path(), dir = exe.parent_path();
    files.push_back({"Nexus-Office/nexus-office", read_text(exe), 0755});
    for (auto& so : glob(dir, "*.so")) files.push_back({"Nexus-Office/" + S(so.filename()), read_text(so), 0755});
    for (auto& f : glob(paths::package(), "*", true))
      if (is_file(f) && !contains(S(f), "__pycache__"))
        files.push_back({"Nexus-Office/nexus/" + S(fs::relative(f, paths::package()).generic_u8string()), read_text(f)});
    files.push_back({"Nexus-Office/install-termux.sh",
                     "#!/data/data/com.termux/files/usr/bin/sh\n# Nexus-Office (edicion C++) en Termux\nset -e\n"
                     "cd \"$(dirname \"$0\")\"\nchmod +x nexus-office\n"
                     "printf '#!/data/data/com.termux/files/usr/bin/sh\\ncd \"%s\"\\nLD_LIBRARY_PATH=\"%s:$LD_LIBRARY_PATH\" exec ./nexus-office \"$@\"\\n' "
                     "\"$(pwd)\" \"$(pwd)\" > start.sh\nchmod +x start.sh\n"
                     "echo 'Listo. Arranca con: ./start.sh  (y abre http://127.0.0.1:8775 en el navegador)'\n",
                     0755});
    return Raw{200, archive::write_targz(files), "application/gzip",
               {{"Content-Disposition", std::string("attachment; filename=Nexus-Office-") + NEXUS_VERSION + "-termux.tar.gz"}}};
#else
    throw HttpErr(501, "Este equipo no lleva el binario de Android: en Termux descarga el paquete de Android "
                       "(Nexus-Office-...-android-arm64) de la pagina de descargas o instala la APK de Nexus-Office.");
#endif
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
                {"coop", {{"running", s.coop->running()}, {"peers", s.coop->peer_count()}, {"vip", s.coop->vip()}}},
                {"pending_approvals", s.supervisor->list_pending().size()},
                {"pending_proposals", s.proposals->list("pendiente").size()},
                {"suggest_smaller_model", s.profile.value("llm", json::object()).value("suggest_smaller_model", false)},
                {"perfil_hw", [&] {
                   std::string a = s.config.str("hardware.perfil", "auto");
                   json p = hw::profiles::get(s.config, a.empty() ? "auto" : a);
                   return p.is_object() ? jstr(p, "nombre") : std::string();
                 }()},
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

  // ------------------------------------------------------------------ IA online (agente Profesor)
  auto cloud = [](Ctx& c) { return std::static_pointer_cast<llm::CloudBackend>(c.app.S().llm->backend("nube")); };
  app.get("/api/nube", [cloud](Ctx& c) -> Out { return cloud(c)->status(); });

  app.put("/api/nube", [cloud](Ctx& c) -> Out {
    Config& cfg = c.app.S().config;
    if (!c.body.is_object()) throw HttpErr(422, "Se esperaba un objeto JSON");
    auto known = [](const std::string& id) {
      for (auto& p : llm::cloud_providers())
        if (p.id == id) return true;
      return false;
    };
    std::string prov = c.body.contains("proveedor") ? jstr(c.body, "proveedor") : cfg.str("nube.proveedor");
    if (!prov.empty() && !known(prov)) {
      std::vector<std::string> ids;
      for (auto& p : llm::cloud_providers()) ids.push_back(p.id);
      throw HttpErr(400, "proveedor desconocido: " + prov + ". Validos: " + join(ids, ", "));
    }
    if (c.body.contains("esfuerzo") &&
        std::find(llm::CLOUD_EFFORTS.begin(), llm::CLOUD_EFFORTS.end(), jstr(c.body, "esfuerzo")) ==
            llm::CLOUD_EFFORTS.end())
      throw HttpErr(400, "esfuerzo no valido. Validos: " + join(llm::CLOUD_EFFORTS, ", "));
    cfg.set("nube.proveedor", prov, false);
    for (const char* k : {"modelo", "base_url", "esfuerzo"})
      if (c.body.contains(k)) cfg.set(std::string("nube.") + k, trim(jstr(c.body, k)), false);
    std::string clave = trim(jstr(c.body, "clave"));
    if (!prov.empty() && !clave.empty() && clave != "********") cfg.set("nube.clave_" + prov, clave, false);
    if (c.body.contains("borrar_claves") && c.body["borrar_claves"].is_array())
      for (auto& p : c.body["borrar_claves"])
        if (p.is_string() && known(p.get<std::string>())) cfg.set("nube.clave_" + p.get<std::string>(), "", false);
    cfg.save();
    return cloud(c)->status();
  }, true);

  app.post("/api/nube/probar", [cloud](Ctx& c) -> Out {
    return cloud(c)->test(c.body.is_object() ? jstr(c.body, "proveedor") : "");
  }, true);

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

  // app Android autonoma: nucleos, hilos medidos, limites por temperatura o bateria y consejo de modelo
  auto tuner = [](Ctx& c) -> android_tune::LiveTuner& {
    auto& t = c.app.S().android_tuner;
    if (!t) throw HttpErr(404, "El autoajuste en vivo solo funciona en la app Android autonoma");
    return *t;
  };
  app.get("/api/android/autoajuste", [tuner](Ctx& c) -> Out { return tuner(c).report(); });
  // vuelve a medir los hilos desde cero (p. ej. tras cambiar de funda o de ajustes de rendimiento)
  app.post("/api/android/autoajuste/medir", [tuner](Ctx& c) -> Out {
    auto& t = tuner(c);
    t.reset_trials();
    return t.step();
  });
  // acepta el consejo de modelo: lo activa, o lo descarga y lo activa al terminar
  app.post("/api/android/autoajuste/modelo/{mid}", [tuner](Ctx& c) -> Out {
    auto& t = tuner(c);
    try {
      return t.switch_model(c.p("mid"));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  });

  app.post("/api/hardware/redetect", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.hw = hw::detect(s.config.str("llm.ollama_url", "http://127.0.0.1:11434"));
    hw::save(s.hw);
    s.profile = hw::build_profile(s.hw, s.config.str("hardware.profile_override"));
    hw::profiles::aplicar_al_perfil(s);
    s.llm->rebuild();
    return json{{"hardware", s.hw.to_json()}, {"profile", s.profile}};
  }, true);

  // selector de inicio: motor llama y acelerador (CPU/GPU/NPU) con su disponibilidad y lo que se usa de verdad
  app.get("/api/accel", [](Ctx& c) -> Out { return sys2::accel_options(c.app.S()); });

  app.post("/api/accel", [](Ctx& c) -> Out {
    json res;
    try {
      res = sys2::accel_apply(c.app.S(), c.bs("engine", "auto"), c.bs("accel", "auto"), c.bb("remember", true),
                              c.b("capas", nullptr), c.bs("runtime", "auto"));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "status_changed"}});
    return res;
  });

  // lectura de webs con JavaScript: modo (auto, siempre, nunca) y navegador encontrado en el equipo
  app.get("/api/rag/navegador", [](Ctx&) -> Out { return rag::browser::info(); });

  // perfiles de hardware (de serie y propios), el activo, las GPU que hay y los modelos de texto
  app.get("/api/hw/perfiles", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json modelos = json::array();
    for (auto& m : s.models->catalog.list())
      if (jstr(m, "kind") == "llm")
        modelos.push_back({{"id", m["id"]}, {"name", m.value("name", m["id"])},
                           {"size_gb", round_to(jnum(m.value("size", json(0))) / 1073741824.0, 1)}});
    json activo = s.models->active()["llm"];
    return json{{"perfiles", hw::profiles::lista(s.config)}, {"activo", s.config.get("hardware.perfil", "auto")},
                {"gpu", s.config.get("hardware.gpu", "")}, {"gpus", hw::profiles::gpus(s)}, {"modelos", modelos},
                {"modelo_activo", truthy(activo) ? activo : json("")},
                {"llm", s.profile["llm"].value("perfil_hw", json::object())}, {"campos", hw::profiles::CAMPOS}};
  });
  app.put("/api/hw/perfiles/{pid}", [](Ctx& c) -> Out {
    std::string pid = c.p("pid");
    json perfil = c.body.is_object() ? c.body : json::object();
    for (auto& d : hw::profiles::DE_SERIE)
      if (jstr(d, "id") == pid && !truthy(perfil.value("nombre", json()))) perfil["nombre"] = pid;
    perfil["id"] = pid;
    return hw::profiles::guardar(c.app.S().config, perfil);
  });
  app.del("/api/hw/perfiles/{pid}", [](Ctx& c) -> Out {
    if (!hw::profiles::borrar(c.app.S().config, c.p("pid"))) throw HttpErr(404, "perfil propio no encontrado");
    return json{{"ok", true}};
  });
  app.post("/api/hw/perfiles/{pid}/activar", [](Ctx& c) -> Out {
    json res;
    try {
      res = hw::profiles::activar(c.p("pid"), c.app.S().config, &c.app.S());
    } catch (const std::invalid_argument& e) {
      throw HttpErr(404, e.what());
    }
    c.app.broadcast({{"type", "status_changed"}});
    return res;
  });
  // elige que GPU usar cuando hay varias (vacio = la mejor) y reinicia el modelo en ella
  app.post("/api/hw/gpu", [](Ctx& c) -> Out {
    json res = hw::profiles::elegir_gpu(c.app.S(), c.bs("gpu", ""));
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
    json motores_rt = motores::estado();
    std::string variant;
    if (auto b = std::dynamic_pointer_cast<llm::LlamaServerBackend>(s.llm->backend("llamaserver"))) variant = b->variant;
    return json{{"installed", s.runtimes->installed()},
                {"progress", s.runtimes->progress()},
                {"bundled", bundled},
                {"recommended", recommended},
                {"descriptions", {{"llama", "LLM / multimodal (llama-server)"}, {"sd", "Generacion de imagen (sd.cpp)"}}},
                {"active", {{"llama", variant}}},
                {"motores", motores_rt}};
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

  // paquete de runtimes sin conexion (Nexus-Office-runtime-*.zip de la Release) ya copiado al equipo
  app.post("/api/runtimes/importar", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json v;
    try {
      v = s.runtimes->import_pack(P(c.bs("ruta")));
    } catch (const std::exception& e) {
      throw HttpErr(400, e.what());
    }
    s.llm->rebuild();
    return json{{"importados", v}};
  }, true);

  // ------------------------------------------------------------------ sesiones / memoria
  app.get("/api/sessions", [](Ctx& c) -> Out { return json(c.app.S().memory->sessions()); });
  app.get("/api/sessions/{sid}", [](Ctx& c) -> Out { return json(c.app.S().memory->history(c.p("sid"), 1000)); });
  app.del("/api/sessions/{sid}", [](Ctx& c) -> Out {
    c.app.S().memory->delete_session(c.p("sid"));
    c.app.S().hilo->olvidar(c.p("sid"));
    return json{{"ok", true}};
  });
  // hilo de la conversacion: resumen progresivo y notas fijas del proyecto
  app.get("/api/sessions/{sid}/hilo", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    json st = s.hilo->estado(c.p("sid"));
    st["n_ctx"] = s.llm->llm_profile().value("n_ctx", json());
    return st;
  });
  app.put("/api/sessions/{sid}/hilo", [](Ctx& c) -> Out {
    if (!c.body.is_object()) throw HttpErr(422, "Se esperaba un objeto JSON");
    json cambios = json::object();
    for (const char* k : {"notas", "resumen"})
      if (c.body.contains(k)) cambios[k] = uleft(c.bs(k), 20000);
    Services& s = c.app.S();
    s.memory->ensure_session(c.p("sid"));
    s.hilo->guardar(c.p("sid"), cambios);
    return s.hilo->estado(c.p("sid"));
  });
  app.post("/api/sessions/{sid}/hilo/resumir", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    int n_ctx = (int)jnum(s.llm->llm_profile().value("n_ctx", json(4096)), 4096);
    int budget = std::max(512, n_ctx * 45 / 100);
    bool todo = truthy(c.b("todo", false));   // todo menos los dos ultimos mensajes
    int n = s.hilo->plegar(c.p("sid"), todo ? 0 : budget / 2, budget * 3 / 10, s.hilo->llm_complete);
    json st = s.hilo->estado(c.p("sid"));
    st["plegados_ahora"] = n;
    return st;
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

  // peticion explicita del usuario desde la Biblioteca: URL -> PDF -> RAG -> conocimiento
  app.post("/api/rag/url", [rag_target](Ctx& c) -> Out {
    std::string url = c.bs("url");
    std::string target = rag_target(c.bs("agent", ""));
    App* a = &c.app;
    json res;
    try {
      res = rag::process_url(c.app.S(), url, [a](const json& e) { a->broadcast(e); }, 8, target);
    } catch (const HttpErr&) {
      throw;
    } catch (const std::exception& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return res;
  });

  // rastreo de un sitio de documentacion; background=true (o todo el dominio): por lotes reanudables
  app.post("/api/rag/crawl", [rag_target](Ctx& c) -> Out {
    std::string url = c.bs("url"), scope = c.bs("scope", "path");
    int max_pages = (int)c.bn("max_pages", 40), max_depth = (int)c.bn("max_depth", 3), batch_pages = (int)c.bn("batch_pages", 0);
    bool background = c.bb("background", false);
    if (scope != "path" && scope != "domain") throw HttpErr(400, "scope debe ser 'path' o 'domain'");
    if (!re_search(R"(^https?://)", url, Rx::I)) throw HttpErr(400, "la URL debe empezar por http:// o https://");
    std::string target = rag_target(c.bs("agent", ""));
    App* a = &c.app;
    rag::Emit emit = [a](const json& e) { a->broadcast(e); };
    if (background || scope == "domain") {   // dominio completo: siempre por lotes, hasta agotar el dominio
      auto [pages, chars] = rag::crawl_jobs::auto_batch(c.app.S().profile);
      json job = rag::crawl_jobs::create(url, max_pages, max_depth, scope, target, true,
                                         std::clamp(batch_pages ? batch_pages : pages, 1, 500), chars);
      rag::crawl_jobs::run_background(c.app.S(), job["id"], emit);
      return json{{"job", rag::crawl_jobs::pub(rag::crawl_jobs::get(job["id"]))}};
    }
    json res;
    try {
      res = rag::crawler::crawl_site(c.app.S(), url, max_pages, max_depth, scope, emit, target);
    } catch (const std::exception& e) {
      throw HttpErr(400, e.what());
    }
    c.app.broadcast({{"type", "graph_update"}});
    return res;
  });

  auto crawl_job = [](const std::string& id) {
    json job = rag::crawl_jobs::get(id);
    if (job.is_null()) throw HttpErr(404, "Rastreo no encontrado");
    return job;
  };
  app.get("/api/rag/crawls", [](Ctx&) -> Out { return rag::crawl_jobs::list_jobs(); });
  app.post("/api/rag/crawls/{job_id}/resume", [crawl_job](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    json job = crawl_job(id);
    App* a = &c.app;
    if (jstr(job, "status") != rag::crawl_jobs::DONE && !rag::crawl_jobs::is_running(id)) {
      try {
        rag::crawl_jobs::run_background(c.app.S(), id, [a](const json& e) { a->broadcast(e); });
      } catch (const std::runtime_error&) {   // ya estaba en marcha
      }
    }
    return rag::crawl_jobs::pub(rag::crawl_jobs::get(id));
  });
  app.post("/api/rag/crawls/{job_id}/knowledge", [crawl_job](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    json job = crawl_job(id);
    if (!path_exists(rag::crawl_jobs::kg_path(id)))
      throw HttpErr(400, "Este rastreo no tiene grafo de dominio (solo los de «todo el dominio»)");
    App* a = &c.app;
    try {
      return rag::crawl_jobs::build_knowledge(c.app.S(), job, [a](const json& e) { a->broadcast(e); });
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  });
  app.get("/api/rag/crawls/{job_id}/knowledge", [crawl_job](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    json job = crawl_job(id);
    fs::path f = rag::crawl_jobs::knowledge_path(id);
    if (!path_exists(f)) throw HttpErr(404, "El grafo del dominio aun no esta construido");
    std::string site = jstr(job, "site").empty() ? id : jstr(job, "site");
    return FileOut{f, uleft(re_sub("[^A-Za-z0-9._-]+", "-", site), 60) + "-conocimiento.zip", "application/zip"};
  });
  app.post("/api/rag/crawls/{job_id}/knowledge/import", [crawl_job, rag_target](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    crawl_job(id);
    fs::path f = rag::crawl_jobs::knowledge_path(id);
    if (!path_exists(f)) throw HttpErr(404, "El grafo del dominio aun no esta construido");
    std::string target = rag_target(c.bs("agent", ""));
    json res = rag::domain_kg::import_package(c.app.S(), rag::domain_kg::read_package(f), target);
    c.app.broadcast({{"type", "graph_update"}});
    return res;
  });
  // paquete de conocimiento de dominio (.zip) hecho en este u otro Nexus-Office
  app.post("/api/rag/domain-knowledge/import", [rag_target](Ctx& c) -> Out {
    auto f = c.file("file");
    if (!f) throw missing_field("body", "file");
    std::string target = rag_target(c.form("agent"));
    fs::path tmp = paths::sub({"cache"}) / P("dk_" + token_hex(6) + ".zip");
    if (!write_file(tmp, f->content)) throw HttpErr(500, "No se pudo guardar el paquete");
    json data;
    try {
      data = rag::domain_kg::read_package(tmp);
    } catch (const std::exception& e) {
      remove_file(tmp);
      throw HttpErr(400, std::string("No es un paquete de conocimiento de dominio: ") + e.what());
    }
    remove_file(tmp);
    json res = rag::domain_kg::import_package(c.app.S(), data, target);
    c.app.broadcast({{"type", "graph_update"}});
    return res;
  });
  app.post("/api/rag/crawls/{job_id}/pause", [crawl_job](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    crawl_job(id);
    return json{{"stopping", rag::crawl_jobs::stop(id)}};
  });
  app.del("/api/rag/crawls/{job_id}", [](Ctx& c) -> Out {
    std::string id = c.p("job_id");
    if (rag::crawl_jobs::stop(id))   // espera (hasta 30 s) a que el lote en curso pare
      for (int i = 0; i < 300 && rag::crawl_jobs::is_running(id); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!rag::crawl_jobs::remove(c.app.S(), id, c.qb("docs", false))) throw HttpErr(404, "Rastreo no encontrado");
    c.app.broadcast({{"type", "graph_update"}});
    return json{{"deleted", id}};
  });

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

  // avatares 3D gratuitos de Internet (Open Source Avatars, modelos de referencia, Sketchfab)
  auto avs = [](Ctx& c) -> avatares::Avatares& {
    Services* s = &c.app.S();
    return avatares::get([s] { return s->config.str("avatares.sketchfab_token"); });
  };
  app.get("/api/avatares", [avs](Ctx& c) -> Out {
    return json{{"biblioteca", avs(c).biblioteca()}, {"fuentes", avatares::FUENTES},
                {"sketchfab_clave", !c.app.S().config.str("avatares.sketchfab_token").empty()}};
  });
  app.get("/api/avatares/buscar", [avs](Ctx& c) -> Out {
    std::string fuente = c.q("fuente", "osa");
    try {
      return avs(c).buscar(fuente, c.q("q"), c.qi("pagina", 1), c.qi("por_pagina", 24));
    } catch (const std::runtime_error& e) {   // HttpError o una respuesta que no es JSON
      throw HttpErr(502, "No se pudo consultar " + fuente + ": " + e.what());
    }
  });
  app.post("/api/avatares/descargar", [avs](Ctx& c) -> Out { return avs(c).descargar(c.bs("id", "")); });
  app.get("/api/avatares/trabajo/{tid}", [avs](Ctx& c) -> Out {
    json j = avs(c).trabajo(c.p("tid"));
    if (j.is_null()) throw HttpErr(404, "trabajo desconocido");
    return j;
  });
  app.post("/api/avatares/{aid}/usar", [avs](Ctx& c) -> Out {
    for (auto& it : avs(c).biblioteca())
      if (jstr(it, "id") == c.p("aid")) {
        c.app.S().config.set("user.avatar_model", it["url"]);
        c.app.S().config.set("user.avatar_style", "glb");
        return json{{"ok", true}, {"url", it["url"]}};
      }
    throw HttpErr(404, "avatar no descargado");
  });
  app.del("/api/avatares/{aid}", [avs](Ctx& c) -> Out {
    if (!avs(c).borrar(c.p("aid"))) throw HttpErr(404, "avatar no descargado");
    return json{{"ok", true}};
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
  app.get("/api/bolsa/serie/{simbolo}", [B](Ctx& c) -> Out {   // serie real (o simulada sin conexion) para graficos
    return B([&](bolsa::Bolsa& b) { return b.serie(c.p("simbolo"), c.q("rango", "1d")); });
  });
  // seguimiento en vivo de una orden: precio actual, serie y niveles (limite, stop, ejecucion, objetivo)
  app.get("/api/bolsa/orden/{oid}/grafico", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa& b) {
      long long oid = c.pi("oid");
      json o = b.orden(oid);
      if (jstr(o, "estado") == "pendiente") b.procesar(oid);
      o = b.orden(oid);
      json q = b.precio(jstr(o, "simbolo"));
      json niveles = json::array();
      if (truthy(o.value("limite", json()))) niveles.push_back({{"tipo", "limite"}, {"precio", o["limite"]}, {"texto", "Límite"}});
      if (truthy(o.value("stop", json())))
        niveles.push_back({{"tipo", "stop"}, {"precio", o["stop"]},
                           {"texto", std::string("Stop") + (truthy(o.value("activada", json())) ? " (activado)" : "")}});
      if (truthy(o.value("precio_ejec", json())))
        niveles.push_back({{"tipo", "ejecucion"}, {"precio", o["precio_ejec"]}, {"texto", "Ejecutada"}, {"ts", o["ejecutada"]}});
      if (b.db().one("SELECT name FROM sqlite_master WHERE name='agente_posiciones'"))
        if (auto g = b.db().one("SELECT entrada, objetivo FROM agente_posiciones WHERE simbolo=? AND (orden_stop=? OR "
                                "orden_entrada=?)",
                                {o["simbolo"], oid, oid})) {
          niveles.push_back({{"tipo", "entrada"}, {"precio", (*g)["entrada"]}, {"texto", "Entrada del agente"}});
          niveles.push_back({{"tipo", "objetivo"}, {"precio", (*g)["objetivo"]}, {"texto", "Objetivo del agente"}});
        }
      json s = b.serie(jstr(o, "simbolo"), c.q("rango", "1d"));
      return json{{"orden", o}, {"cotizacion", q}, {"niveles", niveles}, {"puntos", s["puntos"]}, {"real", s["real"]},
                  {"agente", jstr(o, "origen") == bolsa::ORIGEN_AGENTE}};
    });
  });
  app.get("/api/bolsa/agente", [B](Ctx& c) -> Out {
    return B([&](bolsa::Bolsa&) { return bolsa::agente().estado(); });
  });
  // ajustes del agente autonomo (activo, intervalo_s, riesgo_pct, max_posiciones, max_peso_pct, reserva_pct,
  // universo, capital, ia) e IA financiera (ia_modelo). Siempre opera en simulacion.
  app.post("/api/bolsa/agente", [B](Ctx& c) -> Out {
    Services& s = c.app.S();
    static const std::vector<std::pair<std::string, char>> allowed = {
        {"activo", 'b'}, {"intervalo_s", 'f'}, {"riesgo_pct", 'f'}, {"max_posiciones", 'i'}, {"max_peso_pct", 'f'},
        {"reserva_pct", 'f'}, {"universo", 'l'}, {"capital", 'f'}, {"ia", 'b'}};
    for (auto& [k, typ] : allowed) {
      if (!c.has_b(k)) continue;
      json v = c.body[k];
      json out;
      try {
        if (typ == 'b') out = truthy(v);
        else if (typ == 'f') out = v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
        else if (typ == 'i') out = (long long)(v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>());
        else {
          if (!v.is_array()) throw std::invalid_argument("lista");
          out = json::array();
          for (auto& x : v) {
            std::string t = trim(x.is_string() ? x.get<std::string>() : jdump(x));
            if (!t.empty()) out.push_back(upper(t));
          }
        }
      } catch (const std::exception&) {
        throw HttpErr(400, k + ": valor no valido");
      }
      s.config.set("bolsa.agente." + k, out);
    }
    if (c.has_b("ia_modelo")) {   // IA financiera: auto (por hardware), no (la general) o un modelo concreto
      std::string m = trim(c.bs("ia_modelo", "auto"));
      s.config.set("ia_especializada.finanzas", m.empty() ? "auto" : m);
    }
    auto& ag = bolsa::agente();
    if (c.has_b("activo") && c.body["activo"] == true) {
      if (ag.listeners.empty()) ag.listeners.push_back([&app = c.app](const json& e) { app.broadcast(e); });
      ag.start();
    }
    if (c.has_b("activo") && c.body["activo"] == false) ag.stop();
    if (truthy(c.b("ciclo", false))) B([&](bolsa::Bolsa&) { return ag.ciclo(); });
    return ag.estado();
  });

  // ------------------------------------------------------------------ dispositivos: luces, reloj y avatar Pico W
  auto H = [](Ctx& c) -> devices::Hub& { return devices::hub(c.app.ready() ? &c.app.S() : nullptr); };
  auto D = [](auto fn) -> Out {
    try {
      return fn();
    } catch (const std::out_of_range& e) {
      throw HttpErr(404, e.what());
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  };
  auto first = [](const json& a, const json& b) { return truthy(a) ? a : b; };
  app.get("/api/dispositivos", [H](Ctx& c) -> Out {
    auto& h = H(c);
    return json{{"estado", h.snapshot()}, {"leds", h.leds.list()}, {"modos", h.modos()}, {"modo", h.modo_actual()},
                {"clientes", h.clientes_recientes()}};
  });
  app.get("/api/dispositivos/estado", [H](Ctx& c) -> Out {   // estado ligero para el reloj y el Pico W (cada 1-2 s)
    auto& h = H(c);
    std::string tipo = c.q("tipo");
    h.visto(c.q("cliente"), tipo.empty() ? "web" : tipo, {{"ip", c.host}});
    return h.snapshot();
  });
  app.post("/api/dispositivos/estado", [H, first](Ctx& c) -> Out {
    json st = first(c.b("estado", nullptr), first(c.b("state", nullptr), "idle"));
    return H(c).set_state(Ctx::as_str(st), c.b("nivel", nullptr), c.b("texto", nullptr),
                          truthy(c.b("origen", nullptr)) ? c.bs("origen") : "");
  });
  app.get("/api/dispositivos/reloj", [H](Ctx& c) -> Out {
    auto& h = H(c);
    std::string cl = c.q("cliente");
    h.visto(cl.empty() ? c.host : cl, "reloj", {{"ip", c.host}});
    return h.resumen_reloj();
  });
  app.post("/api/dispositivos/modo", [H, D](Ctx& c) -> Out {
    return D([&] { return H(c).aplicar_modo(c.bs("modo", ""), truthy(c.b("origen", nullptr)) ? c.bs("origen") : ""); });
  });
  app.get("/api/dispositivos/pico/manifiesto", [H](Ctx& c) -> Out { return H(c).pico_manifiesto(); });
  app.get("/api/dispositivos/pico/{estado}.bin", [H](Ctx& c) -> Out {
    return Raw{200, H(c).pico_buffer(c.p("estado")), "application/octet-stream"};
  });
  app.get("/api/dispositivos/pico/{estado}.png", [H](Ctx& c) -> Out {
    return Raw{200, H(c).pico_png(c.p("estado")), "image/png"};
  });
  app.get("/api/dispositivos/leds/buscar", [H](Ctx& c) -> Out {
    std::vector<std::string> hosts;
    for (auto& x : split(c.q("ip"), ","))
      if (!trim(x).empty()) hosts.push_back(trim(x));
    return json{{"encontrados", H(c).leds.discover(1.5, hosts)}};
  });
  app.post("/api/dispositivos/leds", [H](Ctx& c) -> Out {
    std::string host = trim(c.bs("host", ""));
    if (host.empty()) throw HttpErr(400, "falta la IP del dispositivo");
    json port = c.b("port", nullptr);
    return H(c).leds.add(c.bs("host"), truthy(port) ? (int)jnum(port.is_string() ? json(std::stod(port.get<std::string>())) : port) : 80,
                         truthy(c.b("name", nullptr)) ? c.bs("name") : "", truthy(c.b("token", nullptr)) ? c.bs("token") : "",
                         truthy(c.b("id", nullptr)) ? c.bs("id") : "");
  });
  app.put("/api/dispositivos/leds/{did}", [H, D](Ctx& c) -> Out {
    json kw = json::object();
    for (auto k : {"name", "enabled", "follow_state", "token"}) kw[k] = c.b(k, nullptr);
    return D([&] { return H(c).leds.update(c.p("did"), kw); });
  });
  app.del("/api/dispositivos/leds/{did}", [H](Ctx& c) -> Out {
    H(c).leds.remove(c.p("did"));
    return json{{"ok", true}};
  });
  // pasarela a la API del NexusLED (efectos, paletas, presets, brillo...) para la app y el movil
  for (std::string method : {"GET", "POST", "DELETE"})
    app.route(method, "/api/dispositivos/leds/{did}/api/{ruta:path}", [H, method](Ctx& c) -> Out {
      json body = method == "GET" || !c.body.is_object() ? json(nullptr) : c.body;
      try {
        return H(c).leds.call(c.p("did"), method, "/api/" + c.p("ruta"), body);
      } catch (const std::out_of_range& e) {
        throw HttpErr(404, e.what());
      } catch (const std::invalid_argument& e) {
        throw HttpErr(400, e.what());
      } catch (const std::exception& e) {
        throw HttpErr(502, std::string("el dispositivo no responde: ") + e.what());
      }
    });
  app.get("/api/agentes/actividad", [](Ctx&) -> Out { return agents::activity().all(); });

  // ------------------------------------------------------------------ grabar firmware (solo desde el propio equipo)
  auto solo_local = [](Ctx& c) {
    if (!c.local()) throw HttpErr(403, "Solo desde el propio equipo");
  };
  auto nexus_para_dispositivos = [](Ctx& c) {
    std::string port = Ctx::as_str(c.app.S().config.get("server.port", 8775));
    std::vector<json> addrs;
    for (auto& a : coop::overlay_addresses()) addrs.push_back(a);
    std::stable_sort(addrs.begin(), addrs.end(),
                     [](const json& a, const json& b) { return (jstr(a, "kind") != "lan") < (jstr(b, "kind") != "lan"); });
    std::string host;
    for (auto& a : addrs) {
      std::string ip = jstr(a, "ip");
      if (starts_with(ip, "169.254.") || starts_with(ip, "127.")) continue;
      host = ip + ":" + port;
      break;
    }
    std::string sh = c.app.S().config.str("server.host", "127.0.0.1");
    return json{{"host", host}, {"key", c.app.lan_key()}, {"escucha_red", sh != "127.0.0.1" && sh != "localhost"}};
  };
  app.get("/api/firmware", [solo_local, nexus_para_dispositivos](Ctx& c) -> Out {
    solo_local(c);
    namespace fw = devices::firmware;
    json info = fw::disponible();
    if (fw::carpeta().empty()) {
      info["binarios"] = json::array();
      info["puertos"] = json::array();
      info["error"] = "no encuentro la carpeta firmware/ (con bin/firmware.json)";
    } else {
      info["binarios"] = fw::binarios();
      info["puertos"] = fw::puertos();
      info["modelos_luces"] = fw::MODELOS_LUCES;
    }
    json familia = json::array();
    for (auto f : fw::FAMILIA) {   // copia: se le anade si el paquete trae el binario de las luces
      f["binario"] = !fw::carpeta().empty() && is_file(fw::carpeta() / "bin" / ("nexusled-" + jstr(f, "chip") + ".bin"));
      if (jstr(f, "chip") == "esp8266") {   // una imagen por tamano de flash
        json vs = json::array();
        for (auto& v : fw::VARIANTES_ESP8266)
          if (!fw::carpeta().empty() && is_file(fw::carpeta() / "bin" / ("nexusled-" + jstr(v, "modelo") + ".bin"))) vs.push_back(v);
        f["variantes"] = vs;
      }
      familia.push_back(f);
    }
    info["familia"] = familia;
    info["drivers"] = devices::drivers::estado();
    info["nexus"] = nexus_para_dispositivos(c);
    return info;
  });
  app.post("/api/firmware/grabar", [solo_local, nexus_para_dispositivos](Ctx& c) -> Out {
    solo_local(c);
    json cfg = c.b("config", nullptr);
    if (!cfg.is_object()) cfg = json::object();
    std::string tipo = c.bs("tipo", "");
    if (c.bb("conectar_nexus", true) && !cfg.contains("nexus") && (tipo == "reloj" || tipo == "pico")) {
      json n = nexus_para_dispositivos(c);
      if (truthy(n["host"])) cfg["nexus"] = {{"host", n["host"]}, {"key", n["key"]}};
    }
    if (tipo == "luces") cfg.erase("nexus");
    try {
      return devices::firmware::trabajos().lanzar(tipo, truthy(c.b("puerto", nullptr)) ? c.bs("puerto") : "",
                                                  truthy(c.b("modelo", nullptr)) ? c.bs("modelo") : "",
                                                  cfg.empty() ? json(nullptr) : cfg, c.bb("solo_config", false));
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  });
  // comprobacion y autorreparacion: lo que se reviso al arrancar y volver a revisar ahora
  app.get("/api/salud", [](Ctx&) -> Out { return salud::informe(); });
  app.post("/api/salud", [solo_local](Ctx& c) -> Out {
    solo_local(c);
    Services& s = c.app.S();
    salud::previa(false);   // con todo abierto no se mueve nada: solo se mira
    return salud::completa(s, c.bb("reparar", true));
  });
  // drivers USB-serie (CH340/CH9102, CP210x, FTDI): que hay enchufado, que falta e instalarlo sin preguntar
  app.get("/api/firmware/drivers", [solo_local](Ctx& c) -> Out {
    solo_local(c);
    return devices::drivers::estado();
  });
  app.post("/api/firmware/drivers", [solo_local](Ctx& c) -> Out {
    solo_local(c);
    std::vector<std::string> pedidos;
    json d = c.b("drivers", nullptr);
    if (d.is_array())
      for (auto& x : d)
        if (x.is_string()) pedidos.push_back(x.get<std::string>());
    return devices::drivers::instalar(pedidos, c.bb("simular", false));
  });
  app.get("/api/firmware/trabajo/{tid}", [solo_local](Ctx& c) -> Out {
    solo_local(c);
    json j = devices::firmware::trabajos().get(c.p("tid"));
    if (j.is_null()) throw HttpErr(404, "trabajo desconocido");
    return j;
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
      std::string fmt = c.has_q("formato") ? c.q("formato") : "";
      return c.app.S().models->search(source, c.q("q"), fmt.empty() ? c.q("kind") : "fmt:" + fmt);
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

  // explorador de archivos para importar modelos: el selector del sistema solo si la interfaz se abre en el mismo
  // equipo (no por el puerto remoto: el dialogo saldria en el PC y no en el movil que lo pide)
  app.get("/api/fs/listar", [](Ctx& c) -> Out {
    try {
      json r = explorador::listar(c.q("ruta"), c.qb("todos", false));
      r["dialogo"] = !c.remote && c.local() && explorador::dialogo_disponible();
      return r;
    } catch (const std::runtime_error& e) {
      throw HttpErr(404, e.what());
    }
  }, true);

  app.post("/api/fs/dialogo", [](Ctx& c) -> Out {
    if (c.remote || !c.local())
      return json{{"unavailable", true}, {"detail", "el selector del sistema solo se abre en el equipo de Nexus-Office"}};
    return explorador::dialogo(c.bb("carpeta", false));
  }, true);

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

  // ------------------------------------------------------------------ cooperativo
  app.get("/api/coop", [](Ctx& c) -> Out { return c.app.S().coop->status(); });

  app.post("/api/coop/create", [](Ctx& c) -> Out { return c.app.S().coop->create_network(c.bs("name", "")); }, true);

  app.post("/api/coop/join", [](Ctx& c) -> Out {
    std::string code = c.bs("code");
    try {
      return c.app.S().coop->join(code);
    } catch (const std::exception& e) {
      throw HttpErr(400, e.what());
    }
  });

  app.get("/api/coop/invite", [](Ctx& c) -> Out { return json{{"code", c.app.S().coop->invite()}}; }, true);

  app.post("/api/coop/start", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    try {
      s.coop->start();
    } catch (const std::runtime_error& e) {
      throw HttpErr(400, e.what());
    }
    s.config.set("coop.enabled", true);
    return s.coop->status();
  });

  app.post("/api/coop/stop", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.coop->stop();
    s.config.set("coop.enabled", false);
    return s.coop->status();
  });

  app.post("/api/coop/chat", [](Ctx& c) -> Out {
    std::string text = c.bs("text");
    try {
      return c.app.S().coop->send_chat(text);
    } catch (const std::runtime_error& e) {
      throw HttpErr(400, e.what());
    }
  });

  // str(KeyError) de Python lleva comillas
  auto coop_err = [](const std::exception& e) {
    return HttpErr(400, dynamic_cast<const std::out_of_range*>(&e) ? key_msg(e) : std::string(e.what()));
  };

  app.post("/api/coop/ping/{node}", [coop_err](Ctx& c) -> Out {
    try {
      return json{{"ms", c.app.S().coop->ping(c.p("node"))}};
    } catch (const std::exception& e) {
      throw coop_err(e);
    }
  });

  app.post("/api/coop/agent", [coop_err](Ctx& c) -> Out {
    std::string node = c.bs("node"), agent = c.bs("agent", "auto"), text = c.bs("text");
    try {
      return json{{"answer", c.app.S().coop->remote_agent(node, agent, text)}};
    } catch (const std::exception& e) {
      throw coop_err(e);
    }
  });

  app.post("/api/coop/share-proposal/{pid}", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    s.coop->share_proposal(s.proposals->get(c.p("pid")));
    return json{{"ok", true}};
  }, true);

  app.post("/api/coop/share-facts", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    std::vector<std::string> facts;
    for (auto& m : s.memory->list("fact", 30)) facts.push_back(jstr(m, "content"));
    s.coop->share_facts(facts);
    return json{{"shared", facts.size()}};
  }, true);

  // ------------------------------------------------------------------ acceso remoto por internet
  app.post("/api/remote/pair", [](Ctx& c) -> Out {
    // canjea un codigo de emparejamiento por la llave del dispositivo (cookie segura + JSON para apps)
    std::string code = c.bs("code"), name = c.bs("name", "");
    auto& r = *c.app.S().remote;
    if (r.limiter.blocked("pair:" + c.host)) throw HttpErr(429, "Demasiados intentos fallidos: espera 15 minutos");
    auto res = r.devices.pair(code, name, c.host);
    if (!res) {
      r.limiter.fail("pair:" + c.host);
      L.warning("Emparejamiento remoto rechazado desde ", c.host);
      throw HttpErr(401, "Codigo incorrecto o caducado (pide uno nuevo en tu Nexus-Office)");
    }
    auto& [token, dev] = *res;
    r.limiter.reset("pair:" + c.host);
    L.info("Dispositivo emparejado: ", jstr(dev, "name"), " (", c.host, ")");
    c.app.broadcast({{"type", "remote"}, {"event", "paired"}, {"device", dev}});
    c.res.set_header("Set-Cookie", std::string(remote::COOKIE) + "=" + token + "; HttpOnly; Max-Age=31536000; Path=/; SameSite=strict" +
                                       (c.remote ? "; Secure" : ""));
    return json{{"ok", true}, {"device", dev}, {"token", token}};
  });

  app.get("/api/remote", [](Ctx& c) -> Out { return c.app.S().remote->status(); }, true);

  app.post("/api/remote/enable", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    bool enabled = truthy(c.b("enabled"));
    int port = (int)c.bn("port", 8443);
    if (port < 1024 || port > 65535 || port == (int)s.config.num("server.port", 8775))
      throw HttpErr(400, "Puerto no valido (1024-65535 y distinto del puerto local)");
    s.config.set("remote.enabled", enabled);
    s.config.set("remote.port", port);
    s.remote->cert();   // crea el certificado ya para poder mostrar la huella
    int running = s.remote->listening_port;
    return merged(s.remote->status(), {{"restart_required", enabled ? running != port : running != 0}});
  }, true);

  app.post("/api/remote/pairing", [](Ctx& c) -> Out {
    auto& r = *c.app.S().remote;
    std::string name = c.bs("name", "");
    json p = r.devices.new_pairing(name);
    json urls = r.urls(), links = json::array();
    for (auto& u : urls) links.push_back(jstr(u, "url") + "/?pair=" + jstr(p, "code"));
    return merged(p, {{"invite", r.invite(p, name)}, {"urls", urls}, {"fingerprint", remote::pretty(r.fingerprint())},
                      {"links", links}});
  }, true);

  app.del("/api/remote/devices/{dev_id}", [](Ctx& c) -> Out {
    if (!c.app.S().remote->devices.revoke(c.p("dev_id"))) throw HttpErr(404, "Dispositivo no encontrado");
    c.app.close_device(c.p("dev_id"));   // corta al momento sus conexiones abiertas
    return json{{"ok", true}};
  }, true);

  app.post("/api/remote/upnp", [](Ctx& c) -> Out {
    auto& r = *c.app.S().remote;
    bool open = truthy(c.b("open"));
    try {
      auto gw = remote::Gateway::discover();
      std::string ext = gw.external_ip();
      if (open) {
        std::string internal = gw.add_mapping(r.port());
        r.upnp = {{"mapped", true}, {"external_ip", ext}, {"internal", internal}, {"error", ""},
                  {"cgnat", remote::is_cgnat_or_private(ext)}};
      } else {
        gw.delete_mapping(r.port());
        r.upnp = {{"mapped", false}, {"external_ip", ext}, {"error", ""}, {"cgnat", remote::is_cgnat_or_private(ext)}};
      }
    } catch (const std::exception& e) {
      r.upnp["error"] = e.what();
    }
    return r.status();
  }, true);

  auto lookup_public = [](remote::RemoteAccess& r) {
    try {
      r.public_ip = remote::public_ip_lookup();
    } catch (const std::exception& e) {
      throw HttpErr(502, std::string("No se pudo consultar la IP publica: ") + e.what());
    }
    std::string ext = jstr(r.upnp, "external_ip");
    r.behind_cgnat = (!ext.empty() && ext != r.public_ip) || remote::is_cgnat_or_private(ext.empty() ? r.public_ip : ext);
  };

  app.post("/api/remote/public-ip", [lookup_public](Ctx& c) -> Out {
    auto& r = *c.app.S().remote;
    lookup_public(r);   // servicio externo ipify: solo cuando el administrador lo pide
    return r.status();
  }, true);

  // ------------------------------------------------------------------ VPN cifrada (WireGuard)
  app.get("/api/vpn", [](Ctx& c) -> Out { return c.app.S().vpn->status(); }, true);

  app.post("/api/vpn/enable", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    bool enabled = truthy(c.b("enabled"));
    int port = (int)c.bn("port", 51820);
    if (port < 1 || port > 65535) throw HttpErr(400, "Puerto no valido");
    s.config.set("vpn.enabled", enabled);
    s.config.set("vpn.port", port);
    bool restart = false;
    if (enabled) {
      s.vpn->write_conf();
      std::string host = s.config.str("server.host", "127.0.0.1");
      if (host == "127.0.0.1" || host == "localhost") {
        s.config.set("server.host", "0.0.0.0");
        restart = true;
      }
      c.app.lan_key();
    }
    return merged(s.vpn->status(), {{"restart_required", restart}});
  }, true);

  app.post("/api/vpn/peers", [](Ctx& c) -> Out {
    // nuevo dispositivo: el codigo NEXUSV1:... para la app (incluye su clave privada: se muestra una vez)
    Services& s = c.app.S();
    auto& r = *s.remote;
    std::string name = c.bs("name", "");
    bool lookup = c.bb("lookup_public_ip", false);
    if (!s.config.flag("vpn.enabled", false)) throw HttpErr(400, "Activa primero la VPN");
    std::string public_ip = jstr(r.upnp, "external_ip");
    if (public_ip.empty()) public_ip = r.public_ip;
    if (lookup && public_ip.empty()) {
      try {
        public_ip = r.public_ip = remote::public_ip_lookup();
      } catch (const std::exception& e) {
        L.info("VPN: sin IP publica (", e.what(), ")");
      }
    }
    try {
      return s.vpn->add_peer(name, c.app.lan_key(), public_ip);
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  }, true);

  app.del("/api/vpn/peers/{peer_id}", [](Ctx& c) -> Out {
    Services& s = c.app.S();
    if (!s.vpn->remove_peer(c.p("peer_id"))) throw HttpErr(404, "Dispositivo no encontrado");
    return merged(s.vpn->status(), {{"restart_tunnel", true}});
  }, true);

  app.get("/api/vpn/server.conf", [](Ctx& c) -> Out {
    return Raw{200, c.app.S().vpn->server_conf(), "text/plain; charset=utf-8",
               {{"Content-Disposition", std::string("attachment; filename=") + remote::IFACE + ".conf"}}};
  }, true);

  app.post("/api/vpn/apply", [](Ctx& c) -> Out {
    auto& v = *c.app.S().vpn;
    json res = v.apply();
    return merged(res, {{"tunnel_up", v.tunnel_up()}});
  }, true);

  app.post("/api/vpn/upnp", [](Ctx& c) -> Out {
    bool open = truthy(c.b("open"));
    int port = c.app.S().vpn->port();
    try {
      auto gw = remote::Gateway::discover();
      if (open) {
        std::string internal = gw.add_mapping(port, 0, "UDP");
        return json{{"mapped", true}, {"external_ip", gw.external_ip()}, {"internal", internal}, {"port", port}};
      }
      gw.delete_mapping(port, "UDP");
      return json{{"mapped", false}, {"port", port}};
    } catch (const std::exception& e) {
      throw HttpErr(502, std::string("UPnP: ") + e.what());
    }
  }, true);

  app.get("/api/remote/router", [](Ctx& c) -> Out {
    // asistente del router: puerta de enlace, pagina de configuracion, marca, UPnP y la regla exacta a crear
    Services& s = c.app.S();
    auto& r = *s.remote;
    return merged(remote::detect(r.port()), {{"enabled", s.config.flag("remote.enabled", false)},
                                             {"listening", r.listening_port != 0}, {"port", r.port()}});
  }, true);

  app.post("/api/remote/check", [lookup_public](Ctx& c) -> Out {
    // comprueba desde este equipo si el puerto responde en la IP publica (ipify, solo a peticion)
    auto& r = *c.app.S().remote;
    lookup_public(r);
    bool listening = r.listening_port != 0;
    bool ok = listening && remote::reachable_from_outside(r.public_ip, r.port());
    return json{{"public_ip", r.public_ip}, {"port", r.port()}, {"listening", listening}, {"reachable", ok},
                {"behind_cgnat", r.behind_cgnat}, {"url", "https://" + r.public_ip + ":" + std::to_string(r.port())}};
  }, true);

  app.post("/api/remote/certificate", [](Ctx& c) -> Out {
    // certificado nuevo: invalida la huella anclada (habra que volver a emparejar las apps)
    auto& r = *c.app.S().remote;
    r.cert(true);
    return merged(r.status(), {{"restart_required", true}});
  }, true);
}

}  // namespace nx::server
