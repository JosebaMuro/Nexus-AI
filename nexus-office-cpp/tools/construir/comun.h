// nexus-construir: lo comun a todas las ordenes (rutas del repositorio, version, registro, procesos y copias).
#pragma once
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nc {

namespace fs = std::filesystem;
using nx::json;

fs::path raiz();       // nexus-office-cpp/ (NEXUS_SOURCE_DIR o la carpeta de trabajo)
fs::path repo();       // la carpeta superior (firmware/, .github/)
std::string version(); // NEXUS_VERSION de CMakeLists.txt (2.2.0-cpp)

void log(const std::string& tag, const std::string& msg);
std::string mb(const fs::path& p);                     // "12.3 MB"

struct Fallo : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Ejecuta un programa con la salida en la consola; lanza Fallo si no termina con 0.
void ejecutar(const std::vector<std::string>& args, const fs::path& cwd = {},
              const std::map<std::string, std::string>& env = {});
// Igual, pero devuelve el codigo de salida en vez de lanzar.
int ejecutar_codigo(const std::vector<std::string>& args, const fs::path& cwd = {},
                    const std::map<std::string, std::string>& env = {});

// Copia una carpeta entera. ignorar(ruta relativa con /) true: no se copia (ni lo que haya dentro).
void copiar_arbol(const fs::path& src, const fs::path& dst,
                  const std::function<bool(const std::string&)>& ignorar = nullptr);
void copiar(const fs::path& src, const fs::path& dst);   // fichero (crea la carpeta)
void escribir(const fs::path& p, const std::string& data, int mode = 0644);
void modo(const fs::path& p, int mode);                   // chmod (nada en Windows)
std::string sha256_fichero(const fs::path& p);            // hexadecimal
uint64_t tamano_arbol(const fs::path& dir);                // bytes de los ficheros
std::vector<fs::path> ficheros(const fs::path& dir);      // recursivo, ordenados, sin carpetas

// Argumentos: --opcion valor, --opcion v1 v2 ... (las de "multi"), --bandera, y posicionales.
struct Args {
  std::map<std::string, std::vector<std::string>> op;
  std::vector<std::string> pos;
  bool tiene(const std::string& k) const { return op.count(k) > 0; }
  std::string val(const std::string& k, const std::string& def = "") const;
  std::vector<std::string> lista(const std::string& k) const;
};
Args leer_args(const std::vector<std::string>& argv, const std::set<std::string>& multi,
               const std::set<std::string>& banderas);

// ordenes
int paquetes(const std::vector<std::string>& argv);
int apk(const std::vector<std::string>& argv);
int pdf(const std::vector<std::string>& argv);
int humo(const std::vector<std::string>& argv);
int grafo_wikipedia(const std::vector<std::string>& argv);
int datos(const std::vector<std::string>& argv);

// markdown.cpp: Markdown (tablas, bloques de codigo, listas) a HTML, como python-markdown con
// tables, fenced_code y sane_lists
std::string markdown_html(const std::string& md);

}  // namespace nc
