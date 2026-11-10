// Banco de evaluacion de los agentes: tareas doradas con el agente, la herramienta y la respuesta esperados.
//
//  - modo "rapido": sin modelo, comprueba el enrutado y las intenciones directas (corre en las pruebas y en CI);
//  - modo "completo": cada tarea pasa por el agente con el modelo activo y se comprueba que llama a la herramienta
//    esperada con los argumentos esperados y que la respuesta contiene lo que debe; opcionalmente, un juez LLM.
// Orden: nexus-office evaluar [--completo] [--juez] [--tareas fichero.json] [--json]
#pragma once
#include <string>

#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::agents {

const json& tareas_doradas();   // las incluidas en el programa
json evaluar(Services& s, const json& tareas, bool completo, bool juez = false);

}  // namespace nx::agents
