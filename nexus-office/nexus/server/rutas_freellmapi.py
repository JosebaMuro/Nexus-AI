"""Rutas de FreeLLMAPI (nexus/freellmapi.py): estado, arrancar y parar la incluida, ajustes, probar y usarla como
modelo principal o de un agente."""
from __future__ import annotations

import asyncio

from fastapi import Body, Depends, HTTPException

CAMPOS = {"url": str, "clave": str, "modelo": str, "arrancar": bool, "red": bool}


def registrar(app, S, require_admin) -> None:
    admin = [Depends(require_admin)]

    def fl():
        return S().freellmapi

    @app.get("/api/freellmapi")
    async def freellmapi_estado():
        return await asyncio.to_thread(fl().estado)

    @app.post("/api/freellmapi/ajustes", dependencies=admin)
    async def freellmapi_ajustes(body: dict = Body(...)):
        s = S()
        for k, tipo in CAMPOS.items():
            if k not in body:
                continue
            v = body[k]
            if k == "clave" and v == "********":
                continue
            if tipo is bool:
                v = bool(v)
            else:
                v = str(v or "").strip()
            if k == "url" and v and not v.startswith(("http://", "https://")):
                raise HTTPException(400, "La URL empieza por http:// o https://")
            s.config.set(f"freellmapi.{k}", v)
        if "modelo" in body and str(s.config.get("llm.default_model", "")).startswith("freellmapi:"):
            s.config.set("llm.default_model", f"freellmapi:{s.config.get('freellmapi.modelo') or 'auto'}")
        s.llm.actualizar_freellmapi()
        return await asyncio.to_thread(fl().estado)

    @app.post("/api/freellmapi/arrancar", dependencies=admin)
    async def freellmapi_arrancar():
        try:
            return await asyncio.to_thread(fl().arrancar)
        except RuntimeError as e:
            raise HTTPException(400, str(e)) from e
        except OSError as e:
            raise HTTPException(500, f"No se pudo arrancar FreeLLMAPI: {e}") from e

    @app.post("/api/freellmapi/parar", dependencies=admin)
    async def freellmapi_parar():
        return await asyncio.to_thread(fl().parar)

    @app.get("/api/freellmapi/registro", dependencies=admin)
    def freellmapi_registro(n: int = 80):
        return {"lineas": fl().registro(max(1, min(n, 500)))}

    @app.get("/api/freellmapi/modelos")
    async def freellmapi_modelos():
        try:
            return {"modelos": await asyncio.to_thread(fl().modelos)}
        except Exception as e:  # noqa: BLE001 - sin conexion o sin clave
            raise HTTPException(502, f"FreeLLMAPI no responde en {fl().url()}: {e}") from e

    @app.post("/api/freellmapi/probar", dependencies=admin)
    async def freellmapi_probar(texto: str = Body("Responde solo: OK", embed=True)):
        try:
            return await asyncio.to_thread(fl().probar, texto)
        except Exception as e:  # noqa: BLE001
            raise HTTPException(502, f"FreeLLMAPI: {e}") from e

    @app.post("/api/freellmapi/usar", dependencies=admin)
    def freellmapi_usar(principal: bool = Body(True, embed=True)):
        """principal=True: FreeLLMAPI responde por defecto (llm.default_model = freellmapi:<modelo>).
        principal=False: vuelve a los modelos locales (el mejor para este hardware)."""
        s = S()
        if principal:
            s.config.set("llm.default_model", f"freellmapi:{s.config.get('freellmapi.modelo', 'auto') or 'auto'}")
        elif str(s.config.get("llm.default_model", "")).startswith("freellmapi:"):
            s.config.set("llm.default_model", "")
        s.llm.actualizar_freellmapi()
        return {"estado": fl().estado(), "llm": s.llm.status()}
