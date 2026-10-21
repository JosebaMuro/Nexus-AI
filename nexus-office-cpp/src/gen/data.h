// Tablas de datos compartidas con la version Python (generadas por tools/gen_data.py).
#pragma once
#include <string>
#include <string_view>

#include "util/json.h"

namespace nx::data {

std::string_view raw(const std::string& name);
inline json get(const std::string& name) { return json::parse(raw(name)); }

}  // namespace nx::data
