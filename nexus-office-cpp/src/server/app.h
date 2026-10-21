// API REST + WebSocket del webui (server/app.py): las mismas rutas, los mismos JSON y la misma interfaz web.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace httplib {
class Server;
struct Request;
struct Response;
struct FormData;
namespace ws {
class WebSocket;
}
}  // namespace httplib

namespace nx {
class Services;
}

namespace nx::server {

// HTTPException de FastAPI: {"detail": ...} con su codigo
struct HttpErr : std::runtime_error {
  int status;
  json detail;
  HttpErr(int st, const std::string& msg) : std::runtime_error(msg), status(st), detail(msg) {}
  HttpErr(int st, json d) : std::runtime_error(d.dump()), status(st), detail(std::move(d)) {}
};

struct Raw {
  int status = 200;
  std::string body;
  std::string mime = "application/octet-stream";
  std::vector<std::pair<std::string, std::string>> headers;
};

struct FileOut {
  fs::path path;
  std::string filename;   // Content-Disposition: attachment (vacio: en linea)
  std::string mime;       // vacio: por la extension
};

using Out = std::variant<json, Raw, FileOut>;

struct Upload {
  std::string filename, content_type, content;
};

class App;

// lo que recibe cada ruta: parametros de ruta, consulta, cuerpo JSON y formularios multipart
struct Ctx {
  const httplib::Request& req;
  httplib::Response& res;
  App& app;
  std::map<std::string, std::string> path;
  json body = nullptr;
  bool admin = false;
  std::string host;

  bool local() const;
  bool has_q(const std::string& k) const;
  std::string q(const std::string& k, const std::string& def = "") const;
  long long qi(const std::string& k, long long def = 0) const;
  bool qb(const std::string& k, bool def = false) const;
  std::optional<std::string> qopt(const std::string& k) const;
  std::string p(const std::string& k) const;
  long long pi(const std::string& k) const;   // 422 si no es entero
  bool has_b(const std::string& k) const;
  json b(const std::string& k) const;         // obligatorio (422 si falta)
  json b(const std::string& k, const json& def) const;
  std::string bs(const std::string& k) const { return as_str(b(k)); }
  std::string bs(const std::string& k, const std::string& def) const { return as_str(b(k, def)); }
  double bn(const std::string& k, double def) const;
  bool bb(const std::string& k, bool def) const;
  std::string form(const std::string& k, const std::string& def = "") const;
  bool form_bool(const std::string& k, bool def) const;
  std::optional<Upload> file(const std::string& k) const;
  std::vector<Upload> files(const std::string& k) const;
  std::string header(const std::string& k) const;
  static std::string as_str(const json& v);
};

using Handler = std::function<Out(Ctx&)>;

class App {
 public:
  explicit App(Services* services = nullptr);
  ~App();
  // arranca y bloquea hasta stop(); devuelve false si no se pudo abrir el puerto
  bool listen(const std::string& host, int port);
  void stop();
  Services& S();
  bool ready() const { return services_ != nullptr; }

  // difusion a todas las interfaces conectadas (aprobaciones, salon, descargas...)
  void broadcast(const json& event);

  // tokens de administrador (8 h)
  std::string new_admin_token();
  bool is_admin(const std::string& token);
  void drop_admin_token(const std::string& token);

  std::string lan_key();
  bool lan_ok(const httplib::Request& req, const std::string& host);

  void route(const std::string& method, const std::string& pattern, Handler h, bool admin_required = false);
  void get(const std::string& p, Handler h, bool admin = false) { route("GET", p, std::move(h), admin); }
  void post(const std::string& p, Handler h, bool admin = false) { route("POST", p, std::move(h), admin); }
  void put(const std::string& p, Handler h, bool admin = false) { route("PUT", p, std::move(h), admin); }
  void patch(const std::string& p, Handler h, bool admin = false) { route("PATCH", p, std::move(h), admin); }
  void del(const std::string& p, Handler h, bool admin = false) { route("DELETE", p, std::move(h), admin); }

  std::atomic<bool> stopping{false};
  std::function<void()> on_shutdown_request;   // /api/admin/shutdown

 private:
  Services* services_;
  std::unique_ptr<httplib::Server> srv_;
  std::mutex mu_;
  std::map<std::string, double> admin_tokens_;
  std::mutex ws_mu_;
  std::set<httplib::ws::WebSocket*> sockets_;
  void setup();
  void websocket(const httplib::Request& req, httplib::ws::WebSocket& ws);
  void dispatch(const Handler& h, bool admin_required, const std::vector<std::string>& names,
                const httplib::Request& req, httplib::Response& res);
};

void register_routes(App& app);   // routes.cpp

}  // namespace nx::server
