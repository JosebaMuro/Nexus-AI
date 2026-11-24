"""Rutas de los turnos de modelos (nexus/hardware/turnos.py): estado y modo de la carga por etapas."""
from __future__ import annotations

from fastapi import Body, Depends, HTTPException

from ..hardware.turnos import MODOS


def registrar(app, S, require_admin) -> None:
    @app.get("/api/turnos")
    def turnos_estado():
        return S().turnos.estado()

    @app.post("/api/turnos", dependencies=[Depends(require_admin)])
    def turnos_modo(body: dict = Body(...)):
        s = S()
        if "modo" in body:
            if body["modo"] not in MODOS:
                raise HTTPException(400, f"modo: {', '.join(MODOS)}")
            s.config.set("hardware.alternar_modelos", body["modo"])
        if "reserva_ram_mb" in body:
            s.config.set("hardware.reserva_ram_mb", max(0, int(body["reserva_ram_mb"] or 0)))
        s.turnos._cache = (0.0, False, {})
        return s.turnos.estado()
