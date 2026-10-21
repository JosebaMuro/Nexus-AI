// GIMP como motor de edicion de imagen para los agentes (nexus/media/gimp.py): retoque, encuadre, texto, logos y
// exportacion. GIMP 3 con Python-Fu, GIMP 2.10 con Script-Fu y, sin GIMP, el motor integrado (media/raster) con
// las mismas operaciones. Nada se descarga.
#pragma once
#include <array>
#include <functional>
#include <tuple>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Config;
}

namespace nx::gimp {

struct EditError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

extern const std::vector<std::string> POSITIONS;
extern const std::map<std::string, std::string> FORMATS;   // extension -> formato
const json& ops_spec();     // OPS en orden: {op: {param: [tipo, defecto, min, max]}}
const json& op_help();      // OP_HELP
const json& recipes();      // RECIPES

std::array<int, 3> parse_color(const json& v);
json normalize(json ops);
// overlay_size(ruta) -> (w, h); devuelve (primitivas, tamano final)
std::pair<json, std::pair<int, int>> resolve(const json& ops, int W, int H,
                                             const std::function<std::pair<int, int>(const std::string&)>& overlay_size = nullptr);

struct Found {
  std::vector<std::string> cmd;
  std::string version, origin, interpreter;
  int major = 0;
};
std::vector<std::pair<std::vector<std::string>, std::string>> candidates(Config* config);
std::optional<Found> probe(const std::vector<std::string>& cmd, const std::string& origin);
std::optional<Found> find(Config* config, bool refresh = false);

std::string script_fu(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                      bool alpha = true);
std::string python_fu(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                      bool alpha = true);
// sin GIMP: el motor integrado (lo que hace Pillow en la version Python)
void raster_edit(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                 bool alpha = true);

class GimpEngine {
 public:
  explicit GimpEngine(Config* config) : config_(config) {}
  std::optional<Found> info(bool refresh = false);
  json status(bool refresh = false);
  // (ancho, alto, transparencia) ya girada segun su EXIF
  std::tuple<int, int, bool> size_of(const fs::path& src);
  json edit(const fs::path& src, json ops, const fs::path& out, int quality = 90, const std::string& engine = "",
            const std::function<fs::path(const std::string&)>& overlay_resolver = nullptr, double timeout = 300);
  json run_script(const std::string& code, const std::optional<fs::path>& src, const std::optional<fs::path>& out,
                  double timeout = 300);

 private:
  Config* config_;
  std::mutex mu_;
  std::optional<Found> info_;
  double checked_ = 0;
  void run_gimp(const Found& g, const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt,
                int quality, double timeout, bool alpha = true);
};

}  // namespace nx::gimp
