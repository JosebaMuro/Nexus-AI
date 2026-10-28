// nexus-construir: la herramienta de construccion de Nexus-Office, en C++ como el resto del proyecto.
//
//   nexus-construir paquetes [--targets t1 t2 ...] [--apk a.apk ...] [--docs x.pdf ...] [--keep]
//   nexus-construir apk --exe <nexus-office de Android> --libs <libllama-server*.so> --recursos <carpeta> [--gpu-libs d]
//   nexus-construir pdf [carpeta_salida]
//   nexus-construir humo <carpeta instalada> [--exe ruta] [--modelo id] [--sin-imagen] [--sin-servicio] [--acelerador a]
//   nexus-construir grafo-wikipedia [--salida d] [--articulos n] [--por-categoria n] [--profundidad n] [--raices a,b]
//   nexus-construir datos             regenera src/gen/data.cpp desde recursos/datos/*.json
#include <iostream>

#include "comun.h"
#include "util/fs.h"

namespace {

int uso() {
  std::cout << "nexus-construir " << NEXUS_VERSION << ": instaladores, APK, manuales y pruebas de Nexus-Office\n\n"
               "  paquetes [--targets ...] [--apk ...] [--docs ...] [--keep]   instaladores de cada sistema en dist/\n"
               "  apk --exe E --libs D --recursos R [--gpu-libs G]          APK autonoma de Android\n"
               "  pdf [carpeta]                                             Manual-de-usuario.pdf y Documentacion-tecnica.pdf\n"
               "  humo <carpeta instalada> [--sin-imagen] [--sin-servicio]  prueba con modelos reales\n"
               "  grafo-wikipedia [--salida D] [--articulos N]              grafo multicapa de ejemplo (internet)\n"
               "  datos                                                     regenera src/gen/data.cpp\n";
  return 2;
}

int run(std::vector<std::string> a) {
  if (a.empty()) return uso();
  std::string cmd = a.front();
  a.erase(a.begin());
  try {
    if (cmd == "paquetes") return nc::paquetes(a);
    if (cmd == "apk") return nc::apk(a);
    if (cmd == "pdf") return nc::pdf(a);
    if (cmd == "humo") return nc::humo(a);
    if (cmd == "grafo-wikipedia") return nc::grafo_wikipedia(a);
    if (cmd == "datos") return nc::datos(a);
    if (cmd == "--version") {
      std::cout << NEXUS_VERSION << "\n";
      return 0;
    }
  } catch (const std::exception& e) {
    std::cerr << "[nexus-construir] error: " << e.what() << std::endl;
    return 1;
  }
  return uso();
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> a;
  for (int i = 1; i < argc; ++i) a.push_back(nx::S(std::filesystem::path(argv[i])));
  return run(a);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
