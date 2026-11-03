// Herramientas de la mesa de bolsa (tools/builtin/bolsa_tools.py): cotizaciones, ordenes y carteras simuladas.
// Funcionan sin internet. Un agente puede preparar ordenes, pero en una cartera real nunca las confirma ni las ejecuta.
#include "office/bolsa.h"
#include "office/bolsa_cuant.h"
#include "office/bolsa_mesa.h"
#include "office/bolsa_real.h"
#include "core/config.h"
#include "tools/tools.h"
#include "util/text.h"

namespace nx::tools {

namespace {

json cart(const json& args) {
  json c = args.value("cartera", json(nullptr));
  return truthy(c) ? c : json(nullptr);
}

template <class F>
json guard(F f) {
  try {
    return f();
  } catch (const bolsa::PermissionDenied& e) {
    throw ToolError("PermissionError", e.what());
  } catch (const std::invalid_argument& e) {
    throw ToolError("ValueError", e.what());
  }
}

json bolsa_mercados(const json&, ToolContext&) {
  return guard([&] { return json{{"mercados", bolsa::get().mercados()}, {"aviso", bolsa::AVISO}}; });
}

json bolsa_cotizacion(const json& args, ToolContext&) {
  return guard([&] {
    auto& b = bolsa::get();
    if (truthy(args.value("simbolo", json(nullptr)))) {
      json q = b.precio(jstr(args, "simbolo"));
      q["aviso"] = bolsa::AVISO;
      return q;
    }
    json out = json::array();
    for (auto& i : b.instrumentos(jstr(args, "buscar"))) {
      if (out.size() >= 40) break;
      json x;
      for (auto* k : {"simbolo", "nombre", "mercado", "divisa", "tipo"}) x[k] = i[k];
      out.push_back(x);
    }
    return json{{"instrumentos", out}, {"aviso", bolsa::AVISO}};
  });
}

json bolsa_cartera(const json& args, ToolContext&) {
  return guard([&] {
    auto& b = bolsa::get();
    json cid = cart(args);
    json out = b.resumen(cid);
    if (truthy(args.value("operaciones", json(false)))) out["operaciones"] = b.operaciones(cid, 20);
    out["ordenes_abiertas"] = b.ordenes(cid, true);
    json cs = json::array();
    for (auto& c : b.carteras()) cs.push_back({{"id", c["id"]}, {"nombre", c["nombre"]}, {"modo", c["modo"]}, {"tarifa", c["tarifa"]}});
    out["carteras"] = cs;
    return out;
  });
}

json bolsa_costes(const json& args, ToolContext&) {
  return guard([&] {
    auto& b = bolsa::get();
    json q = b.precio(jstr(args, "simbolo"));
    std::string lado = jstr(args, "lado", "compra");
    json pin = args.value("precio", json(nullptr));
    double px = truthy(pin) ? bolsa::num(pin) : jnum(lado == "compra" ? q["venta"] : q["compra"]);
    json g = b.costes(cart(args), jstr(args, "simbolo"), lado, bolsa::num(args.at("cantidad")), px);
    g["total_gastos_eur"] = round_to(jnum(g["comision_eur"]) + jnum(g["impuestos_eur"]) + jnum(g["cambio_eur"]), 2);
    g["precio"] = px;
    return g;
  });
}

json bolsa_orden(const json& args, ToolContext& ctx) {
  return guard([&] {
    return bolsa::get().ordenar(jstr(args, "simbolo"), jstr(args, "lado"), args.at("cantidad"), jstr(args, "tipo", "mercado"),
                                args.value("limite", json(nullptr)), args.value("stop", json(nullptr)),
                                jstr(args, "validez", "dia"), cart(args), ctx.agent.empty() ? "agente" : "agente:" + ctx.agent);
  });
}

json bolsa_cancelar(const json& args, ToolContext&) {
  return guard([&] { return bolsa::get().cancelar((long long)bolsa::num(args.at("orden"))); });
}

json bolsa_informe_fiscal(const json& args, ToolContext&) {
  return guard([&] {
    json a = args.value("anio", json(nullptr));
    return bolsa::get().informe_fiscal(cart(args), truthy(a) ? a : json(nullptr));
  });
}

// ---- laboratorio: senales, backtest, cartera optima, Monte Carlo, noticias, mesa de analistas, etapas y banco
json bolsa_senales(const json& args, ToolContext&) {
  return guard([&] {
    json sr = bolsa::get().serie(jstr(args, "simbolo"), "1y");
    json s = bolsa::senales(sr["puntos"]);
    if (!s.is_object()) throw std::invalid_argument("serie demasiado corta para las señales (60 sesiones)");
    s["simbolo"] = sr["simbolo"];
    s["datos_reales"] = sr["real"];
    s["aviso"] = bolsa::AVISO;
    return s;
  });
}

json bolsa_backtest(const json& args, ToolContext&) {
  return guard([&] {
    json r = bolsa::backtest_mesa(bolsa::get(), args);
    r.erase("curva");   // al modelo le basta el resumen
    if (r["ultimas_operaciones"].size() > 10) {
      json u = json::array();
      for (size_t i = r["ultimas_operaciones"].size() - 10; i < r["ultimas_operaciones"].size(); ++i) u.push_back(r["ultimas_operaciones"][i]);
      r["ultimas_operaciones"] = u;
    }
    return r;
  });
}

json bolsa_optimizar(const json& args, ToolContext&) {
  return guard([&] {
    auto& b = bolsa::get();
    json avisos = json::array();
    auto series = bolsa::series_para(b, bolsa::lista_simbolos(args.value("simbolos", json())), jstr(args, "rango", "1y"), &avisos);
    json r = bolsa::optimizar(series, jstr(args, "metodo", "hrp"), args);
    for (auto& a : avisos) r["avisos"].push_back(a);
    return r;
  });
}

json bolsa_montecarlo(const json& args, ToolContext&) {
  return guard([&] {
    json sr = bolsa::get().serie(jstr(args, "simbolo"), "1y");
    if (!truthy(sr["real"])) throw std::invalid_argument("sin histórico diario de " + jstr(args, "simbolo"));
    std::vector<double> v, rs;
    for (auto& p : sr["puntos"]) v.push_back(jnum(p["precio"]));
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i - 1] > 0) rs.push_back(v[i] / v[i - 1] - 1);
    return bolsa::montecarlo(rs, (int)jnum(args.value("simulaciones", json(1000)), 1000),
                             (int)jnum(args.value("horizonte", json(252)), 252), jnum(args.value("capital", json(10000)), 10000));
  });
}

