// CLI de Nexus-Office (__main__.py), edicion C++:
//
//   nexus-office run [--host H] [--port P] [--no-browser] [--motor auto|llamaserver] [--acelerador auto|cpu|gpu|npu]
//   nexus-office detect
//   nexus-office pull <id-recomendado|auto>
//   nexus-office modelos [--descargar] [--todos]   (evalua el hardware y elige el modelo de IA de cada tipo)
//   nexus-office servicio instalar|quitar|estado [--red] [--puerto P]   (arranque automatico con el equipo)
//   nexus-office chat [--agent A]
//   nexus-office oficina [<perfil>|--quitar] [--reset-agentes]
//   nexus-office bolsa [accion] ...                 (mesa de bolsa simulada)
//   nexus-office grafo formatos|importar|exportar   (grafos de cualquier formato como conocimiento)
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "agents/agents.h"
#include "coop/coop.h"
#include "core/config.h"
#include "core/service.h"
#include "core/services.h"
#include "core/system.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "memory/kg.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "office/bolsa.h"
#include "office/bolsa_agente.h"
#include "rag/web.h"
#include "remote/remote.h"
#include "office/office.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/http.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace nx;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// argumentos sencillos al estilo argparse: --clave valor, --bandera y posicionales
struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  std::set<std::string> flags;
  bool has(const std::string& f) const { return flags.count(f) > 0; }
  std::string get(const std::string& k, const std::string& def = "") const {
    auto it = opt.find(k);
    return it == opt.end() ? def : it->second;
  }
};

Args parse(const std::vector<std::string>& argv, const std::set<std::string>& with_value) {
  Args a;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& x = argv[i];
    if (starts_with(x, "--")) {
      auto eq = x.find('=');
      if (eq != std::string::npos) {
        a.opt[x.substr(2, eq - 2)] = x.substr(eq + 1);
      } else if (with_value.count(x.substr(2)) && i + 1 < argv.size()) {
        a.opt[x.substr(2)] = argv[++i];
      } else {
        a.flags.insert(x.substr(2));
      }
    } else {
      a.pos.push_back(x);
    }
  }
  return a;
}

[[noreturn]] void die(const std::string& msg, int code = 1) {
  std::fprintf(stderr, "%s\n", msg.c_str());
  std::exit(code);
}

hw::HardwareInfo cached_hardware() {
  hw::HardwareInfo info;
  if (!hw::load_cached(info)) info = hw::detect("http://127.0.0.1:11434", false);
  return info;
}

