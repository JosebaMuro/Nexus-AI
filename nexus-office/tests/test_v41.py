"""Agente IA 4.1: lo traido de la edicion C++ 3.x (hilo, ubicacion, MCP, beta, lista de URLs, luz del grafo, voces,
laboratorio de bolsa y video)."""
import json
import math
import os
import struct
import sys
import textwrap

import pytest
from fastapi.testclient import TestClient


# ------------------------------------------------------------------ hilo de las conversaciones largas
def test_hilo_pliega_y_mantiene_notas(services):
    from nexus.memory import hilo as H
    s = services
    sid = s.memory.new_session()
    for i in range(30):
        s.memory.add_message(sid, "user", f"Pregunta {i}: el presupuesto del proyecto Atlas es de {1000 + i} euros. " * 6)
        s.memory.add_message(sid, "assistant", f"Respuesta {i}: anotado el presupuesto del proyecto Atlas. " * 6)
    s.hilo.guardar(sid, {"notas": "El cliente se llama Iratxe y paga a 60 dias."})
    antes = s.hilo.plan(sid, "presupuesto Atlas", 800)
    assert antes.notas.startswith("El cliente") and antes.recientes and antes.omitidos > 0
    assert antes.tokens <= 800 * 1.3
    n = s.hilo.plegar(sid, 400, 200)   # sin LLM: resumen extractivo
    assert n > 0
    st = s.hilo.estado(sid)
    assert st["plegados"] == n and "Usuario:" in st["resumen"] and st["tokens_resumen"] <= 210
    plan = s.hilo.plan(sid, "presupuesto Atlas", 800)
    assert "Resumen de lo hablado" in plan.texto() and "Iratxe" in plan.texto()
    assert plan.recientes[-1]["content"].startswith("Respuesta 29")
    assert s.hilo.plegar(sid, 10 ** 6, 200) == 0   # cabe: no se pliega mas
    assert H.recortar("a" * 4000, 50).count("[...]") == 1
    s.hilo.olvidar(sid)
    assert s.hilo.estado(sid)["resumen"] == ""


def test_hilo_con_llm_y_en_el_prompt(services):
    s = services
    sid = s.memory.new_session()
    for i in range(12):
        s.memory.add_message(sid, "user", f"dato {i} " * 80)
        s.memory.add_message(sid, "assistant", f"vale {i} " * 80)
    s.hilo.llm_real = lambda: True
    assert s.hilo.plegar(sid, 300, 150, lambda p, n: "- Resumen hecho por el modelo con los datos clave") > 0
    assert "hecho por el modelo" in s.hilo.estado(sid)["resumen"]
    s.hilo.guardar(sid, {"notas": "NOTA-FIJA-XYZ"})
    vistos = []

    def falso(msgs, spec="default", **kw):
        vistos.append(msgs)
        yield "de acuerdo"
    real = s.llm.stream
    s.llm.stream = falso
    try:
        eventos = []
        s.agents.run("nexus", "sigue con el proyecto", sid, emit=eventos.append)
    finally:
        s.llm.stream = real
    sistema = vistos[0][0]["content"]
    assert "NOTA-FIJA-XYZ" in sistema and "hecho por el modelo" in sistema
    assert any(e["type"] == "hilo" for e in eventos)
    with TestClient(create(services)) as c:
        r = c.get(f"/api/sessions/{sid}/hilo").json()
        assert r["notas"] == "NOTA-FIJA-XYZ" and r["n_ctx"] > 0
        assert c.put(f"/api/sessions/{sid}/hilo", json={"notas": "otra"}).json()["notas"] == "otra"
        assert "plegados_ahora" in c.post(f"/api/sessions/{sid}/hilo/resumir", json={"todo": True}).json()
        assert c.put("/api/sessions/noexiste/hilo", json={"notas": "x"}).status_code == 404
        assert "Server-Timing" in c.get(f"/api/sessions/{sid}/hilo").headers


def create(services):
    from nexus.server.app import create_app
    return create_app(services)


# ------------------------------------------------------------------ ubicacion
def _nmea(cuerpo: str) -> str:
    x = 0
    for ch in cuerpo:
        x ^= ord(ch)
    return f"${cuerpo}*{x:02X}"


