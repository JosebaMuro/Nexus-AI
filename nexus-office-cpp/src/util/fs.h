// Rutas portables (nexus/paths.py): todo relativo a la carpeta de Nexus-Office; datos en data/ o NEXUS_DATA_DIR.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nx {

namespace fs = std::filesystem;

// UTF-8 <-> ruta (en Windows las rutas son UTF-16)
fs::path P(const std::string& utf8);
std::string S(const fs::path& p);

std::string env(const char* name, const std::string& def = "");
void set_env(const std::string& name, const std::string& value);

namespace paths {
fs::path root();               // carpeta de la instalacion (la del ejecutable o NEXUS_ROOT)
void set_root(const fs::path& p);
fs::path package();            // root/nexus
fs::path web();                // root/nexus/web
fs::path data_dir();
fs::path sub(std::initializer_list<std::string> parts);   // data/<parts...> (se crea)
fs::path models_dir();
fs::path outputs_dir();
fs::path logs_dir();
fs::path agents_dir();
fs::path sandbox_dir();
fs::path config_file();
fs::path hardware_file();
fs::path tuning_file();
fs::path db_file();
fs::path exe_path();
}  // namespace paths

fs::path home_dir();

namespace paths {
}  // namespace paths

// ---------------------------------------------------------------- ficheros
std::optional<std::string> read_file(const fs::path& p, size_t max_bytes = (size_t)-1);
std::string read_text(const fs::path& p);           // "" si no existe
bool write_file(const fs::path& p, const std::string& data);   // escritura atomica (tmp + rename)
bool append_file(const fs::path& p, const std::string& data);
bool path_exists(const fs::path& p);
bool is_file(const fs::path& p);
bool is_dir(const fs::path& p);
void mkdirs(const fs::path& p);
void rm_rf(const fs::path& p);
bool remove_file(const fs::path& p);
uintmax_t fsize(const fs::path& p);
double mtime(const fs::path& p);
std::vector<fs::path> glob(const fs::path& dir, const std::string& pattern, bool recursive = false);
bool fnmatch(const std::string& pattern, const std::string& name);
bool is_relative_to(const fs::path& p, const fs::path& base);
fs::path resolve(const fs::path& p);
std::string stem(const fs::path& p);
std::string suffix_lower(const fs::path& p);
double disk_free_gb(const fs::path& p);

}  // namespace nx
