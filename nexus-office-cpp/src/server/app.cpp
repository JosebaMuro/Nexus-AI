// Servidor HTTP + WebSocket (server/app.py): rutas, guarda de red local, estaticos, administradores y eventos.
#include "server/app.h"

#include <httplib.h>

#include <fstream>
#include <regex>
#include <thread>

#include "agents/agents.h"
#include "core/services.h"
#include "memory/memory.h"
#include "remote/remote.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::server {

static auto L = log::get("server");
static const std::set<std::string> LOCAL = {"127.0.0.1", "::1", "localhost", "testclient", "::ffff:127.0.0.1"};
static const std::vector<std::string> OPEN_PATHS = {"/api/ping", "/sw.js", "/static/manifest.webmanifest",
                                                    "/static/icon.svg", "/connect"};

// ======================================================================== Ctx
std::string Ctx::as_str(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "";
  return v.dump();
}

bool Ctx::local() const { return LOCAL.count(host) > 0; }
bool Ctx::has_q(const std::string& k) const { return req.has_param(k); }
std::string Ctx::q(const std::string& k, const std::string& def) const {
  return req.has_param(k) ? req.get_param_value(k) : def;
}
std::optional<std::string> Ctx::qopt(const std::string& k) const {
  if (!req.has_param(k)) return std::nullopt;
  return req.get_param_value(k);
}

static std::optional<long long> to_int(const std::string& s) {
  try {
    size_t n = 0;
    long long v = std::stoll(trim(s), &n);
    if (n != trim(s).size()) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

// detalle 422 de FastAPI: [{"type", "loc", "msg", "input"}]
static json unprocessable(const std::string& where, const std::string& name, const std::string& msg,
                          const std::string& type = "value_error", const json& input = nullptr) {
  return json::array({{{"type", type}, {"loc", {where, name}}, {"msg", msg}, {"input", input}}});
}

HttpErr missing_field(const std::string& where, const std::string& name) {
  return HttpErr(422, unprocessable(where, name, "Field required", "missing"));
}

long long Ctx::qi(const std::string& k, long long def) const {
  if (!req.has_param(k)) return def;
  auto v = to_int(req.get_param_value(k));
  if (!v)
    throw HttpErr(422, unprocessable("query", k, "Input should be a valid integer, unable to parse string as an integer",
                                     "int_parsing", req.get_param_value(k)));
  return *v;
}

static std::optional<bool> to_bool(std::string s) {
  s = lower(trim(s));
  if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "y" || s == "t") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off" || s == "n" || s == "f") return false;
  return std::nullopt;
}

bool Ctx::qb(const std::string& k, bool def) const {
  if (!req.has_param(k)) return def;
  auto v = to_bool(req.get_param_value(k));
  if (!v)
    throw HttpErr(422, unprocessable("query", k, "Input should be a valid boolean, unable to interpret input",
                                     "bool_parsing", req.get_param_value(k)));
  return *v;
}

std::string Ctx::p(const std::string& k) const {
  auto it = path.find(k);
  return it == path.end() ? "" : it->second;
}

long long Ctx::pi(const std::string& k) const {
  auto v = to_int(p(k));
  if (!v)
    throw HttpErr(422, unprocessable("path", k, "Input should be a valid integer, unable to parse string as an integer",
                                     "int_parsing", p(k)));
  return *v;
}

bool Ctx::has_b(const std::string& k) const { return body.is_object() && body.contains(k); }

json Ctx::b(const std::string& k) const {
  if (!has_b(k)) throw missing_field("body", k);
  return body[k];
}

json Ctx::b(const std::string& k, const json& def) const { return has_b(k) ? body[k] : def; }

double Ctx::bn(const std::string& k, double def) const {
  json v = b(k, json(def));
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
    }
  }
  if (v.is_null()) return def;
  throw HttpErr(422, unprocessable("body", k, "Input should be a valid number"));
}

bool Ctx::bb(const std::string& k, bool def) const {
  json v = b(k, json(def));
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer()) return v.get<long long>() != 0;
  if (v.is_string()) {
    auto r = to_bool(v.get<std::string>());
    if (r) return *r;
  }
  if (v.is_null()) return def;
  throw HttpErr(422, unprocessable("body", k, "Input should be a valid boolean"));
}

// formulario multipart o application/x-www-form-urlencoded (este llega mezclado con la consulta en req.params)
static bool urlencoded(const httplib::Request& req) {
  return starts_with(req.get_header_value("Content-Type"), "application/x-www-form-urlencoded");
}

