// YAML <-> JSON (yaml.safe_load / yaml.safe_dump(allow_unicode=True, sort_keys=False)).
#pragma once
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx::yaml {

// Lanza std::runtime_error si el YAML no es valido.
json load(const std::string& text);
// Igual que load, pero devuelve null si falla o el fichero no existe.
json load_file(const fs::path& p);
std::string dump(const json& j);

}  // namespace nx::yaml
