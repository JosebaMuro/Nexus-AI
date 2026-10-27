// Agente de bolsa autonomo (nexus/office/bolsa_agente.py): gestiona su propia cartera SIMULADA con los datos reales
// que recopila. Nunca usa dinero ni envia ordenes reales: opera en la mesa simulada igual que una persona, con sus
// comisiones, horquilla, deslizamiento y horarios. Lo real son los precios cuando hay conexion.
//
// En cada ciclo (por defecto cada 60 s) recopila series, calcula medias de 20 y 50 sesiones, RSI de 14, momento
// ajustado por volatilidad y ATR; puntua cada valor con pesos que aprende de sus operaciones cerradas; gestiona lo que
// tiene (stop vivo en el libro, trailing, objetivo, salida si se rompe la tendencia) y abre posiciones nuevas
// arriesgando como maximo riesgo_pct del patrimonio por operacion. Todo queda en su diario y en la curva de patrimonio.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "office/bolsa.h"

namespace nx::bolsa {

extern const char* NOMBRE_CARTERA;
extern const char* ORIGEN_AGENTE;   // "agente-autonomo"

// indicadores (std::nullopt cuando no hay datos suficientes, como el None de Python)
std::optional<double> sma(const std::vector<double>& x, size_t n);
std::optional<double> rsi(const std::vector<double>& x, size_t n = 14);
std::optional<double> atr(const json& pts, size_t n = 14);
std::optional<double> volatilidad(const std::vector<double>& x, size_t n = 20);
json analizar(const json& pts);   // indicadores y factores (-1..1) de una serie diaria; null si es corta
double puntuar(const json& an, const json& pesos);

class AgenteBolsa {
 public:
  using Ajustes = std::function<json()>;
  explicit AgenteBolsa(Bolsa& mesa, Ajustes ajustes = {});
  ~AgenteBolsa();

  json cfg();
  long long cartera();
  json pesos();
  std::vector<std::string> universo();
  void diario(const std::string& tipo, const std::string& simbolo, const std::string& texto, const json& datos = {});
  json recopilar(const std::string& sym);
  json ciclo();
  json estado();

  bool vivo() const { return running_.load(); }
  void start();
  void stop();

  std::vector<std::function<void(const json&)>> listeners;
  double ultimo_ciclo = 0;
  std::string ultimo_error;

 private:
  Bolsa& b;
  Ajustes ajustes_;
  std::recursive_mutex mu_;
  std::thread th_;
  std::atomic<bool> running_{false}, stop_{false};
  std::mutex wmu_;
  std::condition_variable cv_;

  json get(const std::string& k, const json& def = nullptr);
  void put(const std::string& k, const json& v);
  json historia_simulada(const std::string& sym, int n = 90);
  void sincronizar(long long cid);
  void borrar(const std::string& sym);
  void aprender(const json& p, double ganancia);
  std::optional<std::string> vender(long long cid, const std::string& sym, double qty, const std::string& motivo,
                                    const json& p);
  std::vector<std::string> gestionar(long long cid, const json& analisis);
  void poner_stop(long long cid, const std::string& sym, double qty, double stop, const json& p);
  std::vector<std::string> abrir(long long cid, const json& analisis, const json& c);
  void run();
  bool wait(double secs);   // true si hay que parar
};

AgenteBolsa& agente();    // el de la mesa actual (ajustes en config bolsa.agente)
void reset_agentes();

}  // namespace nx::bolsa
