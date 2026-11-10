// MCP: servidor (herramientas de Agente IA por JSON-RPC) y cliente (servidor MCP externo por stdio en el registro).
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "mcp/mcp.h"
#include "tools/tools.h"
#include "util/fs.h"

using namespace nx;

TEST("mcp: nombres de herramienta y resultados") {
  CHECK_EQ(mcp::nombre_herramienta("Mi Servidor", "Leer-Fichero"), std::string("mcp_mi_servidor_leer_fichero"));
  CHECK(mcp::nombre_herramienta("x", std::string(200, 'a')).size() <= 64);
  json r = mcp::resultado_a_json({{"content", {{{"type", "text"}, {"text", "hola"}}, {{"type", "text"}, {"text", "mundo"}}}}});
  CHECK_EQ(jstr(r, "resultado"), std::string("hola\nmundo"));
  json e = mcp::resultado_a_json({{"content", {{{"type", "text"}, {"text", "mal"}}}}, {"isError", true}});
  CHECK(e.contains("error"));
}

TEST("mcp: servidor y cliente por stdio con sus herramientas en el registro") {
  auto dir = std::filesystem::temp_directory_path() / ("nexus-mcp-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  Services s(false);
  s.start();

  // ---- servidor
  json ini = mcp::atender(s, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", mcp::PROTOCOLO}}}},
                          "prueba", false);
  CHECK_EQ(jstr(ini["result"]["serverInfo"], "name"), std::string("agente-ia"));
  CHECK(mcp::atender(s, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, "prueba", false).is_null());
  json lista = mcp::atender(s, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, "prueba", false);
  bool calc = false;
  for (auto& t : lista["result"]["tools"]) calc |= jstr(t, "name") == "calculator" && t.contains("inputSchema");
  CHECK(calc);
  json llamada = mcp::atender(
      s, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", {{"name", "calculator"}, {"arguments", {{"expression", "6*7"}}}}}},
      "prueba", false);
  CHECK(!truthy(llamada["result"].value("isError", json(false))));
  CHECK(jdump(llamada["result"]["content"]).find("42") != std::string::npos);
  json lote = mcp::atender(s, json::array({{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}},
                                           {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "no/existe"}}}),
                           "prueba", false);
  CHECK_EQ(lote.size(), (size_t)2);
  CHECK_EQ(lote[1]["error"]["code"].get<int>(), -32601);

#ifndef _WIN32
  // ---- cliente: un servidor MCP minimo en sh (eco)
  fs::path eco = dir / "eco.sh";
  {
    std::ofstream f(eco);
    f << "while IFS= read -r l; do\n"
         "  id=$(printf '%s' \"$l\" | sed -n 's/.*\"id\":\\([0-9]*\\).*/\\1/p')\n"
         "  [ -z \"$id\" ] && continue\n"
         "  case \"$l\" in\n"
         "    *'\"initialize\"'*) printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"2025-06-18\","
         "\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"eco\",\"version\":\"1\"}}}\\n' \"$id\";;\n"
         "    *'\"tools/list\"'*) printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":[{\"name\":\"Repetir\","
         "\"description\":\"repite el texto\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"texto\":{\"type\":\"string\"}}}}]}}\\n' \"$id\";;\n"
         "    *'\"tools/call\"'*) t=$(printf '%s' \"$l\" | sed -n 's/.*\"texto\":\"\\([^\"]*\\)\".*/\\1/p');"
         " printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"eco: %s\"}]}}\\n' \"$id\" \"$t\";;\n"
         "    *) printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}\\n' \"$id\";;\n"
         "  esac\n"
         "done\n";
  }
  s.config.set("mcp.servidores", json::array({{{"nombre", "eco"}, {"orden", {"/bin/sh", S(eco)}}, {"riesgo", "low"}}}));
  s.mcp->conectar();
  std::shared_ptr<tools::ToolSpec> t;
  for (int i = 0; i < 100 && !t; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    t = s.tools->get("mcp_eco_repetir");
  }
  CHECK(t != nullptr);
  if (t) {
    CHECK_EQ(t->source, std::string("mcp"));
    CHECK_EQ(t->risk, std::string("low"));
    tools::ToolContext ctx;
    ctx.services = &s;
    ctx.agent = "prueba";
    ctx.stats = std::make_shared<tools::TurnStats>();
    json r = s.supervisor->execute("mcp_eco_repetir", {{"texto", "hola"}}, ctx);
    CHECK_EQ(jstr(r, "resultado"), std::string("eco: hola"));
    if (jstr(r, "resultado") != "eco: hola") std::cerr << jdump(r) << "\n";
    // las herramientas MCP externas no se reexportan por el servidor
    json l2 = mcp::atender(s, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/list"}}, "prueba", false);
    CHECK(jdump(l2).find("mcp_eco") == std::string::npos);
    // siguen tras recargar el registro
    s.tools->reload();
    CHECK(s.tools->get("mcp_eco_repetir") != nullptr);
  }
  CHECK(s.mcp->estado()["clientes"]["eco"]["ok"].get<bool>());
  // sin servidores: sus herramientas salen del registro
  s.config.set("mcp.servidores", json::array());
  s.mcp->conectar();
  for (int i = 0; i < 100 && s.tools->get("mcp_eco_repetir"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(s.tools->get("mcp_eco_repetir") == nullptr);
#endif
  s.shutdown();
}
