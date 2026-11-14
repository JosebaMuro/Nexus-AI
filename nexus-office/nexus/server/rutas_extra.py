"""Rutas de la 4.1 (paridad con la edicion C++ 3.x): hilo de la conversacion, ubicacion, MCP, laboratorio beta,
laboratorio de bolsa, video y voces. Se registran desde create_app()."""
from __future__ import annotations

import asyncio
import secrets
import time

from fastapi import Body, Depends, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import JSONResponse, PlainTextResponse, Response

from .. import __version__, beta, log, paths

L = log.get("rutas")


def registrar(app, S, hub, require_admin, admin_flag, LOCAL) -> None:   # noqa: C901
    admin = [Depends(require_admin)]

    def es_local(request: Request) -> bool:
        host = request.client.host if request.client else "127.0.0.1"
        return host in LOCAL and not request.scope.get("nexus_remote")

    # ------------------------------------------------------------------ tiempos de la API (laboratorio beta)
    @app.middleware("http")
    async def tiempos_api(request, call_next):
        t0 = time.perf_counter()
        resp = await call_next(request)
        ms = (time.perf_counter() - t0) * 1000
        ruta = request.scope.get("route")
        plantilla = getattr(ruta, "path", "") or ""
        try:
            activo = S().config.get("beta.tiempos", True)
        except KeyError:   # la aplicacion aun no ha arrancado sus servicios
            activo = False
        if plantilla.startswith(("/api/", "/mcp")) and activo:
            beta.anotar(request.method, plantilla, ms, resp.status_code)
            resp.headers["Server-Timing"] = f"app;dur={ms:.1f}"
        return resp

    @app.get("/api/beta")
    def beta_estado():
        return {"version": __version__, "tiempos": beta.tiempos(12), "banco": beta.ultimo_banco(),
                "opiniones": len(beta.opiniones(1000))}

    @app.get("/api/beta/http")
    def beta_http(limite: int = 60):
        return beta.tiempos(max(1, min(limite, 400)))

    @app.delete("/api/beta/http", dependencies=admin)
    def beta_http_borrar():
        beta.borrar_tiempos()
        return {"ok": True}

    @app.get("/api/beta/bench")
    def beta_banco_ultimo():
        return beta.ultimo_banco() or {"pruebas": []}

    @app.post("/api/beta/bench")
    async def beta_banco(llm: bool = Body(True, embed=True)):
        return await asyncio.to_thread(beta.banco, S(), llm)

    @app.post("/api/beta/opinion")
    def beta_opinion(texto: str = Body(...), nota: int = Body(0), vista: str = Body("")):
        try:
            return beta.opinar(texto, nota, vista)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    @app.get("/api/beta/opiniones")
    def beta_opiniones(limite: int = 100):
        return beta.opiniones(max(1, min(limite, 1000)))

    @app.get("/api/beta/informe")
    def beta_informe():
        return PlainTextResponse(beta.informe(), media_type="text/markdown; charset=utf-8")

    # ------------------------------------------------------------------ hilo de la conversacion
    def _n_ctx() -> int:
        return int((S().profile.get("llm") or {}).get("n_ctx") or 4096)

    @app.get("/api/sessions/{sid}/hilo")
    def hilo_estado(sid: str):
        return {**S().hilo.estado(sid), "n_ctx": _n_ctx(), "activo": bool(S().config.get("hilo.activo", True))}

    @app.put("/api/sessions/{sid}/hilo")
    def hilo_guardar(sid: str, cambios: dict = Body(...)):
        s = S()
        if not s.db.one("SELECT id FROM sessions WHERE id=?", (sid,)):
            raise HTTPException(404, "Conversación no encontrada")
        s.hilo.guardar(sid, {k: cambios[k] for k in ("notas", "resumen") if k in cambios})
        return s.hilo.estado(sid)

    @app.post("/api/sessions/{sid}/hilo/resumir")
    async def hilo_resumir(sid: str, todo: bool = Body(False, embed=True)):
        s = S()
        presupuesto = max(512, _n_ctx() * int(s.config.get("hilo.pct_contexto", 45)) // 100)
        n = await asyncio.to_thread(s.hilo.plegar, sid, 0 if todo else presupuesto // 2, presupuesto * 3 // 10,
                                    s.hilo.llm_complete)
        return {**s.hilo.estado(sid), "plegados_ahora": n}

    # ------------------------------------------------------------------ ubicacion
    @app.get("/api/ubicacion")
    def ubicacion_estado():
        from ..devices import ubicacion
        return ubicacion.estado()

    @app.post("/api/ubicacion/consultar")
    async def ubicacion_consultar(opciones: dict = Body(default={})):
        from ..devices import ubicacion
        return await asyncio.to_thread(ubicacion.consultar, opciones)

    @app.post("/api/ubicacion")
    def ubicacion_recibir(body: dict = Body(...)):
        """La interfaz web (o la app) manda la posicion del navegador; solo se guarda en memoria en este equipo."""
        from ..devices import ubicacion
        fuente = "app" if body.get("fuente") == "app" else "navegador"
        if fuente == "navegador" and not S().config.get("ubicacion.navegador", False):
            raise HTTPException(403, "La ubicación del navegador está desactivada en Ajustes")
        res = ubicacion.recibir(body, fuente)
        if res.get("error"):
            raise HTTPException(400, res["error"])
        return res

    @app.put("/api/ubicacion")
    def ubicacion_config(cambios: dict = Body(...)):
        from ..devices import ubicacion
        cfg = S().config
        permitidas = {"activa": bool, "navegador": bool, "fuente": str, "manual": str, "serie": str, "gpsd": str}
        nuevos = {}
        for k, tipo in permitidas.items():
            if k in cambios:
                nuevos[f"ubicacion.{k}"] = tipo(cambios[k]) if tipo is bool else str(cambios[k]).strip()[:120]
        if "fuente" in cambios and nuevos["ubicacion.fuente"] not in ("auto", *ubicacion.FUENTES):
            raise HTTPException(400, "Fuente no válida")
        if nuevos.get("ubicacion.manual") and not ubicacion.parse_coordenadas(nuevos["ubicacion.manual"]):
            lugar = ubicacion.buscar_lugar(nuevos["ubicacion.manual"], cfg.get("ubicacion.lugares", []))
            if not lugar:
                raise HTTPException(400, "Pon las coordenadas como «latitud, longitud» o el nombre de una ciudad conocida")
            nuevos["ubicacion.manual"] = f"{lugar['lat']}, {lugar['lon']}"
        if "lugares" in cambios:
            lugares = []
            for lu in cambios["lugares"] or []:
                if isinstance(lu, dict) and lu.get("nombre") and ubicacion.coordenadas_validas(lu.get("lat"), lu.get("lon")):
                    lugares.append({"nombre": str(lu["nombre"])[:60], "lat": float(lu["lat"]), "lon": float(lu["lon"])})
            nuevos["ubicacion.lugares"] = lugares[:100]
        for k, v in nuevos.items():
            cfg.set(k, v)
        return ubicacion.estado()

    # ------------------------------------------------------------------ MCP
    def _mcp_permitido(request: Request) -> None:
        s = S()
        if not s.config.get("mcp.servidor", True):
            raise HTTPException(404, "El servidor MCP está desactivado")
        if es_local(request):
            return
        token = s.config.get("mcp.token", "")
        dado = (request.headers.get("authorization") or "").removeprefix("Bearer ").strip()
        if not token or not dado or not secrets.compare_digest(token, dado):
            raise HTTPException(401, "Hace falta el token de MCP (Ajustes > MCP) en la cabecera Authorization")

    @app.post("/mcp")
    async def mcp_post(request: Request):
        from .. import mcp
        _mcp_permitido(request)
        try:
            mensaje = await request.json()
        except ValueError:
            return JSONResponse({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "JSON no válido"}},
                                status_code=400)
        cliente = request.client.host if request.client else "local"
        res = await asyncio.to_thread(mcp.atender, S(), mensaje, cliente, not es_local(request))
        if res is None:
            return Response(status_code=202)
        return JSONResponse(res)

    @app.get("/mcp")
    def mcp_get():
        return Response(status_code=405, headers={"Allow": "POST"})   # sin flujo SSE iniciado por el servidor

    @app.delete("/mcp")
    def mcp_delete():
        return Response(status_code=405, headers={"Allow": "POST"})

    @app.get("/api/mcp")
    def mcp_estado(es_admin: bool = Depends(admin_flag)):
        s = S()
        servidores = s.config.get("mcp.servidores", []) or []
        if not es_admin:   # sin variables de entorno ni cabeceras (pueden llevar claves)
            servidores = [{k: v for k, v in sv.items() if k not in ("env", "cabeceras")} for sv in servidores
                          if isinstance(sv, dict)]
        return {"servidor": bool(s.config.get("mcp.servidor", True)), "con_token": bool(s.config.get("mcp.token", "")),
                "url": f"http://127.0.0.1:{s.config.get('server.port', 8775)}/mcp", "orden": "nexus mcp",
                "herramientas": len([t for t in s.tools.for_agent(["*"]) if not t.name.startswith("mcp_")]),
                "servidores": servidores, "clientes": s.mcp.estado()}

    @app.put("/api/mcp/servidores", dependencies=admin)
    def mcp_servidores(servidores: list[dict] = Body(..., embed=True)):
        limpios = []
        for sv in servidores[:30]:
            nombre = "".join(c for c in str(sv.get("nombre", "")).lower() if c.isalnum() or c in "-_")[:24]
            if not nombre or not (sv.get("orden") or sv.get("url")):
                raise HTTPException(400, "Cada servidor necesita un nombre y una orden o una URL")
            fila = {"nombre": nombre, "activo": sv.get("activo", True) is not False,
                    "riesgo": sv.get("riesgo") if sv.get("riesgo") in ("low", "medium", "high") else "medium"}
            if sv.get("url"):
                if not str(sv["url"]).startswith(("http://", "https://")):
                    raise HTTPException(400, "La URL debe empezar por http:// o https://")
                fila["url"] = str(sv["url"])[:500]
                if isinstance(sv.get("cabeceras"), dict):
                    fila["cabeceras"] = {str(k): str(v) for k, v in sv["cabeceras"].items()}
            else:
                fila["orden"] = sv["orden"] if isinstance(sv["orden"], list) else str(sv["orden"])
                if isinstance(sv.get("env"), dict):
                    fila["env"] = {str(k): str(v) for k, v in sv["env"].items()}
            limpios.append(fila)
        S().config.set("mcp.servidores", limpios)
        S().mcp.conectar()
        return {"ok": True, "servidores": limpios}

    @app.put("/api/mcp", dependencies=admin)
    def mcp_ajustes(cambios: dict = Body(...)):
        cfg = S().config
        if "servidor" in cambios:
            cfg.set("mcp.servidor", bool(cambios["servidor"]))
        if cambios.get("nuevo_token"):
            cfg.set("mcp.token", secrets.token_urlsafe(24))
            return {"ok": True, "token": cfg.get("mcp.token")}   # se muestra una sola vez
        if cambios.get("quitar_token"):
            cfg.set("mcp.token", "")
        return {"ok": True}

    @app.post("/api/mcp/reconectar", dependencies=admin)
    async def mcp_reconectar():
        await asyncio.to_thread(S().mcp.conectar_ya)
        return S().mcp.estado()

    # ------------------------------------------------------------------ voces por agente
    @app.get("/api/voice/voces")
    async def voces():
        return await asyncio.to_thread(S().voice.voces)

    # ------------------------------------------------------------------ video
    @app.post("/api/video/rapido")
    async def video_rapido(args: dict = Body(default={})):
        from ..media.video import crear_video
        res = await asyncio.to_thread(crear_video, S(), args)
        if res.get("error"):
            raise HTTPException(400, res["error"])
        return res

    @app.post("/api/video/guardar")
    async def video_guardar(video: UploadFile = File(...), titulo: str = Form("video")):
        """El WebM/MP4 que monta el navegador (MediaRecorder) se guarda en outputs/videos."""
        ext = "." + (video.filename or "").rsplit(".", 1)[-1].lower()
        if ext not in (".webm", ".mp4"):
            ext = ".mp4" if "mp4" in (video.content_type or "") else ".webm"
        limpio = "".join(c if c.isalnum() else "-" for c in titulo.lower())[:40].strip("-") or "video"
        out = paths.sub("outputs", "videos") / f"{int(time.time())}_{limpio}{ext}"
        total = 0
        with open(out, "wb") as f:
            while True:
                trozo = await video.read(1 << 20)
                if not trozo:
                    break
                total += len(trozo)
                if total > 2 << 30:
                    f.close()
                    out.unlink(missing_ok=True)
                    raise HTTPException(413, "Vídeo demasiado grande")
                f.write(trozo)
        return {"url": f"/outputs/videos/{out.name}", "bytes": total}

    @app.get("/api/video")
    def videos(limite: int = 40):
        carpeta = paths.sub("outputs", "videos")
        ficheros = sorted((f for f in carpeta.iterdir() if f.suffix.lower() in (".avi", ".webm", ".mp4")),
                          key=lambda f: f.stat().st_mtime, reverse=True)[:limite]
        return [{"url": f"/outputs/videos/{f.name}", "nombre": f.name, "bytes": f.stat().st_size,
                 "ts": f.stat().st_mtime} for f in ficheros]

    # ------------------------------------------------------------------ laboratorio de bolsa
    def _series(simbolos: list[str], rango: str) -> list[dict]:
        from ..office import bolsa
        b = bolsa.get()
        out = []
        for sym in simbolos[:12]:
            try:
                d = b.serie(sym, rango)
            except ValueError as e:
                raise HTTPException(400, str(e)) from e
            out.append({"simbolo": d["simbolo"], "puntos": d["puntos"], "real": d.get("real", False)})
        return out

    def _lab(fn, *a, **kw):
        try:
            return fn(*a, **kw)
        except ValueError as e:
            raise HTTPException(400, str(e)) from e

    def _simbolos(data: dict) -> list[str]:
        syms = data.get("simbolos") or ([data["simbolo"]] if data.get("simbolo") else [])
        if isinstance(syms, str):
            syms = [x for x in syms.replace(";", ",").replace(" ", ",").split(",") if x]
        if not syms:
            raise HTTPException(400, "Indica al menos un valor")
        return [str(x).upper() for x in syms]

    @app.get("/api/bolsa/lab/senales/{simbolo}")
    async def lab_senales(simbolo: str, rango: str = "1y"):
        from ..office import bolsa_lab
        series = await asyncio.to_thread(_series, [simbolo], rango)
        s = bolsa_lab.senales(series[0]["puntos"])
        if s is None:
            raise HTTPException(400, "No hay histórico suficiente (hacen falta 60 sesiones)")
        return {"simbolo": series[0]["simbolo"], "real": series[0]["real"], **s}

    @app.post("/api/bolsa/lab/backtest")
    async def lab_backtest(data: dict = Body(...)):
        from ..office import bolsa_lab
        series = await asyncio.to_thread(_series, _simbolos(data), data.get("rango", "1y"))
        res = await asyncio.to_thread(_lab, bolsa_lab.backtest, series, data)
        res["datos_reales"] = all(s["real"] for s in series)
        return res

    @app.post("/api/bolsa/lab/montecarlo")
    async def lab_montecarlo(data: dict = Body(...)):
        from ..office import bolsa_lab
        series = await asyncio.to_thread(_series, _simbolos(data)[:1], data.get("rango", "1y"))
        valores = [p["precio"] for p in series[0]["puntos"]]
        return {"simbolo": series[0]["simbolo"],
                **_lab(bolsa_lab.montecarlo, valores, int(data.get("simulaciones", 500)), int(data.get("horizonte", 252)))}

    @app.post("/api/bolsa/lab/optimizar")
    async def lab_optimizar(data: dict = Body(...)):
        from ..office import bolsa_lab
        series = await asyncio.to_thread(_series, _simbolos(data), data.get("rango", "1y"))
        return await asyncio.to_thread(_lab, bolsa_lab.optimizar, series, data.get("metodo", "paridad_riesgo"), data)

    @app.post("/api/bolsa/lab/mesa")
    async def lab_mesa(data: dict = Body(...)):
        from ..office import bolsa_lab
        s = S()
        series = await asyncio.to_thread(_series, _simbolos(data)[:1], data.get("rango", "1y"))
        llm = None
        if data.get("ia", True) and s.llm.has_real_llm():
            llm = lambda p: s.llm.complete(p, max_tokens=320, temperature=0.2)   # noqa: E731
        res = await asyncio.to_thread(bolsa_lab.informe_mesa, series[0]["simbolo"], series[0]["puntos"], data, llm)
        if res.get("error"):
            raise HTTPException(400, res["error"])
        res["datos_reales"] = series[0]["real"]
        return res
