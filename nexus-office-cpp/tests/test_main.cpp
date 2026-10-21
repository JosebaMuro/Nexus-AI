#include <cstring>

#include "check.h"
#include "util/fs.h"
#include "util/log.h"

int main(int argc, char** argv) {
  nx::log::set_quiet(true);
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
