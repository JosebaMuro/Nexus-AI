// Tablas de datos de Nexus-Office (catalogo de modelos recomendados, modelos de dictado, manifiestos de las
// herramientas...): se editan en recursos/datos/<nombre>.json y esta orden las mete en src/gen/data.cpp, que va en el
// repositorio (no hace falta para compilar).
//
//   nexus-construir datos              recursos/datos/*.json -> src/gen/data.cpp
//   nexus-construir datos --extraer    al reves: vuelca a recursos/datos/ las tablas que lleva este ejecutable
#include "comun.h"
#include "gen/data.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {
const std::vector<std::string> NOMBRES = {"recommended", "legacy_llm", "stt_models", "tools"};
}

int datos(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {"extraer"});
  fs::path dir = raiz() / "recursos" / "datos";
  if (a.tiene("extraer")) {
    for (auto& n : NOMBRES) {
      json j = json::parse(std::string(data::raw(n)));
      escribir(dir / (n + ".json"), jdump(j, 2) + "\n");
      log("datos", "-> recursos/datos/" + n + ".json");
    }
    return 0;
  }
  std::vector<std::string> lines = {"// GENERADO por «nexus-construir datos» a partir de recursos/datos/*.json: no editar a mano.",
                                    "#include \"gen/data.h\"", "", "#include <map>", "", "namespace nx::data {", ""};
  std::vector<std::tuple<std::string, std::string, size_t>> names;
  std::vector<fs::path> files = glob(dir, "*.json");
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    std::string name = stem(f);
    json j = json::parse(read_text(f));   // lanza si no es JSON valido
    std::string raw = jdump(j);
    std::string ident = "D_" + upper(name);
    names.emplace_back(name, ident, raw.size());
    lines.push_back("static const unsigned char " + ident + "[] = {");
    for (size_t i = 0; i < raw.size(); i += 24) {
      std::string row = "  ";
      for (size_t k = i; k < std::min(raw.size(), i + 24); ++k) row += std::to_string((unsigned char)raw[k]) + ",";
      lines.push_back(row);
    }
    lines.push_back("  0};");
    lines.push_back("");
  }
  lines.push_back("std::string_view raw(const std::string& name) {");
  lines.push_back("  static const std::map<std::string, std::string_view> M = {");
  for (auto& [name, ident, n] : names)
    lines.push_back(cat("      {\"", name, "\", std::string_view((const char*)", ident, ", ", n, ")},"));
  lines.push_back("  };");
  lines.push_back("  auto it = M.find(name);");
  lines.push_back("  return it == M.end() ? std::string_view(\"null\") : it->second;");
  lines.push_back("}");
  lines.push_back("");
  lines.push_back("}  // namespace nx::data");
  escribir(raiz() / "src" / "gen" / "data.cpp", join(lines, "\n") + "\n");
  std::vector<std::string> info;
  for (auto& [name, ident, n] : names) info.push_back(cat(name, " (", n, " B)"));
  log("datos", join(info, ", "));
  return 0;
}

}  // namespace nc
