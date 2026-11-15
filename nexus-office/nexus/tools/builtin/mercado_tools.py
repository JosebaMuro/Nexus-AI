"""Herramientas del agente de bolsa: datos de mercado por recetas, opciones, riesgo, valoracion, cribas, alertas,
expedientes e informes oficiales. Los calculos estan en nexus/office/bolsa_analisis.py y los datos en mercados.py."""
from __future__ import annotations


def _series(simbolos, rango: str = "1y") -> list[dict]:
    """Historico de cada simbolo: el de la mesa (real con conexion, simulado sin ella) o, si la mesa no conoce el
    valor, el del proveedor de datos reales."""
    import os
    from ...office import bolsa
    if isinstance(simbolos, str):
        simbolos = [s for s in simbolos.replace(";", ",").replace(" ", ",").split(",") if s]
    out = []
    for s in simbolos[:12]:
        sym = s.strip().upper()
        try:
            d = bolsa.get().serie(sym, rango)
            out.append({"simbolo": d["simbolo"], "puntos": d["puntos"], "real": d.get("real", False)})
        except ValueError:
            if os.environ.get("NEXUS_BOLSA_OFFLINE"):
                raise
            from ...office import mercados
            out.append({"simbolo": sym, "puntos": mercados.get().velas(sym, rango), "real": True})
    return out


def _datos_reales(series) -> dict:
    sim = [s["simbolo"] for s in series if not s.get("real")]
    return {"datos": "simulados (sin conexión)" if sim else "reales", **({"simulados": sim} if sim else {})}


def mercado_consulta(args, ctx):
    from ...office import bolsa_terminal, mercados
    nombre = str(args.get("receta") or "").strip().lower()
    if nombre in ("", "lista", "recetas", "ayuda"):
        return {"recetas": [{"receta": k, "para": v[0], "parametros": v[1]} for k, v in mercados.RECETAS.items()]}
    params = dict(args.get("parametros") or {})
    for k in ("simbolo", "simbolos", "rango", "mercado", "filtros", "fecha", "pais", "texto", "limite", "vencimiento"):
        if args.get(k) not in (None, ""):
            params.setdefault(k, args[k])
    marco = mercados.receta(nombre, params)
    bolsa_terminal.recordar(ctx.session_id, [marco])
    if len(marco["filas"]) > 60:
        marco = {**marco, "filas": marco["filas"][:60], "filas_totales": len(marco["filas"])}
    return marco


def mercado_panel(args, ctx):
    from ...office import bolsa_terminal
    p = bolsa_terminal.panel(str(args.get("pregunta") or ""), ctx.session_id)
    for m in p["marcos"]:
        if len(m["filas"]) > 25:
            m["filas_totales"], m["filas"] = len(m["filas"]), m["filas"][:25]
    return p


def bolsa_verificar(args, ctx):
    from ...office import bolsa_terminal
    return bolsa_terminal.verificar(str(args.get("texto") or ""), ctx.session_id, args.get("datos"))