std::string Ctx::form(const std::string& k, const std::string& def) const {
  if (req.form.has_field(k)) return req.form.get_field(k);
  if (urlencoded(req) && req.has_param(k)) return req.get_param_value(k);
  return def;
}

bool Ctx::form_bool(const std::string& k, bool def) const {
  if (!req.form.has_field(k) && !(urlencoded(req) && req.has_param(k))) return def;
  return to_bool(form(k)).value_or(def);
}

std::optional<Upload> Ctx::file(const std::string& k) const {
  if (!req.form.has_file(k)) return std::nullopt;
  auto f = req.form.get_file(k);
  return Upload{f.filename, f.content_type, f.content};
}

std::vector<Upload> Ctx::files(const std::string& k) const {
  std::vector<Upload> out;
  for (auto& f : req.form.get_files(k)) out.push_back({f.filename, f.content_type, f.content});
  return out;
}

std::string Ctx::header(const std::string& k) const { return req.get_header_value(k); }

// ======================================================================== App
App::App(Services* services) : services_(services), srv_(std::make_unique<httplib::Server>()) { setup(); }

App::App(Services* services, App& primary, const fs::path& cert, const fs::path& key) : services_(services), primary_(&primary) {
#ifdef CPPHTTPLIB_SSL_ENABLED
  srv_ = std::make_unique<httplib::SSLServer>(nx::S(cert).c_str(), nx::S(key).c_str());
#else
  (void)cert, (void)key;
  srv_ = std::make_unique<httplib::Server>();
#endif
  setup();
}

bool App::valid() const { return srv_ && srv_->is_valid(); }

App::~App() = default;

Services& App::S() {
  if (!services_) throw HttpErr(503, "Nexus-Office aun esta arrancando");
  return *services_;
}

std::string App::new_admin_token() {
  if (primary_ != this) return primary_->new_admin_token();
  std::string tok = token_hex(16);
  std::lock_guard<std::mutex> lk(mu_);
  admin_tokens_[tok] = now() + 8 * 3600;
  return tok;
}

bool App::is_admin(const std::string& token) {
  if (primary_ != this) return primary_->is_admin(token);
  if (token.empty()) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = admin_tokens_.find(token);
  return it != admin_tokens_.end() && it->second > now();
}

void App::drop_admin_token(const std::string& token) {
  if (primary_ != this) return primary_->drop_admin_token(token);
  std::lock_guard<std::mutex> lk(mu_);
  admin_tokens_.erase(token);
}

std::string App::lan_key() {
  Config& cfg = S().config;
  if (cfg.str("server.lan_key").empty()) cfg.set("server.lan_key", token_urlsafe(9));
  return cfg.str("server.lan_key");
}

static std::string cookie(const httplib::Request& req, const std::string& name) {
  std::string all = req.get_header_value("Cookie");
  for (auto& part : split(all, ";")) {
    auto kv = split(trim(part), "=", 1);
    if (kv.size() == 2 && kv[0] == name) return kv[1];
  }
  return "";
}

bool App::lan_ok(const httplib::Request& req, const std::string& host) {
  if (LOCAL.count(host)) return true;
  std::string supplied = req.get_header_value("X-Nexus-Key");
  if (supplied.empty() && req.has_param("key")) supplied = req.get_param_value("key");
  if (supplied.empty()) supplied = cookie(req, "nexus_key");
  return !supplied.empty() && consteq(supplied, lan_key());
}

void App::broadcast(const json& event) {
  if (primary_ != this) return primary_->broadcast(event);
  std::string text = jdump(event);
  std::lock_guard<std::mutex> lk(ws_mu_);
  for (auto& [ws, dev] : sockets_) {
    try {
      if (ws->is_open()) ws->send(text);
    } catch (...) {
    }
  }
}

