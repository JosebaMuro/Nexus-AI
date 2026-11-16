"""Rutas de la oficina Herrero (estructuras metalicas): calcular un encargo, ver y borrar proyectos y consultar
perfiles y soldaduras. Los ficheros (PDF, SVG, CSV, imagenes) se sirven desde /outputs/herrero/<id>/."""
from __future__ import annotations

import asyncio

from fastapi import Body, Depends, HTTPException

from ..tools.base import ToolContext

_HERRAMIENTAS = {"herrero_perfil", "herrero_soldadura"}


def registrar(app, S, require_admin) -> None:
    admin = [Depends(require_admin)]

    @app.get("/api/herrero")
    def herrero_estado():
        from ..office import herrero
        from ..office import herrero_calculo as C
        from ..office import herrero_modelo as M
        try:
            motor = S().images.status()
        except Exception:  # noqa: BLE001
            motor = {"engine": "escena"}
        return {"aviso": herrero.AVISO,
                "tipos": {k: v[1] for k, v in M.TIPOS.items()},
                "aceros": list(C.ACEROS), "acabados": {k: v[0] for k, v in M.ACABADOS.items()},
                "cubiertas": {k: v[0] for k, v in C.CUBIERTAS.items()}, "usos": {k: v[0] for k, v in C.USOS.items()},
                "forjados": {k: v[0] for k, v in C.FORJADOS.items()}, "zonas_viento": C.ZONAS_VIENTO,
                "series": {s: [p.nombre for p in lista] for s, lista in C.SERIES.items()},
                "imagen_ia": motor.get("engine") == "sd.cpp", "proyectos": herrero.proyectos()}

    @app.post("/api/herrero/proyecto")
    async def herrero_proyecto(encargo: str = Body("", embed=True), parametros: dict = Body(default={}, embed=True),
                               imagen_ia: bool = Body(True, embed=True)):
        from ..office import herrero
        if not encargo.strip() and not parametros.get("tipo"):
            raise HTTPException(400, "Describe la estructura o elige el tipo")
        try:
            return await asyncio.to_thread(herrero.proyecto, encargo, parametros, S(), imagen_ia)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/herrero/proyectos")
    def herrero_proyectos(limite: int = 30):
        from ..office import herrero
        return {"proyectos": herrero.proyectos(min(max(1, limite), 200))}

    @app.get("/api/herrero/proyecto/{pid}")
    def herrero_ver(pid: str):
        from ..office import herrero
        try:
            return herrero.cargar(pid)
        except KeyError as e:
            raise HTTPException(404, "proyecto no encontrado") from e

    @app.delete("/api/herrero/proyecto/{pid}", dependencies=admin)
    def herrero_borrar(pid: str):
        from ..office import herrero
        try:
            return {"borrado": herrero.borrar(pid)}
        except KeyError as e:
            raise HTTPException(404, "proyecto no encontrado") from e

    @app.post("/api/herrero/herramienta/{nombre}")
    async def herrero_herramienta(nombre: str, args: dict = Body(default={})):
        if nombre not in _HERRAMIENTAS:
            raise HTTPException(404, "herramienta no disponible aquí")
        s = S()
        ctx = ToolContext(services=s, agent="interfaz", session_id="herrero")
        r = await asyncio.to_thread(s.supervisor.execute, nombre, args, ctx)
        if isinstance(r, dict) and r.get("error") and len(r) == 1:
            raise HTTPException(400, str(r["error"]))
        return r
