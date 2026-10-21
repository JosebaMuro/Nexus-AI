// Cliente HTTP/HTTPS (net.py + httpx): JSON, flujos SSE/NDJSON y descargas reanudables con progreso.
// Respeta HTTPS_PROXY/HTTP_PROXY/NO_PROXY como httpx. Certificados: nexus/certs/cacert.pem o SSL_CERT_FILE.
#pragma once
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response {
  int status = 0;            // 0: no hubo respuesta (error de red)
  std::string error;         // texto del error de red
  std::map<std::string, std::string> headers;   // claves en minusculas
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
  std::string header(const std::string& k) const;
  json json_body() const { return jparse(body); }
};

struct Request {
  std::string method = "GET";
  std::string url;
  Headers headers;
  std::string body;
  std::string content_type;
  double timeout = 30;               // conexion y lectura
  double read_timeout = -1;          // -1: igual que timeout
  bool follow = true;
  // si se da, el cuerpo se entrega por trozos y no se acumula en Response::body;
  // devolver false corta la transferencia
  std::function<bool(const char*, size_t)> on_data;
  std::function<bool(const Response&)> on_headers;
};

class HttpError : public std::runtime_error {
 public:
  int status;
  HttpError(int st, const std::string& msg) : std::runtime_error(msg), status(st) {}
};

extern const char* UA;   // "nexus-office/1.0"

Response send(const Request& req);
Response get(const std::string& url, const Headers& h = {}, double timeout = 20);
Response post_json(const std::string& url, const json& body, const Headers& h = {}, double timeout = 60);

// net.get_json: lanza HttpError si falla
json get_json(const std::string& url, const Headers& h = {}, const std::map<std::string, std::string>& params = {},
              double timeout = 20);

// Lectura linea a linea (SSE de OpenAI, NDJSON de Ollama). on_line devuelve false para parar.
Response stream_lines(Request req, const std::function<bool(const std::string&)>& on_line);

// net.download: dest.part + Range para reanudar; progress(hecho, total). cancel() true aborta (lanza).
struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("descarga cancelada") {}
};
fs::path download(const std::string& url, const fs::path& dest, const Headers& h = {},
                  const std::function<void(uint64_t, uint64_t)>& progress = nullptr,
                  const std::function<bool()>& cancel = nullptr, int retries = 3);

std::string query_string(const std::map<std::string, std::string>& params);

struct Url {
  std::string scheme, host, path;   // path incluye la query
  int port = 0;
};
Url parse_url(const std::string& url);
bool tls_available();

}  // namespace nx::http
