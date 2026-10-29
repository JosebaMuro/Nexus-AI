// CSV como el modulo csv de Python (dialecto excel): comillas dobles, "" dentro de un campo y saltos de linea en
// campos entre comillas; y TOML (tomllib.loads) para los grafos escritos en TOML.
#pragma once
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::csv {

std::vector<std::vector<std::string>> read(const std::string& text, char delimiter = ',', char quote = '"',
                                           bool skip_initial_space = false);
std::string row(const std::vector<std::string>& fields, char delimiter = ',');   // con \r\n al final

}  // namespace nx::csv

namespace nx::toml {

json load(const std::string& text);   // lanza std::runtime_error si no es TOML valido

}  // namespace nx::toml
