"""Herramientas de la oficina Herrero: proyecto completo de una estructura metalica (calculo, despiece, planos,
soldaduras e imagen), consulta de perfiles y garganta de soldadura. El calculo es determinista y esta en
nexus/office/herrero*.py; el agente solo lo pide y lo explica."""
from __future__ import annotations


def herrero_proyecto(args, ctx):
    from ...office import herrero
    params = {k: args[k] for k in ("tipo", "largo", "ancho", "alto", "acero", "acabado", "cubierta", "uso", "forjado",
                                   "altitud", "zona_viento", "pendiente_pct", "desnivel") if args.get(k) not in (None, "")}
    r = herrero.proyecto(str(args.get("encargo") or ""), params, ctx.services if ctx else None,
                         imagen_ia=bool(args.get("imagen_ia", True)), imagen=bool(args.get("imagen", True)))
    if ctx and ctx.emit:
        try:
            ctx.emit({"type": "herrero", "id": r["id"], "ficheros": r["ficheros"]})
        except Exception:  # noqa: BLE001
            pass
    return {"id": r["id"], "resumen": herrero.resumen(r), "supuestos": r["puestos"]["recepcion"]["supuestos"],
            "despiece": [{k: f[k] for k in ("marca", "cantidad", "perfil", "longitud_mm", "kg_total")} for f in r["despiece"]],
            "soldaduras": [{k: s[k] for k in ("union", "cantidad", "simbolo", "metros")} for s in r["soldaduras"]],
            "ficheros": r["ficheros"], "aviso": r["aviso"],
            "nota": "Los planos, el PDF y la imagen están en la pestaña HERRERO y en los enlaces de ficheros."}


def herrero_perfil(args, ctx):
    from ...office import herrero_calculo as C
    nombre = str(args.get("perfil") or "").strip()
    if not nombre:
        return {"series": {s: [p.nombre for p in lista] for s, lista in C.SERIES.items()}}
    p = C.perfil(nombre)
    out = {"perfil": p.publico()}
    acero = str(args.get("acero") or "S275").upper()[:4]
    luz = float(args.get("luz_m") or 0) * 1000
    q = float(args.get("carga_kn_m") or 0)
    if luz and q:
        out["viga"] = C.comprobar_viga(p, acero, luz, 1.5 * q, q, float(args.get("flecha_l") or 300),
                                       str(args.get("apoyo") or "biapoyada"))
    n = float(args.get("axil_kn") or 0) * 1000
    if luz and n:
        out["pilar"] = C.comprobar_pilar(p, acero, luz, 1.5 * n, 0, float(args.get("beta") or 1), float(args.get("beta") or 1))
    if luz or n:
        out["nota"] = "Carga y axil son característicos: se mayoran x1,5 (ELU) y se añade el peso propio."
    return out


def herrero_soldadura(args, ctx):
    from ...office import herrero_calculo as C
    return C.soldadura_angulo(float(args["espesor1_mm"]), float(args.get("espesor2_mm") or args["espesor1_mm"]),
                              str(args.get("acero") or "S275").upper()[:4], float(args.get("fuerza_kn") or 0),
                              bool(args.get("doble", True)))


_TIPOS = ["porche", "pergola", "nave", "entreplanta", "escalera", "barandilla", "pasarela"]
TOOLS = [
    ({"name": "herrero_proyecto",
      "description": "Oficina Herrero: a partir de un encargo de estructura metálica («porche de 6x4 m galvanizado», "
                     "«entreplanta de 8x5 a 3 m para almacén», «escalera de 2,8 m»...) calcula cargas (CTE) y perfiles "
                     "(Eurocódigo 3), y genera despiece, lista de corte, uniones soldadas ISO 2553, plano de montaje, "
                     "vista isométrica, PDF e imagen realista. Devuelve el resumen y los enlaces",
      "parameters": {"type": "object", "properties": {
          "encargo": {"type": "string", "description": "lo que pide el cliente, con medidas"},
          "tipo": {"type": "string", "enum": _TIPOS, "description": "si no se deduce del encargo"},
          "largo": {"type": "number", "description": "m"}, "ancho": {"type": "number", "description": "m (luz)"},
          "alto": {"type": "number", "description": "m (altura libre o a salvar)"},
          "acero": {"type": "string", "enum": ["S235", "S275", "S355"]},
          "acabado": {"type": "string", "enum": ["galvanizado", "pintado", "oxiron", "blanco", "verde", "corten"]},
          "cubierta": {"type": "string", "enum": ["sandwich", "chapa", "policarbonato", "teja", "lamas", "ninguna"]},
          "uso": {"type": "string", "enum": ["mantenimiento", "vivienda", "oficinas", "publico", "comercial", "almacen", "industrial"]},
          "forjado": {"type": "string", "enum": ["tramex", "chapa", "tablero", "colaborante"]},
          "altitud": {"type": "number", "description": "m sobre el nivel del mar (nieve)"},
          "zona_viento": {"type": "string", "enum": ["A", "B", "C"]},
          "imagen_ia": {"type": "boolean", "default": True}},
          "required": ["encargo"]},
      "risk": "low", "permissions": ["filesystem"], "timeout": 600}, herrero_proyecto),
    ({"name": "herrero_perfil",
      "description": "Propiedades de un perfil de acero (IPE, HEA, HEB, UPN, tubo 80x80x3, Ø, pletina, L) y, con luz y "
                     "carga o axil, su comprobación como viga o pilar. Sin perfil, lista las series",
      "parameters": {"type": "object", "properties": {
          "perfil": {"type": "string"}, "acero": {"type": "string", "enum": ["S235", "S275", "S355"]},
          "luz_m": {"type": "number"}, "carga_kn_m": {"type": "number", "description": "carga característica kN/m"},
          "axil_kn": {"type": "number"}, "apoyo": {"type": "string", "enum": ["biapoyada", "continua", "voladizo"]},
          "beta": {"type": "number", "description": "longitud de pandeo / altura"},
          "flecha_l": {"type": "number", "description": "límite L/x de la flecha (300)"}}},
      "risk": "low", "permissions": []}, herrero_perfil),
    ({"name": "herrero_soldadura",
      "description": "Garganta de una soldadura en ángulo entre dos chapas (EN 1993-1-8): mínima, máxima, recomendada, "
                     "resistencia por cm, longitud para una fuerza y símbolo ISO 2553",
      "parameters": {"type": "object", "properties": {
          "espesor1_mm": {"type": "number"}, "espesor2_mm": {"type": "number"},
          "acero": {"type": "string", "enum": ["S235", "S275", "S355"]}, "fuerza_kn": {"type": "number"},
          "doble": {"type": "boolean", "default": True}}, "required": ["espesor1_mm"]},
      "risk": "low", "permissions": []}, herrero_soldadura),
]
