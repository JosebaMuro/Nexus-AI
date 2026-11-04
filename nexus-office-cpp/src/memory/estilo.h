// Estilo de cada usuario final: aprende de sus interacciones el tono y la forma de trabajar que prefiere.
//
// Cada usuario (el titular "" o un dispositivo emparejado / perfil) tiene un perfil con varias dimensiones entre -1 y 1
// (longitud, formalidad, formato, nivel tecnico, emojis y tono), cada una con su confianza. Se ajusta con:
//  - correcciones explicitas en sus mensajes ("mas corto", "de usted", "en una tabla", "sin emojis"...),
//  - senales implicitas pequenas (como escribe el propio usuario, si reformula la pregunta),
//  - las valoraciones de las respuestas (longitud y formato de lo que gusto o no),
//  - y sus rutinas: secuencias de herramientas que repite (forma de trabajar).
// El resultado entra en el prompt de sistema de todos los agentes como "Estilo de este usuario".
// Solo aprende de lo que escribe el usuario, nunca de resultados de herramientas (defensa ante inyeccion).
#pragma once
#include <string>
#include <vector>

#include "core/db.h"
#include "util/json.h"

namespace nx::mem {

struct StyleSignal {
  std::string kind;   // explicita | implicita | reformula | valoracion
  std::string dim;    // "" para reformula
  double delta = 0;
  std::string label;
};

class StyleLearner {
 public:
  explicit StyleLearner(DB& db) : db_(db) {}
  static const std::vector<std::string>& dims();
  // senales de un mensaje del usuario (sin aplicarlas); prev = su mensaje anterior en la conversacion
  static std::vector<StyleSignal> detect(const std::string& text, const std::string& prev = "");
  // detecta y aplica; devuelve las senales aplicadas (para la interfaz y las metricas)
  json observe(const std::string& user, const std::string& session_id, const std::string& agent,
               const std::string& text, const std::string& prev = "");
  void on_feedback(const std::string& user, const std::string& answer, int score);
  void record_tools(const std::string& user, const std::vector<std::string>& tools, const std::string& text);
  // instrucciones para el prompt de sistema ("" si aun no sabe nada de este usuario)
  std::string prompt(const std::string& user);
  json profile(const std::string& user);
  json routines(const std::string& user, int min_count = 3);
  void set(const std::string& user, const std::string& dim, double value, bool locked);
  void reset(const std::string& user, const std::string& dim = "");
  void apply(const std::string& user, const std::string& session_id, const std::string& agent, const StyleSignal& s,
             const std::string& text);

 private:
  DB& db_;
};

}  // namespace nx::mem
