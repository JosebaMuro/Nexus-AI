// Version beta: herramientas para medir el rendimiento y evaluar el aspecto de la interfaz.
//  - tiempos de cada ruta de la API (p50, p95, maximo, errores) y cabecera Server-Timing
//  - banco de pruebas reproducible: CPU (1 hilo y todos), memoria, disco y LLM (TTFT y tokens/s)
//  - opiniones de los probadores sobre el aspecto (nota por zona, tema, disposicion y metricas del navegador)
// Todo se guarda en data/beta/ para comparar ejecuciones y exportarlo.
#pragma once
#include <string>

#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::server {
class App;
}

namespace nx::beta {

constexpr const char* CANAL = "beta";

// una peticion atendida: metodo + patron de la ruta ("/api/x/{id}"), duracion en ms y codigo HTTP
void record(const std::string& method, const std::string& pattern, double ms, int status);
json http_stats();   // {"rutas": [...], "total": {...}, "desde": ts}
void reset_http();

struct BenchOpts {
  int rondas = 3;            // peticiones al LLM (1..10)
  int max_tokens = 128;      // tokens por respuesta (16..512)
  bool llm = true;           // false: solo CPU, memoria y disco
  bool sistema = true;       // false: solo LLM
  json cliente = nullptr;    // metricas del navegador que envia la interfaz (se guardan con el resultado)
};
BenchOpts bench_opts(const json& body);
json run_bench(Services& s, const BenchOpts& o);   // mide, guarda en data/beta/bench.jsonl y lo devuelve
json bench_history(size_t n = 50);

json save_opinion(const json& body);   // valida y guarda en data/beta/opiniones.jsonl
json opinions(size_t n = 500);

json info(Services& s);   // version, canal, compilacion y equipo
std::string report_markdown(Services& s);   // informe para adjuntar a una incidencia

void register_routes(server::App& app);

}  // namespace nx::beta
