// Comprobacion y autorreparacion al arrancar (ver salud.h).
#include "core/salud.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>

#include "core/services.h"
#include "core/system.h"
#include "devices/drivers.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/sysinfo.h"
#include "util/text.h"
#include "util/yaml.h"

namespace nx::salud {

namespace {

auto L = log::get("salud");
std::mutex mu;
json g_informe = {{"inicio", nullptr}, {"fin", nullptr}, {"comprobaciones", json::array()}};

std::string sello() {   // 20261027-213501
  std::time_t t = std::time(nullptr);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t));
  return b;
}
std::string hoy() { return sello().substr(0, 8); }

// anota una comprobacion (sustituye la que tenga el mismo id) y la deja en el registro si no esta bien
void anotar(const std::string& id, const std::string& nombre, const std::string& estado, const std::string& detalle) {
  if (estado == "error") L.error(nombre, ": ", detalle);
  else if (estado != "ok") L.warning(nombre, ": ", detalle);
  std::lock_guard<std::mutex> lk(mu);
  json& cs = g_informe["comprobaciones"];
  json item = {{"id", id}, {"nombre", nombre}, {"estado", estado}, {"detalle", detalle}, {"ts", now()}};
  for (auto& c : cs)
    if (c["id"] == id) {
      c = item;
      return;
    }
  cs.push_back(item);
}

void guardar_informe() {
  json copia;
  {
    std::lock_guard<std::mutex> lk(mu);
    json r = {{"ok", 0}, {"reparado", 0}, {"aviso", 0}, {"error", 0}};
    for (auto& c : g_informe["comprobaciones"]) r[jstr(c, "estado")] = r.value(jstr(c, "estado"), 0) + 1;
    g_informe["resumen"] = r;
    g_informe["fin"] = now();
    copia = g_informe;
  }
  write_file(paths::data_dir() / "salud.json", jdump(copia, 1));
}

// ---------------------------------------------------------------- sqlite
struct Bd {
  sqlite3* db = nullptr;
  explicit Bd(const fs::path& p, int flags = SQLITE_OPEN_READWRITE) {
    if (sqlite3_open_v2(S(p).c_str(), &db, flags, nullptr) != SQLITE_OK) {
      sqlite3_close_v2(db);
      db = nullptr;
    }
    if (db) sqlite3_busy_timeout(db, 5000);
  }
  ~Bd() {
    if (db) sqlite3_close_v2(db);
  }
  std::string exec(const std::string& sql) {   // "" si fue bien
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    std::string e = rc == SQLITE_OK ? "" : err ? err : sqlite3_errstr(rc);
    sqlite3_free(err);
    return e;
  }
  std::vector<std::vector<std::string>> filas(const std::string& sql, std::string* error = nullptr) {
    std::vector<std::vector<std::string>> out;
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    if (rc == SQLITE_OK) {
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        std::vector<std::string> f;
        for (int i = 0; i < sqlite3_column_count(st); ++i) {
          auto t = sqlite3_column_text(st, i);
          f.push_back(t ? (const char*)t : "");
        }
        out.push_back(f);
      }
    }
    if (error) *error = rc == SQLITE_DONE || rc == SQLITE_OK ? "" : sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return out;
  }
};

std::string q(const std::string& ident) { return "\"" + replace_all(ident, "\"", "\"\"") + "\""; }
std::string lit(const std::string& s) { return "'" + replace_all(s, "'", "''") + "'"; }

// "" si la base de datos esta sana
std::string revisar_bd(const fs::path& f) {
  Bd b(f);
  if (!b.db) return "no se puede abrir";
  std::string err;
  auto r = b.filas("PRAGMA quick_check", &err);
  if (!err.empty()) return err;
  if (r.size() == 1 && !r[0].empty() && r[0][0] == "ok") return "";
  std::vector<std::string> v;
  for (size_t i = 0; i < r.size() && i < 3; ++i) v.push_back(r[i].empty() ? "?" : r[i][0]);
  return r.empty() ? "sin respuesta" : join(v, "; ");
}

