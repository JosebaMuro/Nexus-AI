// Asistente de configuracion de agentes: crea o ajusta un agente («empleado») en cuatro pasos sencillos.
//
//   1. Puesto: una plantilla del catalogo (74 oficios y puestos de oficina + los agentes de serie) o desde cero.
//   2. Forma de ser: nombre, aspecto, trato, extension de las respuestas, creatividad, iniciativa y reglas.
//   3. Que sabe y que puede hacer: especialidad, palabras clave, paquetes de herramientas, modelo, memoria y equipo.
//   4. Revisar: ficha y prompt generados (se pueden retocar) y guardar.
//
// Las respuestas del asistente se guardan en el campo «perfil» del agente: al volver a abrirlo se ve igual. El prompt
// de sistema se genera aqui, en C++, a partir de la plantilla y de esas respuestas.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::mem {
class KnowledgeGraph;
}

namespace nx::agents::asistente {

// paquetes de herramientas: [{id, nombre, icono, desc, tools}] (lenguaje sencillo en lugar de nombres internos)
json paquetes();
// plantillas de puesto: [{id, name, description, icon, color, grupo, perfil}], perfil = respuestas ya rellenas
json plantillas();
json plantilla(const std::string& id);   // YAML completo de una plantilla (null si no existe)
// respuestas de una plantilla (o por defecto si es null): lo que el asistente muestra rellenado de inicio
json perfil_de(const json& plantilla);
// identificador a partir del nombre ("Atención al cliente" -> "atencion-al-cliente")
std::string id_de(const std::string& nombre);
// redacta el prompt de sistema a partir de la plantilla (puede ser null) y las respuestas
std::string redactar_prompt(const json& plantilla, const json& r);

// respuestas -> {agente (listo para AgentManager::save, con "perfil"), avisos}
//   existentes: herramientas instaladas (vacio = no filtrar); agentes: ids de los agentes que ya hay
//   editando: id del agente que se esta ajustando ("" = nuevo: si el id esta ocupado se le pone sufijo)
//   palabras_otros: palabras clave de los demas agentes (aviso si se repiten: el enrutado se repartiria)
json construir(const json& r, const std::vector<std::string>& existentes, const std::vector<std::string>& agentes,
               const std::string& editando = "",
               const std::map<std::string, std::vector<std::string>>& palabras_otros = {});
// siembra el grafo de conocimiento del agente con el de su plantilla (clave "grafo"); devuelve cuantas relaciones
int sembrar(mem::KnowledgeGraph& graph, const std::string& aid, const json& plantilla);

}  // namespace nx::agents::asistente
