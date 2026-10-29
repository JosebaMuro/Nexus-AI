// Conversaciones largas: el hilo del proyecto no se pierde aunque la charla no quepa en el contexto del modelo
#include "check.h"
#include "core/db.h"
#include "llm/llm.h"
#include "memory/hilo.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;

static std::string relleno(int i) {
  return "Mensaje " + std::to_string(i) + ": seguimos con la maquetacion de la pagina de inicio, colores, margenes y " +
         "tipografias de la tienda. " + std::string(400, 'x');
}

// sesion larga: el primer mensaje fija un dato clave que luego queda muy lejos
static void sesion_larga(DB& db, const std::string& sid, int n) {
  db.execute("INSERT INTO sessions(id, title, created, updated) VALUES(?,?,0,0)", {sid, "Proyecto Atlas"});
  db.execute("INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,0)",
             {sid, "user", "El proyecto Atlas usara PostgreSQL 16 como base de datos y el puerto 5433."});
  db.execute("INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,0)",
             {sid, "assistant", "Anotado: Atlas con PostgreSQL 16 en el puerto 5433."});
  for (int i = 0; i < n; ++i)
    db.execute("INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,0)",
               {sid, i % 2 ? "assistant" : "user", relleno(i)});
}

TEST("hilo: fit_context nunca tira la peticion actual") {
  json msgs = json::array();
  msgs.push_back({{"role", "system"}, {"content", "Eres un asistente."}});
  for (int i = 0; i < 40; ++i) msgs.push_back({{"role", i % 2 ? "assistant" : "user"}, {"content", relleno(i)}});
  msgs.push_back({{"role", "user"}, {"content", "PREGUNTA ACTUAL"}, {"keep", true}});
  msgs.push_back({{"role", "assistant"}, {"content", "<tool>{\"name\":\"buscar\"}</tool>"}});
  msgs.push_back({{"role", "user"}, {"content", "Resultado: " + std::string(9000, 'r')}});
  json out = llm::fit_context(msgs, 2048, 600);
  bool pregunta = false;
  for (auto& m : out) {
    if (jstr(m, "content") == "PREGUNTA ACTUAL") pregunta = true;
    CHECK(!m.contains("keep"));
  }
  CHECK(pregunta);
  CHECK_EQ(jstr(out[0], "role"), std::string("system"));
  CHECK(starts_with(jstr(out.back(), "content"), "Resultado: "));   // el resultado se recorta, no se pierde
  double total = 0;
  for (auto& m : out) total += ulen(jstr(m, "content")) / 3.2 + 8;
  CHECK(total <= 2048 - 600);
}

TEST("hilo: presupuesto, mensajes recientes y recuperacion de lo antiguo") {
  DB db(paths::data_dir() / "hilo1.db");
  sesion_larga(db, "s1", 120);
  mem::Hilo h(db);
  auto p = h.plan("s1", "¿Que base de datos y que puerto usaba Atlas?", 1500);
  CHECK(p.tokens <= 1500);
  CHECK(!p.recientes.empty());
  CHECK(contains(jstr(p.recientes.back(), "content"), "Mensaje 119"));   // lo ultimo siempre entra
  CHECK(p.omitidos > 0);
  bool dato = false;
  for (auto& r : p.recuerdos) dato = dato || contains(r, "PostgreSQL 16");
  CHECK(dato);   // el dato del principio vuelve porque viene al caso
}