// abre, lee el esquema y la primera y ultima fila de cada tabla: detecta un fichero cortado o pisado en un momento
std::string revisar_bd_rapido(const fs::path& f) {
  Bd b(f);
  if (!b.db) return "no se puede abrir";
  std::string err;
  auto ts = b.filas("SELECT name FROM sqlite_master WHERE type='table' AND sql NOT LIKE 'CREATE VIRTUAL%'", &err);
  if (!err.empty()) return err;
  for (auto& t : ts) {
    for (auto orden : {"ASC", "DESC"}) {
      b.filas("SELECT * FROM " + q(t[0]) + " ORDER BY rowid " + orden + " LIMIT 1", &err);
      if (!err.empty() && err.find("rowid") == std::string::npos) return t[0] + ": " + err;   // WITHOUT ROWID: no cuenta
    }
  }
  return "";
}

void mover_bd(const fs::path& de, const fs::path& a) {
  std::error_code ec;
  for (auto suf : {"", "-wal", "-shm"}) {
    fs::path x = S(de) + suf;
    if (path_exists(x)) fs::rename(x, S(a) + suf, ec);
  }
}

fs::path ultima_copia() {
  auto cs = glob(detail::carpeta_copias(), "nexus-*.db");
  std::sort(cs.begin(), cs.end());
  return cs.empty() ? fs::path() : cs.back();
}

// deja solo las `n` mas recientes de un patron
void podar(const fs::path& dir, const std::string& patron, size_t n) {
  auto fs_ = glob(dir, patron);
  std::sort(fs_.begin(), fs_.end(), [](auto& a, auto& b) { return mtime(a) > mtime(b); });
  for (size_t i = n; i < fs_.size(); ++i) remove_file(fs_[i]);
}

bool escribible(const fs::path& d, std::string& error) {
  std::error_code ec;
  fs::create_directories(d, ec);
  fs::path t = d / (".nexus-prueba-" + token_hex(3));
  {
    std::ofstream f(t, std::ios::binary);
    f << "ok";
    if (!f) {
      error = ec ? ec.message() : "sin permiso de escritura";
      return false;
    }
  }
  fs::remove(t, ec);
  return true;
}

// ---------------------------------------------------------------- comprobaciones previas
void carpetas(bool reparar) {
  std::vector<std::string> mal, arregladas;
  for (auto& d : {paths::data_dir(), paths::models_dir(), paths::logs_dir(), paths::outputs_dir(), paths::sub({"runtimes"})}) {
    std::string e;
    if (escribible(d, e)) continue;
#ifndef _WIN32
    if (reparar) {   // carpeta propia sin permiso de escritura (copiada de otro equipo, restaurada de una copia)
      std::error_code ec;
      fs::permissions(d, fs::perms::owner_all, fs::perm_options::add, ec);
      if (escribible(d, e)) {
        arregladas.push_back(S(d));
        continue;
      }
    }
#endif
    mal.push_back(S(d) + " (" + e + ")");
  }
  if (!mal.empty())
    anotar("carpetas", "Carpetas de datos", "error",
           "no se puede escribir en " + join(mal, ", ") + ". Da permiso de escritura a tu usuario o define NEXUS_DATA_DIR con otra carpeta.");
  else if (!arregladas.empty())
    anotar("carpetas", "Carpetas de datos", "reparado", "permiso de escritura recuperado en " + join(arregladas, ", "));
  else
    anotar("carpetas", "Carpetas de datos", "ok", "datos en " + S(paths::data_dir()));
}

void recursos() {
  std::vector<std::string> faltan;
  for (auto& p : {paths::web() / "index.html", paths::web() / "js" / "app.js", paths::package() / "agents" / "defaults",
                  paths::package() / "certs" / "cacert.pem"})
    if (!path_exists(p)) faltan.push_back(S(p));
  if (faltan.empty())
    anotar("recursos", "Ficheros del programa", "ok", "interfaz, agentes y certificados en su sitio");
  else
    anotar("recursos", "Ficheros del programa", "error",
           "faltan " + join(faltan, ", ") + ": vuelve a instalar Nexus-Office (tus datos no se tocan)");
}

