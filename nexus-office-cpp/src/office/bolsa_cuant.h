// Laboratorio cuantitativo de la mesa de bolsa: lo que los proyectos libres de inversion con IA hacen antes de
// arriesgar dinero, reescrito en C++ sin dependencias (ver docs/BOLSA_PROYECTOS.md):
//   - senales: conjunto de 5 estrategias tecnicas con confianza (idea del agente tecnico de ai-hedge-fund, MIT)
//   - backtest: reproduce dia a dia las reglas del agente autonomo con comisiones y deslizamiento, frente a comprar y
//     mantener, con tramo fuera de muestra (backtrader, Qlib, freqtrade)
//   - metricas: rentabilidad, Sharpe, Sortino, caida maxima, Calmar, VaR/CVaR... (QuantStats, Apache-2.0)
//   - optimizar: pesos de cartera por varianza minima, paridad de riesgo, Sharpe maximo y HRP (PyPortfolioOpt, MIT)
//   - montecarlo: remuestreo de rentabilidades para ver el abanico de resultados y la probabilidad de perder
//   - protecciones: frenos que paran las compras tras una caida, una racha de perdidas o un mal dia (freqtrade)
// Todo es calculo puro sobre series: no toca la mesa ni la red, y se prueba con datos sinteticos.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::bolsa {

struct SerieBT {
  std::string simbolo;
  std::vector<double> ts, precio, maximo, minimo;   // maximo/minimo pueden ir vacios (se usa el cierre)
  double fx = 1.0;                                  // unidades de su divisa por euro
};

// serie de la mesa ({"puntos": [{ts, precio, maximo?, minimo?}]}) a SerieBT
SerieBT serie_bt(const std::string& simbolo, const json& puntos, double fx = 1.0);

std::optional<double> ema(const std::vector<double>& x, size_t n);
std::optional<double> hurst(const std::vector<double>& x, size_t max_lag = 20);
json macd(const std::vector<double>& x);        // {macd, senal, histograma} o null
json bollinger(const std::vector<double>& x, size_t n = 20, double k = 2);   // {media, alta, baja, pct_b} o null

// conjunto de estrategias: {estrategias: {tendencia, reversion, momento, volatilidad, estadistica: {senal -1..1,
// confianza 0..1, detalle}}, combinada, confianza, lectura: alcista|neutral|bajista} o null si la serie es corta
json senales(const json& puntos);

double periodos_anio(const std::vector<double>& ts);   // 252 si es diaria, 52 si semanal...
json metricas(const std::vector<double>& valores, double periodos = 252);
json metricas_operaciones(const std::vector<double>& ganancias);   // aciertos, factor de beneficio, esperanza

// estrategia: agente (reglas del agente autonomo), senales, cruce_medias o comprar_mantener. Parametros: capital,
// riesgo_pct, max_posiciones, max_peso_pct, reserva_pct, comision_pct, comision_min_eur, deslizamiento_bps, umbral,
// aprender, fuera_muestra_pct, pesos
json backtest(const std::vector<SerieBT>& series, const json& params = json::object());

// metodo: igual, inversa_vol, min_varianza, paridad_riesgo, max_sharpe, hrp. Opcional capital y precios (para la
// asignacion en titulos enteros) y max_peso_pct.
json optimizar(const std::vector<SerieBT>& series, const std::string& metodo, const json& params = json::object());

json montecarlo(const std::vector<double>& rentabilidades, int simulaciones = 1000, int horizonte = 252,
                double capital = 10000, uint64_t semilla = 7);

// curva: [{ts, total_eur}], cerradas: [{ts, ganancia_eur}] (en orden). Ajustes: max_drawdown_pct, racha_perdidas,
// pausa_racha_h, perdida_diaria_pct (0 desactiva cada freno). Devuelve {bloqueado, motivos[]}
json protecciones(const json& curva, const json& cerradas, const json& ajustes, double ahora);

}  // namespace nx::bolsa
