// Biblioteca de avatares 3D gratuitos (tests/test_avatares.py): catalogos, descarga con pasarelas IPFS de respaldo,
// uso y modo sin conexion.
#include <httplib.h>

#include <thread>

#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "media/avatares.h"
#include "server/app.h"
#include "util/fs.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/text.h"

using namespace nx;

namespace {

const std::string GLB = std::string("glTF\x02\x00\x00\x00", 8) + std::string(100, '\0');

const json PROJECTS = jparse(R"([
  {"id": "p1", "name": "100Avatars R1", "creator_id": "Polygonal-Mind", "license": "CC0", "is_public": true,
   "avatar_data_file": "avatars/p1.json"},
  {"id": "oculto", "name": "Oculto", "is_public": false, "avatar_data_file": "avatars/x.json"}])");
const json AVS = jparse(R"([
  {"id": "a1", "name": "Devil", "description": "Avatar 031: Devil", "format": "VRM", "is_public": true,
   "model_file_url": "https://dweb.link/ipfs/QmX/devil.vrm", "thumbnail_url": "https://arweave.net/thumb"},
  {"id": "a2", "name": "Robot", "description": "", "format": "VRM", "is_public": true,
   "model_file_url": "https://arweave.net/robot", "thumbnail_url": ""},
  {"id": "a3", "name": "Borrador", "is_draft": true, "model_file_url": "https://arweave.net/x"}])");

// como httpx.MockTransport: entrega el cuerpo por on_data si la peticion lo pide
avatares::Avatares::Fetch transport(bool caido = false) {
  return [caido](const http::Request& req) {
    http::Response r;
    if (caido) {
      r.error = "sin red";
      return r;
    }
    const std::string& u = req.url;
    r.status = 200;
    if (ends_with(u, "projects.json"))
      r.body = PROJECTS.dump();
    else if (ends_with(u, "avatars/p1.json"))
      r.body = AVS.dump();
    else if (starts_with(u, "https://dweb.link/"))
      r.status = 504;   // pasarela caida: se prueba la siguiente
    else if (starts_with(u, "https://ipfs.io/ipfs/QmX/devil.vrm"))
      r.body = GLB;
    else if (u == "https://arweave.net/thumb")
      r.body = "\x89PNG....";
    else if (u == "https://arweave.net/robot")
      r.body = "<html>no</html>";
    else if (contains(u, "raw.githubusercontent.com") && ends_with(u, ".glb")) {
      r.body = GLB;
      r.headers["content-length"] = std::to_string(GLB.size());
    } else
      r.status = 404;
    if (req.on_headers) req.on_headers(r);
    if (req.on_data && r.ok()) {
      req.on_data(r.body.data(), r.body.size());
      r.body.clear();
    }
    return r;
  };
}

fs::path datos(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() /
             ("nexus-avs-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  avatares::reset();
  return dir;
}

template <class E, class F>
bool lanza(F f) {
  try {
    f();
  } catch (const E&) {
    return true;
  } catch (...) {
  }
  return false;
}

}  // namespace

TEST("avatares: catalogos, descarga con pasarelas IPFS y sin conexion") {
  datos("cat");
  auto& av = avatares::get([] { return std::string(); });
  av.fetch = transport();
  json r = av.buscar("osa");
  CHECK(r["total"] == 2 && r["resultados"].size() == 2);
  std::set<std::string> nombres;
  for (auto& i : r["resultados"]) nombres.insert(jstr(i, "nombre"));
  CHECK(nombres == std::set<std::string>({"Devil", "Robot"}));
  CHECK(r["resultados"][0]["licencia"] == "CC0" && starts_with(jstr(r["resultados"][0], "id"), "osa:"));
  CHECK(av.buscar("osa", "devil")["total"] == 1);
  json m = av.buscar("muestras", "robot");
  CHECK(m["total"].get<int>() >= 1);
  for (auto& i : m["resultados"]) CHECK(starts_with(jstr(i, "id"), "muestras:"));
  json it = av.descargar_ya("osa:a1");
  CHECK(ends_with(jstr(it, "archivo"), ".vrm") && !jstr(it, "miniatura_local").empty() && it["licencia"] == "CC0");
  CHECK(av.buscar("osa", "devil")["resultados"][0]["descargado"] == true);
  CHECK(av.descargar_ya("osa:a1")["id"] == it["id"]);   // no se descarga dos veces
  CHECK(lanza<avatares::ValueError>([&] { av.descargar_ya("osa:a2"); }));   // no es un glTF
  CHECK(lanza<avatares::LookupError>([&] { av.descargar_ya("osa:a3"); }));  // borrador: no esta en el catalogo
  CHECK(lanza<avatares::LookupError>([&] { av.descargar_ya("https://evil.example/x.glb"); }));   // solo el catalogo
  CHECK(lanza<avatares::PermissionError>([&] { av.item("sketchfab:abc"); }));   // Sketchfab sin clave
  // sin conexion: el indice guardado sigue sirviendo y lo descargado tambien
  av.fetch = transport(true);
  av.olvidar_indice();
  CHECK(av.buscar("osa")["total"] == 2);
  json bib = av.biblioteca();
  CHECK(bib.size() == 1 && bib[0]["id"] == it["id"]);
  CHECK(av.borrar(jstr(it, "id")) && av.biblioteca().empty());
  avatares::reset();
}

TEST("avatares: la API descarga, usa y borra") {
  datos("api");
  auto s = std::make_unique<Services>(false);
  server::App app(s.get());
  s->start();
  avatares::get().fetch = transport();
  int port = net::free_port(18995);
  std::thread th([&] { app.listen("127.0.0.1", port); });
  httplib::Client c("127.0.0.1", port);
  for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto body = [](const httplib::Result& r) { return r ? jparse(r->body) : json(); };

  json d = body(c.Get("/api/avatares"));
  CHECK(d["fuentes"].size() == 3 && d["fuentes"].contains("sketchfab") && d["biblioteca"].empty());
  CHECK(body(c.Get("/api/avatares/buscar?fuente=osa"))["total"] == 2);
  json j = body(c.Post("/api/avatares/descargar", R"({"id": "muestras:xbot"})", "application/json"));
  json st;
  for (int i = 0; i < 100; ++i) {
    st = body(c.Get("/api/avatares/trabajo/" + jstr(j, "id")));
    if (st["estado"] != "en_curso") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(st["estado"] == "hecho");
  std::string aid = jstr(st["avatar"], "id");
  auto g = c.Get(jstr(st["avatar"], "url"));
  CHECK(g && g->body == GLB);
  CHECK(body(c.Post("/api/avatares/" + aid + "/usar", "", "application/json"))["ok"] == true);
  json cfg = body(c.Get("/api/config"));
  CHECK(cfg["user"]["avatar_style"] == "glb" && cfg["user"]["avatar_model"] == st["avatar"]["url"]);
  auto p = c.Put("/api/config", R"({"avatares.sketchfab_token": "abc"})", "application/json");
  CHECK(p && p->status == 200);
  CHECK(body(c.Get("/api/avatares"))["sketchfab_clave"] == true);
  CHECK(body(c.Delete("/api/avatares/" + aid))["ok"] == true);
  auto d2 = c.Delete("/api/avatares/" + aid);
  CHECK(d2 && d2->status == 404);

  app.stop();
  th.join();
  s->shutdown();
  avatares::reset();
}
