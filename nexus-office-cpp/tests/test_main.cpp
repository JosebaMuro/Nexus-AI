#include <cstring>

#include "check.h"
#include "tools/tools.h"
#include "util/fs.h"
#include "util/log.h"

namespace nx::tools {
void link_builtin_gimp();
}

// nexus-tests --tool-calls llamadas.json: ejecuta herramientas integradas sin servicios (paridad con Python)
static int tool_calls(const char* file) {
  nx::tools::link_builtin_gimp();
  auto& fns = nx::tools::builtin_functions();
  nx::json out = nx::json::array();
  for (auto& c : nx::json::parse(nx::read_text(file))) {
    nx::tools::ToolContext ctx;
    std::string name = c["tool"];
    try {
      out.push_back({{"ok", fns.at(name)(c["args"], ctx)}});
    } catch (const nx::tools::ToolError& e) {
      out.push_back({{"error", e.type + ": " + e.what()}});
    } catch (const std::exception& e) {
      out.push_back({{"error", std::string("Exception: ") + e.what()}});
    }
  }
  std::cout << out.dump() << "\n";
  return 0;
}

int main(int argc, char** argv) {
  nx::log::set_quiet(true);
  if (argc > 2 && !std::strcmp(argv[1], "--tool-calls")) return tool_calls(argv[2]);
  // datos de prueba aislados
  auto tmp = std::filesystem::temp_directory_path() / ("nexus-tests-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(tmp);
  nx::set_env("NEXUS_DATA_DIR", nx::S(tmp));
  int run = 0;
  for (auto& c : nxt::cases()) {
    if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
    int before = nxt::failures();
    try {
      c.fn();
    } catch (const std::exception& e) {
      std::cerr << c.name << ": excepcion: " << e.what() << "\n";
      ++nxt::failures();
    }
    ++run;
    std::cout << (nxt::failures() == before ? "ok    " : "FALLO ") << c.name << "\n";
  }
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);
  std::cout << run << " pruebas, " << nxt::failures() << " fallos\n";
  return nxt::failures() ? 1 : 0;
}
