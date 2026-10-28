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
