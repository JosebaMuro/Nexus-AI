// Cotizaciones reales para la mesa de bolsa (nexus/office/bolsa_datos.py). La mesa sigue siendo SIEMPRE una
// simulacion: nunca hay dinero ni ordenes reales; lo real son los precios.
//
// Fuentes, por orden, sin clave ni registro: Yahoo Finance (/v8/finance/chart), Google Finance (data-last-price),
// Stooq (CSV), Coinbase y Kraken para cripto y el BCE para el tipo de cambio. Sin internet quote() devuelve null y la
// mesa sigue con su simulacion anclada al ultimo precio real. Cada fuente que falla se aparta un rato (backoff).
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "util/http.h"
#include "util/json.h"

namespace nx::bolsa {

// Lo que la mesa necesita de un proveedor de precios (en las pruebas se sustituye por uno determinista).
class Proveedor {
 public:
  virtual ~Proveedor() = default;
  // {precio, cierre_anterior, ts, divisa, fuente, retraso, puntos, simbolo} o null
  virtual json quote(const std::string& sym, double max_age = -1) = 0;
  virtual json serie(const std::string& sym, const std::string& rango = "1d") = 0;   // [{ts, precio, ...}]
  virtual double fx(const std::string& divisa) = 0;                                    // 0: sin dato
  virtual json estado() = 0;
  virtual void set_mercado(const std::string& sym, const std::string& mercado) {}
  virtual bool tiene_mercado(const std::string& sym) { return true; }
};

struct Fuente {
  std::string nombre;
  int fallos = 0;
  double hasta = 0;
  bool disponible() const;
  void ok() { fallos = 0, hasta = 0; }
  void fallo(const std::string& err);   // se aparta 30 s, 60 s, 120 s... hasta 15 min
};

class DatosMercado : public Proveedor {
 public:
  using Fetch = std::function<http::Response(const http::Request&)>;
  explicit DatosMercado(Fetch fetch = {}, double ttl = 20.0, double timeout = 6.0);

  json quote(const std::string& sym, double max_age = -1) override;
  json quotes(const std::vector<std::string>& syms, double max_age = -1);
  json serie(const std::string& sym, const std::string& rango = "1d") override;
  double fx(const std::string& divisa) override;
  json estado() override;
  void set_mercado(const std::string& sym, const std::string& mercado) override;
  bool tiene_mercado(const std::string& sym) override;

  double ttl;
  std::map<std::string, Fuente> fuentes;
  bool disponible(const std::string& fuente);

 private:
  Fetch fetch_;
  double timeout_;
  std::mutex mu_;
  std::map<std::string, std::pair<double, json>> quotes_;
  std::map<std::string, std::pair<double, json>> series_;
  std::map<std::string, std::pair<double, double>> fx_;
  std::map<std::string, std::string> mercado_de_;
  std::map<std::string, int> ultimo_http_;
  std::string ultima_fuente_;
  double ultimo_ok_ = 0;

  http::Response get(const std::string& fuente, const std::string& url,
                     const std::map<std::string, std::string>& params = {}, const http::Headers& extra = {});
  json yahoo_chart(const std::string& sym, const std::string& rango, const std::string& intervalo);
  json quote_yahoo(const std::string& sym);
  json quote_stooq(const std::string& sym);
  json quote_google(const std::string& sym);
  json quote_crypto(const std::string& sym);
  std::string google_symbol(const std::string& sym);
};

json yahoo_points(const json& res);   // velas de una respuesta de /v8/finance/chart

Proveedor& proveedor();   // el compartido del proceso (DatosMercado)

}  // namespace nx::bolsa
