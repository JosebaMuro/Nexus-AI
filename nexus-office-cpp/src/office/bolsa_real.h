// Del dato a la inversion real: banco de datos, backtests guardados, las cinco etapas que debe pasar una estrategia
// antes de tocar dinero y el enlace con un broker. Es el recorrido que siguen freqtrade (backtest -> dry-run -> live),
// FinRL y Lean (backtest -> paper -> live con Alpaca u otros), ver docs/BOLSA_PROYECTOS.md.
//
// Reglas que no cambian:
//   - el agente autonomo opera SIEMPRE en su cartera simulada;
//   - en una cartera real el agente solo PROPONE ordenes (estado por_confirmar), y solo si la estrategia ha pasado el
//     backtest y la simulacion y la persona ha habilitado el modo real (bolsa.real.habilitado);
//   - ninguna orden real sale sin que una persona escriba CONFIRMO; solo entonces se envia al broker (si hay uno
//     configurado) o se ejecuta a mano y se registra. Alpaca empieza en su entorno de papel (paper-api).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "office/bolsa.h"
#include "office/bolsa_cuant.h"
#include "util/http.h"

namespace nx::bolsa {

class AgenteBolsa;

// ---- banco de datos
json banco(Bolsa& b);   // {simbolos: [{simbolo, sesiones, desde, hasta}], series, sesiones, noticias, informes}
json banco_descargar(Bolsa& b, const std::vector<std::string>& simbolos, const std::string& rango = "1y");
// solo series diarias reales o guardadas (los ticks simulados no valen para un backtest); lo que falta va a avisos
std::vector<SerieBT> series_para(Bolsa& b, const std::vector<std::string>& simbolos, const std::string& rango = "1y",
                                 json* avisos = nullptr);
std::vector<std::string> lista_simbolos(const json& v);   // ["SAN.MC", ...] o "SAN.MC, ITX.MC"

// ---- backtests: {simbolos, rango, estrategia, ...parametros de backtest()}; se guarda el resumen
json backtest_mesa(Bolsa& b, const json& params);
json backtests(Bolsa& b, int limite = 20);
json backtest_guardado(Bolsa& b, long long id);

// ---- etapas
json criterios();   // umbrales de cada etapa
json etapas(Bolsa& b, const json& real_cfg);   // real_cfg = config bolsa.real

// el agente propone en la cartera real (por_confirmar) lo que acaba de hacer en la simulada, si todo lo permite
json proponer_real(Bolsa& b, const json& real_cfg, const std::string& simbolo, const std::string& lado, double qty_sim,
                   double total_sim_eur);

// ---- broker. cfg = config bolsa.broker {tipo: manual|alpaca, entorno: papel|real, clave, secreto,
// enviar_al_confirmar}
using FetchHttp = std::function<http::Response(const http::Request&)>;
json broker_estado(Bolsa& b, const json& cfg, bool consultar_cuenta = false, FetchHttp fetch = {});
json enviar_orden(Bolsa& b, long long oid, const json& cfg, FetchHttp fetch = {});
json sincronizar_broker(Bolsa& b, const json& cfg, FetchHttp fetch = {});

}  // namespace nx::bolsa