void configuracion(bool reparar) {
  fs::path f = paths::config_file(), bueno = S(f) + ".bueno";
  if (!is_file(f)) {
    anotar("configuracion", "Configuracion", "ok", "se crea con los valores por defecto");
    return;
  }
  auto valida = [](const fs::path& p, std::string& e) {
    try {
      std::string t = read_text(p);
      if (trim(t).empty()) {
        e = "esta vacia";
        return false;
      }
      json j = yaml::load(t);
      if (!j.is_object()) {
        e = "no es un diccionario";
        return false;
      }
      return true;
    } catch (const std::exception& ex) {
      e = ex.what();
      return false;
    }
  };
  std::string e, e2;
  if (valida(f, e)) {
    anotar("configuracion", "Configuracion", "ok", S(f.filename()) + " correcta");
    return;
  }
  if (!reparar) {
    anotar("configuracion", "Configuracion", "error", S(f.filename()) + " danada: " + e);
    return;
  }
  fs::path danada = S(f) + ".danado-" + sello();
  std::error_code ec;
  fs::rename(f, danada, ec);
  podar(f.parent_path(), "config.yaml.danado-*", 3);
  if (is_file(bueno) && valida(bueno, e2)) {
    fs::copy_file(bueno, f, fs::copy_options::overwrite_existing, ec);
    anotar("configuracion", "Configuracion", "reparado",
           "estaba danada (" + e + "): se ha restaurado la ultima configuracion buena; la danada queda en " + S(danada.filename()));
  } else {
    anotar("configuracion", "Configuracion", "reparado",
           "estaba danada (" + e + ") y no habia copia: se usan los valores por defecto; la danada queda en " + S(danada.filename()));
  }
}

void base_de_datos(bool reparar) {
  fs::path f = paths::db_file();
  if (!is_file(f) || fsize(f) == 0) {
    anotar("base_datos", "Base de datos", "ok", "se crea al arrancar");
    return;
  }
  // la revision completa recorre todo el fichero: tras un cierre brusco, una vez al dia o si se pide; si no, la rapida
  fs::path marca = paths::data_dir() / ".salud-bd";
  bool completa = !reparar || path_exists(paths::data_dir() / ".running") || !is_file(marca) || now() - mtime(marca) > 24 * 3600;
  std::string e = completa ? revisar_bd(f) : revisar_bd_rapido(f);
  if (e.empty()) {
    if (completa) write_file(marca, sello());
    anotar("base_datos", "Base de datos", "ok",
           std::string(completa ? "integridad correcta" : "abre bien (la revision completa se hace a diario y tras un cierre brusco)") +
               " (" + std::to_string(fsize(f) / (1024 * 1024)) + " MB)");
    return;
  }
  if (!completa) e = revisar_bd(f).empty() ? "" : e;   // la rapida avisa; se confirma con la completa
  if (e.empty()) {
    anotar("base_datos", "Base de datos", "ok", "integridad correcta");
    return;
  }
  if (!reparar) {
    anotar("base_datos", "Base de datos", "error", "danada: " + e);
    return;
  }
  json r = detail::rescatar_bd(f, ultima_copia());
  std::string d = "estaba danada (" + e + "): ";
  if (truthy(r["ok"])) {
    d += "rescatadas " + std::to_string(r["tablas"].size()) + " tablas";
    if (!r["recuperadas"].empty()) d += "; de la copia del " + jstr(r, "copia") + ": " + join(r["recuperadas"].get<std::vector<std::string>>(), ", ");
    if (!r["perdidas"].empty()) d += "; sin poder recuperar: " + join(r["perdidas"].get<std::vector<std::string>>(), ", ");
  } else {
    d += jstr(r, "detalle");
  }
  d += ". La original queda en " + jstr(r, "danada");
  anotar("base_datos", "Base de datos", r["perdidas"].empty() ? "reparado" : "aviso", d);
}

