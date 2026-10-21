// CLI de Nexus-Office (__main__.py), edicion C++:
//
//   nexus-office run [--host H] [--port P] [--no-browser] [--motor auto|llamaserver] [--acelerador auto|cpu|gpu|npu]
//   nexus-office detect
//   nexus-office pull <id-recomendado|auto>
//   nexus-office modelos [--descargar] [--todos]   (evalua el hardware y elige el modelo de IA de cada tipo)
//   nexus-office servicio instalar|quitar|estado [--red] [--puerto P]   (arranque automatico con el equipo)
//   nexus-office chat [--agent A]
//   nexus-office oficina [<perfil>|--quitar] [--reset-agentes]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include "agents/agents.h"
#include "core/config.h"
#include "core/service.h"
#include "core/services.h"
#include "core/system.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/models.h"
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
  for (auto* listeners : {&s.supervisor->listeners, &s.lounge->listeners})
    listeners->push_back([&app](const json& e) { app.broadcast(e); });
  app.on_shutdown_request = [] { g_stop = true; };
  s.start();
  std::thread watcher([&app] {
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.stop();
  });
  if (browser)
    std::thread([url] {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (!g_stop) sys2::open_url(url);
    }).detach();
  bool ok = app.listen(host, port);
  g_stop = true;
  watcher.join();
  if (!ok && !app.stopping) std::fprintf(stderr, "  No se pudo abrir el puerto %d en %s (¿ya esta en uso?)\n", port, host.c_str());
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

const char* HELP =
    "Nexus-Office " NEXUS_VERSION " - oficina de IA local para pymes (edicion C++)\n\n"
    "  nexus-office run [--host H] [--port P] [--no-browser] [--motor auto|llamaserver] [--acelerador auto|cpu|gpu|npu]\n"
    "  nexus-office detect                      detecta el hardware y muestra el perfil\n"
    "  nexus-office pull <id|auto>              descarga un modelo recomendado\n"
    "  nexus-office modelos [--descargar] [--todos]   elige (y descarga) el modelo de IA de cada tipo\n"
    "  nexus-office servicio instalar|quitar|estado [--red] [--puerto P]\n"
    "  nexus-office chat [--agent A]            chat en la terminal\n"
    "  nexus-office oficina [<perfil>|--quitar] [--reset-agentes]\n"
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
  if (cmd == "grafo" || cmd == "bolsa" || cmd == "wheels" || cmd == "cleanup" || cmd == "setup") {
    std::fprintf(stderr, "'%s' todavia no esta disponible en la edicion C++ (2.0 beta C). Usa la edicion Python.\n", cmd.c_str());
    return 2;
  }
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
