// Perfiles de oficina (nexus/office): Nexus-Office configurado como la oficina de una pyme con agentes de su gremio.
// Mismos ficheros que la version Python: nexus/office/perfiles, nexus/office/agentes y nexus/office/conocimiento.
#pragma once
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "core/config.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}
namespace nx::mem {
class KnowledgeGraph;
}

namespace nx::office {

extern const std::vector<std::string> OFFICE_ROLES;
extern const std::vector<std::string> COORDINATOR_TOOLS;
extern const char* COORDINATOR_PROMPT;

fs::path catalog_dir();
fs::path profiles_dir();
fs::path knowledge_dir();

json catalog();    // id -> YAML del agente (con su clave "grafo"), en orden de fichero
json profiles();   // id -> YAML del perfil
std::vector<std::tuple<std::string, std::string, std::string, std::string>> seeds(const json& agent);
bool is_trade(const std::string& aid);
json sector_models(const std::string& pid);   // {id de modelo: motivo}
json describe(const std::string& pid, const std::string& active = "");
json coordinator(const json& profile, const std::map<std::string, std::string>& agent_names);

class Office {
 public:
  explicit Office(Config& config, fs::path agents_dir = {});
  std::string active();
  json list();
  json apply(const std::string& pid, bool reset_agents = false);
  void clear();
  json seed_graphs(mem::KnowledgeGraph& graph, const std::vector<std::string>& agent_ids);

 private:
  Config& config_;
  fs::path dir_;
  void backup(const fs::path& f);
  void write(const json& data);
  void set_enabled(const fs::path& f, bool enabled);
};

json packs();   // id -> paquete.yaml + "dir" + "grafo"
std::vector<std::string> sections(const std::string& text);
int install_pack(Services& s, const json& pack, const std::string& aid);
json install_packs(Services& s, const std::vector<std::string>* agent_ids = nullptr);
json ensure(Services& s);

}  // namespace nx::office