void temporales() {
  double viejo = now() - 24 * 3600;
  int n = 0;
  for (auto& d : {paths::sub({"tmp"}), paths::sub({"runtimes"})})
    for (auto& p : glob(d, "*.tmp", true))
      if (mtime(p) < viejo && remove_file(p)) ++n;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::temp_directory_path(ec), ec)) {
    std::string nm = S(e.path().filename());
    if ((starts_with(nm, "nexus-esptool-") || starts_with(nm, "nexus-render-")) && mtime(e.path()) < viejo && remove_file(e.path())) ++n;
  }
  auto partes = glob(paths::models_dir(), "*.part", true);
  std::string d = n ? std::to_string(n) + " ficheros temporales viejos borrados" : "sin restos";
  if (!partes.empty()) d += "; " + std::to_string(partes.size()) + " descargas a medias (se reanudan al volver a descargarlas)";
  anotar("temporales", "Ficheros temporales", n ? "reparado" : "ok", d);
}

void registros() {
  fs::path d = paths::logs_dir();
  auto fs_ = glob(d, "*", true);
  uintmax_t total = 0;
  for (auto& p : fs_) total += is_file(p) ? fsize(p) : 0;
  const uintmax_t MAX = 200ull << 20, OBJ = 100ull << 20;
  if (total <= MAX) {
    anotar("registros", "Registros", "ok", std::to_string(total >> 20) + " MB");
    return;
  }
  std::sort(fs_.begin(), fs_.end(), [](auto& a, auto& b) { return mtime(a) < mtime(b); });
  int n = 0;
  double reciente = now() - 3600;   // el registro en uso no se toca
  for (auto& p : fs_) {
    if (total <= OBJ) break;
    if (!is_file(p) || mtime(p) > reciente) continue;
    uintmax_t sz = fsize(p);
    if (remove_file(p)) total -= sz, ++n;
  }
  anotar("registros", "Registros", "reparado", "ocupaban demasiado: " + std::to_string(n) + " registros antiguos borrados");
}

void disco() {
  double gb = disk_free_gb(paths::data_dir());
  if (gb < 0) return;
  char b[32];
  std::snprintf(b, sizeof b, "%.1f GB libres", gb);
  if (gb < 0.3)
    anotar("disco", "Espacio en disco", "error", std::string(b) + ": libera espacio, Nexus-Office puede no guardar los datos");
  else if (gb < 2)
    anotar("disco", "Espacio en disco", "aviso", std::string(b) + ": queda poco para modelos y copias");
  else
    anotar("disco", "Espacio en disco", "ok", b);
}

void sesion_anterior() {
  if (path_exists(paths::data_dir() / ".running"))
    anotar("sesion", "Cierre anterior", "aviso", "la ultima sesion no se cerro bien (apagon o cierre forzado): se ha revisado todo y se arranca con un perfil prudente");
  else
    anotar("sesion", "Cierre anterior", "ok", "la ultima sesion se cerro bien");
}

// ---------------------------------------------------------------- comprobaciones con los servicios
void modelos(bool reparar) {
  fs::path dir = paths::models_dir(), apartados = dir / "danados";
  int bien = 0;
  std::vector<std::string> malos, movidos;
  for (auto& p : glob(dir, "*.gguf", true)) {
    if (is_relative_to(p, apartados)) continue;
    std::string motivo;
    char magic[4] = {0};
    {
      std::ifstream f(p, std::ios::binary);
      f.read(magic, 4);
    }
    if (fsize(p) < 32) motivo = "vacio";
    else if (std::string(magic, 4) != "GGUF") motivo = "no es un GGUF";
    else {
      try {
        models::gguf_metadata(p, 8);
      } catch (const std::exception& e) {
        if (std::string(e.what()).find("truncado") != std::string::npos) motivo = "cortado";
      }
    }
    if (motivo.empty()) {
      ++bien;
      continue;
    }
    std::error_code ec;
    if (reparar) {
      fs::create_directories(apartados, ec);
      fs::rename(p, apartados / p.filename(), ec);
    }
    (reparar && !ec ? movidos : malos).push_back(S(p.filename()) + " (" + motivo + ")");
  }
  std::string d = std::to_string(bien) + " modelos GGUF correctos";
  if (!movidos.empty()) d += "; apartados a models/danados: " + join(movidos, ", ") + ". Vuelve a descargarlos en Modelos";
  if (!malos.empty()) d += "; danados: " + join(malos, ", ");
  anotar("modelos", "Modelos", !malos.empty() ? "error" : !movidos.empty() ? "reparado" : bien ? "ok" : "aviso",
         bien || !movidos.empty() || !malos.empty() ? d : "no hay modelos de lenguaje: descarga uno en Modelos");
}

