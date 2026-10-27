// Que esta haciendo cada agente ahora mismo (nexus/agents/activity.py), para los despachos animados, el reloj y el
// salon. Estados: idle, thinking, writing, tool, meeting, delegating.
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::agents {

class Activity {
 public:
  void set(const std::string& agent, const std::string& estado, const std::optional<std::string>& tarea = std::nullopt,
           const std::string& herramienta = "");
  void on_event(const std::string& agent, const json& e, const std::string& task = "");
  json get(const std::string& agent);
  json all();
  void clear();
  std::vector<std::function<void(const json&)>> listeners;

 private:
  std::mutex mu_;
  std::map<std::string, json> state_;
};

Activity& activity();   // ACTIVITY

}  // namespace nx::agents
