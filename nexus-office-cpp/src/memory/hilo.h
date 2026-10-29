// Hilo de las conversaciones largas: que el agente no pierda el hilo del proyecto aunque la charla no quepa en el
// contexto del modelo local. Cada conversacion guarda:
//   - un RESUMEN PROGRESIVO de lo ya hablado (se pliega por tandas, con el LLM o, sin el, de forma extractiva),
//   - NOTAS FIJAS del proyecto que el usuario escribe y siempre entran en el prompt,
// y en cada turno se arma el contexto con un presupuesto en tokens calculado a partir de n_ctx:
//   notas + resumen + fragmentos antiguos relevantes para la pregunta + los ultimos mensajes literales.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/db.h"
#include "memory/memory.h"
#include "util/json.h"

namespace nx::mem {

// lo que entra en el prompt en este turno
struct HiloPlan {
  std::string notas, resumen;
  std::vector<std::string> recuerdos;   // trozos de mensajes antiguos que vienen al caso
  json recientes = json::array();       // [{role, content}] en orden
  long long plegados = 0;               // mensajes ya resumidos
  long long omitidos = 0;               // mensajes que no entran literales (resumidos o recuperados)
  int tokens = 0;                       // estimacion de lo usado
  std::string texto() const;            // bloque para el mensaje de sistema
};

class Hilo {
 public:
  using Completar = std::function<std::string(const std::string& prompt, int max_tokens)>;
  // busy: el LLM esta respondiendo (el plegado espera a que quede libre)
  Hilo(DB& db, std::function<bool()> busy = nullptr);
  ~Hilo();

  static int tokens(const std::string& s);
  // recorta por el medio (cabeza y cola) para que quepa en max_tokens
  static std::string recortar(const std::string& s, int max_tokens);

  // contexto de la conversacion con presupuesto en tokens (sin contar sistema, pregunta ni respuesta)
  HiloPlan plan(const std::string& sid, const std::string& query, int budget);
  // pliega en el resumen los mensajes mas antiguos cuando lo literal pasa de 'recent_budget' tokens.
  // resumen_tokens: tamano maximo del resumen. Devuelve los mensajes plegados en esta llamada.
  int plegar(const std::string& sid, int recent_budget, int resumen_tokens, const Completar& llm = nullptr);
  // igual pero en segundo plano (no retrasa la respuesta)
  void encolar(const std::string& sid, int recent_budget, int resumen_tokens);

  json estado(const std::string& sid);
  void guardar(const std::string& sid, const json& cambios);   // {"notas"?, "resumen"?}
  void olvidar(const std::string& sid);                        // borra resumen y notas

  Completar llm_complete;   // resumen con el LLM; vacio = extractivo
  std::function<bool()> llm_real = [] { return true; };

 private:
  DB& db_;
  std::function<bool()> busy_;
  HashingEmbedder fast_{512};
  std::mutex mu_;   // un plegado a la vez
  std::thread th_;
  std::mutex qmu_;
  std::condition_variable cv_;
  struct Tarea {
    std::string sid;
    int recent, resumen;
  };
  std::deque<Tarea> cola_;
  bool stop_ = false;
  void loop();
  Row fila(const std::string& sid);
  std::string resumir(const std::string& previo, const std::string& nuevos, int resumen_tokens, const Completar& llm);
};

std::string resumen_extractivo(const std::string& previo, const json& mensajes, int resumen_tokens);

}  // namespace nx::mem
