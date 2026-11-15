"""Herramientas del Terminal IA (nexus/terminal): consola con sesion, interprete con memoria, procesos de fondo y
ficheros con deshacer. Sin aislamiento: ven todo el equipo. En modo plan solo se puede leer y buscar."""
from __future__ import annotations


def _t(ctx):
    t = getattr(ctx.services, "terminal", None)
    if t is None or not t.cfg("activo", True):
        raise RuntimeError("El Terminal IA está apagado en Ajustes")
    return t


def _sid(args, ctx) -> str:
    return str(args.get("sesion") or ctx.session_id or "agente")


def _bloqueo(t, sid):
    aviso = t.solo_lectura(sid)
    return {"error": aviso} if aviso else None


def terminal_ejecutar(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    if ctx.remote:
        return {"error": "La consola de este equipo no se ofrece a otros nodos"}
    b = _bloqueo(t, sid)
    if b:
        return b
    if args.get("carpeta"):
        t.cambiar_carpeta(sid, args["carpeta"])
    if args.get("fondo"):
        return {"proceso": t.lanzar(args["orden"], sid, str(args.get("nombre") or "")),
                "nota": "Proceso de fondo en marcha: lee su salida con terminal_proceso"}

    def salida(texto: str) -> None:
        if ctx.emit:
            ctx.emit({"type": "terminal", "evento": "salida", "sesion": sid, "texto": texto[-4000:]})
    return t.ejecutar(args["orden"], sid, args.get("espera_s"), salida)


def terminal_codigo(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    if ctx.remote:
        return {"error": "El intérprete de este equipo no se ofrece a otros nodos"}
    b = _bloqueo(t, sid)
    if b:
        return b
    if args.get("reiniciar"):
        t.reiniciar_interprete(sid)
    return t.codigo(args.get("lenguaje") or "python", args["codigo"], sid, args.get("espera_s"))


def terminal_proceso(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    if ctx.remote:
        return {"error": "Los procesos de este equipo no se ofrecen a otros nodos"}
    accion = str(args.get("accion") or "lista")
    if accion == "lista":
        return {"procesos": t.procesos()}
    if accion == "leer":
        return t.leer_proceso(args["id"], int(args.get("desde") or 0), float(args.get("espera_s") or 0))
    b = _bloqueo(t, sid)
    if b:
        return b
    if accion == "lanzar":
        return t.lanzar(args["orden"], sid, str(args.get("nombre") or ""))
    if accion == "escribir":
        return t.escribir_proceso(args["id"], str(args.get("texto") or ""))
    if accion == "parar":
        return t.parar_proceso(args["id"])
    if accion == "interrumpir":
        return {"interrumpida": t.interrumpir(sid)}
    return {"error": "accion debe ser lista, lanzar, leer, escribir, parar o interrumpir"}


def terminal_leer(args, ctx):
    return _t(ctx).leer(args["ruta"], _sid(args, ctx), int(args.get("desde") or 1), int(args.get("lineas") or 400))


def terminal_escribir(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    return _bloqueo(t, sid) or t.escribir(args["ruta"], args.get("contenido") or "", sid)


def terminal_editar(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    b = _bloqueo(t, sid)
    if b:
        return b
    if args.get("deshacer"):
        return t.deshacer(sid, str(args.get("ruta") or ""))
    cambios = args.get("cambios") or [{"buscar": args.get("buscar"), "reemplazar": args.get("reemplazar"),
                                       "todas": args.get("todas")}]
    hechos = []
    for c in cambios:
        if c.get("buscar") is None or c.get("reemplazar") is None:
            return {"error": "Cada cambio necesita «buscar» y «reemplazar»", "hechos": hechos}
        try:
            hechos.append(t.editar(args["ruta"], str(c["buscar"]), str(c["reemplazar"]), sid, bool(c.get("todas"))))
        except ValueError as e:
            return {"error": str(e), "hechos": hechos}
    return hechos[0] if len(hechos) == 1 else {"ruta": hechos[-1]["ruta"], "cambios": len(hechos), "detalle": hechos}


def terminal_buscar(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    ruta = str(args.get("ruta") or ".")
    if args.get("arbol"):
        return t.arbol(ruta, sid, int(args.get("profundidad") or 3))
    if not args.get("patron"):
        return t.listar(ruta, sid, str(args.get("ficheros") or ""))
    return t.buscar(args["patron"], ruta, sid, str(args.get("ficheros") or ""), int(args.get("limite") or 120),
                    int(args.get("contexto") or 0))


def terminal_tareas(args, ctx):
    t, sid = _t(ctx), _sid(args, ctx)
    if args.get("modo"):
        return {"modo": t.modo(sid), "nota": "El modo lo cambia el usuario (botón Plan/Construir o /modo)"}
    return {"tareas": t.tareas(sid, args.get("tareas")), "modo": t.modo(sid)}


_SES = {"type": "string", "description": "sesión de terminal (por defecto, la de esta conversación)"}
_RUTA = {"type": "string", "description": "ruta relativa a la carpeta de la sesión o absoluta"}

TOOLS = [
    ({"name": "terminal_ejecutar", "description": "Ejecuta una orden en la consola del equipo (bash en Linux, macOS y "
      "Android; PowerShell en Windows). La sesión recuerda la carpeta (cd) y las variables exportadas. Devuelve la "
      "salida y el código de salida. Con fondo=true la deja en marcha (servidores, compilaciones largas)",
      "parameters": {"type": "object", "properties": {
          "orden": {"type": "string"}, "carpeta": {"type": "string", "description": "cambiar antes a esta carpeta"},
          "espera_s": {"type": "number", "description": "segundos antes de cortarla (por defecto 120)"},
          "fondo": {"type": "boolean", "default": False}, "nombre": {"type": "string"}, "sesion": _SES},
          "required": ["orden"]},
      "risk": "high", "permissions": ["code_exec", "system"], "timeout": 900}, terminal_ejecutar),
    ({"name": "terminal_codigo", "description": "Ejecuta código en un intérprete: python (conserva variables e "
      "importaciones entre llamadas, devuelve el valor de la última expresión), javascript (Node), consola o html",
      "parameters": {"type": "object", "properties": {
          "codigo": {"type": "string"},
          "lenguaje": {"type": "string", "enum": ["python", "javascript", "consola", "html"], "default": "python"},
          "espera_s": {"type": "number"}, "reiniciar": {"type": "boolean", "default": False}, "sesion": _SES},
          "required": ["codigo"]},
      "risk": "high", "permissions": ["code_exec"], "timeout": 900}, terminal_codigo),
    ({"name": "terminal_proceso", "description": "Procesos de fondo del terminal: lista, lanzar, leer su salida "
      "(desde = lo ya leído), escribir en su entrada, parar, o interrumpir la orden en curso de la sesión",
      "parameters": {"type": "object", "properties": {
          "accion": {"type": "string", "enum": ["lista", "lanzar", "leer", "escribir", "parar", "interrumpir"]},
          "id": {"type": "string"}, "orden": {"type": "string"}, "texto": {"type": "string"},
          "desde": {"type": "integer"}, "espera_s": {"type": "number"}, "nombre": {"type": "string"}, "sesion": _SES},
          "required": ["accion"]},
      "risk": "high", "permissions": ["code_exec", "system"], "timeout": 120}, terminal_proceso),
    ({"name": "terminal_leer", "description": "Lee un fichero de texto con números de línea, por tramos (desde, "
      "lineas). De una carpeta devuelve su contenido",
      "parameters": {"type": "object", "properties": {
          "ruta": _RUTA, "desde": {"type": "integer", "default": 1}, "lineas": {"type": "integer", "default": 400},
          "sesion": _SES}, "required": ["ruta"]},
      "risk": "low", "permissions": ["filesystem"]}, terminal_leer),
    ({"name": "terminal_escribir", "description": "Crea o sustituye un fichero entero (se guarda copia para deshacer). "
      "Para cambiar parte de un fichero usa terminal_editar",
      "parameters": {"type": "object", "properties": {"ruta": _RUTA, "contenido": {"type": "string"}, "sesion": _SES},
                     "required": ["ruta", "contenido"]},
      "risk": "medium", "permissions": ["filesystem"]}, terminal_escribir),
    ({"name": "terminal_editar", "description": "Cambia en un fichero un texto exacto por otro. «buscar» tiene que "
      "aparecer una sola vez (añade líneas de contexto) o usa todas=true. Admite varios cambios seguidos y "
      "deshacer=true para volver atrás el último cambio",
      "parameters": {"type": "object", "properties": {
          "ruta": _RUTA, "buscar": {"type": "string"}, "reemplazar": {"type": "string"},
          "todas": {"type": "boolean", "default": False},
          "cambios": {"type": "array", "items": {"type": "object"}, "description": "[{buscar, reemplazar, todas}]"},
          "deshacer": {"type": "boolean", "default": False}, "sesion": _SES}, "required": ["ruta"]},
      "risk": "medium", "permissions": ["filesystem"]}, terminal_editar),
    ({"name": "terminal_buscar", "description": "Busca en el proyecto: con «patron» (expresión regular) busca en el "
      "contenido como grep; sin él lista la carpeta o los ficheros que casan con «ficheros» (src/**/*.py); con "
      "arbol=true dibuja el árbol de carpetas",
      "parameters": {"type": "object", "properties": {
          "patron": {"type": "string"}, "ruta": _RUTA, "ficheros": {"type": "string"},
          "contexto": {"type": "integer", "default": 0}, "limite": {"type": "integer", "default": 120},
          "arbol": {"type": "boolean", "default": False}, "profundidad": {"type": "integer", "default": 3},
          "sesion": _SES}},
      "risk": "low", "permissions": ["filesystem"]}, terminal_buscar),
    ({"name": "terminal_tareas", "description": "Lista de tareas de un trabajo largo: pásala entera cada vez con el "
      "estado de cada una (pendiente, en_curso, hecha). Sin argumentos la devuelve con el modo de la sesión",
      "parameters": {"type": "object", "properties": {
          "tareas": {"type": "array", "items": {"type": "object"}, "description": "[{texto, estado}]"},
          "sesion": _SES}},
      "risk": "low", "permissions": []}, terminal_tareas),
]
