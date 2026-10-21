// Herramientas (nexus/tools): registro modular (integradas y de la comunidad), supervision con politica por riesgo,
// aprobaciones humanas, limites de tiempo, auditoria y propuestas de herramientas hechas por los agentes.
#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/db.h"
#include "memory/memory.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::tools {

extern const std::vector<std::string> RISKS;
extern const std::vector<std::string> PERMISSIONS;

using Emit = std::function<void(const json&)>;

struct ToolContext {
  Services* services = nullptr;
  std::string agent;
  std::string session_id;
  Emit emit;
  bool remote = false;   // peticion originada por otro nodo cooperativo
  int depth = 0;         // profundidad de delegacion entre agentes
};

// error de una herramienta: el supervisor lo devuelve como {"error": "Tipo: mensaje"}
struct ToolError : std::runtime_error {
  std::string type;
  ToolError(const std::string& t, const std::string& m) : std::runtime_error(m), type(t) {}
};

using ToolFn = std::function<json(const json& args, ToolContext& ctx)>;

struct ToolSpec {
  std::string name, description;
  json parameters = {{"type", "object"}, {"properties", json::object()}};
  std::string risk = "low";
  std::vector<std::string> permissions;
  std::string version = "1.0", author = "nexus", source = "builtin", path;
  ToolFn func;
  bool enabled = true;
  std::optional<double> timeout;
  json public_json() const;
  mem::ToolView view() const;
};

std::pair<json, std::vector<std::string>> validate_args(const json& schema, const json& args);

// herramientas integradas: nombre -> funcion (cada fichero builtin_*.cpp aporta las suyas)
std::map<std::string, ToolFn>& builtin_functions();
struct Register {
  Register(const std::string& name, ToolFn fn) { builtin_functions()[name] = std::move(fn); }
};

// ruta dentro de data/sandbox (ToolError si se sale)
fs::path sandbox_safe(const std::string& rel);

// Python del sistema para las herramientas de la comunidad y python_exec ("" si no hay)
std::vector<std::string> python_cmd();
// ayudante Python (manifiesto, analisis, ejecucion aislada); lanza ToolError si no hay Python
json python_helper(const json& req, double timeout, const fs::path& cwd);

class ToolRegistry {
 public:
  explicit ToolRegistry(bool sandbox_community = true);
  void reload();
  void set_enabled(const std::string& name, bool enabled);
  std::shared_ptr<ToolSpec> get(const std::string& name);
  std::vector<std::shared_ptr<ToolSpec>> for_agent(const std::vector<std::string>& allowed);
  std::vector<std::shared_ptr<ToolSpec>> all();
  json list();
  std::vector<std::string> missing;   // manifiestos de la version Python sin funcion portada

 private:
  bool sandbox_community_;
  std::recursive_mutex mu_;
  std::vector<std::shared_ptr<ToolSpec>> order_;
  std::map<std::string, std::shared_ptr<ToolSpec>> tools_;
  std::set<std::string> disabled_;
  fs::path disabled_file() const;
  std::shared_ptr<ToolSpec> spec(const json& m, const std::string& source, const std::string& path, ToolFn func);
};

json read_manifest(const fs::path& path);   // TOOL = {...} sin ejecutar el codigo (null si no hay)

class Supervisor {
 public:
  Supervisor(ToolRegistry& registry, DB& db, Config& config);
  std::string decision_for(const std::string& risk, bool remote);
  bool resolve(const std::string& pid, bool approve, bool is_admin);
  json list_pending();
  json execute(std::string name, json args, ToolContext& ctx);
  json run_sandboxed(const fs::path& path, const json& args, double timeout);
  json audit_log(int limit = 100);
  std::vector<std::function<void(const json&)>> listeners;
  mem::SemanticAssist* assist = nullptr;
  ToolRegistry& registry;

 private:
  struct Pending {
    json item;
    std::optional<bool> approved;
    std::string approver;
    std::condition_variable cv;
  };
  DB& db_;
  Config& config_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<Pending>> pending_;
  std::pair<bool, std::string> await_approval(const json& call, ToolContext& ctx, const std::string& level,
                                              double timeout = 180);
  void audit(ToolContext& ctx, const json& call, const std::string& decision, const std::string& approver,
             const std::string& status, const json& result, double dur);
};

json analyze(const std::string& code, const std::string& name = "");

class ProposalManager {
 public:
  ProposalManager(ToolRegistry& registry, Supervisor& supervisor, mem::Learner* learner = nullptr);
  json create(std::string name, const std::string& description, std::string code, const json& parameters,
              const std::string& risk = "medium", const std::string& rationale = "", const std::string& author = "");
  json get(const std::string& pid);
  json list(const std::string& status = "");
  json update_code(const std::string& pid, const std::string& code);
  json test(const std::string& pid, const json& args);
  json approve(const std::string& pid, const std::string& reviewer = "admin", std::string note = "");
  json reject(const std::string& pid, const std::string& reason, const std::string& reviewer = "admin");
  bool uninstall(const std::string& name);

 private:
  ToolRegistry& registry_;
  Supervisor& supervisor_;
  mem::Learner* learner_;
  fs::path root_;
  json save(const json& prop);
};

}  // namespace nx::tools