std::string probar(const fs::path& exe) {   // "" si arranca
  proc::Options o;
  o.cwd = exe.parent_path();
  o.env = runtimes::lib_env(exe);
  auto r = proc::run({S(exe), "--version"}, 30, o);
  if (r.ok()) return "";
  if (r.timed_out) return "no responde";
  if (!r.started) return "no se puede ejecutar";
  auto ls = splitlines(trim(r.err + "\n" + r.out));
  return ls.empty() ? "sale con codigo " + std::to_string(r.code) : ls.back();
}

void motor(Services& s, bool reparar) {
  std::string pref = s.config.str("hardware.llama_variant", "");
  auto f = s.runtimes->exe("llama", s.profile, pref);
  if (!f) {
    anotar("motor", "Motor de IA (llama-server)", "error", "no esta el binario llama-server: instala el runtime en Sistema > Runtimes nativos");
    return;
  }
  std::vector<std::string> hecho;
#ifndef _WIN32
  std::error_code ec;
  auto perms = fs::status(f->exe, ec).permissions();
  if (!ec && (perms & fs::perms::owner_exec) == fs::perms::none && reparar) {
    fs::permissions(f->exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    hecho.push_back("permiso de ejecucion");
  }
#endif
  std::string e = probar(f->exe);
  if (!e.empty() && reparar && sys::is_macos()) {   // descargado y copiado: macOS lo deja en cuarentena
    proc::run({"xattr", "-dr", "com.apple.quarantine", S(f->exe.parent_path())}, 30);
    e = probar(f->exe);
    if (e.empty()) hecho.push_back("cuarentena de macOS quitada");
  }
  if (e.empty()) {
    anotar("motor", "Motor de IA (llama-server)", hecho.empty() ? "ok" : "reparado",
           "variante " + f->variant + " funciona" + (hecho.empty() ? "" : " (" + join(hecho, ", ") + ")"));
    return;
  }
  if (!reparar) {
    anotar("motor", "Motor de IA (llama-server)", "error", "la variante " + f->variant + " no arranca: " + e);
    return;
  }
  // la variante elegida no arranca en este equipo (driver de GPU, CPU sin AVX2...): la primera que funcione
  for (std::string v : {"cpu", "cpu-compat"}) {
    if (v == f->variant) continue;
    auto alt = s.runtimes->exe("llama", s.profile, v);
    if (!alt || alt->variant != v || !probar(alt->exe).empty()) continue;
    try {
      sys2::accel_apply(s, s.config.str("llm.engine", "auto"), "cpu", std::nullopt);
    } catch (const std::exception& ex) {
      L.warning("no se pudo pasar a la CPU: ", ex.what());
    }
    s.config.set("hardware.llama_variant", v);
    anotar("motor", "Motor de IA (llama-server)", "reparado",
           "la variante " + f->variant + " no arranca en este equipo (" + e + "): se usa la " + v + ". Puedes volver a probar la GPU en Motor y CPU/GPU/NPU");
    return;
  }
  anotar("motor", "Motor de IA (llama-server)", "error", "la variante " + f->variant + " no arranca (" + e + ") y no hay otra que funcione: reinstala Nexus-Office");
}

void imagen(Services& s) {
  auto f = s.runtimes->exe("sd", s.profile);
  if (!f) anotar("imagen", "Motor de imagen (stable-diffusion.cpp)", "aviso", "no esta instalado: el Estudio crea imagenes cuando lo instales en Sistema");
  else anotar("imagen", "Motor de imagen (stable-diffusion.cpp)", "ok", "variante " + f->variant);
}

void drivers_serie() {
  if (hw::android()) return;
  try {
    json st = devices::drivers::estado();
    json faltan = st["faltan"];
    if (faltan.empty())
      anotar("drivers_serie", "Drivers USB-serie de las placas", "ok",
             st["dispositivos"].empty() ? "sin placas enchufadas" : std::to_string(st["dispositivos"].size()) + " placas listas");
    else
      anotar("drivers_serie", "Drivers USB-serie de las placas", "aviso",
             "falta el driver " + join(faltan.get<std::vector<std::string>>(), ", ") + ": instalalo en Dispositivos > Grabar firmware");
  } catch (const std::exception& e) {
    anotar("drivers_serie", "Drivers USB-serie de las placas", "aviso", e.what());
  }
}

}  // namespace

