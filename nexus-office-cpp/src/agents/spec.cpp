// Ficha de un agente: lectura tolerante del YAML y volcado con el mismo orden que dataclasses.asdict.
#include "agents/spec.h"

#include <algorithm>
#include <stdexcept>

namespace nx::agents {

static std::string as_str(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "None";
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  return v.dump();
}

static std::vector<std::string> as_list(const json& v) {
  std::vector<std::string> out;
  if (v.is_array()) {
    for (auto& x : v) out.push_back(as_str(x));
  } else if (v.is_string()) {   // list("abc") en Python daria letras; un YAML con texto suelto se toma como un elemento
    out.push_back(v.get<std::string>());
  }
  return out;
}

AgentSpec AgentSpec::from_json(const json& d) {
  if (!d.is_object() || !d.contains("id") || !d.contains("name"))
    throw std::invalid_argument("AgentSpec necesita 'id' y 'name'");
  AgentSpec a;
  a.id = as_str(d["id"]);
  a.name = as_str(d["name"]);
  auto s = [&](const char* k, std::string& dst) {
    if (d.contains(k) && !d[k].is_null()) dst = as_str(d[k]);
  };
  s("description", a.description);
  s("system_prompt", a.system_prompt);
  s("model", a.model);
  s("specialty", a.specialty);
  s("color", a.color);
  s("icon", a.icon);
  if (d.contains("temperature") && d["temperature"].is_number()) a.temperature = d["temperature"].get<double>();
  if (d.contains("max_tokens") && d["max_tokens"].is_number()) a.max_tokens = (int)d["max_tokens"].get<double>();
  if (d.contains("tools") && !d["tools"].is_null()) a.tools = as_list(d["tools"]);
  if (d.contains("can_delegate_to") && !d["can_delegate_to"].is_null()) a.can_delegate_to = as_list(d["can_delegate_to"]);
  if (d.contains("keywords") && !d["keywords"].is_null()) a.keywords = as_list(d["keywords"]);
  if (d.contains("memory")) a.memory = truthy(d["memory"]);
  if (d.contains("enabled")) a.enabled = truthy(d["enabled"]);
  return a;
}

json AgentSpec::to_json() const {
  json j = json::object();
  j["id"] = id;
  j["name"] = name;
  j["description"] = description;
  j["system_prompt"] = system_prompt;
  j["model"] = model;
  j["temperature"] = temperature ? json(*temperature) : json(nullptr);
  j["max_tokens"] = max_tokens ? json(*max_tokens) : json(nullptr);
  j["tools"] = tools;
  j["memory"] = memory;
  j["can_delegate_to"] = can_delegate_to;
  j["keywords"] = keywords;
  j["specialty"] = specialty;
  j["color"] = color;
  j["icon"] = icon;
  j["enabled"] = enabled;
  return j;
}

bool AgentSpec::has_tool(const std::string& t) const {
  return std::find(tools.begin(), tools.end(), "*") != tools.end() || std::find(tools.begin(), tools.end(), t) != tools.end();
}

}  // namespace nx::agents