TEST("hilo: resumen progresivo y notas fijas") {
  DB db(paths::data_dir() / "hilo2.db");
  sesion_larga(db, "s2", 60);
  mem::Hilo h(db);
  // sin LLM: resumen extractivo
  int n = h.plegar("s2", 800, 300);
  CHECK(n > 0);
  auto st = h.estado("s2");
  CHECK(st["plegados"].get<long long>() == n);
  CHECK(contains(jstr(st, "resumen"), "PostgreSQL"));   // el arranque del proyecto se conserva
  CHECK(mem::Hilo::tokens(jstr(st, "resumen")) <= 300);
  CHECK_EQ(h.plegar("s2", 800, 300), 0);   // ya cabe: no se vuelve a plegar en cada turno

  // con LLM: recibe el resumen anterior y los mensajes nuevos
  for (int i = 60; i < 100; ++i)
    db.execute("INSERT INTO messages(session_id, role, content, ts) VALUES(?,?,?,0)",
               {"s2", i % 2 ? "assistant" : "user", relleno(i)});
  std::string visto;
  int llamadas = 0;
  int m = h.plegar("s2", 800, 300, [&](const std::string& prompt, int) {
    visto = prompt;
    ++llamadas;
    return "<think>pensando</think>- Objetivo: tienda Atlas\n- Datos: PostgreSQL 16, puerto 5433\n- Pendiente: portada";
  });
  CHECK(m > 0 && llamadas > 0);
  CHECK(contains(visto, "RESUMEN ACTUAL"));
  CHECK(contains(visto, "PostgreSQL"));
  st = h.estado("s2");
  CHECK(starts_with(jstr(st, "resumen"), "- Objetivo: tienda Atlas"));

  h.guardar("s2", {{"notas", "Entrega el 15 de noviembre. Todo en C++."}});
  auto p = h.plan("s2", "¿Cuando entregamos?", 1200);
  std::string t = p.texto();
  CHECK(contains(t, "Notas fijas"));
  CHECK(contains(t, "15 de noviembre"));
  CHECK(contains(t, "tienda Atlas"));
  CHECK(p.tokens <= 1200);
  h.olvidar("s2");
  CHECK(jstr(h.estado("s2"), "notas").empty());
}

#include <httplib.h>

#include <thread>

#include "core/config.h"
#include "core/services.h"
#include "util/net.h"

// el servidor dice que la peticion no cabe (como llama-server): se reajusta con sus cifras y se reintenta una vez,
// sin perder la pregunta actual
TEST("hilo: si el servidor dice que no cabe, se reajusta y se reintenta") {
  httplib::Server srv;
  std::vector<json> peticiones;
  std::mutex mu;
  srv.Get("/api/version", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"version":"0.9"})", "application/json");
  });
  srv.Get("/api/tags", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"models":[{"name":"fake"}]})", "application/json");
  });
  srv.Post("/api/chat", [&](const httplib::Request& req, httplib::Response& res) {
    json body = jparse(req.body);
    size_t chars = 0;
    for (auto& m : body["messages"]) chars += ulen(jstr(m, "content"));
    bool primera;
    {
      std::lock_guard<std::mutex> lk(mu);
      peticiones.push_back(body);
      primera = peticiones.size() == 1;
    }
    if (primera) {   // el servidor cuenta el doble de tokens de los que estimamos
      res.status = 400;
      int real = (int)(chars / 3.2 * 2);
      res.set_content(cat(R"({"error":{"code":400,"message":"request exceeds the available context size",)",
                          R"("type":"exceed_context_size_error","n_prompt_tokens":)", real, R"(,"n_ctx":4096}})"),
                      "application/json");
      return;
    }
    res.set_content(R"({"message":{"content":"respuesta"},"done":true,"eval_count":1})", "application/x-ndjson");
  });
  int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  auto dir = std::filesystem::temp_directory_path() / ("nexus-hilo-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  {
    Services s(false);
    s.config.set("llm.ollama_url", "http://127.0.0.1:" + std::to_string(port));
    s.llm->rebuild();
    s.llm->update_llm_profile({{"n_ctx", 4096}});
    json msgs = json::array();
    msgs.push_back({{"role", "system"}, {"content", "Eres un asistente."}});
    for (int i = 0; i < 30; ++i) msgs.push_back({{"role", i % 2 ? "assistant" : "user"}, {"content", relleno(i)}});
    msgs.push_back({{"role", "user"}, {"content", "PREGUNTA ACTUAL"}, {"keep", true}});
    std::string out;
    llm::Opts o;
    s.llm->stream(msgs, "ollama:fake", o, [&](const std::string& p) { out += p; });
    CHECK_EQ(out, std::string("respuesta"));
    CHECK_EQ(peticiones.size(), (size_t)2);
    if (peticiones.size() == 2) {
      auto& seg = peticiones[1]["messages"];
      CHECK(seg.size() < peticiones[0]["messages"].size());
      CHECK_EQ(jstr(seg.back(), "content"), std::string("PREGUNTA ACTUAL"));
      size_t chars = 0;
      for (auto& m : seg) chars += ulen(jstr(m, "content"));
      CHECK(chars / 3.2 * 2 < 4096);   // con la cuenta del servidor ya cabe
    }
    s.shutdown();
  }
  srv.stop();
  th.join();
}