void App::route(const std::string& method, const std::string& pattern, Handler h, bool admin_required) {
  // "/api/x/{id}" -> "/api/x/([^/]+)"; "{rel:path}" -> "(.+)" (las rutas solo llevan letras, '/', '-' y '.')
  static const std::regex PARAM(R"(\{([A-Za-z_][A-Za-z0-9_]*)(:path)?\})");
  std::vector<std::string> names;
  std::string rx;
  size_t last = 0;
  for (auto it = std::sregex_iterator(pattern.begin(), pattern.end(), PARAM); it != std::sregex_iterator(); ++it) {
    rx += replace_all(pattern.substr(last, it->position() - last), ".", "\\.");
    rx += (*it)[2].matched ? "(.+)" : "([^/]+)";
    names.push_back((*it)[1].str());
    last = it->position() + it->length();
  }
  rx += replace_all(pattern.substr(last), ".", "\\.");
  auto fn = [this, h = std::move(h), admin_required, names](const httplib::Request& req, httplib::Response& res) {
    dispatch(h, admin_required, names, req, res);
  };
  if (method == "GET")
    srv_->Get(rx, fn);
  else if (method == "POST")
    srv_->Post(rx, fn);
  else if (method == "PUT")
    srv_->Put(rx, fn);
  else if (method == "PATCH")
    srv_->Patch(rx, fn);
  else if (method == "DELETE")
    srv_->Delete(rx, fn);
}

static std::string mime_for(const fs::path& p) {
  static const std::map<std::string, std::string> M = {
      {".html", "text/html; charset=utf-8"}, {".js", "text/javascript"},      {".css", "text/css"},
      {".json", "application/json"},        {".png", "image/png"},           {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},              {".gif", "image/gif"},           {".webp", "image/webp"},
      {".svg", "image/svg+xml"},            {".wav", "audio/wav"},           {".mid", "audio/midi"},
      {".mp3", "audio/mpeg"},               {".pdf", "application/pdf"},     {".zip", "application/zip"},
      {".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"},
      {".apk", "application/vnd.android.package-archive"}, {".glb", "model/gltf-binary"},
      {".gltf", "model/gltf+json"},         {".vrm", "model/gltf-binary"},   {".webmanifest", "application/manifest+json"},
      {".ico", "image/x-icon"},             {".woff2", "font/woff2"},        {".wasm", "application/wasm"}};
  auto it = M.find(suffix_lower(p));
  return it == M.end() ? "application/octet-stream" : it->second;
}

static void send_file(httplib::Response& res, const FileOut& f) {
  if (!is_file(f.path)) {
    res.status = 404;
    res.set_content(json{{"detail", "Not Found"}}.dump(), "application/json");
    return;
  }
  std::string mime = f.mime.empty() ? mime_for(f.path) : f.mime;
  auto path = f.path;
  uintmax_t size = fsize(path);
  res.set_content_provider(
      (size_t)size, mime,
      [path](size_t offset, size_t length, httplib::DataSink& sink) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        in.seekg((std::streamoff)offset);
        std::vector<char> buf(std::min<size_t>(length, 1 << 20));
        size_t left = length;
        while (left > 0 && in) {
          size_t n = std::min(left, buf.size());
          in.read(buf.data(), (std::streamsize)n);
          auto got = (size_t)in.gcount();
          if (!got) break;
          if (!sink.write(buf.data(), got)) return false;
          left -= got;
        }
        return true;
      });
  if (!f.filename.empty())
    res.set_header("Content-Disposition", "attachment; filename=\"" + replace_all(f.filename, "\"", "") +
                                              "\"; filename*=UTF-8''" + url_encode(f.filename));
}

void App::dispatch(const Handler& h, bool admin_required, const std::vector<std::string>& names,
                   const httplib::Request& req, httplib::Response& res) {
  Ctx c{req, res, *this};
  c.host = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
  for (size_t i = 0; i < names.size() && i + 1 < req.matches.size(); ++i) c.path[names[i]] = url_decode(req.matches[i + 1].str());
  c.admin = is_admin(req.get_header_value("X-Admin-Token"));
  c.remote = remote();
  try {
    if (admin_required && !c.admin) throw HttpErr(403, "Requiere modo administrador");
    std::string ctype = req.get_header_value("Content-Type");
    if (!req.body.empty() && !starts_with(ctype, "multipart/form-data") &&
        !starts_with(ctype, "application/x-www-form-urlencoded")) {
      c.body = jparse(req.body);
      if (c.body.is_discarded()) throw HttpErr(422, unprocessable("body", "", "JSON decode error"));
    }
    Out out = h(c);
    if (auto* j = std::get_if<json>(&out)) {
      res.status = res.status == -1 || res.status == 0 ? 200 : res.status;
      res.set_content(jdump(*j), "application/json");
    } else if (auto* r = std::get_if<Raw>(&out)) {
      res.status = r->status;
      for (auto& [k, v] : r->headers) res.set_header(k, v);
      if (r->status != 204) res.set_content(r->body, r->mime);
    } else if (auto* f = std::get_if<FileOut>(&out)) {
      res.status = 200;
      send_file(res, *f);
    }
  } catch (const HttpErr& e) {
    res.status = e.status;
    res.set_content(jdump(json{{"detail", e.detail}}), "application/json");
  } catch (const PermissionError& e) {
    res.status = 403;
    res.set_content(jdump(json{{"detail", e.what()}}), "application/json");
  } catch (const tools::ToolError& e) {
    res.status = (e.type == "KeyError" || e.type == "FileNotFoundError") ? 404 : 400;
    res.set_content(jdump(json{{"detail", e.what()}}), "application/json");
  } catch (const std::exception& e) {
    L.error("Error en ", req.method, " ", req.path, ": ", e.what());
    res.status = 500;
    res.set_content(jdump(json{{"detail", std::string("Internal Server Error: ") + e.what()}}), "application/json");
  }
}

