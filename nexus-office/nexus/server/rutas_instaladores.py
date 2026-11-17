"""Rutas de los instaladores sin conexion (nexus/instaladores.py): ver que hay, bajarlos una vez de la Release,
importarlos de una carpeta, borrarlos, descargarlos desde este equipo y repartirlos por la red local."""
from __future__ import annotations

import asyncio

from fastapi import Body, Depends, HTTPException
from fastapi.responses import FileResponse


def registrar(app, require_admin) -> None:
    admin = [Depends(require_admin)]

    def m():
        from ..instaladores import maletin
        return maletin()

    def _err(fn, *a):
        try:
            return fn(*a)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/instaladores")
    def instaladores_estado():
        return m().estado()

    @app.post("/api/instaladores/release", dependencies=admin)
    async def instaladores_release():
        try:
            await asyncio.to_thread(m().actualizar_release)
        except Exception as e:  # noqa: BLE001 - sin conexion o GitHub no responde
            raise HTTPException(502, f"No se ha podido consultar la Release (hace falta conexión una vez): {e}") from e
        return m().estado()

    @app.post("/api/instaladores/descargar", dependencies=admin)
    async def instaladores_descargar(ids: list[str] = Body(..., embed=True)):
        try:
            return await asyncio.to_thread(m().descargar, ids)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"Sin conexión con GitHub: {e}") from e

    @app.post("/api/instaladores/cancelar", dependencies=admin)
    def instaladores_cancelar():
        m().cancelar()
        return m().estado()

    @app.post("/api/instaladores/revisar", dependencies=admin)
    async def instaladores_revisar():
        return await asyncio.to_thread(m().revisar)

    @app.post("/api/instaladores/importar", dependencies=admin)
    async def instaladores_importar(carpeta: str = Body(..., embed=True)):
        return await asyncio.to_thread(_err, m().importar, carpeta)

    @app.delete("/api/instaladores/{ident}", dependencies=admin)
    def instaladores_borrar(ident: str):
        _err(m().borrar, ident)
        return m().estado()

    @app.post("/api/instaladores/compartir", dependencies=admin)
    def instaladores_compartir(activo: bool = Body(True, embed=True), puerto: int = Body(8776, embed=True)):
        if not 1024 <= puerto <= 65535:
            raise HTTPException(400, "Puerto entre 1024 y 65535")
        try:
            return m().compartir(activo, puerto)
        except OSError as e:
            raise HTTPException(409, f"No se puede abrir el puerto {puerto}: {e}") from e

    @app.get("/api/instaladores/fichero/{ident}")
    def instaladores_fichero(ident: str):
        from ..instaladores import CATALOGO
        if ident not in {c[0] for c in CATALOGO}:
            raise HTTPException(404, "instalador desconocido")
        ruta, nombre, _ = m().localizar(ident)
        if not ruta:
            raise HTTPException(404, "no está en este equipo")
        return FileResponse(ruta, filename=nombre, media_type="application/octet-stream")
