// MCP (Model Context Protocol): servidor y cliente (ver mcp.h).
#include "mcp/mcp.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>

#include "core/services.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/http.h"
#include "util/log.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nx::mcp {

namespace {
const auto L = log::get("mcp");

json error_rpc(const json& id, int codigo, const std::string& msg) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", codigo}, {"message", msg}}}};
}
json ok_rpc(const json& id, const json& r) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", r}}; }

// esquema de entrada MCP a partir del de la herramienta (JSON Schema de tipo object)
json esquema(const json& p) {
  json e = p.is_object() ? p : json{{"type", "object"}, {"properties", json::object()}};
  if (!e.contains("type")) e["type"] = "object";
  if (!e.contains("properties")) e["properties"] = json::object();
  return e;
}

// resultado de una herramienta de Agente IA a contenido MCP (texto JSON y, si es imagen o video, el enlace)
json contenido(const json& r) {
  json c = json::array();
  bool error = r.is_object() && r.contains("error") && r.size() <= 2;
  c.push_back({{"type", "text"}, {"text", r.is_string() ? r.get<std::string>() : jdump(r)}});
  if (r.is_object())
    for (const char* k : {"url", "video_url", "pdf_url"})
      if (r.contains(k) && r[k].is_string() && starts_with(r[k].get<std::string>(), "/outputs/"))
        c.push_back({{"type", "resource_link"}, {"uri", "agente-ia:" + r[k].get<std::string>()}, {"name", r[k].get<std::string>()}});
  json out = {{"content", c}, {"isError", error}};
  if (r.is_object() && !error) out["structuredContent"] = r;
  return out;
}
}  // namespace

std::string nombre_herramienta(const std::string& servidor, const std::string& herramienta) {
  auto limpia = [](const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += std::isalnum(c) ? (char)std::tolower(c) : '_';
    return o;
  };
  return uleft("mcp_" + limpia(servidor) + "_" + limpia(herramienta), 64);
}

json resultado_a_json(const json& r) {
  if (!r.is_object()) return {{"resultado", r}};
  std::string texto;
  json enlaces = json::array();
  for (auto& c : r.value("content", json::array())) {
    std::string t = jstr(c, "type");
    if (t == "text") texto += (texto.empty() ? "" : "\n") + jstr(c, "text");
    else if (t == "resource_link" || t == "resource") enlaces.push_back(c.value("uri", c.value("resource", json::object()).value("uri", json(""))));
    else if (t == "image") texto += "\n[imagen " + jstr(c, "mimeType") + "]";
  }
  if (truthy(r.value("isError", json(false)))) return {{"error", "MCP: " + uleft(texto, 2000)}};
  json o = {{"resultado", uleft(texto, 20000)}};
  if (r.contains("structuredContent")) o["datos"] = r["structuredContent"];
  if (!enlaces.empty()) o["enlaces"] = enlaces;
  return o;
}