void App::setup() {
  auto& s = *srv_;
  s.set_payload_max_length((size_t)4 << 30);
  s.set_read_timeout(300, 0);
  s.set_write_timeout(300, 0);
  s.new_task_queue = [] { return new httplib::ThreadPool(48); };
  if (remote()) {
    // puerto remoto: todo exige la llave de un dispositivo emparejado, venga de donde venga (tambien desde 127.0.0.1,
    // para que un tunel o proxy no pueda saltarselo)
    s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
      auto deny = [&](int status, const std::string& body, const std::string& mime) {
        res.status = status;
        for (auto& [k, v] : remote::SECURITY_HEADERS) res.set_header(k, v);
        res.set_content(body, mime);
        return httplib::Server::HandlerResponse::Handled;
      };
      if (!services_) return deny(503, json{{"detail", "Nexus-Office esta arrancando"}}.dump(), "application/json");
      std::string host = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
      std::string token = cookie(req, remote::COOKIE);
      if (token.empty()) token = req.get_header_value("X-Nexus-Device");
      if (!services_->remote->devices.check(token, host).empty() || remote::open_path(req.path))
        return httplib::Server::HandlerResponse::Unhandled;
      if (req.path == "/") return deny(401, services_->remote->pair_page(), "text/html; charset=utf-8");
      return deny(401, json{{"detail", "Dispositivo no emparejado"}}.dump(), "application/json");
    });
    s.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
      for (auto& [k, v] : remote::SECURITY_HEADERS)
        if (!(k == "cache-control" && starts_with(req.path, "/static/"))) res.set_header(k, v);
    });
  } else
  // guarda de la red local: fuera de este equipo hace falta la clave de red
  s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    std::string host = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    bool open = false;
    for (auto& p : OPEN_PATHS) open |= starts_with(req.path, p);
    if (!services_ || open || lan_ok(req, host)) {
      if (req.has_param("key") && !LOCAL.count(host))
        res.set_header("Set-Cookie", "nexus_key=" + req.get_param_value("key") +
                                         "; HttpOnly; SameSite=Lax; Max-Age=31536000; Path=/");
      return httplib::Server::HandlerResponse::Unhandled;
    }
    res.status = 401;
    if (req.path == "/") {
      res.set_content(
          "<!doctype html><meta name=viewport content='width=device-width'><body style='background:#04060c;color:#d8f6ff;"
          "font-family:sans-serif;padding:24px'><h2 style='color:#00f0ff'>Nexus-Office</h2><p>Introduce la clave de red "
          "(en el PC: Red COOP &gt; Conectar dispositivos).</p><form><input name=key autofocus style='font-size:18px;padding:8px'>"
          " <button style='font-size:18px;padding:8px'>Entrar</button></form>",
          "text/html; charset=utf-8");
    } else {
      res.set_content(json{{"detail", "Clave de red requerida"}}.dump(), "application/json");
    }
    return httplib::Server::HandlerResponse::Handled;
  });
  // estaticos: se revalidan en cada carga (tras actualizar no quedan versiones viejas)
  httplib::Headers nocache = {{"Cache-Control", "no-cache"}};
  s.set_mount_point("/static", nx::S(paths::web()), nocache);
  s.set_mount_point("/outputs", nx::S(paths::outputs_dir()));
  s.set_file_extension_and_mimetype_mapping("webmanifest", "application/manifest+json");
  s.set_file_extension_and_mimetype_mapping("glb", "model/gltf-binary");
  s.set_file_extension_and_mimetype_mapping("vrm", "model/gltf-binary");
  s.set_file_extension_and_mimetype_mapping("gltf", "model/gltf+json");
  s.set_file_extension_and_mimetype_mapping("mid", "audio/midi");
  s.set_file_extension_and_mimetype_mapping("js", "text/javascript");
  s.WebSocket("/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) { websocket(req, ws); });
  register_routes(*this);
}