json bolsa_noticias(const json& args, ToolContext&) {
  return guard([&] {
    json n = bolsa::mesa().noticias(jstr(args, "simbolo"), truthy(args.value("refrescar", json(false))));
    if (n["noticias"].size() > 12) n["noticias"] = json(std::vector<json>(n["noticias"].begin(), n["noticias"].begin() + 12));
    return n;
  });
}

json bolsa_informe(const json& args, ToolContext&) {
  return guard([&] {
    json op = {{"rondas", args.value("rondas", json(1))}};
    json r = bolsa::mesa().informe(jstr(args, "simbolo"), op);
    r.erase("senales");
    return r;
  });
}

json bolsa_etapas(const json&, ToolContext&) {
  return guard([&] { return bolsa::etapas(bolsa::get(), get_config().get("bolsa.real", json::object())); });
}

json bolsa_banco(const json& args, ToolContext&) {
  return guard([&] {
    auto& b = bolsa::get();
    auto syms = bolsa::lista_simbolos(args.value("simbolos", json()));
    if (syms.empty()) return bolsa::banco(b);
    return bolsa::banco_descargar(b, syms, jstr(args, "rango", "1y"));
  });
}

Register r1("bolsa_mercados", bolsa_mercados), r2("bolsa_cotizacion", bolsa_cotizacion), r3("bolsa_cartera", bolsa_cartera),
    r4("bolsa_costes", bolsa_costes), r5("bolsa_orden", bolsa_orden), r6("bolsa_cancelar", bolsa_cancelar),
    r7("bolsa_informe_fiscal", bolsa_informe_fiscal), r8("bolsa_senales", bolsa_senales),
    r9("bolsa_backtest", bolsa_backtest), r10("bolsa_optimizar", bolsa_optimizar), r11("bolsa_montecarlo", bolsa_montecarlo),
    r12("bolsa_noticias", bolsa_noticias), r13("bolsa_informe", bolsa_informe), r14("bolsa_etapas", bolsa_etapas),
    r15("bolsa_banco", bolsa_banco);

}  // namespace

void link_builtin_bolsa() {}

}  // namespace nx::tools
