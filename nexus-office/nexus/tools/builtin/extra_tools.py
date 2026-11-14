"""Herramientas de la 4.1: ubicacion en tiempo real, video a partir de imagenes y laboratorio de bolsa."""
from __future__ import annotations


def ubicacion_actual(args, ctx):
    from ...devices import ubicacion
    if getattr(ctx, "remote", False):
        return {"error": "La ubicación de este equipo no se comparte con otros nodos"}
    return ubicacion.consultar({k: args[k] for k in ("fuente", "hasta") if args.get(k)})


def crear_video(args, ctx):
    from ...media.video import crear_video as _crear
    return _crear(ctx.services, args)


def _serie(simbolo: str, rango: str) -> dict:
    from ...office import bolsa
    d = bolsa.get().serie(simbolo, rango)
    return {"simbolo": d["simbolo"], "puntos": d["puntos"], "real": d.get("real", False)}


def bolsa_senales(args, ctx):
    from ...office import bolsa_lab
    d = _serie(args["simbolo"], args.get("rango") or "1y")
    s = bolsa_lab.senales(d["puntos"])
    if s is None:
        return {"error": "No hay histórico suficiente (hacen falta 60 sesiones)"}
    return {"simbolo": d["simbolo"], "datos_reales": d["real"], **s}


def bolsa_backtest(args, ctx):
    from ...office import bolsa_lab
    simbolos = [x for x in str(args["simbolos"]).replace(";", ",").replace(" ", ",").split(",") if x][:8]
    series = [_serie(sym, args.get("rango") or "1y") for sym in simbolos]
    res = bolsa_lab.backtest(series, {k: v for k, v in args.items() if k != "simbolos"})
    res.pop("curva", None)   # la curva es para el grafico, no para el modelo
    return res


def bolsa_analistas(args, ctx):
    from ...office import bolsa_lab
    d = _serie(args["simbolo"], "1y")
    s = ctx.services
    llm = None
    if s is not None and s.llm.has_real_llm() and getattr(ctx, "depth", 0) == 0:
        llm = lambda p: s.llm.complete(p, max_tokens=320, temperature=0.2)   # noqa: E731
    res = bolsa_lab.informe_mesa(d["simbolo"], d["puntos"], args, llm)
    res.pop("traza", None)
    return res


TOOLS = [
    ({"name": "ubicacion_actual", "description": "Posición actual de este equipo o móvil (GPS, servicio de ubicación "
      "del sistema, navegador o posición fija), con la ciudad más cercana. Opcionalmente la distancia en línea recta "
      "hasta un lugar. Todo se calcula en el equipo: la posición no se envía a ningún servicio",
      "parameters": {"type": "object", "properties": {
          "hasta": {"type": "string", "description": "lugar guardado, ciudad o «latitud, longitud» hasta el que medir"},
          "fuente": {"type": "string", "enum": ["auto", "app", "gpsd", "nmea", "windows", "geoclue", "navegador",
                                                "manual"], "default": "auto"}}},
      "risk": "low", "permissions": ["system"], "timeout": 45}, ubicacion_actual),
    ({"name": "crear_video", "description": "Crea un vídeo (AVI) con una secuencia de imágenes ya generadas: zoom "
      "lento y fundido entre ellas. Sin lista de imágenes usa las últimas generadas",
      "parameters": {"type": "object", "properties": {
          "imagenes": {"type": "array", "description": "URLs /outputs/... de las imágenes, en orden"},
          "ultimas": {"type": "integer", "default": 6, "description": "si no hay lista: cuántas de las últimas usar"},
          "segundos": {"type": "number", "default": 3, "description": "segundos por imagen"},
          "formato": {"type": "string", "enum": ["16:9", "1080p", "9:16", "1:1", "4:3"], "default": "16:9"},
          "titulo": {"type": "string"}}},
      "risk": "low", "permissions": ["media"], "timeout": 600}, crear_video),
    ({"name": "bolsa_senales", "description": "Lectura técnica de un valor con cinco estrategias (tendencia, reversión, "
      "momento, volatilidad y estadística): señal combinada, confianza y si es alcista, neutral o bajista",
      "parameters": {"type": "object", "properties": {
          "simbolo": {"type": "string"}, "rango": {"type": "string", "enum": ["6mo", "1y", "5y"], "default": "1y"}},
          "required": ["simbolo"]}, "risk": "low", "permissions": [], "timeout": 60}, bolsa_senales),
    ({"name": "bolsa_backtest", "description": "Prueba una estrategia con el histórico de uno o varios valores "
      "(comisiones y deslizamiento incluidos) y la compara con comprar y mantener: rentabilidad, Sharpe, caída máxima",
      "parameters": {"type": "object", "properties": {
          "simbolos": {"type": "string", "description": "uno o varios, separados por comas"},
          "estrategia": {"type": "string", "enum": ["senales", "cruce_medias", "comprar_mantener"], "default": "senales"},
          "rango": {"type": "string", "enum": ["6mo", "1y", "5y"], "default": "1y"},
          "capital": {"type": "number", "default": 10000}}, "required": ["simbolos"]},
      "risk": "low", "permissions": [], "timeout": 120}, bolsa_backtest),
    ({"name": "bolsa_analistas", "description": "Informe de la mesa de analistas sobre un valor: técnico, debate "
      "alcista frente a bajista, riesgo y decisión (comprar, mantener o vender) con el peso sugerido. Solo informa: "
      "no da órdenes", "parameters": {"type": "object", "properties": {"simbolo": {"type": "string"}},
                                      "required": ["simbolo"]},
      "risk": "low", "permissions": [], "timeout": 300}, bolsa_analistas),
]
