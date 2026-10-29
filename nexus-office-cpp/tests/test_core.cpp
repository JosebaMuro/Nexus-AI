#include "check.h"
#include "core/config.h"
#include "core/db.h"
#include "util/fs.h"
#include "util/yaml.h"

using namespace nx;

TEST("db: esquema, filas, blobs y transacciones") {
  DB db(paths::data_dir() / "t1.db");
  db.execute("INSERT INTO sessions(id, title, created, updated) VALUES(?,?,?,?)", {"s1", "Hola ñ", 1.5, 2});
  auto r = db.one("SELECT * FROM sessions WHERE id=?", {"s1"});
  CHECK(r.has_value());
  CHECK_EQ((*r)["title"].get<std::string>(), std::string("Hola ñ"));
  CHECK_EQ((*r)["updated"].get<long long>(), 2LL);
  std::vector<float> v = {0.5f, -1.0f, 3.25f};
  db.execute("INSERT INTO memories(kind, content, embedding) VALUES(?,?,?)", {"fact", "x", blob(v)});
  auto m = db.one("SELECT embedding FROM memories");
  CHECK(floats((*m)["embedding"]) == v);
  bool threw = false;
  try {
    db.transaction([&] {
      db.execute("INSERT INTO sessions(id) VALUES('s2')");
      throw std::runtime_error("x");
    });
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  CHECK_EQ(db.scalar("SELECT COUNT(*) FROM sessions").get<long long>(), 1LL);
  db.executemany("INSERT INTO lounge(agent, kind) VALUES(?,?)", {{"a", "k"}, {"b", "k"}});
  CHECK_EQ(db.scalar("SELECT COUNT(*) FROM lounge").get<long long>(), 2LL);
  auto ret = db.returning("INSERT INTO feedback(score) VALUES(1) RETURNING id");
  CHECK(ret && (*ret)["id"].get<long long>() == 1);
  db.set_profile("gusto", json{{"a", 1}});
  CHECK_EQ(db.get_profile()["gusto"]["a"].get<int>(), 1);
}

TEST("config: valores por defecto, PIN y vista") {
  reset_config();
  Config& c = get_config();
  CHECK_EQ(c.str("server.port"), std::string("8775"));
  CHECK_EQ(c.num("llm.temperature"), 0.7);
  c.set_pin("4321");
  CHECK(c.check_pin("4321"));
  CHECK(!c.check_pin("0000"));
  c.set("llm.openai_api_key", "sk-x");
  auto v = c.view(true);
  CHECK_EQ(v["llm"]["openai_api_key"].get<std::string>(), std::string("********"));
  CHECK(!v.contains("admin"));
  bool denied = false;
  try {
    c.update({{"llm.temperature", 0.2}}, false);
  } catch (const PermissionError&) {
    denied = true;
  }
  CHECK(denied);
  reset_config();
  CHECK(get_config().check_pin("4321"));   // persistido y releido
}

#ifdef NEXUS_TESTS_DIR
#include "memory/kg.h"
#include "util/text.h"

// Un ejemplo real de cada formato de grafo (tests/kg_samples), comprimidos incluidos: todos se leen y dan nodos,
// salvo x.xml, un XML cualquiera que no es un grafo y debe rechazarse con un error claro
TEST("grafos: los ejemplos de cada formato se leen") {
  int leidos = 0;
  for (auto& e : fs::directory_iterator(fs::path(NEXUS_TESTS_DIR) / "kg_samples")) {
    auto data = nx::read_file(e.path());
    CHECK(data.has_value());
    std::string name = e.path().filename().string();
    json g;
    std::string error;
    try {
      g = nx::kg::parse_many({{name, *data}});
    } catch (const std::exception& ex) {
      error = ex.what();
    }
    if (name == "x.xml") {
      CHECK(contains(error, "no tiene elementos de nodo o enlace"));
      continue;
    }
    bool ok = g.is_object() && !g.value("nodes", json::array()).empty();
    if (!ok) std::fprintf(stderr, "  %s: %s\n", name.c_str(), error.empty() ? "sin nodos" : error.c_str());
    CHECK(ok);
    leidos += ok;
  }
  CHECK(leidos >= 50);
}
#endif