int cmd_run(const Args& a) {
  Config& cfg = get_config();
  if (hw::android() && !cfg.flag("android.configured", false)) {
    // valores pensados para un movil (android_main.py); despues el usuario los cambia en Ajustes
    cfg.set("server.host", "127.0.0.1");
    cfg.set("server.open_browser", false);
    cfg.set("llm.max_tokens", 768);
    cfg.set("memory.llm_graph_extraction", false);
    cfg.set("android.configured", true);
  }
  std::string motor = a.get("motor"), accel = a.get("acelerador");
  if (!motor.empty() || !accel.empty()) {   // eleccion por linea de comandos: no preguntar
    if (!motor.empty()) {
      if (motor != "auto" && motor != "llamaserver") {
        std::printf("  La edicion C++ usa llama-server; '%s' no esta disponible (se usa llamaserver).\n", motor.c_str());
        motor = "llamaserver";
      }
      cfg.set("llm.engine", motor);
    }
    if (!accel.empty()) {
      if (accel != "auto" && accel != "cpu" && accel != "gpu" && accel != "npu") die("--acelerador: auto, cpu, gpu o npu", 2);
      cfg.set("hardware.accel", accel);
      cfg.set("hardware.llama_variant", hw::plan(cached_hardware(), accel).value("variant", json("")));
    }
    cfg.set("startup.ask_accel", false);
  }
  std::string host = a.get("host", cfg.str("server.host", "127.0.0.1"));
  int port = a.opt.count("port") ? std::atoi(a.get("port").c_str()) : (int)cfg.num("server.port", 8775);
  std::string url = "http://" + std::string(host == "0.0.0.0" || host == "::" ? "127.0.0.1" : host) + ":" + std::to_string(port);
  bool browser = cfg.flag("server.open_browser", true) && !a.has("no-browser");
  if (service::running(port)) {   // ya lo arranco el servicio: el acceso directo solo abre el navegador
    std::printf("\n  Nexus-Office ya esta en marcha (servicio)  ->  %s\n\n", url.c_str());
    if (!a.has("no-browser")) sys2::open_url(url);
    return 0;
  }
  std::printf("\n  Nexus-Office %s (C++)  ->  %s\n\n", NEXUS_VERSION, url.c_str());
  std::fflush(stdout);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

  Services s;
  server::App app(&s);
  for (auto* listeners : {&s.supervisor->listeners, &s.lounge->listeners, &s.coop->listeners})
    listeners->push_back([&app](const json& e) { app.broadcast(e); });
  app.on_shutdown_request = [] { g_stop = true; };
  s.start();
  // siguen solos los rastreos de dominio completo que se cortaron (cierre, apagado)
  rag::crawl_jobs::resume_interrupted(s, [&app](const json& e) { app.broadcast(e); });
  // el agente de bolsa autonomo trabaja solo, siempre en simulacion (con precios reales si hay conexion)
  if (cfg.flag("bolsa.agente.activo", true)) {
    try {
      auto& ag = bolsa::agente();
      ag.listeners.push_back([&app](const json& e) { app.broadcast(e); });
      ag.start();
    } catch (const std::exception& e) {
      log::get("bolsa.agente").warning("no se pudo arrancar el agente de bolsa: ", e.what());
    }
  }
  // acceso remoto por internet: la misma aplicacion por HTTPS en su puerto, solo para dispositivos emparejados
  std::unique_ptr<server::App> remote_app;
  std::thread remote_thread;
  if (cfg.flag("remote.enabled", false)) {
    try {
      auto [cert, key, fp] = s.remote->cert();
      int rport = s.remote->port();
      std::string rhost = cfg.str("remote.host", "0.0.0.0");
      remote_app = std::make_unique<server::App>(&s, app, cert, key);
      if (!remote_app->valid()) {
        std::fprintf(stderr, "  Acceso remoto: no se pudo cargar el certificado de %s\n", S(cert).c_str());
      } else if (!remote_app->bind(rhost, rport)) {
        std::fprintf(stderr, "  Acceso remoto: el puerto %d esta ocupado\n", rport);
      } else {
        s.remote->listening_port = rport;
        remote_thread = std::thread([&] {
          remote_app->listen_after_bind();
          s.remote->listening_port = 0;
        });
        log::get("remote").info("Acceso remoto HTTPS en el puerto ", rport, " · huella ", remote::pretty(fp));
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  Acceso remoto no disponible: %s\n", e.what());
    }
  }
  std::thread watcher([&app, &remote_app] {
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.stop();
    if (remote_app) remote_app->stop();
  });
  if (browser)
    std::thread([url] {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (!g_stop) sys2::open_url(url);
    }).detach();
  bool ok = app.listen(host, port);
  g_stop = true;
  watcher.join();
  if (remote_thread.joinable()) remote_thread.join();
  if (!ok && !app.stopping) std::fprintf(stderr, "  No se pudo abrir el puerto %d en %s (¿ya esta en uso?)\n", port, host.c_str());
  bolsa::reset_agentes();
  s.shutdown();
  proc::cleanup();
  return ok || app.stopping ? 0 : 1;
}

int cmd_detect() {
  log::setup();
  auto info = hw::detect();
  hw::save(info);
  json prof = hw::build_profile(info);
  std::printf("%s\n", jdump(json{{"hardware", info.to_json()}, {"profile", prof}}, 2).c_str());
  return 0;
}

int pull_one(models::ModelManager& mm, const std::string& mid) {
  std::string jid;
  try {
    jid = mm.download_recommended(mid);
  } catch (const std::exception& e) {
    std::printf("%s: no es un modelo recomendado (%s)\n", mid.c_str(), e.what());
    return 1;
  }
  while (true) {
    json j = mm.job(jid);
    std::string st = jstr(j, "status");
    if (st == "completado" || st == "error" || st == "cancelado") {
      std::string line = mid + ": " + st + " " + jstr(j, "error");
      std::printf("\r%-70s\n", line.c_str());
      return st == "completado" ? 0 : 1;
    }
    double total = jnum(j.value("total", json(0))), done = jnum(j.value("done", json(0)));
    std::printf("\r%s: %-44s %5.1f%%", mid.c_str(), uleft(st, 44).c_str(), total > 0 ? 100 * done / total : 0.0);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

int cmd_pull(const std::string& model) {
  log::setup();
  json prof = hw::build_profile(cached_hardware());
  models::ModelManager mm(get_config(), [prof] { return prof; });
  std::vector<std::string> ids = {model};
  if (model == "auto") {   // el mejor modelo de cada tipo para este hardware
    ids.clear();
    for (auto& r : mm.recommended())
      if (truthy(r.value("best", json(false))) && !truthy(r.value("installed", json(false)))) ids.push_back(jstr(r, "id"));
    std::printf("Modelos recomendados para este equipo: %s\n", ids.empty() ? "ya instalados" : join(ids, ", ").c_str());
  }
  int rc = 0;
  for (auto& id : ids) rc |= pull_one(mm, id);
  return rc;
}

bool online(double timeout = 6) {
  auto r = http::get("https://huggingface.co/api/whoami-v2", {}, timeout);
  return r.status != 0;   // responde (aunque sea 401 sin sesion): hay internet
}

int cmd_models(const Args& a) {
  log::setup();
  json prof = hw::build_profile(cached_hardware());
  models::ModelManager mm(get_config(), [prof] { return prof; });
  static const std::map<std::string, std::string> KIND_NAME = {
      {"llm", "Chat y vision"}, {"embed", "Documentos (RAG)"}, {"tts", "Voz"}, {"image", "Imagen"}};
  double vram = jnum(prof.value("vram_gb", json(0)));
  std::string gpu = vram > 0 ? fnum(vram, 0) + " GB de VRAM" : "sin GPU dedicada";
  std::printf("Equipo: %s | %s GB de RAM | %s | acelerador %s | gama %s\n", jstr(prof, "device_name", "?").c_str(),
              fnum(jnum(prof.value("ram_gb", json(0))), 0).c_str(), gpu.c_str(), jstr(prof, "accelerator", "cpu").c_str(),
              jstr(prof, "tier", "?").c_str());
  json recs = mm.recommended();
  std::vector<json> chosen;
  for (auto* kind : {"llm", "embed", "tts", "image"}) {
    for (auto& r : recs) {
      if (jstr(r, "kind") != kind || !truthy(r.value("best", json(false)))) continue;
      bool inst = truthy(r.value("installed", json(false)));
      char size[32];
      std::snprintf(size, sizeof size, "%.1f GB", jnum(r.value("size_mb", json(0))) / 1024);
      std::printf("  %-18s -> %s (%s)\n", KIND_NAME.at(kind).c_str(), jstr(r, "name").c_str(), inst ? "ya instalado" : size);
      if (std::string(kind) != "image" || a.has("todos")) chosen.push_back(r);
      break;
    }
  }
  std::vector<json> sector;
  for (auto& r : recs)
    if (truthy(r.value("sector", json())) && !truthy(r.value("best", json(false)))) sector.push_back(r);
  if (!sector.empty()) {
    std::printf("  Sugeridos para tu oficina (en la app: Modelos > Recomendados):\n");
    static const std::map<std::string, std::string> HOW = {
        {"ideal", ""}, {"ok", ""}, {"pesado", " · lento en este equipo"}, {"no_cabe", " · no cabe en este equipo"}};
    for (auto& r : sector) {
      auto h = HOW.find(jstr(r, "fit"));
      std::printf("    - %s: %s%s%s\n", jstr(r, "name").c_str(), jstr(r, "sector").c_str(),
                  h == HOW.end() ? "" : h->second.c_str(), truthy(r.value("installed", json(false))) ? " (ya instalado)" : "");
    }
  }
  bool has_llm = false;
  std::vector<std::string> alt;
  for (auto& r : recs) {
    if (jstr(r, "kind") != "llm") continue;
    has_llm |= truthy(r.value("best", json(false)));
    if (jstr(r, "fit") == "pesado") alt.push_back(jstr(r, "name"));
  }
  if (has_llm)
    std::printf("  Motivo: el modelo de chat mas capaz que cabe en este equipo con fluidez%s\n",
                alt.empty() ? "" : (" (mas grande pero lento aqui: " + join(alt, ", ") + ")").c_str());
  std::vector<std::string> todo;
  for (auto& r : chosen)
    if (!truthy(r.value("installed", json(false)))) todo.push_back(jstr(r, "id"));
  if (!a.has("descargar")) {
    if (!todo.empty())
      std::printf("Para descargarlos: nexus-office modelos --descargar   (o en la app: Modelos > Recomendados)\n");
    return 0;
  }
  if (todo.empty()) {
    std::printf("Los modelos elegidos ya estan instalados.\n");
    return 0;
  }
  if (!online()) {
    std::printf("Sin conexion a internet: los modelos elegidos se descargaran en cuanto haya red "
                "(nexus-office modelos --descargar, o Modelos > Recomendados en la app).\n");
    return 3;   // pendiente: los lanzadores lo reintentan en el siguiente arranque
  }
  int rc = 0;
  for (auto& id : todo) rc |= pull_one(mm, id);
  return rc;
}

int cmd_chat(const Args& a) {
  Services s;
  s.start();
  std::string sid = s.memory->new_session("Terminal");
  std::printf("Nexus-Office (%s) - escribe 'salir' para terminar\n", jstr(s.llm->status(), "backend").c_str());
  std::string agent = a.get("agent", "auto");
  std::string line;
  while (true) {
    std::printf("\n> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    std::string text = trim(line);
    std::string low = lower(text);
    if (low == "salir" || low == "exit" || low == "quit") break;
    if (text.empty()) continue;
    tools::Emit emit = [&s](const json& e) {
      std::string type = jstr(e, "type");
      if (type == "token") {
        std::printf("%s", jstr(e, "text").c_str());
        std::fflush(stdout);
      } else if (type == "tool_call") {
        std::printf("\n[herramienta %s %s]\n", jstr(e, "name").c_str(), jdump(e.value("args", json::object())).c_str());
      } else if (type == "approval_required") {
        json ap = e.value("approval", json::object());
        std::printf("\n¿Aprobar %s %s? (s/n) ", jstr(ap, "tool").c_str(), jdump(ap.value("args", json::object())).c_str());
        std::fflush(stdout);
        std::string ans;
        std::getline(std::cin, ans);
        s.supervisor->resolve(jstr(ap, "id"), starts_with(lower(trim(ans)), "s"), jstr(ap, "level") == "admin");
      }
    };
    try {
      s.agents->run(agent, text, sid, {}, emit);
    } catch (const std::exception& e) {
      std::printf("\n[error] %s", e.what());
    }
    std::printf("\n");
  }
  s.shutdown();
  proc::cleanup();
  return 0;
}

int cmd_office(const Args& a) {
  office::Office o(get_config());
  if (a.has("quitar")) {
    o.clear();
    std::printf("Perfil de oficina quitado: vuelven los agentes por defecto de Nexus-Office.\n");
    return 0;
  }
  if (a.pos.empty()) {
    std::string active = o.active();
    std::printf("Perfiles de oficina (nexus-office oficina <perfil>):\n\n");
    for (auto& p : o.list()) {
      std::vector<std::string> trades, desk;
      for (auto& ag : p.value("agents", json::array())) (truthy(ag.value("gremio", json(false))) ? trades : desk).push_back(jstr(ag, "name"));
      std::printf("  %s %-26s %s\n", jstr(p, "id") == active ? "*" : " ", jstr(p, "id").c_str(), jstr(p, "name").c_str());
      std::printf("      gremio: %s\n      oficina: %s\n", join(trades, ", ").c_str(), join(desk, ", ").c_str());
    }
    std::printf(active.empty() ? "\n  Ningun perfil activo (agentes por defecto de Nexus-Office)\n" : "\n  * = perfil activo\n");
    return 0;
  }
  std::string pid = a.pos[0];
  json p;
  try {
    p = o.apply(pid, a.has("reset-agentes"));
  } catch (const std::invalid_argument& e) {
    die(e.what());
  }
  std::vector<std::string> names;
  for (auto& ag : p.value("agents", json::array())) names.push_back(jstr(ag, "name"));
  std::printf("Oficina «%s» lista (%s). Agentes: %s\n", jstr(p, "assistant_name").c_str(), jstr(p, "name").c_str(),
              join(names, ", ").c_str());
  std::printf("Cada agente tiene su propio grafo de conocimiento; se siembra al arrancar (nexus-office run).\n");
  json sugg = office::sector_models(pid);
  if (!sugg.empty()) {
    std::map<std::string, std::string> rec_names;
    for (auto& r : models::recommended_list()) rec_names[jstr(r, "id")] = jstr(r, "name");
    std::printf("Modelos de IA sugeridos para esta oficina (Modelos > Recomendados, o nexus-office modelos):\n");
    for (auto& [mid, why] : sugg.items())
      std::printf("  - %s: %s\n", (rec_names.count(mid) ? rec_names[mid] : mid).c_str(), server::Ctx::as_str(why).c_str());
  }
  return 0;
}

std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

double num(const json& v) { return v.is_number() ? v.get<double>() : 0.0; }

// importar grafos de cualquier formato como conocimiento, o exportarlos, desde la terminal y sin red
int cmd_graph(const Args& a) {
  std::string acc = a.pos.empty() ? "formatos" : a.pos[0];
  std::vector<std::string> files(a.pos.begin() + (a.pos.empty() ? 0 : 1), a.pos.end());
  std::string en = a.get("en");
  if (acc == "formatos") {
    std::printf("Formatos de grafo que se pueden importar (se reconocen por el contenido):\n\n");
    for (auto& f : kg::formats_json()["formats"]) {
      std::string ext = join(f["extensions"].get<std::vector<std::string>>(), " ");
      std::printf("  %s %s %s\n", rpad(jstr(f, "name"), 34).c_str(), rpad(ext, 34).c_str(), jstr(f, "description").c_str());
    }
    std::printf("\nUso: nexus-office grafo importar FICHERO [FICHERO...] [--en general|auto|<agente>]\n");
    return 0;
  }
  if (acc == "leer") {   // solo lee y muestra el grafo normalizado (sin tocar la memoria): pruebas y diagnostico
    kg::Files items;
    for (auto& f : files) items.emplace_back(S(fs::path(f).filename()), read_file(fs::path(f)).value_or(""));
    try {
      std::printf("%s\n", kg::parse_many(items).dump(1).c_str());
    } catch (const std::exception& e) {
      die(std::string("No se pudo leer el grafo: ") + e.what(), 1);
    }
    return 0;
  }
  Services s;
  if (acc == "importar") {
    if (files.empty()) die("Indica uno o varios ficheros", 1);
    kg::Files items;
    std::string names;
    for (auto& f : files) {
      auto data = read_file(fs::path(f));
      if (!data) die("No se puede leer " + f, 1);
      items.emplace_back(S(fs::path(f).filename()), std::move(*data));
      names += (names.empty() ? "" : ", ") + items.back().first;
    }
    json parsed;
    try {
      parsed = kg::parse_many(items);
    } catch (const std::exception& e) {
      die(std::string("No se pudo leer el grafo: ") + e.what(), 1);
    }
    std::string target = en == "general" ? "" : en;
    if (!target.empty() && target != "auto" && !s.agents->get(target))
      die("Agente desconocido: " + target + ". Agentes: " + join(s.agents->ids(), ", "), 1);
    std::printf("Formato %s: %zu nodos y %zu relaciones\n", jstr(parsed, "format").c_str(), parsed["nodes"].size(),
                parsed["edges"].size());
    json res = kg::insert_graph(*s.graph, parsed, target, s.organizer.get(), s.agents->ids(), "grafo:" + uleft(names, 100));
    std::printf("Importado: %lld nodos (%lld nuevos, %lld fusionados con los que ya habia) y %lld relaciones.\n",
                res["nodes"].get<long long>(), res["new"].get<long long>(), res["merged"].get<long long>(),
                res["relations"].get<long long>());
    std::vector<std::string> by;
    for (auto& [k, v] : res["by_scope"].items()) {
      auto ag = s.agents->get(k);
      by.push_back((ag ? ag->name : k) + ": " + server::Ctx::as_str(v));
    }
    std::printf("Por grafo: %s\n", join(by, ", ").c_str());
    return 0;
  }
  if (acc == "exportar") {
    std::string f = a.get("formato", "json");
    if (f != "json" && f != "graphml" && f != "csv") die("--formato: json, graphml o csv", 2);
    std::optional<std::string> scope;
    if (!en.empty() && en != "todo") scope = en == "general" ? "" : en;
    auto [data, mime, ext] = kg::export_graph(*s.graph, scope, f);
    fs::path out = files.empty() ? fs::path("nexus-conocimiento." + ext) : fs::path(files[0]);
    if (!write_file(out, data)) die("No se pudo escribir " + S(out), 1);
    std::printf("Exportado a %s (%zu bytes)\n", S(out).c_str(), data.size());
    return 0;
  }
  die("Acciones: formatos | importar | exportar", 1);
  return 1;
}

// mesa de bolsa simulada en la terminal (office/bolsa.h)
int cmd_bolsa(const Args& a) {
  auto& b = bolsa::get();
  std::string acc = a.pos.empty() ? "cartera" : a.pos[0];
  std::vector<std::string> rest(a.pos.begin() + (a.pos.empty() ? 0 : 1), a.pos.end());
  json cid = a.get("cartera").empty() ? json(nullptr) : json(std::stoll(a.get("cartera")));
  auto show = [](const json& x) { std::printf("%s\n", x.dump(2).c_str()); };
  auto need = [&](size_t n, const std::string& uso) {
    if (rest.size() < n) die("Uso: nexus-office bolsa " + uso, 1);
  };
  auto optnum = [&](const std::string& k) { return a.get(k).empty() ? json(nullptr) : json(std::stod(a.get(k))); };
  try {
    if (acc == "cartera") {
      json r = b.resumen(cid);
      std::printf("%s (%s, tarifa %s) · total %.2f € · efectivo %.2f € · rentabilidad %+.2f %% · gastos %.2f €\n",
                  jstr(r, "nombre").c_str(), jstr(r, "modo").c_str(), jstr(r, "tarifa").c_str(), num(r["total_eur"]),
                  num(r["efectivo_eur"]), num(r["rentabilidad_pct"]), num(r["gastos_eur"]));
      for (auto& p : r["posiciones"])
        std::printf("  %-10s %12g  coste %10.4f €  precio %10s %s  valor %10.2f €  %+.2f %%\n", jstr(p, "simbolo").c_str(),
                    num(p["cantidad"]), num(p["coste_medio_eur"]), server::Ctx::as_str(p["precio"]).c_str(),
                    jstr(p, "divisa").c_str(), num(p["valor_eur"]), num(p["plusvalia_pct"]));
      std::printf("\n%s\n", bolsa::AVISO);
    } else if (acc == "mercados") {
      for (auto& m : b.mercados()) {
        bool open = m.value("abierto", false);
        std::string nxt = open ? "cierra " + server::Ctx::as_str(m["cierra"]) : "abre " + server::Ctx::as_str(m["abre"]);
        std::printf("  %s %-8s %s  %s\n", rpad(jstr(m, "nombre"), 32).c_str(), open ? "ABIERTA" : "cerrada",
                    jstr(m, "hora_local").c_str(), nxt.c_str());
      }
    } else if (acc == "valores") {
      for (auto& i : b.instrumentos(join(rest, " ")))
        std::printf("  %-10s %-9s %s  %s\n", jstr(i, "simbolo").c_str(), jstr(i, "mercado").c_str(), jstr(i, "divisa").c_str(),
                    jstr(i, "nombre").c_str());
    } else if (acc == "cotizar") {
      need(1, "cotizar SIMBOLO");
      show(b.precio(rest[0]));
    } else if (acc == "comprar" || acc == "vender") {
      need(2, acc + " SIMBOLO CANTIDAD [--tipo limitada --limite P] [--stop P] [--validez hasta_cancelar]");
      show(b.ordenar(rest[0], acc, rest[1], a.get("tipo", "mercado"), optnum("limite"), optnum("stop"),
                     a.get("validez", "dia"), cid, "terminal"));
    } else if (acc == "ordenes") {
      for (auto& o : b.ordenes(cid))
        std::printf("  #%-5lld %-13s %-6s %10g %-10s %s %s\n", o["id"].get<long long>(), jstr(o, "estado").c_str(),
                    jstr(o, "lado").c_str(), num(o["cantidad"]), jstr(o, "simbolo").c_str(), jstr(o, "tipo").c_str(),
                    jstr(o, "motivo").c_str());
    } else if (acc == "operaciones") {
      for (auto& o : b.operaciones(cid)) {
        std::string g = o["ganancia_eur"].is_null() ? "" : fmt(" resultado %+.2f €", num(o["ganancia_eur"]));
        std::printf("  %s %-6s %10g %-10s a %.4f  total %.2f € (comisión %.2f, impuestos %.2f, cambio %.2f)%s\n",
                    jstr(o, "fecha").c_str(), jstr(o, "lado").c_str(), num(o["cantidad"]), jstr(o, "simbolo").c_str(),
                    num(o["precio"]), num(o["total_eur"]), num(o["comision_eur"]), num(o["impuestos_eur"]),
                    num(o["cambio_eur"]), g.c_str());
      }
    } else if (acc == "cancelar") {
      need(1, "cancelar ID");
      show(b.cancelar(std::stoll(rest[0])));
    } else if (acc == "confirmar") {   // solo una persona, en la terminal, confirma una orden de cartera real
      need(1, "confirmar ID");
      json o = b.orden(std::stoll(rest[0]));
      std::printf("Orden REAL #%lld: %s %g %s (%s). Nexus-Office no la envía a ningún bróker: después la ejecutas tú.\n",
                  o["id"].get<long long>(), jstr(o, "lado").c_str(), num(o["cantidad"]), jstr(o, "simbolo").c_str(),
                  jstr(o, "tipo").c_str());
      std::printf("Escribe %s para confirmarla: ", bolsa::CONFIRMACION);
      std::fflush(stdout);
      std::string line;
      std::getline(std::cin, line);
      show(b.confirmar(o["id"].get<long long>(), line));
    } else if (acc == "ejecucion") {
      need(2, "ejecucion ID PRECIO [--comision EUROS]");
      json com = optnum("comision");
      show(b.registrar_ejecucion(std::stoll(rest[0]), rest[1], com.is_null() ? json(0.0) : com));
    } else if (acc == "nueva") {
      show(b.crear_cartera(rest.empty() ? "Cartera simulada" : join(rest, " "), std::stod(a.get("capital", "10000")),
                           a.get("tarifa", "broker-online"), a.get("modo", "simulado")));
    } else if (acc == "reiniciar") {
      double cap = std::stod(a.get("capital", "10000"));
      show(b.reiniciar(cid, cap != 10000 ? json(cap) : json(nullptr)));
    } else if (acc == "importar") {
      need(2, "importar SIMBOLO fichero.csv");
      auto txt = read_file(fs::path(rest[1]));
      if (!txt) die("No se puede leer " + rest[1], 1);
      show(b.importar_csv(rest[0], *txt));
    } else if (acc == "fiscal") {
      show(b.informe_fiscal(cid, rest.empty() ? json(nullptr) : json(std::stoll(rest[0]))));
    } else {
      die("acción desconocida: " + acc, 1);
    }
  } catch (const std::exception& e) {   // ValueError, PermissionError y numeros mal escritos
    die(e.what(), 1);
  }
  return 0;
}

// ---------------------------------------------------------------- setup, wheels y cleanup
// setup: comprueba que los runtimes nativos de este equipo estan en el paquete (la edicion C++ no instala nada:
// voz, imagen, musica y NPU van dentro del binario o en bundled/runtimes)
int cmd_setup(const Args&) {
  log::setup();
  auto info = hw::detect();
  hw::save(info);
  json prof = hw::build_profile(info);
  runtimes::RuntimeManager rm;
  std::printf("Equipo: %s | acelerador %s\n", jstr(prof, "device_name", "?").c_str(), jstr(prof, "accelerator", "cpu").c_str());
  int missing = 0;
  for (auto* rt : {"llama", "sd"}) {
    auto want = runtimes::candidates(prof, rt);
    auto f = rm.exe(rt, prof);
    std::string name = std::string(rt) == "llama" ? "Runtime LLM llama.cpp" : "Runtime imagen sd.cpp";
    if (f) {
      std::printf("  [ok] %-24s %s (%s)\n", name.c_str(), f->variant.c_str(), S(f->exe).c_str());
    } else {
      ++missing;
      std::printf("  [--] %-24s falta la variante %s: copia el paquete offline completo o usa\n"
                  "       'nexus-office run' con internet para descargarla una vez\n", name.c_str(),
                  want.empty() ? "?" : want[0].c_str());
    }
  }
  json npu = prof.value("npu", json::object());
  if (truthy(npu.value("present", json(false))))
    std::printf("  [%s] NPU %s: %s\n", truthy(npu.value("ready", json(false))) ? "ok" : "--", jstr(npu, "vendor").c_str(),
                jstr(npu, "hint", "lista").c_str());
  std::printf("  [ok] Voz, transcripcion, musica y edicion de imagen: integradas en este binario\n");
  return missing ? 1 : 0;
}

int cmd_wheels(const Args&) {
  std::printf("La edicion C++ no usa Python ni wheels: todo va en el binario y en bundled/runtimes.\n"
              "Solo las herramientas de la comunidad (data/tools/community/*.py) usan el Python del sistema si lo hay.\n"
              "Para preparar un wheelhouse/ usa la edicion Python: python -m nexus wheels --help\n");
  return 0;
}

uintmax_t tree_size(const fs::path& p) {
  if (is_file(p)) return fsize(p);
  uintmax_t t = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    if (!ec && it->is_regular_file(ec)) t += it->file_size(ec);
  return t;
}

// variantes de bundled/runtimes que usa este equipo: la de su perfil, las que el selector de inicio puede pedir
// (GPU y NPU) y una CPU de respaldo. Ante cualquier duda no se borra ninguna.
std::set<fs::path> runtimes_in_use(const fs::path& bundled) {
  std::set<fs::path> keep;
  try {
    hw::HardwareInfo info;
    if (!hw::load_cached(info)) info = hw::detect("http://127.0.0.1:11434", false);
    json prof = hw::build_profile(info);
    runtimes::RuntimeManager rm;
    for (auto* rt : {"llama", "sd"}) {
      if (!is_dir(bundled / rt)) continue;
      std::vector<std::string> prefs = {""};
      if (std::string(rt) == "llama")
        for (auto* a : {"gpu", "npu"}) prefs.push_back(jstr(hw::plan(info, a), "variant"));
      for (auto& pref : prefs) {
        auto f = rm.exe(rt, prof, pref);
        if (!f || !is_relative_to(f->exe, bundled / rt)) continue;
        fs::path rel = resolve(f->exe).lexically_relative(resolve(bundled / rt));
        if (!rel.empty()) keep.insert(resolve(bundled / rt / *rel.begin()));
      }
      for (auto& v : runtimes::bundled_variants(rt))
        if (starts_with(v, "cpu")) {
          keep.insert(resolve(bundled / rt / P(v)));
          break;
        }
    }
  } catch (const std::exception&) {
    for (auto* rt : {"llama", "sd"})
      for (auto& v : glob(bundled / rt, "*")) keep.insert(resolve(v));
  }
  return keep;
}

bool ask(const std::string& q, bool def) {
#ifdef _WIN32
  bool tty = _isatty(_fileno(stdin));
#else
  bool tty = isatty(fileno(stdin));
#endif
  if (!tty) return def;
  std::printf("[Nexus-Office] %s [%s] ", q.c_str(), def ? "S/n" : "s/N");
  std::fflush(stdout);
  std::string ans;
  if (!std::getline(std::cin, ans)) return def;
  ans = lower(trim(ans));
  return ans.empty() ? def : (ans[0] == 's' || ans[0] == 'y');
}

// cleanup: quita del paquete offline las variantes de runtime (GPU o CPU) que este equipo no usa
int cmd_cleanup(const Args& a) {
  log::setup();
  fs::path root = paths::root(), bundled = root / "bundled" / "runtimes";
  auto keep = runtimes_in_use(bundled);
  std::vector<fs::path> unused;
  for (auto* rt : {"llama", "sd"})
    for (auto& v : glob(bundled / rt, "*"))
      if (is_dir(v) && !keep.count(resolve(v))) unused.push_back(v);
  for (auto& t : {root / ".cache" / "extract"})
    if (path_exists(t)) unused.push_back(t);
  uintmax_t bytes = 0;
  for (auto& p : unused) bytes += tree_size(p);
  double mb = bytes / 1048576.0;
  std::printf("[Nexus-Office] Limpieza de la instalacion: %zu elementos sin usar (%.0f MB).\n", unused.size(), mb);
  for (auto& k : keep)
    std::printf("[Nexus-Office]   se conserva el runtime %s\n", k.lexically_relative(resolve(root)).generic_string().c_str());
  json report = {{"removed", json::array()}, {"kept_for_reinstall", json::array()}, {"dry_run", a.has("dry-run")}};
  for (auto& p : unused) report["removed"].push_back(p.lexically_relative(root).generic_string());
  if (a.has("dry-run")) {
    for (auto& r : report["removed"]) std::printf("%s\n", r.get<std::string>().c_str());
    return 0;
  }
  if (!unused.empty() && (a.has("yes") || ask("¿Eliminar lo que este equipo no usa (" + fnum(mb, 0) + " MB)?", false))) {
    for (auto& p : unused) rm_rf(p);
    std::printf("[Nexus-Office] Eliminado: %.0f MB.\n", mb);
  } else {
    report["removed"] = json::array();
    std::printf("[Nexus-Office] Sin limpieza: el paquete queda completo (reinstalable en cualquier equipo de esta "
                "plataforma).\n");
  }
  mkdirs(root / ".cache");
  write_file(root / ".cache" / "limpieza.json", jdump(report, 2) + "\n");
  return 0;
}

const char* HELP =
    "Nexus-Office " NEXUS_VERSION " - oficina de IA local para pymes (edicion C++)\n\n"
    "  nexus-office run [--host H] [--port P] [--no-browser] [--motor auto|llamaserver] [--acelerador auto|cpu|gpu|npu]\n"
    "  nexus-office detect                      detecta el hardware y muestra el perfil\n"
    "  nexus-office pull <id|auto>              descarga un modelo recomendado\n"
    "  nexus-office modelos [--descargar] [--todos]   elige (y descarga) el modelo de IA de cada tipo\n"
    "  nexus-office servicio instalar|quitar|estado [--red] [--puerto P]\n"
    "  nexus-office chat [--agent A]            chat en la terminal\n"
    "  nexus-office oficina [<perfil>|--quitar] [--reset-agentes]\n"
    "  nexus-office bolsa [cartera|mercados|valores|cotizar|comprar|vender|ordenes|operaciones|cancelar|confirmar|\n"
    "                     ejecucion|nueva|reiniciar|importar|fiscal] [--cartera N] [--tipo T] [--limite P] [--stop P]\n"
    "                     [--validez dia|hasta_cancelar] [--capital C] [--tarifa T] [--modo simulado|real] [--comision E]\n"
    "  nexus-office grafo [formatos|importar|exportar] [FICHEROS...] [--en general|auto|<agente>|todo]\n"
    "                     [--formato json|graphml|csv]\n"
    "  nexus-office setup                       comprueba los runtimes de este equipo en el paquete\n"
    "  nexus-office cleanup [--yes] [--dry-run] quita las variantes de runtime que este equipo no usa\n"
    "  nexus-office --version\n";

}  // namespace

int run_main(std::vector<std::string> argv) {
  std::string cmd = argv.empty() ? "run" : argv[0];
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    std::printf("%s", HELP);
    return 0;
  }
  if (cmd == "--version" || cmd == "-V") {
    std::printf("Nexus-Office %s\n", NEXUS_VERSION);
    return 0;
  }
  if (starts_with(cmd, "--")) cmd = "run";   // "nexus-office --no-browser" = run --no-browser
  else argv.erase(argv.begin());
  if (cmd == "servicio") return service::main(argv);
  if (cmd == "run") return cmd_run(parse(argv, {"host", "port", "motor", "acelerador"}));
  if (cmd == "detect") return cmd_detect();
  if (cmd == "pull") {
    if (argv.empty()) die("uso: nexus-office pull <id-recomendado|auto>", 2);
    return cmd_pull(argv[0]);
  }
  if (cmd == "modelos") return cmd_models(parse(argv, {}));
  if (cmd == "chat") return cmd_chat(parse(argv, {"agent"}));
  if (cmd == "oficina") return cmd_office(parse(argv, {}));
  if (cmd == "grafo") return cmd_graph(parse(argv, {"en", "formato"}));
  if (cmd == "bolsa")
    return cmd_bolsa(parse(argv, {"cartera", "tipo", "limite", "stop", "validez", "capital", "tarifa", "modo", "comision"}));
  if (cmd == "setup") return cmd_setup(parse(argv, {}));
  if (cmd == "wheels") return cmd_wheels(parse(argv, {}));
  if (cmd == "cleanup") return cmd_cleanup(parse(argv, {}));
  std::fprintf(stderr, "orden desconocida: %s\n\n%s", cmd.c_str(), HELP);
  return 2;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(S(fs::path(argv[i])));
  return run_main(args);
}
#else
int main(int argc, char** argv) { return run_main(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