def test_ubicacion_piezas():
    from nexus.devices import ubicacion as U
    gga = _nmea("GPGGA,123519,4315.780,N,00256.100,W,1,08,0.9,19.0,M,46.9,M,,")
    rmc = _nmea("GPRMC,123519,A,4315.780,N,00256.100,W,10.0,084.4,230394,003.1,W")
    f = U.parse_nmea(gga + "\r\n" + rmc + "\r\n")
    assert abs(f["lat"] - 43.263) < 0.001 and abs(f["lon"] + 2.935) < 0.001
    assert f["satelites"] == 8 and f["altitud"] == 19.0 and abs(f["velocidad"] - 5.14) < 0.01
    assert U.parse_nmea(gga.replace("4315", "4415")) is None            # suma de control rota
    g = U.parse_gpsd('{"class":"VERSION"}\n{"class":"TPV","mode":1}\n'
                     '{"class":"TPV","mode":3,"lat":40.4168,"lon":-3.7038,"eph":4.5,"speed":1.2}\n')
    assert g["lat"] == 40.4168 and g["precision"] == 4.5 and g["fuente"] == "gpsd"
    assert U.parse_coordenadas("43,26; -2,93") == (43.26, -2.93) and U.parse_coordenadas("hola") is None
    assert not U.coordenadas_validas(91, 0) and not U.coordenadas_validas(0, 0)
    assert abs(U.distancia_km(43.263, -2.935, 40.4168, -3.7038) - 323) < 4     # Bilbao - Madrid
    assert U.rumbo_texto(U.rumbo_grados(43.263, -2.935, 40.4168, -3.7038)) in ("sur", "suroeste")
    c = U.ciudad_cercana(43.27, -2.94)
    assert c["nombre"] == "Bilbao" and c["distancia_km"] < 3
    assert U.buscar_lugar("bilbao")["lat"] > 43 and U.buscar_lugar("Casa", [{"nombre": "casa", "lat": 1.5, "lon": 2.5}])["lon"] == 2.5


def test_ubicacion_api_y_herramienta(services):
    from nexus.devices import ubicacion as U
    from nexus.tools import ToolContext
    U._recibidas.clear()
    U._ultima = None
    s = services
    s.config.set("ubicacion.gpsd", "127.0.0.1:1")   # sin gpsd
    with TestClient(create(s)) as c:
        st = c.get("/api/ubicacion").json()
        assert st["activa"] and any(f["id"] == "manual" for f in st["fuentes"])
        assert c.post("/api/ubicacion/consultar", json={"timeout_s": 1}).json().get("error")
        assert c.post("/api/ubicacion", json={"lat": 43.26, "lon": -2.93}).status_code == 403   # navegador apagado
        assert c.put("/api/ubicacion", json={"manual": "Donostia"}).status_code == 400
        st = c.put("/api/ubicacion", json={"manual": "Bilbao", "navegador": True,
                                           "lugares": [{"nombre": "Oficina", "lat": 43.3, "lon": -2.0}]}).json()
        assert st["navegador"] and st["manual"].startswith("43.")
        r = c.post("/api/ubicacion/consultar", json={"timeout_s": 1, "hasta": "Madrid"}).json()
        assert r["fuente"] == "manual" and "aviso" in r and 300 < r["hasta"]["distancia_km"] < 340
        assert c.post("/api/ubicacion", json={"lat": 41.65, "lon": -0.88, "precision": 12}).json()["ok"]
        r = c.post("/api/ubicacion/consultar", json={"timeout_s": 1, "hasta": "Oficina"}).json()
        assert r["fuente"] == "navegador" and r["cerca_de"]["nombre"] == "Zaragoza" and r["hasta"]["nombre"] == "Oficina"
        r = s.supervisor.execute("ubicacion_actual", {}, ToolContext(services=s, agent="nexus"))
        assert r["fuente"] == "navegador" and "privacidad" in r
        r = s.supervisor.execute("ubicacion_actual", {}, ToolContext(services=s, agent="nexus", remote=True))
        assert "error" in r     # no se comparte con otros nodos
    U._recibidas.clear()
    U._ultima = None


