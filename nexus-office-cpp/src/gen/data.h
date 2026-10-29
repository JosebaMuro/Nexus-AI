// Tablas de datos (recursos/datos/*.json, metidas en data.cpp por «nexus-construir datos»).
#pragma once
#include <string>
#include <string_view>

#include "util/json.h"

namespace nx::data {

std::string_view raw(const std::string& name);
inline json get(const std::string& name) { return json::parse(raw(name)); }

}  // namespace nx::data
