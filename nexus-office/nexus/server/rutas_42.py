"""Rutas de la 4.2: Mundo (fuentes por categorias y paises), terminal de bolsa y Terminal IA.

El Terminal IA pide modo administrador o la ficha de terminal.ficha (cabecera «Authorization: Bearer ...»), que
permite usarlo desde otros agentes y programas, como hace Open Terminal.
"""
from __future__ import annotations

import asyncio
import json
import queue
import secrets
import threading

from fastapi import Body, Depends, Header, HTTPException
from fastapi.responses import StreamingResponse

from .. import log
from ..tools.base import ToolContext

L = log.get("rutas42")
# herramientas de bolsa de riesgo bajo que la interfaz puede llamar tal cual
_BOLSA = {"mercado_consulta", "mercado_panel", "bolsa_verificar", "bolsa_opciones", "bolsa_riesgo", "bolsa_valoracion",
          "bolsa_criba", "bolsa_alerta", "bolsa_expediente", "bolsa_informe_oficial", "bolsa_validar",
          "bolsa_senales", "bolsa_backtest"}


def registrar(app, S, hub, require_admin, admin_flag) -> None:   # noqa: C901
    admin = [Depends(require_admin)]

    def _error(e: Exception) -> HTTPException:
        if isinstance(e, HTTPException):
            return e
        codigo = 404 if isinstance(e, (KeyError, FileNotFoundError)) else 400
        return HTTPException(codigo, str(e).strip("'\"") or type(e).__name__)

    # ------------------------------------------------------------------ Mundo
    @app.get("/api/mundo")
    def mundo_estado():
        return S().mundo.estado()

    @app.get("/api/mundo/resumen")
    def mundo_resumen(pais: str = "", categoria: str = ""):
        return S().mundo.resumen(pais, categoria)

    @app.get("/api/mundo/registros")
    def mundo_registros(categoria: str = "", pais: str = "", texto: str = "", fuente: str = "", horas: float = 0,
                        limite: int = 50, orden: str = "reciente"):
        return {"registros": S().mundo.consultar(categoria=categoria, pais=pais, texto=texto, fuente=fuente,
                                                 horas=horas, limite=min(max(1, limite), 500), orden=orden)}

    @app.get("/api/mundo/paises")
    def mundo_paises():
        from ..mundo import paises
        return {"paises": [{"codigo": p["a2"], "nombre": p["es"], "continente": p.get("cont", "")}
                           for p in sorted(paises.lista(), key=lambda p: p["es"])]}

    @app.post("/api/mundo/config", dependencies=admin)
    def mundo_config(cambios: dict = Body(...)):
        try:
            return S().mundo.configurar(cambios)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.post("/api/mundo/actualizar", dependencies=admin)
    async def mundo_actualizar(fuente: str = Body("", embed=True)):
        m = S().mundo
        if fuente and fuente not in m.fuentes:
            raise HTTPException(404, f"fuente desconocida: {fuente}")
        if fuente:
            return await asyncio.to_thread(m.actualizar, fuente)
        hechas = await asyncio.to_thread(m.ciclo)
        return {"pedidas": hechas}

    # ------------------------------------------------------------------ terminal de bolsa
    @app.get("/api/mercados/recetas")
    def mercados_recetas():
        from ..office import bolsa_analisis as ba, mercados
        return {"recetas": [{"receta": k, "para": v[0], "parametros": v[1]} for k, v in mercados.RECETAS.items()],
                "cribas": {k: v[0] for k, v in ba.CRIBAS.items()},
                "estrategias": {k: v[0] for k, v in ba.ESTRATEGIAS.items()},
                "escenarios": {k: v[0] for k, v in ba.ESCENARIOS.items()}}

    @app.post("/api/mercados/herramienta/{nombre}")
    async def mercados_herramienta(nombre: str, args: dict = Body(default={}), sesion: str = "bolsa"):
        if nombre not in _BOLSA:
            raise HTTPException(404, "herramienta no disponible aquí")
        s = S()
        ctx = ToolContext(services=s, agent="interfaz", session_id=sesion)
        r = await asyncio.to_thread(s.supervisor.execute, nombre, args, ctx)
        if isinstance(r, dict) and r.get("error") and len(r) == 1:
            raise HTTPException(400, str(r["error"]))
        return r

    @app.get("/api/mercados/escritorio")
    async def mercados_escritorio(datos: bool = True, sesion: str = "bolsa"):
        from ..office import bolsa_terminal
        if not datos:
            return {"paneles": bolsa_terminal.escritorio()}
        return {"paneles": await asyncio.to_thread(bolsa_terminal.escritorio_datos, None, sesion)}

    @app.post("/api/mercados/escritorio", dependencies=admin)
    def mercados_escritorio_guardar(paneles: list = Body(..., embed=True)):
        from ..office import bolsa_terminal
        try:
            return {"paneles": bolsa_terminal.escritorio_guardar(paneles)}
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    # ------------------------------------------------------------------ Terminal IA
    def acceso_terminal(x_admin_token: str | None = Header(default=None),
                        authorization: str | None = Header(default=None)) -> bool:
        s = S()
        if not s.terminal.cfg("activo", True):
            raise HTTPException(403, "El Terminal IA está apagado en Ajustes")
        ficha = str(s.config.get("terminal.ficha", "") or "")
        dada = (authorization or "").removeprefix("Bearer ").strip()
        if ficha and dada and secrets.compare_digest(dada, ficha):
            return True
        return require_admin(x_admin_token)

    term = [Depends(acceso_terminal)]

    @app.get("/api/terminal", dependencies=term)
    def terminal_estado(sesion: str = ""):
        t = S().terminal
        e = t.estado()
        if sesion:
            s = t.sesion(sesion)
            e["sesion"] = s.publico()
            e["ordenes"] = [{k: o[k] for k in ("nombre", "descripcion")} for o in t.ordenes(s.carpeta).values()]
            e["habilidades"] = t.habilidades(s.carpeta)
            e["instrucciones"] = t.instrucciones(s.carpeta)[0]
        return e

    @app.post("/api/terminal/ejecutar", dependencies=term)
    async def terminal_ejecutar(orden: str = Body(..., embed=True), sesion: str = Body("principal", embed=True),
                                espera_s: float | None = Body(None, embed=True), fondo: bool = Body(False, embed=True),
                                flujo: bool = Body(False, embed=True)):
        t = S().terminal
        try:
            if fondo:
                return {"proceso": t.lanzar(orden, sesion)}
            if not flujo:
                return await asyncio.to_thread(t.ejecutar, orden, sesion, espera_s)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e
        cola: queue.Queue = queue.Queue()

        def trabajo() -> None:
            try:
                r = t.ejecutar(orden, sesion, espera_s, lambda x: cola.put({"t": "salida", "texto": x}))
                cola.put({"t": "fin", **{k: v for k, v in r.items() if k != "salida"}})
            except Exception as e:  # noqa: BLE001
                cola.put({"t": "fin", "error": str(e)})
            cola.put(None)

        threading.Thread(target=trabajo, daemon=True, name="nexus-term-flujo").start()

        async def lineas():
            while True:
                item = await asyncio.to_thread(cola.get)
                if item is None:
                    break
                yield json.dumps(item, ensure_ascii=False) + "\n"
        return StreamingResponse(lineas(), media_type="application/x-ndjson")

    @app.post("/api/terminal/interrumpir", dependencies=term)
    def terminal_interrumpir(sesion: str = Body("principal", embed=True)):
        return {"interrumpida": S().terminal.interrumpir(sesion)}

    @app.post("/api/terminal/codigo", dependencies=term)
    async def terminal_codigo(codigo: str = Body(..., embed=True), lenguaje: str = Body("python", embed=True),
                              sesion: str = Body("principal", embed=True), espera_s: float | None = Body(None, embed=True),
                              reiniciar: bool = Body(False, embed=True)):
        t = S().terminal
        if reiniciar:
            t.reiniciar_interprete(sesion)
        return await asyncio.to_thread(t.codigo, lenguaje, codigo, sesion, espera_s)

    @app.post("/api/terminal/sesion", dependencies=term)
    def terminal_sesion(sesion: str = Body("principal", embed=True), modo: str = Body("", embed=True),
                        carpeta: str = Body("", embed=True), tareas: list | None = Body(None, embed=True)):
        t = S().terminal
        try:
            if sesion in ("", "nueva"):   # la misma id vale para la consola y para la conversacion con el agente
                sesion = S().memory.new_session("Terminal IA")
            s = t.sesion(sesion)
            if modo:
                t.modo(sesion, modo)
            if carpeta:
                t.cambiar_carpeta(sesion, carpeta)
            if tareas is not None:
                t.tareas(sesion, tareas)
            return s.publico()
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.delete("/api/terminal/sesion/{sesion}", dependencies=term)
    def terminal_sesion_cerrar(sesion: str):
        return {"cerrada": S().terminal.cerrar_sesion(sesion)}

    @app.get("/api/terminal/ficheros", dependencies=term)
    def terminal_ficheros(ruta: str = ".", sesion: str = "principal", patron: str = "", arbol: bool = False):
        t = S().terminal
        try:
            return t.arbol(ruta, sesion) if arbol else t.listar(ruta, sesion, patron)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.get("/api/terminal/fichero", dependencies=term)
    def terminal_fichero(ruta: str, sesion: str = "principal", desde: int = 1, lineas: int = 3000):
        try:
            return S().terminal.leer(ruta, sesion, desde, lineas, numeradas=False)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.put("/api/terminal/fichero", dependencies=term)
    def terminal_fichero_guardar(ruta: str = Body(..., embed=True), contenido: str = Body(..., embed=True),
                                 sesion: str = Body("principal", embed=True)):
        try:
            return S().terminal.escribir(ruta, contenido, sesion)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.post("/api/terminal/editar", dependencies=term)
    def terminal_editar(ruta: str = Body(..., embed=True), buscar: str = Body(..., embed=True),
                        reemplazar: str = Body(..., embed=True), todas: bool = Body(False, embed=True),
                        sesion: str = Body("principal", embed=True)):
        try:
            return S().terminal.editar(ruta, buscar, reemplazar, sesion, todas)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.post("/api/terminal/deshacer", dependencies=term)
    def terminal_deshacer(ruta: str = Body("", embed=True), sesion: str = Body("principal", embed=True)):
        return S().terminal.deshacer(sesion, ruta)

    @app.get("/api/terminal/buscar", dependencies=term)
    async def terminal_buscar(patron: str, ruta: str = ".", sesion: str = "principal", ficheros: str = "",
                              limite: int = 120, contexto: int = 0):
        try:
            return await asyncio.to_thread(S().terminal.buscar, patron, ruta, sesion, ficheros, limite, contexto)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.get("/api/terminal/procesos", dependencies=term)
    def terminal_procesos():
        return {"procesos": S().terminal.procesos()}

    @app.get("/api/terminal/procesos/{pid}", dependencies=term)
    async def terminal_proceso(pid: str, desde: int = 0, espera_s: float = 0):
        try:
            return await asyncio.to_thread(S().terminal.leer_proceso, pid, desde, min(espera_s, 30))
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.post("/api/terminal/procesos/{pid}/entrada", dependencies=term)
    def terminal_proceso_entrada(pid: str, texto: str = Body(..., embed=True)):
        try:
            return S().terminal.escribir_proceso(pid, texto)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.delete("/api/terminal/procesos/{pid}", dependencies=term)
    def terminal_proceso_parar(pid: str):
        try:
            return S().terminal.parar_proceso(pid)
        except Exception as e:  # noqa: BLE001
            raise _error(e) from e

    @app.post("/api/terminal/expandir", dependencies=term)
    def terminal_expandir(texto: str = Body(..., embed=True), sesion: str = Body("principal", embed=True)):
        return {"texto": S().terminal.expandir(texto, sesion)}

    @app.post("/api/terminal/ficha", dependencies=admin)
    def terminal_ficha(quitar: bool = Body(False, embed=True)):
        """Crea (o quita) la ficha con la que otros programas y agentes usan la API del terminal."""
        nueva = "" if quitar else secrets.token_urlsafe(24)
        S().config.set("terminal.ficha", nueva)
        return {"ficha": nueva, "nota": "" if quitar else "Guárdala: no se vuelve a mostrar. Se envía en la cabecera "
                "«Authorization: Bearer <ficha>» a las rutas /api/terminal/..."}