def bolsa_opciones(args, ctx):
    from ...office import bolsa_analisis as ba, mercados
    sym = str(args.get("simbolo") or "").strip().upper()
    subyacente, dias, vol, cadena = args.get("subyacente"), args.get("dias"), args.get("volatilidad_pct"), None
    if sym and (not subyacente or not args.get("patas")):
        try:
            c = mercados.get().opciones(sym, str(args.get("vencimiento") or ""), 40)
            subyacente, dias, cadena = subyacente or c["subyacente"], dias or c["dias"], c["filas"]
            vis = [f[t].get("vi_pct") for f in cadena for t in ("call", "put") if f[t].get("vi_pct")]
            if vis and not vol:
                vol = sorted(vis)[len(vis) // 2]
        except Exception as e:  # noqa: BLE001
            if not subyacente:
                return {"error": f"No hay cadena de opciones de {sym} ({e}). Da el precio del subyacente y las patas."}
    if not subyacente:
        return {"error": "Hace falta el símbolo o el precio del subyacente", "estrategias": {k: v[0] for k, v in ba.ESTRATEGIAS.items()}}
    dias, volf = float(dias or 30), float(vol or 25) / 100
    patas = args.get("patas") or ba.patas_de(str(args.get("estrategia") or "call_comprada"), float(subyacente), dias,
                                            volf, cadena=cadena)
    out = ba.estrategia_opciones(patas, float(subyacente), int(args.get("multiplicador") or 100), dias, volf)
    out.update(simbolo=sym, subyacente=float(subyacente), dias=dias, volatilidad_pct=round(volf * 100, 2),
               primas="de mercado (Nasdaq)" if cadena else "teóricas (Black-Scholes)")
    return out


def bolsa_riesgo(args, ctx):
    from ...office import bolsa_analisis as ba
    modo = str(args.get("modo") or "cartera")
    if modo == "posicion":
        return ba.tamano_posicion(float(args.get("capital") or 10000), float(args.get("riesgo_pct") or 1),
                                  float(args["entrada"]), float(args["stop"]), args.get("objetivo"),
                                  args.get("prob_acierto"), float(args.get("max_peso_pct") or 100))
    if modo == "estres":
        pos = args.get("posiciones")
        if not pos:
            from ...office import bolsa
            r = bolsa.get().resumen(args.get("cartera") or None)
            pos = [{"simbolo": p.get("simbolo"), "valor": p.get("valor") or p.get("valor_mercado") or 0,
                    "clase": "cripto" if "-USD" in str(p.get("simbolo")) else "renta_variable"}
                   for p in r.get("posiciones", [])]
            if not pos:
                return {"error": "La cartera no tiene posiciones; pasa «posiciones» con su valor"}
        return ba.estres(pos, args.get("escenarios"))
    series = _series(args.get("simbolos") or [], args.get("rango") or "1y")
    try:
        ref = _series([args.get("referencia") or "^GSPC"], args.get("rango") or "1y")[0]["puntos"]
    except Exception:  # noqa: BLE001
        ref = None   # sin referencia no hay beta, pero el resto del calculo vale
    return {**ba.riesgo(series, args.get("pesos"), float(args.get("capital") or 10000),
                        float(args.get("confianza") or 0.95), int(args.get("horizonte") or 1), ref),
            **_datos_reales(series)}


def bolsa_valoracion(args, ctx):
    from ...office import bolsa_analisis as ba
    comun = dict(descuento_pct=float(args.get("descuento_pct") or 9), terminal_pct=float(args.get("terminal_pct") or 2),
                 anios=int(args.get("anios") or 10), deuda_neta=float(args.get("deuda_neta") or 0))
    if str(args.get("modo") or "dcf") == "dcf_inverso":
        return ba.dcf_inverso(float(args["precio"]), float(args["acciones"]), float(args["flujo_caja"]), **comun)
    return ba.dcf(float(args["flujo_caja"]), float(args.get("crecimiento_pct") or 5), acciones=float(args.get("acciones") or 0),
                  **comun)


def bolsa_criba(args, ctx):
    from ...office import bolsa_analisis as ba, bolsa_terminal, mercados
    nombre = str(args.get("criba") or "").strip().lower()
    if not nombre and not args.get("filtros") and not args.get("formula"):
        return {"cribas": {k: v[0] for k, v in ba.CRIBAS.items()},
                "campos": sorted(set(mercados.TV_NOMBRES.values())),
                "nota": "Elige una criba o da filtros («per < 15, roe_pct > 20») y, si quieres, una fórmula para ordenar"}
    filtros, formula, desc, titulo = args.get("filtros") or "", args.get("formula") or "", True, "Criba a medida"
    if nombre:
        c = ba.criba_preparada(nombre)
        filtros, formula, desc, titulo = filtros or c["filtros"], formula or c["formula"], c["descendente"], c["titulo"]
    if isinstance(filtros, str):
        filtros = mercados.leer_filtros(filtros)
    limite = int(args.get("limite") or 20)
    filas = mercados.get().cribar(str(args.get("mercado") or "america"), filtros, "capitalizacion", True,
                                  max(limite * 6, 120) if formula else limite)
    if formula:
        filas = ba.puntuar(filas, formula, bool(args.get("descendente", desc)), limite)
    corto = ("simbolo", "nombre", "precio", "cambio_pct", "capitalizacion", "per", "roe_pct", "rentabilidad_dividendo_pct",
             "precio_valor_contable", "sector", "puntuacion")
    filas = [{k: f[k] for k in corto if f.get(k) is not None} for f in filas]
    marco = {"receta": "criba", "titulo": titulo, "filtros": filtros, "formula": formula, "filas": filas,
             "fuente": "TradingView", "aviso": ba.AVISO}
    bolsa_terminal.recordar(ctx.session_id, [marco])
    return marco


def bolsa_alerta(args, ctx):
    from ...office import bolsa_analisis as ba
    accion = str(args.get("accion") or "listar")
    if accion == "crear":
        return {"alerta": ba.alerta_guardar(str(args.get("simbolo") or ""), args.get("condiciones") or "",
                                            str(args.get("nota") or ""), bool(args.get("todas", True)),
                                            float(args.get("repetir_h") or 24)),
                "nota": "Se revisa sola con la fuente «alertas_bolsa» de Mundo y aparece en la categoría mercados"}
    if accion == "borrar":
        return {"borrada": ba.alerta_borrar(int(args.get("id") or 0))}
    if accion == "revisar":
        return {"saltan": ba.alertas_evaluar()}
    return {"alertas": ba.alertas(), "campos": ["precio", "cambio_pct", "volumen", "rsi", "sma20", "sma50", "sma200",
                                                "distancia_maximo_pct"]}


def bolsa_expediente(args, ctx):
    from ...office import bolsa_analisis as ba
    accion, sym = str(args.get("accion") or "leer"), str(args.get("simbolo") or "")
    if accion == "listar" or not sym:
        return {"expedientes": ba.expedientes()}
    if accion == "guardar":
        ev = args.get("evidencia")
        if ev and args.get("fuente") and isinstance(ev, str):
            ev = {"texto": ev, "fuente": args.get("fuente"), "url": args.get("url") or "",
                  "a_favor": bool(args.get("a_favor", True))}
        return ba.expediente_guardar(sym, args.get("tesis"), args.get("supuesto"), ev, args.get("riesgo"),
                                     args.get("revision"))
    return ba.expediente(sym)


def bolsa_informe_oficial(args, ctx):
    from ...office import bolsa_informes as bi
    accion, sym = str(args.get("accion") or "lista"), str(args.get("simbolo") or "")
    tipo = str(args.get("tipo") or "10-K")
    if accion == "leer":
        return bi.leer_informe(sym, tipo, int(args.get("indice") or 0), comparar=bool(args.get("comparar")))
    if accion == "texto":
        return bi.texto_informe(sym, tipo, str(args.get("apartado") or "analisis_direccion"), int(args.get("indice") or 0),
                                int(args.get("desde") or 0), int(args.get("largo") or 6000))
    if accion == "cifras":
        return bi.cifras(sym, not args.get("trimestral"), int(args.get("limite") or 6))
    if accion == "verificar":
        t = bi.texto_informe(sym, tipo, "completo", int(args.get("indice") or 0), 0, 20000)
        completo = bi.limpiar(bi._pedir(t["informe"]["url"], 86400, como_json=False))
        return {"citas": bi.verificar_citas(completo, list(args.get("citas") or [])), "informe": t["informe"]}
    return bi.presentaciones(sym, args.get("tipos") or None, int(args.get("limite") or 12))


def bolsa_validar(args, ctx):
    from ...office import bolsa_analisis as ba
    series = _series(args.get("simbolos") or [], args.get("rango") or "5y")
    params = {k: args[k] for k in ("estrategia", "capital", "comision_pct") if args.get(k) is not None}
    return {**ba.validar_por_tramos(series, params, int(args.get("tramos") or 4)), **_datos_reales(series)}


_SYM = {"type": "string", "description": "símbolo (AAPL, SAN.MC, ^IBEX, BTC-USD)"}
_SYMS = {"type": "array", "items": {"type": "string"}, "description": "símbolos"}

TOOLS = [
    ({"name": "mercado_consulta", "description": "Datos de mercado al día por receta: buscar, cotizacion, velas, "
      "indicadores, metricas, perfil, noticias, opciones, resultados, historial_bpa, estimaciones, financieros, "
      "dividendos, internos, instituciones, cortos, criba, sectores, movimientos, cripto, macro, calendario, "
      "salidas_bolsa. Con receta «lista» explica cada una. Cada respuesta lleva su fuente y hora",
      "parameters": {"type": "object", "properties": {
          "receta": {"type": "string"}, "simbolo": _SYM, "simbolos": _SYMS,
          "rango": {"type": "string", "enum": ["1d", "5d", "1mo", "6mo", "1y", "5y"]},
          "mercado": {"type": "string", "description": "país o mercado de la criba y los sectores (US, ES, DE...)"},
          "filtros": {"type": "string", "description": "criba: «per < 15, roe_pct > 20»"},
          "fecha": {"type": "string"}, "pais": {"type": "string"}, "texto": {"type": "string"},
          "limite": {"type": "integer"}, "parametros": {"type": "object"}}, "required": ["receta"]},
      "risk": "low", "permissions": ["network"], "timeout": 60}, mercado_consulta),
    ({"name": "mercado_panel", "description": "Responde a una pregunta de bolsa en lenguaje corriente eligiendo y "
      "ejecutando las recetas de datos que hagan falta (cotización, noticias, opciones, criba, macro...)",
      "parameters": {"type": "object", "properties": {"pregunta": {"type": "string"}}, "required": ["pregunta"]},
      "risk": "low", "permissions": ["network"], "timeout": 90}, mercado_panel),
    ({"name": "bolsa_verificar", "description": "Comprueba que las cifras de un texto salen de los datos de mercado "
      "consultados en esta conversación. Úsala antes de dar una respuesta con precios o ratios",
      "parameters": {"type": "object", "properties": {"texto": {"type": "string"}}, "required": ["texto"]},
      "risk": "low", "permissions": []}, bolsa_verificar),
    ({"name": "bolsa_opciones", "description": "Estrategia con opciones: coste, beneficio y pérdida máximos, puntos "
      "de equilibrio, griegas y probabilidad de beneficio. Plantillas: call_comprada, put_comprada, call_cubierta, "
      "put_protectora, collar, diferencial_alcista, diferencial_bajista, cono, cuna, mariposa, condor_hierro; o patas "
      "a medida",
      "parameters": {"type": "object", "properties": {
          "simbolo": _SYM, "estrategia": {"type": "string"}, "vencimiento": {"type": "string"},
          "patas": {"type": "array", "items": {"type": "object"},
                    "description": "[{tipo: call|put|accion, lado: compra|venta, strike, prima, cantidad}]"},
          "subyacente": {"type": "number"}, "dias": {"type": "number"}, "volatilidad_pct": {"type": "number"},
          "multiplicador": {"type": "integer", "default": 100}}},
      "risk": "low", "permissions": ["network"], "timeout": 60}, bolsa_opciones),
    ({"name": "bolsa_riesgo", "description": "Riesgo: modo «cartera» (VaR, CVaR, volatilidad, caída máxima, beta y "
      "correlaciones de varios valores), «estres» (qué pasaría en 2008, 2020, 2022...) o «posicion» (cuántas "
      "acciones comprar para arriesgar un % del capital entre la entrada y el stop, con Kelly)",
      "parameters": {"type": "object", "properties": {
          "modo": {"type": "string", "enum": ["cartera", "estres", "posicion"], "default": "cartera"},
          "simbolos": _SYMS, "pesos": {"type": "object"}, "capital": {"type": "number"},
          "confianza": {"type": "number", "default": 0.95}, "horizonte": {"type": "integer", "default": 1},
          "referencia": {"type": "string", "default": "^GSPC"}, "rango": {"type": "string", "default": "1y"},
          "posiciones": {"type": "array", "items": {"type": "object"},
                         "description": "estres: [{simbolo, valor, clase, beta}]"},
          "cartera": {"type": "string"}, "escenarios": {"type": "array", "items": {"type": "string"}},
          "riesgo_pct": {"type": "number"}, "entrada": {"type": "number"}, "stop": {"type": "number"},
          "objetivo": {"type": "number"}, "prob_acierto": {"type": "number"}, "max_peso_pct": {"type": "number"}}},
      "risk": "low", "permissions": ["network"], "timeout": 90}, bolsa_riesgo),
    ({"name": "bolsa_valoracion", "description": "Valoración por descuento de flujos de caja: «dcf» da el valor por "
      "acción con tus supuestos; «dcf_inverso» dice qué crecimiento descuenta ya el precio",
      "parameters": {"type": "object", "properties": {
          "modo": {"type": "string", "enum": ["dcf", "dcf_inverso"], "default": "dcf"},
          "flujo_caja": {"type": "number", "description": "flujo de caja libre del último año"},
          "crecimiento_pct": {"type": "number"}, "descuento_pct": {"type": "number", "default": 9},
          "terminal_pct": {"type": "number", "default": 2}, "anios": {"type": "integer", "default": 10},
          "deuda_neta": {"type": "number"}, "acciones": {"type": "number"}, "precio": {"type": "number"}},
          "required": ["flujo_caja"]}, "risk": "low", "permissions": []}, bolsa_valoracion),
    ({"name": "bolsa_criba", "description": "Busca valores con las cribas de inversores conocidos (graham, lynch, "
      "greenblatt, buffett, dividendo, impulso, pequenas_valor) o con filtros y una fórmula propios. Sin "
      "argumentos lista las cribas y los campos",
      "parameters": {"type": "object", "properties": {
          "criba": {"type": "string"}, "mercado": {"type": "string", "default": "US"},
          "filtros": {"type": "string"}, "formula": {"type": "string", "description": "orden: «roe_pct / per»"},
          "descendente": {"type": "boolean"}, "limite": {"type": "integer", "default": 20}}},
      "risk": "low", "permissions": ["network"], "timeout": 60}, bolsa_criba),
    ({"name": "bolsa_alerta", "description": "Alertas de bolsa con una o varias condiciones (precio, cambio_pct, "
      "volumen, rsi, sma20, sma50, sma200, distancia_maximo_pct): crear, listar, borrar o revisar ahora",
      "parameters": {"type": "object", "properties": {
          "accion": {"type": "string", "enum": ["crear", "listar", "borrar", "revisar"], "default": "listar"},
          "simbolo": _SYM, "condiciones": {"type": "string", "description": "«precio > 200, rsi < 30»"},
          "todas": {"type": "boolean", "default": True}, "nota": {"type": "string"},
          "repetir_h": {"type": "number", "default": 24}, "id": {"type": "integer"}}},
      "risk": "low", "permissions": ["network"]}, bolsa_alerta),
    ({"name": "bolsa_expediente", "description": "Expediente de un valor: tesis de inversión, supuestos, evidencias "
      "con su fuente, riesgos y revisiones. Acciones: leer, guardar (añade lo que se pase) y listar",
      "parameters": {"type": "object", "properties": {
          "accion": {"type": "string", "enum": ["leer", "guardar", "listar"], "default": "leer"}, "simbolo": _SYM,
          "tesis": {"type": "string"}, "supuesto": {"type": "string"}, "evidencia": {"type": "string"},
          "fuente": {"type": "string"}, "url": {"type": "string"}, "a_favor": {"type": "boolean"},
          "riesgo": {"type": "string"}, "revision": {"type": "string"}}},
      "risk": "low", "permissions": []}, bolsa_expediente),
    ({"name": "bolsa_informe_oficial", "description": "Informes oficiales de empresas que presentan en Estados "
      "Unidos (SEC EDGAR): «lista» de presentaciones, «leer» un 10-K o 10-Q con impulsores y frenos y citas "
      "literales verificadas, «texto» de un apartado, «cifras» contables por ejercicio y «verificar» citas",
      "parameters": {"type": "object", "properties": {
          "accion": {"type": "string", "enum": ["lista", "leer", "texto", "cifras", "verificar"], "default": "lista"},
          "simbolo": _SYM, "tipo": {"type": "string", "default": "10-K"}, "indice": {"type": "integer", "default": 0},
          "apartado": {"type": "string", "enum": ["negocio", "riesgos", "analisis_direccion", "riesgo_mercado",
                                                  "litigios", "completo"]},
          "desde": {"type": "integer"}, "largo": {"type": "integer"}, "comparar": {"type": "boolean"},
          "trimestral": {"type": "boolean"}, "citas": {"type": "array", "items": {"type": "string"}},
          "limite": {"type": "integer"}}, "required": ["simbolo"]},
      "risk": "low", "permissions": ["network"], "timeout": 120}, bolsa_informe_oficial),
    ({"name": "bolsa_validar", "description": "Valida una estrategia por tramos sucesivos del histórico "
      "(walk-forward) para ver si aguanta fuera del periodo en que se eligió",
      "parameters": {"type": "object", "properties": {
          "simbolos": _SYMS, "estrategia": {"type": "string"}, "tramos": {"type": "integer", "default": 4},
          "rango": {"type": "string", "default": "5y"}}, "required": ["simbolos"]},
      "risk": "low", "permissions": ["network"], "timeout": 180}, bolsa_validar),
]
