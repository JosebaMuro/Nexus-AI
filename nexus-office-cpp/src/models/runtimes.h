// Runtimes nativos precompilados (runtimes.py): llama-server (llama.cpp) y sd-cli (stable-diffusion.cpp).
// Busca primero en bundled/runtimes/<rt>/<variante> (paquetes offline) y en data/runtimes; puede descargar de GitHub.
#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::runtimes {

struct Found {
  fs::path exe;
  std::string variant;
};

std::vector<std::string> candidates(const json& profile, const std::string& runtime);
std::vector<std::string> bundled_variants(const std::string& runtime);
std::optional<fs::path> bundled_exe(const std::string& runtime, const std::string& variant);
fs::path bundled_dir();
// entorno para lanzar el binario (Linux: su carpeta en LD_LIBRARY_PATH)
std::map<std::string, std::string> lib_env(const fs::path& exe);
std::string asset_pattern(const std::string& runtime, const std::string& variant);

class RuntimeManager {
 public:
  RuntimeManager();
  std::optional<fs::path> find_exe(const std::string& runtime, const std::string& variant);
  json installed();
  std::optional<Found> exe(const std::string& runtime, const json& profile, const std::string& preferred = "");
  fs::path install(const std::string& runtime, const std::string& variant);
  json progress();
  // paquete de runtimes sin conexion (Nexus-Office-runtime-<variante>-<version>-<plataforma>.zip, con
  // runtimes/<rt>/<variante>/ dentro): se copia a data/runtimes. Devuelve las variantes que trae.
  json import_pack(const fs::path& zip);
  // los paquetes que haya junto al programa, en su carpeta superior o en data/paquetes: se importan una vez
  json import_found();

 private:
  fs::path root_;
  std::mutex mu_;
  json progress_ = json::object();
  void set_progress(const std::string& key, json v);
};

std::optional<fs::path> find_in(const fs::path& dir, const std::string& runtime);

}  // namespace nx::runtimes
