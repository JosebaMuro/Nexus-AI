// Comprobacion y autorreparacion al arrancar: configuracion danada, base de datos danada y copia diaria.
#include <httplib.h>
#include <sqlite3.h>

#include <chrono>
#include <fstream>

#include "check.h"
#include "core/config.h"
#include "core/salud.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;

namespace {

fs::path datos_salud(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() /
             ("nexus-salud-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  return dir;
}

void sql(const fs::path& db, const std::string& s) {
  sqlite3* h = nullptr;
  sqlite3_open(S(db).c_str(), &h);
  sqlite3_exec(h, s.c_str(), nullptr, nullptr, nullptr);
  sqlite3_close(h);
}

long long cuenta(const fs::path& db, const std::string& tabla) {
  sqlite3* h = nullptr;
  sqlite3_stmt* st = nullptr;
  long long n = -1;
  sqlite3_open_v2(S(db).c_str(), &h, SQLITE_OPEN_READONLY, nullptr);
  if (sqlite3_prepare_v2(h, ("SELECT count(*) FROM " + tabla).c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(h);
  return n;
}

json item(const json& inf, const std::string& id) {
  for (auto& c : inf["comprobaciones"])
    if (c["id"] == id) return c;
  return json();
}

// base de datos de 3 paginas de 4 KB: esquema, notas (pagina 2) y tareas (pagina 3)
void crear_bd(const fs::path& db) {
  sql(db,
      "PRAGMA page_size=4096; PRAGMA journal_mode=DELETE;"
      "CREATE TABLE notas(id INTEGER PRIMARY KEY, txt TEXT); CREATE TABLE tareas(id INTEGER PRIMARY KEY, txt TEXT);"
      "INSERT INTO notas(txt) VALUES ('uno'),('dos'),('tres'); INSERT INTO tareas(txt) VALUES ('a'),('b');");
}

}  // namespace

TEST("salud: configuracion danada se restaura de la ultima buena") {
  std::string antes = env("NEXUS_DATA_DIR");
  fs::path d = datos_salud("config");
  write_file(d / "config.yaml", "server: [1, 2\n  port: :\n");
  write_file(d / "config.yaml.bueno", "server:\n  port: 9123\n");
  json inf = salud::previa(true);
  CHECK_EQ(jstr(item(inf, "configuracion"), "estado"), std::string("reparado"));
  CHECK(read_text(d / "config.yaml").find("9123") != std::string::npos);
  CHECK(!glob(d, "config.yaml.danado-*").empty());
  CHECK_EQ(jstr(item(inf, "carpetas"), "estado"), std::string("ok"));
  // sin copia buena: valores por defecto (el fichero danado se aparta y Config lo vuelve a crear)
  write_file(d / "config.yaml", "{{{");
  remove_file(d / "config.yaml.bueno");
  inf = salud::previa(true);
  CHECK_EQ(jstr(item(inf, "configuracion"), "estado"), std::string("reparado"));
  CHECK(!is_file(d / "config.yaml"));
  // guardar_copias deja la configuracion buena para la proxima vez
  write_file(d / "config.yaml", "server:\n  port: 9555\n");
  salud::guardar_copias();
  CHECK(read_text(d / "config.yaml.bueno").find("9555") != std::string::npos);
  CHECK(is_file(d / "salud.json"));
  set_env("NEXUS_DATA_DIR", antes);
}

TEST("salud: base de datos danada se rescata y lo ilegible sale de la copia diaria") {
  std::string antes = env("NEXUS_DATA_DIR");
  fs::path d = datos_salud("bd");
  fs::path db = d / "nexus.db";
  crear_bd(db);
  CHECK_EQ(cuenta(db, "notas"), 3LL);
  // copia diaria de hoy (VACUUM INTO) con todo sano
  salud::guardar_copias();
  auto copias = glob(salud::detail::carpeta_copias(), "nexus-*.db");
  CHECK_EQ(copias.size(), (size_t)1);
  CHECK_EQ(cuenta(copias[0], "tareas"), 2LL);
  // se estropea la pagina de "tareas"
  {
    std::fstream f(db, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(2 * 4096);
    std::string basura(4096, '\x5A');
    f.write(basura.data(), basura.size());
  }
  json inf = salud::previa(true);
  json bd = item(inf, "base_datos");
  CHECK_EQ(jstr(bd, "estado"), std::string("reparado"));
  CHECK(jstr(bd, "detalle").find("tareas") != std::string::npos);
  CHECK_EQ(cuenta(db, "notas"), 3LL);    // rescatada de la danada
  CHECK_EQ(cuenta(db, "tareas"), 2LL);   // de la copia
  CHECK(!glob(d, "nexus.db.danado-*").empty());
  inf = salud::previa(true);
  CHECK_EQ(jstr(item(inf, "base_datos"), "estado"), std::string("ok"));

  // ilegible del todo: se vuelve a la copia diaria
  write_file(db, std::string(8192, '\x01'));
  inf = salud::previa(true);
  CHECK_EQ(jstr(item(inf, "base_datos"), "estado"), std::string("reparado"));
  CHECK_EQ(cuenta(db, "notas"), 3LL);
  set_env("NEXUS_DATA_DIR", antes);
}

TEST("salud: sin copia, una base de datos ilegible deja sitio a una nueva") {
  std::string antes = env("NEXUS_DATA_DIR");
  fs::path d = datos_salud("nueva");
  write_file(d / "nexus.db", "esto no es una base de datos, pero ocupa algo de sitio en el disco.......");
  json r = salud::detail::rescatar_bd(d / "nexus.db", {});
  CHECK(r["ok"] == true);
  CHECK(jstr(r, "detalle").find("nueva") != std::string::npos);
  CHECK(!glob(d, "nexus.db.danado-*").empty());
  set_env("NEXUS_DATA_DIR", antes);
}

TEST("salud: puerto ocupado por otro programa, se usa el siguiente libre") {
  httplib::Server otro;
  int p = otro.bind_to_any_port("127.0.0.1");
  CHECK(p > 0);
  int usado = salud::puerto("127.0.0.1", p);
  CHECK(usado != p && usado > 0);
  CHECK_EQ(jstr(item(salud::informe(), "puerto"), "estado"), std::string("reparado"));
  otro.stop();
}
