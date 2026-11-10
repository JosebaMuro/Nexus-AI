// MCP (Model Context Protocol) en Agente IA, en los dos sentidos:
//
// * Servidor: las herramientas de Agente IA (oficina, documentos, imagen, percepcion, bolsa, web...) para otros
//   programas con MCP (asistentes de escritorio, editores, otros agentes). Transportes:
//     - HTTP: POST /mcp en el servidor de Agente IA (JSON-RPC 2.0; una peticion o un lote por POST).
//     - stdio: «nexus-office mcp» lee JSON-RPC por la entrada estandar y lo pasa al /mcp del servidor local.
//   Cada llamada pasa por el supervisor de herramientas como cualquier agente: la politica por riesgo, las
//   aprobaciones y la auditoria se aplican igual. Fuera del propio equipo hace falta el token mcp.token.
//
// * Cliente: los agentes usan herramientas de servidores MCP externos (mcp.servidores en la configuracion: una orden
//   que se arranca con stdio o una URL HTTP). Sus herramientas entran en el registro como «mcp_<servidor>_<nombre>»
//   con el riesgo que diga la configuracion (medio por defecto: piden aprobacion segun la politica).
//
// Rendimiento: no acelera la inferencia (es otro canal para las herramientas); dentro de Agente IA las herramientas
// integradas se siguen llamando directamente, sin JSON-RPC.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/json.h"

namespace nx {
class Services;
}
namespace nx::tools {
struct ToolSpec;
}

namespace nx::mcp {

inline constexpr const char* PROTOCOLO = "2025-06-18";

// ---------------------------------------------------------------- servidor
// atiende un mensaje JSON-RPC (o un lote); null para las notificaciones (sin respuesta)
json atender(Services& s, const json& mensaje, const std::string& cliente, bool remoto);

// «nexus-office mcp [--url http://127.0.0.1:8775] [--token T]»: puente stdio -> HTTP
int main_stdio(const std::vector<std::string>& argv);

// ---------------------------------------------------------------- cliente
class Canal {
 public:
  virtual ~Canal() = default;
  virtual json pedir(const json& peticion, double timeout_s) = 0;   // respuesta con el mismo id (lanza si no llega)
  virtual void avisar(const json& notificacion) = 0;
  virtual bool vivo() = 0;
};

// servidor MCP arrancado como proceso hijo, JSON-RPC por lineas en stdin/stdout
std::unique_ptr<Canal> canal_stdio(const std::vector<std::string>& orden, const std::map<std::string, std::string>& env);
// servidor MCP por HTTP (Streamable HTTP: respuesta JSON o text/event-stream)
std::unique_ptr<Canal> canal_http(const std::string& url, const std::map<std::string, std::string>& cabeceras);

class Cliente {
 public:
  Cliente(std::string nombre, std::unique_ptr<Canal> canal);
  json iniciar();                       // initialize + notifications/initialized
  json herramientas();                  // tools/list (todas las paginas)
  json llamar(const std::string& herramienta, const json& args, double timeout_s = 120);
  const std::string& nombre() const { return nombre_; }
  bool vivo() { return canal_ && canal_->vivo(); }
  json info_servidor;

 private:
  std::string nombre_;
  std::unique_ptr<Canal> canal_;
  std::atomic<long long> siguiente_{1};
  std::mutex mu_;
};

// clientes de mcp.servidores y sus herramientas en el registro
class Gestor {
 public:
  explicit Gestor(Services& s);
  ~Gestor();
  void conectar();   // (re)conecta todos en segundo plano y registra sus herramientas
  json estado();

 private:
  void conectar_ya();
  Services& s_;
  std::mutex mu_;
  std::vector<std::shared_ptr<Cliente>> clientes_;
  json estado_ = json::object();
  std::thread hilo_;
};

// nombre de herramienta valido para el registro: mcp_<servidor>_<herramienta> con [a-z0-9_]
std::string nombre_herramienta(const std::string& servidor, const std::string& herramienta);
// texto de un resultado MCP (content[]) para el agente; {"error"} si isError
json resultado_a_json(const json& r);

}  // namespace nx::mcp