bool App::listen(const std::string& host, int port) {
  bool ok = srv_->listen(host, port);
  return ok;
}

bool App::bind(const std::string& host, int port) { return srv_->bind_to_port(host, port); }
bool App::listen_after_bind() { return srv_->listen_after_bind(); }

void App::close_device(const std::string& dev_id) {
  if (primary_ != this) return primary_->close_device(dev_id);
  std::lock_guard<std::mutex> lk(ws_mu_);
  for (auto& [ws, dev] : sockets_)
    if (dev == dev_id && ws->is_open()) {
      try {
        ws->close((httplib::ws::CloseStatus)4401, "revocado");
      } catch (...) {
      }
    }
}

void App::stop() {
  stopping = true;
  srv_->stop();
}

// ------------------------------------------------------------------ websocket
namespace {
// el chat sigue en otro hilo: si la interfaz se desconecta, los eventos se descartan
struct Link {
  std::mutex mu;
  httplib::ws::WebSocket* ws;
  bool alive = true;
  void send(const json& e) {
    std::lock_guard<std::mutex> lk(mu);
    if (!alive || !ws->is_open()) return;
    try {
      ws->send(jdump(e));
    } catch (...) {
    }
  }
};
}  // namespace

void App::websocket(const httplib::Request& req, httplib::ws::WebSocket& ws) {
  std::string host = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
  std::string device;
  if (remote()) {
    std::string token = cookie(req, remote::COOKIE);
    if (token.empty()) token = req.get_header_value("X-Nexus-Device");
    if (services_) device = services_->remote->devices.check(token, host);
    if (device.empty()) {
      ws.close((httplib::ws::CloseStatus)4401, "Dispositivo no emparejado");
      return;
    }
  } else if (!services_ || !lan_ok(req, host)) {
    ws.close(httplib::ws::CloseStatus::PolicyViolation, "Clave de red requerida");
    return;
  }
  auto link = std::make_shared<Link>();
  link->ws = &ws;
  App& hub = *primary_;
  {
    std::lock_guard<std::mutex> lk(hub.ws_mu_);
    hub.sockets_[&ws] = device;
  }
  Services& s = *services_;
  link->send({{"type", "hello"}, {"version", NEXUS_VERSION}});
  ws.set_read_timeout(30);
  std::string msg;
  while (!stopping) {
    auto r = ws.read(msg);
    if (r == httplib::ws::ReadResult::Timeout) continue;
    if (r == httplib::ws::ReadResult::Fail) break;
    json data = jparse(msg);
    if (!data.is_object()) continue;
    std::string type = jstr(data, "type");
    if (type == "ping") {
      link->send({{"type", "pong"}});
    } else if (type == "chat") {
      std::thread([&s, link, data] {
        tools::Emit emit = [link](const json& e) { link->send(e); };
        try {
          std::string text = jstr(data, "text");
          std::string sid = s.memory->ensure_session(jstr(data, "session_id"), uleft(text, 50));
          emit({{"type", "session"}, {"session_id", sid}});
          std::vector<std::string> images;
          for (auto& i : data.value("images", json::array())) {
            if (images.size() >= 4 || !i.is_string()) continue;
            std::string v = i.get<std::string>();
            size_t comma = v.find(',');
            images.push_back(comma == std::string::npos ? v : v.substr(comma + 1));
          }
          std::string agent = data.contains("agent") && data["agent"].is_string() ? data["agent"].get<std::string>() : "auto";
          s.agents->run(agent, text, sid, images, emit, false, 0, true, truthy(data.value("live", json(false))));
        } catch (const std::exception& e) {
          L.error("Error en el chat: ", e.what());
          emit({{"type", "error"}, {"message", e.what()}});
          emit({{"type", "state"}, {"state", "idle"}});
        }
      }).detach();
    }
  }
  {
    std::lock_guard<std::mutex> lk(hub.ws_mu_);
    hub.sockets_.erase(&ws);
  }
  std::lock_guard<std::mutex> lk(link->mu);
  link->alive = false;
}

}  // namespace nx::server
