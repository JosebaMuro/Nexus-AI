// Importar y exportar grafos de conocimiento (memory/kg_formats.py y kg_io.py): mas de 50 formatos detectados por el
// contenido, normalizados a {"format", "nodes": [{id, name, type, category, note, scope?, weight?}],
// "edges": [{src, dst, rel, weight}]}, fusion con el grafo general o el de cada agente y exportacion.
#pragma once
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "util/json.h"

namespace nx::mem {
class KnowledgeGraph;
class Organizer;
}  // namespace nx::mem

namespace nx::kg {

struct FormatError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

constexpr size_t MAX_NODES = 200000;
constexpr size_t MAX_BYTES = 512u * 1024 * 1024;

using Files = std::vector<std::pair<std::string, std::string>>;   // (nombre, bytes)

json parse_bytes(std::string data, std::string name = "");
json parse_many(const Files& files);
json parse_json(json data);
json parse_wiki_notes(const Files& notes);   // null si no hay [[enlaces]]
json merge(const std::vector<std::pair<std::string, json>>& parts);
std::string detect(const std::string& text, const std::string& ext = "");

json formats_json();                      // GET /api/kg/formats
extern const std::set<std::string> GRAPH_EXT;
bool looks_like_graph(const std::string& name, const std::string& data);

using Progress = std::function<void(size_t done, size_t total)>;
json insert_graph(mem::KnowledgeGraph& graph, json parsed, const std::string& target = "",
                  mem::Organizer* organizer = nullptr, const std::vector<std::string>& known_agents = {},
                  const std::string& source = "", const Progress& progress = {});
// (contenido, tipo MIME, extension); scope vacio = todo (general y agentes)
std::tuple<std::string, std::string, std::string> export_graph(mem::KnowledgeGraph& graph,
                                                               const std::optional<std::string>& scope,
                                                               const std::string& fmt = "json");

}  // namespace nx::kg