# ------------------------------------------------------------------ MCP
def test_mcp_servidor(services):
    s = services
    with TestClient(create(s)) as c:
        ini = c.post("/mcp", json={"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                   "params": {"protocolVersion": "2025-03-26"}}).json()
        assert ini["result"]["protocolVersion"] == "2025-03-26" and ini["result"]["serverInfo"]["name"] == "agente-ia"
        assert c.post("/mcp", json={"jsonrpc": "2.0", "method": "notifications/initialized"}).status_code == 202
        lote = c.post("/mcp", json=[{"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                                    {"jsonrpc": "2.0", "id": 3, "method": "ping"}]).json()
        nombres = {t["name"] for t in lote[0]["result"]["tools"]}
        assert "calculator" in nombres and "ubicacion_actual" in nombres and lote[1]["result"] == {}
        r = c.post("/mcp", json={"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                                 "params": {"name": "calculator", "arguments": {"expression": "6*7"}}}).json()
        assert "42" in r["result"]["content"][0]["text"] and r["result"]["isError"] is False
        assert c.post("/mcp", json={"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                                    "params": {"name": "noexiste"}}).json()["error"]["code"] == -32602
        assert c.post("/mcp", json={"jsonrpc": "2.0", "id": 6, "method": "otra/cosa"}).json()["error"]["code"] == -32601
        assert c.post("/mcp", content=b"{mal").status_code == 400
        assert c.get("/mcp").status_code == 405
        est = c.get("/api/mcp").json()
        assert est["servidor"] and est["herramientas"] > 10 and est["orden"] == "nexus mcp"
        s.config.set("mcp.servidor", False)
        assert c.post("/mcp", json={"jsonrpc": "2.0", "id": 7, "method": "ping"}).status_code == 404
        s.config.set("mcp.servidor", True)
        assert s.db.one("SELECT agent FROM tool_calls WHERE tool='calculator' ORDER BY id DESC")["agent"].startswith("mcp:")


SERVIDOR_MCP = textwrap.dedent('''
    import json, sys
    for linea in sys.stdin:
        m = json.loads(linea)
        if "id" not in m:
            continue
        if m["method"] == "initialize":
            r = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "eco", "version": "1"}}
        elif m["method"] == "tools/list":
            r = {"tools": [{"name": "Decir-Eco", "description": "repite", "inputSchema": {"type": "object",
                 "properties": {"texto": {"type": "string"}}, "required": ["texto"]}},
                 {"name": "falla", "description": "siempre falla", "inputSchema": {"type": "object"}}]}
        elif m["method"] == "tools/call":
            if m["params"]["name"] == "falla":
                r = {"content": [{"type": "text", "text": "no pude"}], "isError": True}
            else:
                r = {"content": [{"type": "text", "text": "eco: " + m["params"]["arguments"]["texto"]}]}
        else:
            print(json.dumps({"jsonrpc": "2.0", "id": m["id"], "error": {"code": -32601, "message": "no"}}), flush=True)
            continue
        print(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": r}), flush=True)
''')


def test_mcp_cliente_stdio(services, tmp_path):
    from nexus import mcp
    from nexus.tools import ToolContext
    s = services
    guion = tmp_path / "servidor_mcp.py"
    guion.write_text(SERVIDOR_MCP, encoding="utf-8")
    assert mcp.nombre_herramienta("Mi Servidor", "Decir-Eco") == "mcp_mi_servidor_decir_eco"
    s.config.set("mcp.servidores", [{"nombre": "eco", "orden": [sys.executable, str(guion)], "riesgo": "low"},
                                    {"nombre": "roto", "orden": [sys.executable, "-c", "raise SystemExit(1)"]},
                                    {"nombre": "apagado", "url": "http://127.0.0.1:1/mcp", "activo": False}])
    try:
        s.mcp.conectar_ya()
        est = s.mcp.estado()
        assert est["eco"]["conectado"] and est["eco"]["herramientas"] == ["mcp_eco_decir_eco", "mcp_eco_falla"]
        assert not est["roto"]["conectado"] and est["roto"]["error"] and est["apagado"]["activo"] is False
        ctx = ToolContext(services=s, agent="nexus")
        assert s.supervisor.execute("mcp_eco_decir_eco", {"texto": "hola"}, ctx)["resultado"] == "eco: hola"
        assert s.supervisor.execute("mcp_eco_falla", {}, ctx)["error"] == "no pude"
        s.tools.reload()   # las herramientas externas sobreviven a recargar el registro
        assert s.tools.get("mcp_eco_decir_eco") is not None
        with TestClient(create(s)) as c:   # las de otros servidores MCP no se reexportan
            nombres = {t["name"] for t in c.post("/mcp", json={"jsonrpc": "2.0", "id": 1, "method": "tools/list"}).json()["result"]["tools"]}
            assert not any(n.startswith("mcp_") for n in nombres)
        s.config.set("mcp.servidores", [])
        s.mcp.conectar_ya()
        assert s.tools.get("mcp_eco_decir_eco") is None
    finally:
        s.mcp.cerrar()


def test_mcp_resultados():
    from nexus import mcp
    assert mcp.resultado_mcp({"error": "x"})["isError"] and mcp.resultado_mcp({"a": 1})["structuredContent"] == {"a": 1}
    assert mcp.resultado_a_json({"content": [{"type": "text", "text": "a"}, {"type": "image", "mimeType": "image/png"}]}) == \
        {"resultado": "a\n[image image/png]"}
    assert mcp.atender(None, {"jsonrpc": "1.0"})["error"]["code"] == -32600
    assert mcp.atender(None, [])["error"]["code"] == -32600


# ------------------------------------------------------------------ beta
def test_beta_banco_tiempos_y_opiniones(services):
    from nexus import beta
    beta.borrar_tiempos()
    with TestClient(create(services)) as c:
        for _ in range(3):
            c.get("/api/ping")
            c.get("/api/agents")
        t = c.get("/api/beta/http").json()
        fila = next(r for r in t["rutas"] if r["ruta"] == "GET /api/agents")
        assert fila["n"] == 3 and fila["p95_ms"] >= fila["p50_ms"] > 0
        b = c.post("/api/beta/bench", json={"llm": False}).json()
        nombres = {p["prueba"]: p for p in b["pruebas"]}
        assert nombres["Base de datos: escribir"]["ok"] and nombres["Base de datos: escribir"]["valor"] > 100
        assert nombres["Memoria: recordar"]["ok"] and b["fallos"] == 0
        assert c.get("/api/beta/bench").json()["version"] == b["version"]
        assert c.post("/api/beta/opinion", json={"texto": "", "nota": 3}).status_code == 400
        assert c.post("/api/beta/opinion", json={"texto": "Va fino", "nota": 9, "vista": "chat"}).json()["nota"] == 5
        assert c.get("/api/beta/opiniones").json()[0]["texto"] == "Va fino"
        inf = c.get("/api/beta/informe").text
        assert "Banco de pruebas" in inf and "Va fino" in inf and "GET /api/agents" in inf
        assert c.delete("/api/beta/http").status_code == 403   # solo administrador
        assert not services.db.query("SELECT name FROM sqlite_master WHERE name='beta_banco'")


# ------------------------------------------------------------------ rastreador: lista de URLs
def test_lista_de_urls_en_cola(services, monkeypatch):
    import threading
    import time
    from nexus.rag import crawl_jobs
    assert crawl_jobs.leer_lista("# docs\nhttps://a.example/x, https://b.example\n\nftp://no\nhttps://a.example/x "
                                 "<https://c.example/y>") == ["https://a.example/x", "https://b.example", "https://c.example/y"]
    activos, maximo, hechos, puerta = [0], [0], [], threading.Event()

    def falso_run(services_, job_id, emit=None, **kw):
        activos[0] += 1
        maximo[0] = max(maximo[0], activos[0])
        puerta.wait(5)
        job = crawl_jobs.get(job_id)
        job.update(status=crawl_jobs.DONE)
        crawl_jobs.save(job)
        hechos.append(job["url"])
        activos[0] -= 1
        return job
    monkeypatch.setattr(crawl_jobs, "run", falso_run)
    with TestClient(create(services)) as c:
        assert c.post("/api/rag/crawl", json={"urls": "nada util"}).status_code == 400
        assert c.post("/api/rag/crawl", json={"url": "sin-esquema"}).status_code == 400
        r = c.post("/api/rag/crawl", json={"urls": ["https://a.example/1", "https://b.example/2", "https://c.example/3",
                                                    "https://d.example/4"], "max_pages": 5}).json()
        assert r["urls"] == 4 and len(r["jobs"]) == 4
        time.sleep(0.3)
        estados = [j["status"] for j in c.get("/api/rag/crawls").json()]
        assert estados.count(crawl_jobs.QUEUED) >= 2          # dos a la vez, el resto espera
        en_cola = next(j for j in r["jobs"] if crawl_jobs.get(j["id"])["status"] == crawl_jobs.QUEUED and
                       j["id"] in crawl_jobs._cola)
        assert c.post(f"/api/rag/crawls/{en_cola['id']}/pause").json()["stopping"]
        puerta.set()
        for _ in range(100):
            if len(hechos) == 3:
                break
            time.sleep(0.05)
    assert len(hechos) == 3 and maximo[0] <= 2 and en_cola["url"] not in hechos
    assert crawl_jobs.get(en_cola["id"])["status"] == crawl_jobs.PAUSED


# ------------------------------------------------------------------ luz del grafo y voces por agente
def test_recorrido_del_grafo_y_voz_del_agente(services):
    from nexus.memory.graph import Recorrido
    s = services
    g = s.graph.general()
    g.relate("Atlas", "usa", "Python", "proyecto", "tecnologia")
    g.relate("Atlas", "dirigido_por", "Iratxe", "proyecto", "persona")
    with Recorrido() as rec:
        hechos = g.facts_about(["Atlas"], 5)
    assert len(hechos) == 2 and len(rec.aristas) == 2 and len(rec.nodos) == 3
    ev = rec.evento("nexus")
    assert ev["type"] == "graph_trace" and ev["links"][0][0] in ev["nodes"]
    assert Recorrido.actual() is None and Recorrido().evento() is None
    ids = {n["id"] for n in g.to_json(50)["nodes"]}
    assert set(ev["nodes"]) <= ids     # los mismos ids que dibuja el avatar
    s.agents.save({"id": "locutor", "name": "Locutor", "voz": "Helena", "voz_ritmo": 1.2, "tools": []})
    eventos = []
    s.agents.run("locutor", "que sabes del proyecto Atlas", s.memory.new_session(), emit=eventos.append)
    assert any(e["type"] == "graph_trace" for e in eventos)
    fin = next(e for e in eventos if e["type"] == "done")
    assert fin["voz"] == "Helena" and fin["voz_ritmo"] == 1.2
    with TestClient(create(s)) as c:
        assert next(a for a in c.get("/api/agents").json() if a["id"] == "locutor")["voz"] == "Helena"
        assert isinstance(c.get("/api/voice/voces").json(), list)


# ------------------------------------------------------------------ laboratorio de bolsa
def _serie(n=300, tendencia=0.0008, amplitud=0.03, base=100.0, fase=0.0):
    return [{"ts": 1_700_000_000 + i * 86400, "precio": base * (1 + tendencia) ** i * (1 + amplitud * math.sin(i / 9 + fase))}
            for i in range(n)]


def test_bolsa_lab_calculo():
    from nexus.office import bolsa_lab as B
    sube, baja = _serie(tendencia=0.002), _serie(tendencia=-0.004, amplitud=0.01)
    assert B.senales(sube)["lectura"] == "alcista" and B.senales(baja)["lectura"] == "bajista"
    assert B.senales(sube[:40]) is None
    x = [p["precio"] for p in sube]
    assert B.rsi([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]) == 100.0
    assert abs(B.sma(x, 5) - sum(x[-5:]) / 5) < 1e-9 and B.macd(x)["macd"] > 0 and 0 <= B.bollinger(x)["pct_b"] <= 1.2
    m = B.metricas([100, 110, 99, 121])
    assert m["rentabilidad_pct"] == 21.0 and m["caida_maxima_pct"] == -10.0
    assert B.metricas_operaciones([10, -5, 20])["aciertos_pct"] == 66.7
    bt = B.backtest([{"simbolo": "AAA", "puntos": sube}, {"simbolo": "BBB", "puntos": _serie(fase=2)}],
                    {"estrategia": "cruce_medias", "capital": 10000})
    assert bt["simbolos"] == ["AAA", "BBB"] and bt["gastos"] > 0 and bt["metricas"]["periodos"] == 299
    assert bt["operaciones"]["operaciones"] >= 1 and "fuera_muestra" in bt and len(bt["curva"]) <= 300 and bt["veredicto"]
    ref = B.backtest([{"simbolo": "AAA", "puntos": sube}], {"estrategia": "comprar_mantener", "comision_pct": 0,
                                                            "comision_min_eur": 0, "deslizamiento_bps": 0})
    assert abs(ref["valor_final"] / 10000 - x[-1] / x[1]) < 1e-6        # entra al cierre siguiente: sin mirar al futuro
    with pytest.raises(ValueError):
        B.backtest([{"simbolo": "C", "puntos": sube[:20]}])
    mc = B.montecarlo(x, 200, 60, semilla=7)
    assert mc == B.montecarlo(x, 200, 60, semilla=7) and mc["p5_pct"] <= mc["mediana_pct"] <= mc["p95_pct"]
    series = [{"simbolo": "A", "puntos": _serie(amplitud=0.01)}, {"simbolo": "B", "puntos": _serie(amplitud=0.08, fase=1)},
              {"simbolo": "C", "puntos": _serie(amplitud=0.04, fase=3)}]
    for metodo in ("igual", "inversa_vol", "min_varianza", "paridad_riesgo"):
        o = B.optimizar(series, metodo, {"capital": 9000})
        assert abs(sum(o["pesos"].values()) - 100) < 0.1 and abs(sum(o["importes"].values()) - 9000) < 1
    assert B.optimizar(series, "inversa_vol")["pesos"]["A"] > B.optimizar(series, "inversa_vol")["pesos"]["B"]
    assert max(B.optimizar(series, "inversa_vol", {"max_peso_pct": 40})["pesos"].values()) <= 40.01
    assert B.sentimiento("La acción sube y gana un récord") > 0 > B.sentimiento("Desplome: cae y pierde")
    assert B.extraer_json('bla {"a": 1} y al final {"accion": "vender"}') == {"accion": "vender"}


def test_bolsa_mesa_de_analistas():
    from nexus.office import bolsa_lab as B
    r = B.informe_mesa("AAA", _serie(tendencia=0.002))
    assert r["decision"]["accion"] == "comprar" and 0 < r["decision"]["peso_pct"] <= 20 and not r["con_ia"]
    assert r["riesgo"]["pesos_por_perfil"]["conservador"] < r["riesgo"]["pesos_por_perfil"]["agresivo"]
    assert B.informe_mesa("AAA", _serie(tendencia=-0.002))["decision"]["peso_pct"] == 0
    assert "error" in B.informe_mesa("AAA", _serie(30))
    prompts = []

    def llm(p):
        prompts.append(p)
        return 'Creo que {"accion": "mantener", "confianza": 0.4, "peso_pct": 50, "motivo": "esperar resultados"}'
    r = B.informe_mesa("AAA", _serie(tendencia=0.002), {}, llm, ["La empresa gana un récord"])
    assert r["con_ia"] and r["decision"]["accion"] == "mantener" and r["decision"]["peso_pct"] == 0
    assert len(prompts) == 3 and r["analistas"]["sentimiento"] > 0

    def roto(p):
        raise RuntimeError("sin modelo")
    assert B.informe_mesa("AAA", _serie(tendencia=0.002), {}, roto)["decision"]["accion"] == "comprar"


def test_bolsa_lab_api(services, monkeypatch):
    from nexus.office import bolsa
    b = bolsa.get()
    sym = b.instrumentos("")[0]["simbolo"]
    monkeypatch.setattr(type(b), "serie", lambda self, s, rango="1y": {"simbolo": s.upper(), "rango": rango, "real": False,
                                                                       "puntos": _serie(tendencia=0.0015)})
    with TestClient(create(services)) as c:
        assert c.get(f"/api/bolsa/lab/senales/{sym}").json()["lectura"] == "alcista"
        bt = c.post("/api/bolsa/lab/backtest", json={"simbolos": f"{sym}, {sym}", "estrategia": "senales"}).json()
        assert bt["metricas"] and bt["datos_reales"] is False
        assert c.post("/api/bolsa/lab/backtest", json={"simbolo": sym, "estrategia": "magia"}).status_code == 400
        assert c.post("/api/bolsa/lab/backtest", json={}).status_code == 400
        assert c.post("/api/bolsa/lab/montecarlo", json={"simbolo": sym, "simulaciones": 100}).json()["simulaciones"] == 100
        assert c.post("/api/bolsa/lab/mesa", json={"simbolo": sym}).json()["decision"]["accion"] in ("comprar", "mantener")
        assert c.post("/api/bolsa/lab/optimizar", json={"simbolo": sym}).status_code == 400   # hacen falta dos
        from nexus.tools import ToolContext
        r = services.supervisor.execute("bolsa_backtest", {"simbolos": sym}, ToolContext(services=services, agent="nexus"))
        assert "curva" not in r and r["estrategia"] == "senales"
        assert services.supervisor.execute("bolsa_analistas", {"simbolo": sym}, ToolContext(services=services))["decision"]


# ------------------------------------------------------------------ video
def test_video_avi(services, data_dir):
    from PIL import Image
    from nexus import paths
    from nexus.media import video as V
    carpeta = paths.sub("outputs", "images")
    for i, color in enumerate(("red", "green", "blue")):
        Image.new("RGB", (320 + i * 40, 240), color).save(carpeta / f"prueba_{i}.png")
    o = V.Opts(w=161, h=91, fps=10, segundos=0.5, transicion=0.2)
    out = data_dir / "v.avi"
    info = V.render_avi(sorted(carpeta.glob("*.png")), o, out)
    assert info["frames"] == 15 and info["width"] == 160 and info["height"] == 90 and info["seconds"] == 1.5
    datos = out.read_bytes()
    assert datos[:4] == b"RIFF" and datos[8:12] == b"AVI " and struct.unpack("<I", datos[4:8])[0] == len(datos) - 8
    assert info["bytes"] == len(datos)
    movi = datos.index(b"movi")
    idx = datos.index(b"idx1", movi)
    assert struct.unpack("<I", datos[idx + 4:idx + 8])[0] == 16 * 15
    for k in range(15):   # cada entrada del indice apunta a un JPEG entero
        fourcc, _flags, pos, tam = struct.unpack("<4sIII", datos[idx + 8 + 16 * k:idx + 24 + 16 * k])
        trozo = datos[movi + pos:movi + pos + 8 + tam]
        assert fourcc == b"00dc" and trozo[:4] == b"00dc" and trozo[8:10] == b"\xff\xd8" and trozo[-2:] == b"\xff\xd9"
    assert struct.unpack("<I", datos[48:52])[0] == 15      # dwTotalFrames
    with pytest.raises(ValueError):
        V.render_avi([data_dir / "noexiste.png"], o, data_dir / "x.avi")
    with TestClient(create(services)) as c:
        r = c.post("/api/video/rapido", json={"ultimas": 3, "segundos": 0.5, "formato": "1:1", "titulo": "Mi vídeo"}).json()
        assert r["imagenes"] == 3 and r["url"].endswith(".avi") and r["width"] == 1080
        r2 = c.post("/api/video/rapido", json={"imagenes": ["/outputs/images/prueba_0.png", "/outputs/../../etc/passwd"],
                                               "segundos": 0.5}).json()
        assert r2["imagenes"] == 1
        g = c.post("/api/video/guardar", files={"video": ("clip.webm", b"\x1aE\xdf\xa3datos", "video/webm")},
                   data={"titulo": "Montaje 1"}).json()
        assert g["url"].endswith("_montaje-1.webm") and g["bytes"] == 9
        assert len(c.get("/api/video").json()) == 3
    assert V.Opts().formato("9:16").h == 1280
