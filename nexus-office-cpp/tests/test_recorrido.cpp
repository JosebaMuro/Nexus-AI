// Recorrido de las consultas por el grafo: lo que el avatar ilumina como una luz que busca y recoge la informacion
#include "check.h"
#include "core/db.h"
#include "memory/memory.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;

static bool hay_nodo(const json& pasos, long long id) {
  for (auto& p : pasos)
    if (p.contains("n") && p["n"].get<long long>() == id) return true;
  return false;
}

TEST("recorrido: semillas, aristas recorridas desde la semilla y datos de los nodos") {
  fs::remove(paths::data_dir() / "recorrido1.db");
  DB db(paths::data_dir() / "recorrido1.db");
  mem::KnowledgeGraph g(db);
  g.relate("Atlas", "usa", "PostgreSQL", "concepto", "concepto", 2.0);
  g.relate("Ana", "dirige", "Atlas", "persona", "concepto", 1.5);
  g.upsert_entity("Atlas", "concepto", 1.0, "", "Proyecto de la tienda en linea");
  auto id_de = [&](const std::string& n) {
    return db.one("SELECT id FROM entities WHERE scope='' AND key=?", {mem::gkey(n)}).value()["id"].get<long long>();
  };
  long long atlas = id_de("Atlas"), pg = id_de("PostgreSQL"), ana = id_de("Ana");

  // sin grabar: nada cambia y no se apunta nada
  CHECK(mem::Recorrido::actual() == nullptr);
  auto sin = g.facts_about({"Atlas"});
  CHECK_EQ(sin.size(), (size_t)2);

  json ev;
  {
    mem::Recorrido rec;
    CHECK(mem::Recorrido::actual() == &rec);
    auto hechos = g.facts_about({"Atlas"});
    CHECK(hechos == sin);   // la grabacion no altera la respuesta
    auto notas = g.notes_about({"Atlas"});
    CHECK_EQ(notas.size(), (size_t)1);
    ev = rec.evento("", "contexto");
  }
  CHECK(mem::Recorrido::actual() == nullptr);   // al salir se desinstala
  CHECK_EQ(jstr(ev, "type"), std::string("graph_trace"));
  const json& pasos = ev["pasos"];
  CHECK(!pasos.empty());
  if (pasos.empty()) return;
  CHECK(pasos[0].contains("n") && pasos[0]["n"].get<long long>() == atlas);   // la luz parte de la semilla
  CHECK(hay_nodo(pasos, atlas));
  int aristas = 0, desde_semilla = 0;
  for (auto& p : pasos)
    if (p.contains("a")) {
      ++aristas;
      if (p["a"].get<long long>() == atlas) ++desde_semilla;
      long long b = p["b"].get<long long>();
      CHECK(b == pg || b == ana);   // recoge el dato del otro extremo
    }
  CHECK_EQ(aristas, 2);
  CHECK_EQ(desde_semilla, 2);   // aunque la relacion apunte a la semilla (Ana dirige Atlas), se recorre desde ella
  int notas_atlas = 0;
  for (auto& p : pasos) notas_atlas += p.contains("n") && p["n"].get<long long>() == atlas;
  CHECK_EQ(notas_atlas, 1);   // sin pasos repetidos
  // la web conoce cada nodo aunque no este en el grafo que dibuja
  CHECK_EQ(jstr(ev["nodos"][std::to_string(pg)], "name"), std::string("PostgreSQL"));
  CHECK_EQ(jstr(ev["nodos"][std::to_string(ana)], "type"), std::string("persona"));
}

TEST("recorrido: limite de pasos y recorridos anidados") {
  fs::remove(paths::data_dir() / "recorrido2.db");
  DB db(paths::data_dir() / "recorrido2.db");
  mem::KnowledgeGraph g(db);
  for (int i = 0; i < 30; ++i) g.relate("Centro", "tiene", "Pieza " + std::to_string(i));
  mem::Recorrido fuera(10);
  {
    mem::Recorrido dentro(5);
    g.facts_about({"Centro"}, 30);
    CHECK_EQ(dentro.to_json()["pasos"].size(), (size_t)5);
  }
  CHECK(mem::Recorrido::actual() == &fuera);
  CHECK(fuera.empty());
  g.facts_about({"Centro"}, 30);
  CHECK_EQ(fuera.to_json()["pasos"].size(), (size_t)10);
}
