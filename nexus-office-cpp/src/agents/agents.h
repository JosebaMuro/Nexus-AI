// Agentes (nexus/agents): gestor de agentes configurables en YAML, ejecucion con herramientas y delegacion,
// y salon virtual (habilidades, reuniones y propuestas de nuevos agentes).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agents/spec.h"
#include "tools/tools.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::agents {

fs::path defaults_dir();   // nexus/agents/defaults
std::vector<std::string> store_images(const std::vector<std::string>& images);
std::string yaml_dump_spec(const AgentSpec& spec);   // yaml.safe_dump(asdict(spec), sort_keys=False)

class AgentManager {
 public:
  explicit AgentManager(Services& s);
  void reload();
  AgentSpec save(json data);
  void remove(const std::string& aid);
  std::optional<AgentSpec> get(const std::string& aid);
  std::vector<std::string> ids();
  std::vector<AgentSpec> list();        // activos
  std::vector<AgentSpec> all();         // tambien los desactivados
  static double keyword_score(const AgentSpec& agent, const std::string& text);
  std::string route(const std::string& text, bool has_images = false);
  std::string build_system(const AgentSpec& spec, const json& info, const std::vector<std::shared_ptr<tools::ToolSpec>>& tools,
                           bool text_tools = true);
  // live: modo en vivo por voz (respuesta corta y hablada, sin razonamiento largo)
  json run(std::string agent_id, const std::string& text, const std::string& session_id,
           const std::vector<std::string>& images = {}, tools::Emit emit = nullptr, bool remote = false, int depth = 0,
           bool save = true, bool live = false);
  std::string run_subtask(const std::string& target, const std::string& task, tools::ToolContext& ctx);
  // reparto del contexto del modelo en un turno (tokens)
  struct Presupuesto {
    int n_ctx = 0, reply = 0;
    int system_max = 1 << 20;   // tope del prompt de sistema base
    int hilo = 0;               // para la conversacion: notas, resumen, fragmentos y mensajes recientes
  };
  Presupuesto presupuesto(const AgentSpec& spec, const json& info, const std::string& text,
                          const std::vector<std::shared_ptr<tools::ToolSpec>>& tools, bool native, bool live);
  fs::path dir;

 private:
  Services& s_;
  std::recursive_mutex mu_;
  std::map<std::string, AgentSpec> agents_;
  void seed();
};

class Lounge {
 public:
  explicit Lounge(Services& s);
  json post(const std::string& agent, const std::string& content, const std::string& kind = "chat",
            const json& meta = json::object());
  json history(int limit = 120);
  static std::vector<std::string> topics(const std::string& text, size_t n = 4);
  void bump(const std::string& agent, const std::vector<std::string>& topics, double ok, double fail);
  void record_task(const std::string& agent, const std::string& text, const json& tool_log,
                   const std::string& routed_by = "auto");
  void on_feedback(const std::string& agent, const std::string& question, int score);
  json skills();
  double skill_score(const std::string& agent, const std::string& text);
  std::string directory(const std::string& exclude = "");
  json detect_gaps(int window = 60);
  json propose_agent(const json& spec, const std::string& reason, const std::string& proposer = "nexus");
  json proposals();
  json resolve_proposal(long long pid, bool approve, const std::string& note = "");
  bool busy() const { return meeting_; }
  json meet(bool quick = false, const std::string& topic = "");
  void user_says(const std::string& text);
  std::vector<std::function<void(const json&)>> listeners;
  std::atomic<int> tasks_since_meeting{0};

 private:
  Services& s_;
  std::atomic<bool> meeting_{false};
  json spec_for_gap(const json& gap, const std::vector<std::string>& related);
  std::string say(const AgentSpec& agent, const std::string& prompt_ctx, const std::string& fallback);
};

}  // namespace nx::agents