// ---------------------------------------------------------------- rescate de la base de datos
namespace detail {

fs::path carpeta_copias() { return paths::sub({"copias"}); }

json rescatar_bd(const fs::path& f, const fs::path& copia) {
  fs::path danada = S(f) + ".danado-" + sello();
  mover_bd(f, danada);
  podar(f.parent_path(), S(f.filename()) + ".danado-*", 6);   // con sus -wal y -shm: 2 copias
  json r = {{"ok", false}, {"tablas", json::array()}, {"perdidas", json::array()}, {"recuperadas", json::array()},
            {"danada", S(danada.filename())}, {"copia", ""}};
  Bd nueva(f, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  if (!nueva.db) {
    r["detalle"] = "no se pudo crear una base de datos nueva";
    return r;
  }
  std::string e = nueva.exec("ATTACH DATABASE " + lit(S(danada)) + " AS v");
  std::vector<std::vector<std::string>> tablas;
  if (e.empty()) tablas = nueva.filas("SELECT name, sql FROM v.sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'", &e);
  if (!e.empty() || tablas.empty()) {
    nueva.exec("DETACH DATABASE v");
    if (!copia.empty() && is_file(copia)) {   // ilegible del todo: la copia diaria
      sqlite3_close_v2(nueva.db);
      nueva.db = nullptr;
      std::error_code ec;
      fs::copy_file(copia, f, fs::copy_options::overwrite_existing, ec);
      r["ok"] = !ec;
      r["copia"] = stem(copia).substr(6);
      r["detalle"] = "ilegible (" + (e.empty() ? "sin tablas" : e) + "): restaurada la copia del " + jstr(r, "copia");
    } else {
      r["ok"] = true;
      r["detalle"] = "ilegible (" + (e.empty() ? "sin tablas" : e) + ") y sin copia: se empieza con una base de datos nueva";
    }
    return r;
  }
  // tablas virtuales (FTS) y sus tablas internas: se rehacen al arrancar
  std::vector<std::string> virtuales;
  for (auto& t : tablas)
    if (starts_with(upper(t[1]), "CREATE VIRTUAL")) virtuales.push_back(t[0]);
  for (auto& t : tablas) {
    bool interna = false;
    for (auto& v : virtuales) interna |= t[0] == v || starts_with(t[0], v + "_");
    if (interna) continue;
    if (!nueva.exec(t[1]).empty()) continue;
    if (nueva.exec("INSERT INTO main." + q(t[0]) + " SELECT * FROM v." + q(t[0])).empty()) r["tablas"].push_back(t[0]);
    else {
      nueva.exec("DELETE FROM main." + q(t[0]));
      r["perdidas"].push_back(t[0]);
    }
  }
  for (auto& i : nueva.filas("SELECT sql FROM v.sqlite_master WHERE type='index' AND sql IS NOT NULL")) nueva.exec(i[0]);
  nueva.exec("DETACH DATABASE v");
  // lo que no se pudo leer, de la copia diaria
  if (!r["perdidas"].empty() && !copia.empty() && is_file(copia) && nueva.exec("ATTACH DATABASE " + lit(S(copia)) + " AS c").empty()) {
    json siguen = json::array();
    for (auto& t : r["perdidas"]) {
      std::string n = t.get<std::string>();
      if (nueva.exec("INSERT INTO main." + q(n) + " SELECT * FROM c." + q(n)).empty()) r["recuperadas"].push_back(n);
      else siguen.push_back(n);
    }
    r["perdidas"] = siguen;
    r["copia"] = stem(copia).substr(6);
    nueva.exec("DETACH DATABASE c");
  }
  r["ok"] = true;
  return r;
}

}  // namespace detail

// ---------------------------------------------------------------- API
json previa(bool reparar) {
  {
    std::lock_guard<std::mutex> lk(mu);
    g_informe = {{"inicio", now()}, {"fin", nullptr}, {"comprobaciones", json::array()}, {"version", NEXUS_VERSION},
                 {"sistema", sys::os_name() + " " + sys::arch()}};
  }
  carpetas(reparar);
  recursos();
  configuracion(reparar);
  base_de_datos(reparar);
  sesion_anterior();
  if (reparar) {
    temporales();
    registros();
  }
  disco();
  guardar_informe();
  return informe();
}

json completa(Services& s, bool reparar) {
  for (auto paso : {+[](Services& s, bool r) { modelos(r); }, +[](Services& s, bool r) { motor(s, r); },
                    +[](Services& s, bool) { imagen(s); }, +[](Services&, bool) { drivers_serie(); }}) {
    try {
      paso(s, reparar);
    } catch (const std::exception& e) {
      L.warning("comprobacion: ", e.what());
    }
  }
  guardar_informe();
  json inf = informe();
  json r = inf.value("resumen", json::object());
  L.info("Comprobacion al arrancar: ", r.value("ok", 0), " bien, ", r.value("reparado", 0), " reparadas, ", r.value("aviso", 0),
         " avisos, ", r.value("error", 0), " errores");
  return inf;
}

int puerto(const std::string& host, int port) {
  if (!net::port_in_use(host, port)) {
    anotar("puerto", "Puerto de la aplicacion", "ok", std::to_string(port) + " libre");
    return port;
  }
  int otro = net::free_port(port + 1);
  anotar("puerto", "Puerto de la aplicacion", "reparado",
         "el " + std::to_string(port) + " lo ocupa otro programa: se usa el " + std::to_string(otro) + " en esta sesion");
  guardar_informe();
  return otro;
}

void guardar_copias() {
  fs::path f = paths::config_file();
  try {
    if (is_file(f) && yaml::load(read_text(f)).is_object()) {
      std::error_code ec;
      fs::copy_file(f, S(f) + ".bueno", fs::copy_options::overwrite_existing, ec);
    }
  } catch (const std::exception&) {
  }
  fs::path db = paths::db_file(), dir = detail::carpeta_copias(), destino = dir / ("nexus-" + hoy() + ".db");
  if (!is_file(db) || is_file(destino)) return;
  // espacio: la copia y 1 GB de margen
  double libre = disk_free_gb(dir);
  if (libre >= 0 && libre * 1e9 < 2.0 * fsize(db) + 1e9) {
    L.warning("sin espacio para la copia diaria de la base de datos");
    return;
  }
  Bd b(db, SQLITE_OPEN_READONLY);
  if (!b.db) return;
  fs::path tmp = S(destino) + ".tmp";
  remove_file(tmp);
  std::string e = b.exec("VACUUM INTO " + lit(S(tmp)));
  if (!e.empty()) {
    L.warning("copia diaria de la base de datos: ", e);
    remove_file(tmp);
    return;
  }
  std::error_code ec;
  fs::rename(tmp, destino, ec);
  podar(dir, "nexus-*.db", 3);
}

json informe() {
  std::lock_guard<std::mutex> lk(mu);
  return g_informe;
}

}  // namespace nx::salud
