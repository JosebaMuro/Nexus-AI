// Asistente de configuracion de agentes: crea o ajusta un agente respondiendo unas preguntas sencillas
// (para que sirve, como habla, que puede usar y con quien colabora) en vez de escribir el YAML a mano.
//  - catalogo(): plantillas por gremio de pyme, grupos de herramientas en lenguaje llano, tonos y niveles
//  - borrador(): convierte las respuestas en una ficha de agente lista para guardar, con avisos (internet,
//    aprobaciones, palabras clave que ya usa otro agente) y una prueba de enrutado con frases de ejemplo
// Todo es local y determinista: no hace falta modelo de lenguaje ni conexion.
#pragma once
#include <string>
#include <vector>

#include "agents/spec.h"
#include "util/json.h"

namespace nx::agents::asistente {

json catalogo();

// herramientas: [{name, risk}] de las disponibles en esta instalacion
// politica: tools.policy de la configuracion ({"low":"auto","medium":"ask","high":"admin"})
json grupos_disponibles(const json& herramientas, const json& politica);

// respuestas: {plantilla, base, id, nombre, descripcion, mision, tareas[], reglas[], tono, idioma, creatividad,
//              longitud, grupos[], delegar[], palabras[], especialidad, icono, color, sexo, memoria, frases_prueba[]}
// existentes: los agentes ya creados (para no repetir id, avisar de palabras clave y probar el enrutado)
// Devuelve {spec, avisos[], enrutado[]}. Lanza std::invalid_argument si falta el nombre.
json borrador(const json& respuestas, const std::vector<AgentSpec>& existentes, const json& herramientas,
              const json& politica);

std::string id_desde_nombre(const std::string& nombre, const std::vector<std::string>& ocupados);

}  // namespace nx::agents::asistente
