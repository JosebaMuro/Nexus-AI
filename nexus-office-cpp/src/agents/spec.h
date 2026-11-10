// Ficha de un agente (agents/manager.py: AgentSpec). Mismo YAML que la version Python.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::agents {

struct AgentSpec {
  std::string id;
  std::string name;
  std::string description;
  std::string system_prompt = "Eres un asistente util.";
  std::string model = "default";
  std::optional<double> temperature;
  std::optional<int> max_tokens;
  std::vector<std::string> tools = {"*"};
  bool memory = true;
  std::vector<std::string> can_delegate_to;
  std::vector<std::string> keywords;
  std::string specialty;
  std::string color = "#00f0ff";
  std::string icon = "◆";
  bool enabled = true;
  std::string sexo;   // aspecto en el salon y los despachos: mujer | hombre | "" (al azar segun su id)
  // respuestas del asistente de configuracion (agents/asistente.h): para volver a abrirlo igual
  json perfil = json::object();
  // voz propia para reconocer quien habla: {motor, modelo, hablante, nombre, velocidad, tono, volumen} (vacia = la general)
  json voz = json::object();

  static AgentSpec from_json(const json& d);   // ignora claves desconocidas; lanza si falta id o name
  json to_json() const;                         // dataclasses.asdict (mismo orden de claves)
  bool has_tool(const std::string& t) const;
};

}  // namespace nx::agents
