"""Herramientas de Mundo (nexus/mundo): informacion publica al dia, ordenada por categorias y paises."""
from __future__ import annotations

import time


def _m(ctx):
    m = getattr(ctx.services, "mundo", None)
    if m is None:
        raise RuntimeError("El servicio Mundo no está disponible en este equipo")
    return m


def mundo_resumen(args, ctx):
    m = _m(ctx)
    r = m.resumen(args.get("pais") or "", args.get("categoria") or "")
    if not r["total"]:
        r["nota"] = ("Todavía no hay información recibida. " +
                     ("Mundo está apagado en Ajustes." if not m.activo() else
                      "Las fuentes se piden cada cierto tiempo; mira mundo_fuentes para ver su estado."))
    return r


def mundo_consultar(args, ctx):
    m = _m(ctx)
    filas = m.consultar(categoria=args.get("categoria") or "", pais=args.get("pais") or "",
                        texto=args.get("texto") or "", fuente=args.get("fuente") or "",
                        limite=int(args.get("limite") or 20), horas=float(args.get("horas") or 0),
                        orden=args.get("orden") or "reciente", corto=not args.get("detalle"))
    out = {"registros": filas, "total": len(filas)}
    if not filas:
        out["nota"] = "Sin registros con esos filtros. Prueba sin país o sin categoría, o mira mundo_resumen."
    return out


def mundo_fuentes(args, ctx):
    m = _m(ctx)
    fid = args.get("actualizar") or ""
    hecho = None
    if fid:
        est = {f["id"]: f for f in m.estado()["fuentes"]}
        if fid not in est:
            return {"error": f"fuente desconocida: {fid}", "fuentes": list(est)}
        f = est[fid]
        if f.get("ultimo_ok") and time.time() - f["ultimo_ok"] < f["minimo_s"]:
            hecho = {"fuente": fid, "nota": "Recién actualizada: se usa lo que ya hay", "registros": f["registros"]}
        else:
            hecho = m.actualizar(fid)
    e = m.estado()
    campos = ("id", "nombre", "categoria", "activa", "intervalo_s", "registros", "hace_s", "error", "con_clave", "origen")
    out = {"activo": e["activo"], "factor": e["factor"], "paises": e["paises"],
           "fuentes": [{k: f.get(k) for k in campos} for f in e["fuentes"]
                       if not args.get("categoria") or f["categoria"] == m.categoria(args["categoria"])]}
    if hecho is not None:
        out["actualizada"] = hecho
    return out


_CAT = {"type": "string", "description": "movimiento, sucesos, clima, espacio, noticias, camaras, audio, naturaleza o mercados"}
_PAIS = {"type": "string", "description": "país por nombre o código ISO (España, FR, Japón)"}

TOOLS = [
    ({"name": "mundo_resumen", "description": "Qué información del mundo hay recibida ahora mismo, contada por "
      "categoría y por país, con lo más destacado de cada categoría (terremotos, desastres, tiempo, vuelos, noticias, "
      "mercados...). Empieza por aquí",
      "parameters": {"type": "object", "properties": {"pais": _PAIS, "categoria": _CAT}},
      "risk": "low", "permissions": []}, mundo_resumen),
    ({"name": "mundo_consultar", "description": "Consulta la información del mundo recibida de fuentes públicas, "
      "filtrada por categoría, país, texto o fuente. Cada registro lleva fecha, país y enlace",
      "parameters": {"type": "object", "properties": {
          "categoria": _CAT, "pais": _PAIS, "texto": {"type": "string", "description": "palabras que debe contener"},
          "fuente": {"type": "string", "description": "id de la fuente (sismos, tiempo, vuelos, indices...)"},
          "horas": {"type": "number", "description": "solo lo de las últimas N horas"},
          "orden": {"type": "string", "enum": ["reciente", "valor"], "default": "reciente"},
          "limite": {"type": "integer", "default": 20}, "detalle": {"type": "boolean", "default": False}}},
      "risk": "low", "permissions": []}, mundo_consultar),
    ({"name": "mundo_fuentes", "description": "Fuentes de información del mundo con su estado: cada cuánto se piden, "
      "cuándo llegaron por última vez y si fallan. Con «actualizar» pide una ahora",
      "parameters": {"type": "object", "properties": {
          "categoria": _CAT, "actualizar": {"type": "string", "description": "id de la fuente que pedir ahora"}}},
      "risk": "low", "permissions": ["network"], "timeout": 90}, mundo_fuentes),
]