// ================================================================ servidor
json atender(Services& s, const json& m, const std::string& cliente, bool remoto) {
  if (m.is_array()) {   // lote
    json out = json::array();
    for (auto& x : m) {
      json r = atender(s, x, cliente, remoto);
      if (!r.is_null()) out.push_back(r);
    }
    return out.empty() ? json() : out;
  }
  if (!m.is_object() || jstr(m, "jsonrpc") != "2.0") return error_rpc(nullptr, -32600, "Peticion JSON-RPC no valida");
  const json id = m.value("id", json());
  const std::string metodo = jstr(m, "method");
  const json p = m.value("params", json::object());
  if (!m.contains("id")) return json();   // notificacion (initialized, cancelled...)
  try {
    if (metodo == "initialize")
      return ok_rpc(id, {{"protocolVersion", p.value("protocolVersion", json(PROTOCOLO))},
                         {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", json::object()}}},
                         {"serverInfo", {{"name", "agente-ia"}, {"title", "Agente IA"}, {"version", NEXUS_VERSION}}},
                         {"instructions",
                          "Herramientas de Agente IA (oficina de IA local): documentos, imagen, percepcion, bolsa, web y "
                          "ficheros. Las de riesgo medio o alto pueden quedar pendientes de aprobacion en Agente IA."}});
    if (metodo == "ping") return ok_rpc(id, json::object());
    if (metodo == "tools/list") {
      json lista = json::array();
      for (auto& t : s.tools->all()) {
        if (!t->enabled || starts_with(t->name, "mcp_")) continue;   // las de otros servidores MCP no se reexportan
        lista.push_back({{"name", t->name},
                         {"description", t->description},
                         {"inputSchema", esquema(t->parameters)},
                         {"annotations", {{"readOnlyHint", t->risk == "low"}, {"destructiveHint", t->risk == "high"}}}});
      }
      return ok_rpc(id, {{"tools", lista}});
    }
    if (metodo == "tools/call") {
      std::string nombre = jstr(p, "name");
      auto t = s.tools->get(nombre);
      if (!t || !t->enabled || starts_with(nombre, "mcp_")) return error_rpc(id, -32602, "Herramienta desconocida: " + nombre);
      tools::ToolContext ctx;
      ctx.services = &s;
      ctx.agent = "mcp:" + (cliente.empty() ? std::string("cliente") : cliente);
      ctx.session_id = "mcp-" + token_hex(4);
      ctx.remote = remoto;
      ctx.stats = std::make_shared<tools::TurnStats>();
      json r = s.supervisor->execute(nombre, p.value("arguments", json::object()), ctx);
      return ok_rpc(id, contenido(r));
    }
    if (metodo == "resources/list") return ok_rpc(id, {{"resources", json::array()}});
    if (metodo == "prompts/list") return ok_rpc(id, {{"prompts", json::array()}});
    return error_rpc(id, -32601, "Metodo no admitido: " + metodo);
  } catch (const std::exception& e) {
    return error_rpc(id, -32603, e.what());
  }
}

int main_stdio(const std::vector<std::string>& argv) {
  std::string url = "http://127.0.0.1:8775", token;
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--url") url = argv[i + 1];
    if (argv[i] == "--token") token = argv[i + 1];
  }
  if (token.empty()) token = env("NEXUS_MCP_TOKEN");
  // una linea de la entrada = un mensaje; la respuesta, una linea en la salida (stdout solo lleva JSON-RPC)
  std::string linea;
  while (std::getline(std::cin, linea)) {
    if (trim(linea).empty()) continue;
    json m = jparse(linea);
    if (m.is_discarded()) {
      std::cout << jdump(error_rpc(nullptr, -32700, "JSON no valido")) << "\n" << std::flush;
      continue;
    }
    http::Headers h;
    if (!token.empty()) h.push_back({"Authorization", "Bearer " + token});
    auto r = http::post_json(url + "/mcp", m, h, 600);
    if (r.status == 202 || (r.ok() && trim(r.body).empty())) continue;   // notificacion
    if (!r.ok()) {
      if (m.contains("id"))
        std::cout << jdump(error_rpc(m["id"], -32603, "Agente IA no responde en " + url + " (" +
                                                          (r.status ? std::to_string(r.status) : r.error) + ")"))
                  << "\n" << std::flush;
      continue;
    }
    std::cout << trim(r.body) << "\n" << std::flush;
  }
  return 0;
}

