// Herramientas de la mesa de bolsa (tools/builtin/bolsa_tools.py): cotizaciones, ordenes y carteras simuladas.
// Funcionan sin internet. Un agente puede preparar ordenes, pero en una cartera real nunca las confirma ni las ejecuta.
#include "office/bolsa.h"
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

Register r1("bolsa_mercados", bolsa_mercados), r2("bolsa_cotizacion", bolsa_cotizacion), r3("bolsa_cartera", bolsa_cartera),
    r4("bolsa_costes", bolsa_costes), r5("bolsa_orden", bolsa_orden), r6("bolsa_cancelar", bolsa_cancelar),
    r7("bolsa_informe_fiscal", bolsa_informe_fiscal);

}  // namespace

void link_builtin_bolsa() {}

}  // namespace nx::tools
