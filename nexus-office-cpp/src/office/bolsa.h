// Mesa de bolsa simulada de la oficina de bolsa e inversiones (nexus/office/bolsa.py): carteras ficticias, mercados
// con su horario y festivos, ordenes a mercado/limitadas/stop, comisiones, impuestos, lotes FIFO y modo real que solo
// confirma una persona. 100 % local: data/bolsa/bolsa.db, la misma base que la edicion Python.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/db.h"
#include "util/json.h"

namespace nx::bolsa {

extern const char* AVISO;
extern const char* CONFIRMACION;
extern const std::vector<std::string> ORDER_TYPES;

// PermissionError de Python (la API responde 403)
struct PermissionDenied : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// fechas en dias desde 1970-01-01 (UTC) y horas en segundos UNIX
long easter(int y);                               // domingo de Pascua
std::vector<long> holidays(const std::string& kind, int y);
double utc_offset(const std::string& zona, double t);   // segundos
bool is_open(const std::string& market, double t);
std::string fmt_local(const std::string& market, double t);
std::string iso_utc(double t);                    // 2026-10-21T20:08:22+00:00
std::string ymd(long day);                        // 2026-10-21
json tarifas_json();                              // {id: nombre}

class Bolsa {
 public:
  explicit Bolsa(const fs::path& path = {}, std::optional<uint64_t> seed = {}, std::function<double()> clock = {});

  double now() const { return clock_(); }
  json mercados();
  json instrumentos(const std::string& texto = "");
  json instrumento(const std::string& simbolo);
  json anadir_instrumento(const std::string& simbolo, const std::string& nombre, const std::string& mercado,
                          const json& precio, const std::string& divisa = "", const std::string& tipo = "acción",
                          const json& vol = 0.25, const json& spread_bps = 5, const json& adv = 1e6,
                          const json& impuesto_pct = 0.0);
  json precio(const std::string& simbolo);
  json ticks(const std::string& simbolo, int n = 200);
  double fx(const std::string& divisa);
  json importar_csv(const std::string& simbolo, const std::string& texto);

  json carteras();
  json crear_cartera(const std::string& nombre = "Cartera simulada", const json& capital = 10000.0,
                     const std::string& tarifa = "broker-online", const std::string& modo = "simulado");
  json cartera_info(const json& cid = nullptr);
  json reiniciar(const json& cid = nullptr, const json& capital = nullptr);
  json posiciones(const json& cid = nullptr);
  json resumen(const json& cid = nullptr);
  json costes(const json& cid, const std::string& simbolo, const std::string& lado, double cantidad, double precio);

  json ordenar(const std::string& simbolo, std::string lado, const json& cantidad, std::string tipo = "mercado",
               const json& limite = nullptr, const json& stop = nullptr, const std::string& validez = "dia",
               const json& cartera = nullptr, const std::string& origen = "");
  json orden(long long oid);
  json ordenes(const json& cartera = nullptr, bool abiertas = false);
  json cancelar(long long oid);
  std::vector<long long> procesar(long long only = 0);
  json operaciones(const json& cartera = nullptr, int limite = 100);
  json informe_fiscal(const json& cartera = nullptr, const json& anio = nullptr);

  json confirmar(long long oid, const std::string& confirmacion);
  json registrar_ejecucion(long long oid, const json& precio, const json& comision_eur = 0.0,
                           const json& impuestos_eur = nullptr, const json& fx = nullptr);

 private:
  DB db_;
  std::mt19937_64 rng_;
  std::function<double()> clock_;
  std::recursive_mutex mu_;
  double gauss(double mu, double sigma);
  double step(const json& inst, double price, double t0, double t1);
  double held(long long cid, const std::string& sym);
  double reserved(long long cid, const std::string& sym);
  bool process_one(json o);
  void set(long long oid, const json& kw);
  bool execute(const json& o, double px, double slip_eur, const json& gastos = nullptr);
  std::pair<double, double> fill_price(const json& inst, const json& quote, const std::string& lado, double cantidad);
};

double num(const json& x);   // _num: acepta "1.234,56 €"

Bolsa& get();             // mesa de la carpeta de datos actual
void reset_instances();

}  // namespace nx::bolsa