// ================================================================ cliente: canales
namespace {

// respuestas por id que llegan desde un hilo lector
struct Buzon {
  std::mutex mu;
  std::condition_variable cv;
  std::map<std::string, json> respuestas;
  bool cerrado = false;
  void entregar(const json& m) {
    if (!m.is_object() || !m.contains("id") || m.contains("method")) return;   // peticiones del servidor: no se usan
    std::lock_guard<std::mutex> lk(mu);
    respuestas[jdump(m["id"])] = m;
    cv.notify_all();
  }
  json esperar(const json& id, double timeout) {
    std::unique_lock<std::mutex> lk(mu);
    std::string k = jdump(id);
    if (!cv.wait_for(lk, std::chrono::milliseconds((long long)(timeout * 1000)), [&] { return cerrado || respuestas.count(k); }))
      throw std::runtime_error("el servidor MCP no respondio a tiempo");
    if (!respuestas.count(k)) throw std::runtime_error("el servidor MCP se cerro");
    json r = respuestas[k];
    respuestas.erase(k);
    return r;
  }
};

class CanalStdio : public Canal {
 public:
  CanalStdio(const std::vector<std::string>& orden, const std::map<std::string, std::string>& env) {
    if (orden.empty()) throw std::runtime_error("orden vacia");
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) throw std::runtime_error("sin tuberias");
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    for (auto& a : orden) {
      std::wstring w = P(a).wstring();
      cmd += (cmd.empty() ? L"" : L" ") + (w.find_first_of(L" \t\"") == std::wstring::npos ? w : L"\"" + w + L"\"");
    }
    // entorno: el del proceso mas las variables del servidor
    std::wstring bloque;
    if (!env.empty()) {
      wchar_t* e = GetEnvironmentStringsW();
      for (wchar_t* q = e; *q; q += wcslen(q) + 1) bloque += std::wstring(q) + L'\0';
      FreeEnvironmentStringsW(e);
      for (auto& [k, v] : env) bloque += P(k + "=" + v).wstring() + L'\0';
      bloque += L'\0';
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env.empty() ? nullptr : bloque.data(), nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
      CloseHandle(in_w);
      CloseHandle(out_r);
      throw std::runtime_error("no se pudo arrancar " + orden[0]);
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    escribir_ = in_w;
    leer_ = out_r;
#else
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) throw std::runtime_error("sin tuberias");
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork fallo");
    if (pid == 0) {
      // sin PR_SET_PDEATHSIG: salta cuando termina el hilo que hizo fork (el de conectar); si Agente IA se cae, el
      // servidor MCP ve el fin de su entrada y termina solo
      dup2(in[0], 0);
      dup2(out[1], 1);
      close(in[0]), close(in[1]), close(out[0]), close(out[1]);
      for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
      std::vector<char*> args;
      for (auto& a : orden) args.push_back(const_cast<char*>(a.c_str()));
      args.push_back(nullptr);
      execvp(args[0], args.data());
      _exit(127);
    }
    close(in[0]);
    close(out[1]);
    pid_ = pid;
    escribir_ = in[1];
    leer_ = out[0];
    signal(SIGPIPE, SIG_IGN);
#endif
    lector_ = std::thread([this] { leer(); });
  }
  ~CanalStdio() override {
#ifdef _WIN32
    CloseHandle((HANDLE)escribir_);
    if (WaitForSingleObject((HANDLE)proc_, 2000) != WAIT_OBJECT_0) TerminateProcess((HANDLE)proc_, 1);
    CloseHandle((HANDLE)leer_);
    if (lector_.joinable()) lector_.join();
    CloseHandle((HANDLE)proc_);
#else
    close(escribir_);
    for (int i = 0; i < 20 && waitpid(pid_, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (waitpid(pid_, nullptr, WNOHANG) == 0) {
      ::kill(pid_, SIGTERM);
      waitpid(pid_, nullptr, 0);
    }
    close(leer_);
    if (lector_.joinable()) lector_.join();
#endif
  }
  json pedir(const json& req, double timeout) override {
    enviar(req);
    return buzon_.esperar(req["id"], timeout);
  }
  void avisar(const json& n) override { enviar(n); }
  bool vivo() override { return !buzon_.cerrado; }

 private:
  void enviar(const json& m) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string s = jdump(m) + "\n";
#ifdef _WIN32
    DWORD w = 0;
    if (!WriteFile((HANDLE)escribir_, s.data(), (DWORD)s.size(), &w, nullptr)) throw std::runtime_error("el servidor MCP se cerro");
#else
    size_t off = 0;
    while (off < s.size()) {
      ssize_t n = write(escribir_, s.data() + off, s.size() - off);
      if (n <= 0) throw std::runtime_error("el servidor MCP se cerro");
      off += (size_t)n;
    }
#endif
  }
  void leer() {
    std::string buf;
    char tmp[4096];
    for (;;) {
#ifdef _WIN32
      DWORD n = 0;
      if (!ReadFile((HANDLE)leer_, tmp, sizeof(tmp), &n, nullptr) || n == 0) break;
#else
      ssize_t n = read(leer_, tmp, sizeof(tmp));
      if (n <= 0) break;
#endif
      buf.append(tmp, (size_t)n);
      size_t p;
      while ((p = buf.find('\n')) != std::string::npos) {
        std::string linea = trim(buf.substr(0, p));
        buf.erase(0, p + 1);
        if (linea.empty()) continue;
        json m = jparse(linea);
        if (!m.is_discarded()) buzon_.entregar(m);
      }
    }
    std::lock_guard<std::mutex> lk(buzon_.mu);
    buzon_.cerrado = true;
    buzon_.cv.notify_all();
  }
#ifdef _WIN32
  void* proc_ = nullptr;
  void* escribir_ = nullptr;
  void* leer_ = nullptr;
#else
  pid_t pid_ = -1;
  int escribir_ = -1, leer_ = -1;
#endif
  std::mutex mu_;
  Buzon buzon_;
  std::thread lector_;
};

class CanalHttp : public Canal {
 public:
  CanalHttp(std::string url, std::map<std::string, std::string> cab) : url_(std::move(url)), cab_(std::move(cab)) {}
  json pedir(const json& req, double timeout) override {
    auto r = enviar(req, timeout);
    if (!r.ok()) throw std::runtime_error("HTTP " + std::to_string(r.status) + " " + uright(r.error.empty() ? r.body : r.error, 200));
    std::string ct = lower(r.header("content-type"));
    if (contains(ct, "text/event-stream")) {
      // Streamable HTTP con SSE: la respuesta es el evento «data:» con nuestro id
      for (auto& l : splitlines(r.body))
        if (starts_with(l, "data:")) {
          json m = jparse(trim(l.substr(5)));
          if (m.is_object() && m.contains("id") && m["id"] == req["id"]) return m;
        }
      throw std::runtime_error("respuesta SSE sin el resultado");
    }
    json m = r.json_body();
    if (m.is_array())
      for (auto& x : m)
        if (x.value("id", json()) == req["id"]) return x;
    return m;
  }
  void avisar(const json& n) override { enviar(n, 30); }
  bool vivo() override { return true; }

