// Mesa de analistas con IA (idea de TradingAgents, Apache-2.0, y ai-hedge-fund, MIT; reescrita en C++): antes de
// decidir sobre un valor trabajan por turnos
//   1. analistas: tecnico (senales y medias), noticias y sentimiento (titulares RSS) y fundamental (solo con IA, de
//      su conocimiento, marcado como posiblemente desfasado: no hay datos contables sin clave)
//   2. debate alcista frente a bajista (una o varias rondas)
//   3. operador: propone comprar, mantener o vender
//   4. equipo de riesgo: agresivo, neutral y conservador proponen el peso
//   5. gestor de cartera: decision final con confianza y peso, teniendo en cuenta las lecciones de operaciones pasadas
// Sin IA instalada cada papel lo hace una regla, asi que el informe sale siempre. Los informes y los titulares quedan
// en el banco de datos de la mesa (data/bolsa/bolsa.db). Nunca ordena nada: solo informa.
#pragma once
#include <functional>
#include <string>

#include "office/bolsa.h"
#include "util/http.h"

namespace nx::bolsa {

json leer_rss(const std::string& xml);            // [{titulo, enlace, fecha, ts, fuente}]
double sentimiento(const std::string& texto);     // -1..1 con un lexico financiero en espanol e ingles
json extraer_json(const std::string& respuesta);  // el ultimo objeto {...} valido de una respuesta de modelo

class Mesa {
 public:
  using Fetch = std::function<http::Response(const http::Request&)>;
  // llm(prompt) -> texto; vacio o sin poner = sin IA (reglas)
  using LLM = std::function<std::string(const std::string&)>;
  explicit Mesa(Bolsa& mesa, Fetch fetch = {});

  json noticias(const std::string& simbolo, bool refrescar = false);
  // opciones: rondas (debate, 1 por defecto), riesgo_pct, max_peso_pct
  json informe(const std::string& simbolo, const json& opciones = json::object());
  json informes(const std::string& simbolo = "", int limite = 20);
  json lecciones(const std::string& simbolo, int limite = 5);   // de las operaciones cerradas del agente

  LLM llm;
  std::function<json()> ia_info;
  double ttl_noticias = 1800;

 private:
  Bolsa& b;
  Fetch fetch_;
  std::string preguntar(const std::string& prompt, json& traza, const std::string& papel);
};

Mesa& mesa();                                  // la de la mesa actual
extern Mesa::LLM mesa_llm;                     // lo pone main.cpp con el gestor de LLM
extern std::function<json()> mesa_ia_info;
void reset_mesas();

}  // namespace nx::bolsa
