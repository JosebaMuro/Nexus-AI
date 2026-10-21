// Prueba de integracion: servicios completos + servidor HTTP/WebSocket en un puerto libre, como lo usa la interfaz.
#include <httplib.h>

#include <thread>

#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/fs.h"
#include "util/net.h"

using namespace nx;

namespace {

struct Server {
  std::unique_ptr<Services> s;
  std::unique_ptr<server::App> app;
  std::thread th;
  int port = 0;
  Server() {
    // carpeta de datos propia: otras pruebas ya fijan un PIN de administrador
    auto dir = std::filesystem::temp_directory_path() / ("nexus-server-" + std::to_string(::time(nullptr)));
    std::filesystem::create_directories(dir);
    set_env("NEXUS_DATA_DIR", S(dir));
    reset_config();
    s = std::make_unique<Services>(false);
    app = std::make_unique<server::App>(s.get());
    s->supervisor->listeners.push_back([this](const json& e) { app->broadcast(e); });
    s->start();
    port = net::free_port(18775);
    th = std::thread([this] { app->listen("127.0.0.1", port); });
    httplib::Client c("127.0.0.1", port);
    for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ~Server() {
    app->stop();
    th.join();
    s->shutdown();
  }
};

json body(const httplib::Result& r) { return r ? jparse(r->body) : json(); }

}  // namespace

TEST("servidor: API, administrador, memoria, biblioteca y chat por WebSocket") {
  Server srv;
  httplib::Client c("127.0.0.1", srv.port);
  c.set_read_timeout(120);

  auto ping = c.Get("/api/ping");
  CHECK(ping && ping->status == 200);
  CHECK_EQ(jstr(body(ping), "version"), std::string(NEXUS_VERSION));

  auto st = c.Get("/api/status");
  CHECK(st && st->status == 200);
  json status = body(st);
  for (auto* k : {"profile", "llm", "memory", "graph", "rag", "voice", "image"}) CHECK(status.contains(k));

  auto index = c.Get("/");
  CHECK(index && index->status == 200 && index->body.find("Nexus-Office") != std::string::npos);
  auto js = c.Get("/static/js/app.js");
  CHECK(js && js->status == 200);

  // 422 al estilo de FastAPI y 403 sin administrador
  auto missing = c.Post("/api/memory", "{}", "application/json");
  CHECK(missing && missing->status == 422);
  CHECK_EQ(jstr(body(missing)["detail"][0], "type"), std::string("missing"));
  auto forbidden = c.Put("/api/agents", R"({"id":"x","name":"X"})", "application/json");
  CHECK(forbidden && forbidden->status == 403);

  // administrador
  auto setup = c.Post("/api/admin/setup", R"({"pin":"2468"})", "application/json");
  CHECK(setup && setup->status == 200);
  std::string tok = jstr(body(setup), "token");
  CHECK(!tok.empty());
  auto bad = c.Post("/api/admin/login", R"({"pin":"0000"})", "application/json");
  CHECK(bad && bad->status == 401);
  httplib::Headers adm = {{"X-Admin-Token", tok}};
  auto agent = c.Put("/api/agents", adm, R"({"id":"probador","name":"Probador","keywords":["prueba"]})", "application/json");
  CHECK(agent && agent->status == 200 && jstr(body(agent), "id") == "probador");

  // memoria y grafo
  auto mem = c.Post("/api/memory", R"({"content":"Me llamo Ana y trabajo en una carpinteria de Bilbao"})", "application/json");
  CHECK(mem && mem->status == 200 && body(mem).contains("id"));
  auto search = c.Get("/api/memory/search?q=carpinteria");
  CHECK(search && search->status == 200 && body(search).is_array() && !body(search).empty());

  // biblioteca RAG por multipart
  httplib::UploadFormDataItems items = {
      {"file", std::string(200, 'x') + "\nEl IVA general es del 21 por ciento.\n", "nota.md", "text/markdown"},
      {"agent", "", "", ""}};
  auto up = c.Post("/api/rag/upload", items);
  CHECK(up && up->status == 200 && jnum(body(up).value("chunks", json(0))) > 0);
  auto rs = c.Get("/api/rag/search?q=IVA");
  CHECK(rs && rs->status == 200 && !body(rs).empty());

  // multimedia sin modelos: imagen procedimental y musica compuesta
  auto img = c.Post("/api/image/generate", R"({"prompt":"un faro al atardecer","seed":3})", "application/json");
  CHECK(img && img->status == 200 && !jstr(body(img), "url").empty());
  auto mus = c.Post("/api/music/generate", R"({"prompt":"musica tranquila","seconds":3})", "application/json");
  CHECK(mus && mus->status == 200 && !body(mus).contains("path"));

  // lo que aun no esta en C++ responde 501 con un mensaje
  auto coop = c.Post("/api/coop/start", "{}", "application/json");
  CHECK(coop && coop->status == 501);

  // chat por WebSocket con el motor integrado (sin modelo): usa la calculadora
  httplib::ws::WebSocketClient ws("ws://127.0.0.1:" + std::to_string(srv.port) + "/ws");
  CHECK(ws.connect());
  ws.set_read_timeout(60);
  std::string msg;
  CHECK(ws.read(msg) == httplib::ws::ReadResult::Text);
  CHECK_EQ(jstr(jparse(msg), "type"), std::string("hello"));
  ws.send(R"({"type":"chat","text":"Cuanto es 12*7?","agent":"auto"})");
  std::string answer, last;
  for (int i = 0; i < 200 && last != "done"; ++i) {
    if (ws.read(msg) != httplib::ws::ReadResult::Text) break;
    json e = jparse(msg);
    last = jstr(e, "type");
    if (last == "token") answer += jstr(e, "text");
  }
  CHECK_EQ(last, std::string("done"));
  CHECK(answer.find("84") != std::string::npos);
  ws.close();
}