 private:
  http::Response enviar(const json& m, double timeout) {
    http::Request q;
    q.method = "POST";
    q.url = url_;
    q.body = jdump(m);
    q.content_type = "application/json";
    q.timeout = timeout;
    q.headers.push_back({"Accept", "application/json, text/event-stream"});
    q.headers.push_back({"MCP-Protocol-Version", PROTOCOLO});
    for (auto& [k, v] : cab_) q.headers.push_back({k, v});
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!sesion_.empty()) q.headers.push_back({"Mcp-Session-Id", sesion_});
    }
    auto r = http::send(q);
    std::string s = r.header("mcp-session-id");
    if (!s.empty()) {
      std::lock_guard<std::mutex> lk(mu_);
      sesion_ = s;
    }
    return r;
  }
  std::string url_;
  std::map<std::string, std::string> cab_;
  std::mutex mu_;
  std::string sesion_;
};

}  // namespace

std::unique_ptr<Canal> canal_stdio(const std::vector<std::string>& orden, const std::map<std::string, std::string>& env) {
  return std::make_unique<CanalStdio>(orden, env);
}
std::unique_ptr<Canal> canal_http(const std::string& url, const std::map<std::string, std::string>& cab) {
  return std::make_unique<CanalHttp>(url, cab);
}

// ================================================================ cliente
Cliente::Cliente(std::string nombre, std::unique_ptr<Canal> canal) : nombre_(std::move(nombre)), canal_(std::move(canal)) {}

json Cliente::iniciar() {
  json r = canal_->pedir({{"jsonrpc", "2.0"},
                          {"id", siguiente_++},
                          {"method", "initialize"},
                          {"params",
                           {{"protocolVersion", PROTOCOLO},
                            {"capabilities", json::object()},
                            {"clientInfo", {{"name", "agente-ia"}, {"version", NEXUS_VERSION}}}}}},
                         30);
  if (r.contains("error")) throw std::runtime_error("initialize: " + jstr(r["error"], "message"));
  info_servidor = r.value("result", json::object());
  canal_->avisar({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
  return info_servidor;
}

json Cliente::herramientas() {
  json todas = json::array();
  json cursor;
  for (int pagina = 0; pagina < 20; ++pagina) {
    json params = json::object();
    if (!cursor.is_null()) params["cursor"] = cursor;
    json r = canal_->pedir({{"jsonrpc", "2.0"}, {"id", siguiente_++}, {"method", "tools/list"}, {"params", params}}, 30);
    if (r.contains("error")) throw std::runtime_error("tools/list: " + jstr(r["error"], "message"));
    json res = r.value("result", json::object());
    for (auto& t : res.value("tools", json::array())) todas.push_back(t);
    cursor = res.value("nextCursor", json());
    if (cursor.is_null() || (cursor.is_string() && cursor.get<std::string>().empty())) break;
  }
  return todas;
}

json Cliente::llamar(const std::string& h, const json& args, double timeout) {
  json r = canal_->pedir(
      {{"jsonrpc", "2.0"}, {"id", siguiente_++}, {"method", "tools/call"}, {"params", {{"name", h}, {"arguments", args}}}},
      timeout);
  if (r.contains("error")) return {{"error", "MCP " + nombre_ + ": " + jstr(r["error"], "message")}};
  return resultado_a_json(r.value("result", json::object()));
}

// ================================================================ gestor
Gestor::Gestor(Services& s) : s_(s) {}
Gestor::~Gestor() {
  if (hilo_.joinable()) hilo_.join();
}

void Gestor::conectar() {
  if (hilo_.joinable()) hilo_.join();
  hilo_ = std::thread([this] {
    try {
      conectar_ya();
    } catch (const std::exception& e) {
      L.warning("MCP: ", e.what());
    }
  });
}

void Gestor::conectar_ya() {
  json lista = s_.config.get("mcp.servidores", json::array());
  std::vector<std::shared_ptr<Cliente>> nuevos;
  std::vector<std::shared_ptr<tools::ToolSpec>> specs;
  json est = json::object();
  if (lista.is_array())
    for (auto& sv : lista) {
      std::string nombre = jstr(sv, "nombre");
      if (nombre.empty() || (sv.contains("activo") && !truthy(sv["activo"]))) continue;
      json e = {{"nombre", nombre}};
      try {
        std::unique_ptr<Canal> canal;
        std::map<std::string, std::string> extra;
        const json env_sv = sv.value("env", json::object());   // copia: items() de un temporal queda colgando
        for (auto& [k, v] : env_sv.items()) extra[k] = v.is_string() ? v.get<std::string>() : jdump(v);
        if (!jstr(sv, "url").empty()) {
          std::map<std::string, std::string> cab;
          const json cab_sv = sv.value("cabeceras", json::object());
          for (auto& [k, v] : cab_sv.items()) cab[k] = v.is_string() ? v.get<std::string>() : jdump(v);
          canal = canal_http(jstr(sv, "url"), cab);
        } else {
          std::vector<std::string> orden;
          for (auto& a : sv.value("orden", json::array())) orden.push_back(a.get<std::string>());
          canal = canal_stdio(orden, extra);
        }
        auto c = std::make_shared<Cliente>(nombre, std::move(canal));
        json info = c->iniciar();
        json hs = c->herramientas();
        std::string riesgo = jstr(sv, "riesgo", "medium");
        for (auto& h : hs) {
          auto spec = std::make_shared<tools::ToolSpec>();
          std::string original = jstr(h, "name");
          spec->name = nombre_herramienta(nombre, original);
          spec->description = "[MCP " + nombre + "] " + jstr(h, "description");
          spec->parameters = h.value("inputSchema", json{{"type", "object"}, {"properties", json::object()}});
          spec->risk = riesgo;
          spec->source = "mcp";
          spec->author = jstr(info.value("serverInfo", json::object()), "name", nombre);
          spec->path = "mcp:" + nombre;
          std::weak_ptr<Cliente> w = c;
          spec->func = [w, original](const json& args, tools::ToolContext&) -> json {
            auto cl = w.lock();
            if (!cl || !cl->vivo()) throw tools::ToolError("RuntimeError", "el servidor MCP no esta conectado");
            return cl->llamar(original, args);
          };
          specs.push_back(spec);
        }
        e["ok"] = true;
        e["servidor"] = info.value("serverInfo", json::object());
        e["herramientas"] = hs.size();
        nuevos.push_back(c);
        L.info("MCP ", nombre, ": ", hs.size(), " herramientas");
      } catch (const std::exception& ex) {
        e["ok"] = false;
        e["error"] = ex.what();
        L.warning("MCP ", nombre, " no conecto: ", ex.what());
      }
      est[nombre] = e;
    }
  std::vector<std::shared_ptr<Cliente>> viejos;
  {
    // los clientes nuevos quedan guardados antes de que sus herramientas entren en el registro
    std::lock_guard<std::mutex> lk(mu_);
    viejos = std::move(clientes_);
    clientes_ = std::move(nuevos);
    estado_ = est;
  }
  s_.tools->set_externas("mcp", specs);
  viejos.clear();   // los anteriores se cierran ya fuera del registro
}

json Gestor::estado() {
  std::lock_guard<std::mutex> lk(mu_);
  return {{"clientes", estado_}, {"protocolo", PROTOCOLO}};
}

}  // namespace nx::mcp
