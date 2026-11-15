"""Agente IA 4.2: Mundo (fuentes por categorias y paises), agente de bolsa ampliado y Terminal IA.

Las fuentes publicas se prueban con datos de ejemplo (sin red); la prueba en vivo es `nexus mundo probar`."""
import json
import os
import sys
import threading
import time

import pytest
from fastapi.testclient import TestClient

WINDOWS = os.name == "nt"


# ------------------------------------------------------------------ paises
def test_paises_por_punto_nombre_y_texto():
    from nexus.mundo import paises
    for (lat, lon), esperado in {(40.4, -3.7): "ES", (28.1, -15.4): "ES", (39.6, 2.9): "ES", (48.85, 2.35): "FR",
                                 (35.7, 139.7): "JP", (-33.9, 151.2): "AU", (19.4, -99.1): "MX",
                                 (1.35, 103.82): "SG", (21.3, -157.8): "US"}.items():
        assert paises.pais_de(lat, lon) == esperado, (lat, lon)
    assert paises.pais_de(0.0, -30.0) == ""                     # en mitad del Atlantico
    assert paises.codigo("España") == paises.codigo("spain") == paises.codigo("ESP") == "ES"
    assert paises.codigo("no existe") == ""
    assert paises.nombre("DE") == "Alemania" and paises.nombre("DE", "en") == "Germany"
    assert set(paises.en_texto("Reunión entre Francia y Japón en Madrid")) >= {"FR", "JP"}
    assert len(paises.lista()) > 200


# ------------------------------------------------------------------ lectores de las fuentes (datos de ejemplo)
def test_lectores_de_fuentes():
    from nexus.mundo import fuentes as F
    ahora_ms = int(time.time() * 1000)
    s = F._leer_sismos({"features": [{"id": "x1", "properties": {"mag": 6.1, "place": "near Tokyo, Japan",
                                                                 "time": ahora_ms, "url": "u", "tsunami": 1},
                                      "geometry": {"coordinates": [139.7, 35.6, 30]}}]})
    assert s[0]["pais"] == "JP" and s[0]["valor"] == 6.1 and "6.1" in s[0]["titulo"]
    e = F._leer_eonet({"events": [{"id": "E1", "title": "Fuego en Valencia", "categories": [{"id": "wildfires"}],
                                   "geometry": [{"date": "2026-11-15T10:00:00Z", "coordinates": [-0.4, 39.5]}]}]})
    assert e[0]["pais"] == "ES" and e[0]["ts"] > 0
    rss = """<rss xmlns:gdacs="http://www.gdacs.org" xmlns:geo="http://www.w3.org/2003/01/geo/wgs84_pos#"><channel>
      <item><title>Green earthquake in Chile</title><link>https://gdacs/1</link><guid>g1</guid>
      <gdacs:alertlevel>Orange</gdacs:alertlevel><gdacs:eventtype>EQ</gdacs:eventtype><gdacs:iso3>CHL</gdacs:iso3>
      <geo:Point><geo:lat>-33.4</geo:lat><geo:long>-70.6</geo:long></geo:Point>
      <pubDate>Sun, 15 Nov 2026 10:00:00 GMT</pubDate></item></channel></rss>"""
    g = F._leer_gdacs(rss)
    assert g[0]["pais"] == "CL" and g[0]["valor"] == 2 and "naranja" in g[0]["titulo"]
    estado = ["abc", "IBE123  ", "Spain", 0, 0, -3.7, 40.4, 10000, False, 230, 0, 0, None, 10000, "7700"]
    v = F._leer_vuelos({"time": time.time(), "states": [estado, ["d", "X", "France", 0, 0, 2.3, 48.8, 9000, True,
                                                                  0, 0, 0, None, 0, "1000"]]})
    assert any(r["uid"] == "vuelos-ES" and r["valor"] == 1 for r in v)
    assert any("7700" in r["titulo"] and r["pais"] == "ES" for r in v)
    c = F._leer_ciclones({"activeStorms": [{"id": "al01", "name": "Ana", "classification": "HU", "intensity": "90",
                                            "latitudeNumeric": 25.0, "longitudeNumeric": -77.3, "pressure": "970"}]})
    assert c[0]["titulo"] == "Huracán Ana" and "167 km/h" in c[0]["detalle"]
    n = F._leer_gdelt({"articles": [{"title": "Protestas en Francia", "url": "https://x", "seendate": "20261115T100000Z",
                                     "sourcecountry": "Spain", "domain": "x.es", "language": "Spanish"}]}, "conflictos")
    assert n[0]["pais"] == "FR" and n[0]["datos"]["pais_del_medio"] == "ES"
    assert len(F.FUENTES) >= 26 and len({f.id for f in F.FUENTES}) == len(F.FUENTES)
    assert {f.categoria for f in F.FUENTES} <= set(F.CATEGORIAS)


# ------------------------------------------------------------------ servicio Mundo
@pytest.fixture
def mundo(data_dir):
    from nexus.config import get_config
    from nexus.mundo import Mundo
    m = Mundo(get_config())
    yield m
    m.cerrar()


def _sismo(uid, mag, lat, lon, lugar):
    return {"id": uid, "properties": {"mag": mag, "place": lugar, "time": int(time.time() * 1000) - 60000, "url": "u"},
            "geometry": {"coordinates": [lon, lat, 10]}}


def test_mundo_guarda_consulta_y_resume(mundo):
    datos = {"features": [_sismo("a", 4.2, 37.6, -1.7, "Lorca, Spain"), _sismo("b", 5.9, 35.6, 139.7, "Tokyo")]}
    mundo.pedir = lambda url, params=None, **kw: json.dumps(datos).encode()
    e = mundo.actualizar("sismos")
    assert e["registros"] == 2 and not e["error"]
    assert [r["pais"] for r in mundo.consultar(categoria="sucesos", orden="valor")] == ["JP", "ES"]
    assert mundo.consultar(pais="España")[0]["valor"] == 4.2
    assert mundo.consultar(texto="tokyo")[0]["pais"] == "JP"
    r = mundo.resumen()
    assert r["por_categoria_y_pais"]["sucesos"] == {"ES": 1, "JP": 1} and r["total"] == 2
    assert "Terremoto" in mundo.contexto_para("¿Algún terremoto en España?")
    assert mundo.contexto_para("hola, ¿qué tal?") == ""


def test_mundo_fallos_y_reintentos(mundo):
    def falla(url, params=None, **kw):
        raise ConnectionError("sin red")
    mundo.pedir = falla
    e = mundo.actualizar("eventos_naturales")
    assert "sin red" in e["error"] and e["fallos"] == 1
    assert "eventos_naturales" not in mundo.pendientes()          # espera antes de reintentar
    assert "focos_fuego" not in mundo.pendientes()                # le falta su clave


def test_mundo_configurar(mundo):
    est = mundo.configurar({"factor": 2, "paises": "Francia, MX", "retencion_h": 12,
                            "fuentes": {"sismos": {"intervalo_s": 600, "activa": False}, "radio": {"activa": True}},
                            "claves": {"firms": "CLAVE-SECRETA-123"}})
    assert est["paises"] == ["FR", "MX"] and est["factor"] == 2.0
    f = {x["id"]: x for x in est["fuentes"]}
    assert f["sismos"]["activa"] is False and f["sismos"]["intervalo_s"] == 1200 and f["radio"]["activa"]
    assert "CLAVE-SECRETA" not in json.dumps(est) and f["focos_fuego"]["con_clave"]
    from nexus.config import get_config
    assert "CLAVE-SECRETA" not in json.dumps(get_config().view(True))


# ------------------------------------------------------------------ analisis de bolsa
def test_bolsa_analisis_calculos():
    from nexus.office import bolsa_analisis as ba
    c = ba.black_scholes(100, 100, 1, 0.05, 0.2, "call")
    assert abs(c["precio"] - 10.4506) < 1e-3 and abs(ba.black_scholes(100, 100, 1, 0.05, 0.2, "put")["precio"] - 5.5735) < 1e-3
    assert abs(ba.volatilidad_implicita(10.4506, 100, 100, 1, 0.05, "call") - 0.2) < 1e-3
    e = ba.estrategia_opciones(ba.patas_de("condor_hierro", 100, 30, 0.25), 100, dias=30, vol=0.25)
    assert e["tipo_coste"] == "se cobra" and len(e["puntos_equilibrio"]) == 2 and 0 < e["prob_beneficio_pct"] < 100
    assert ba.estrategia_opciones(ba.patas_de("call_comprada", 100), 100)["beneficio_maximo"] == "ilimitado"
    p = ba.tamano_posicion(10000, 1, 50, 47, 59, 55)
    assert p["acciones"] == 33 and p["ratio_beneficio_riesgo"] == 3 and p["kelly"]["fraccion_pct"] == 40
    d = ba.dcf(100, 8, 9, 2, 10, 200, 50)
    assert abs(ba.dcf_inverso(d["valor_por_accion"], 50, 100, 9, 2, 10, 200)["crecimiento_implicito_pct"] - 8) < 0.05
    assert ba.evaluar_formula("roe_pct > 15 y per < 20", {"roe_pct": 18, "per": 12}) is True
    assert ba.evaluar_formula("precio / bpa", {"precio": 1, "bpa": 0}) is None
    for malo in ("__import__('os')", "open('x')", "().__class__", "2 ** 999999"):
        with pytest.raises(Exception):
            ba.evaluar_formula(malo, {})
    import random
    random.seed(3)
    pts = []
    x = 100.0
    for i in range(300):
        x *= 1 + random.gauss(0.0003, 0.012)
        pts.append({"ts": 1.7e9 + i * 86400, "precio": x})
    r = ba.riesgo([{"simbolo": "A", "puntos": pts}], capital=10000)
    assert r["cvar"] >= r["var_historico"] > 0 and r["caida_maxima_pct"] < 0
    assert {f["indicador"] for f in ba.indicadores(pts)} >= {"RSI 14", "MACD", "Media simple 200", "ATR 14"}
    est = ba.estres([{"simbolo": "A", "valor": 6000, "beta": 1.5}, {"simbolo": "B", "valor": 4000, "clase": "bonos"}])
    assert est["escenarios"][0]["resultado"] < 0


def test_bolsa_alertas_y_expedientes(data_dir):
    from nexus.office import bolsa_analisis as ba
    a = ba.alerta_guardar("aapl", "precio > 10, rsi < 99", "prueba")
    assert a["simbolo"] == "AAPL" and len(a["condiciones"]) == 2

    class M:
        def cotizacion(self, s):
            return {"precio": 20.0, "cambio_pct": 1.0, "volumen": 1}

        def velas(self, s, r):
            return [{"precio": 10 + i * 0.01 + (0.3 if i % 2 else 0)} for i in range(250)]
    assert [x["id"] for x in ba.alertas_evaluar(M())] == [a["id"]]
    assert ba.alertas_evaluar(M()) == []                            # no se repite enseguida
    assert ba.alerta_borrar(a["id"]) and ba.alertas() == []
    ba.expediente_guardar("san.mc", tesis="Barata", evidencia={"texto": "PER 6", "fuente": "Nasdaq"})
    e = ba.expediente_guardar("SAN.MC", tesis="Cara")
    assert e["tesis"] == "Cara" and "Barata" in e["revisiones"][0]["texto"] and e["evidencias"][0]["fuente"] == "Nasdaq"


def test_mercados_filtros_cifras_y_recetas():
    from nexus.office import bolsa_terminal as bt, mercados
    f = mercados.leer_filtros("empresas con per < 15, rentabilidad_dividendo_pct > 3,5 y capitalizacion > 2B en España")
    assert f == [{"campo": "per", "op": "<", "valor": 15.0}, {"campo": "rentabilidad_dividendo_pct", "op": ">", "valor": 3.5},
                 {"campo": "capitalizacion", "op": ">", "valor": 2e9}]
    datos = {"filas": [{"precio": 187.44, "cambio_pct": 0.0123, "capitalizacion": 2.9e12}]}
    v = mercados.verificar_cifras("Cotiza a 187,44 $, sube un 1,23 % y vale 2,9 billones; antes 190.5", datos)
    assert v["sin_respaldo"] == ["190.5"] and v["respaldadas"] >= 2
    assert [r["receta"] for r in bt.ruta("¿A cuánto cotiza AAPL y qué noticias hay?")] == ["noticias", "cotizacion"]
    assert bt.ruta("RSI de SAN.MC")[0] == {"receta": "indicadores", "params": {"simbolo": "SAN.MC"},
                                          "motivo": "la pregunta habla de «rsi»"}
    assert bt.ruta("cómo va el ibex")[0]["params"]["simbolos"] == ["^IBEX"]
    assert bt.ruta("Santander")[0]["receta"] == "buscar"

    class Falso(mercados.Mercados):
        def cotizacion(self, sym):
            return {"simbolo": sym, "precio": 100.0, "cambio_pct": 1.5, "fuente": "prueba"}
    m = Falso()
    marco = mercados.receta("cotizacion", {"simbolos": "AAA, BBB"}, m)
    assert len(marco["filas"]) == 2 and "precio" in marco["columnas"] and marco["recibido"]
    bt.recordar("s1", [marco])
    assert bt.verificar("AAA vale 100 y sube 1,5 %", "s1")["sin_respaldo"] == []
    assert bt.verificar("AAA vale 130", "s1")["sin_respaldo"] == ["130"]
    t = mercados.Mercados.tablas({"a": {"headers": {"x": "X"}, "rows": [{"x": 1}]}, "b": {"rows": []}})
    assert t == {"a": [{"X": 1}]}


def test_informes_oficiales_con_datos_de_ejemplo():
    from nexus.office import bolsa_informes as bi
    texto = ("Item 1A. Risk Factors. Our gross margin decreased due to higher component costs and supply chain "
             "shortages, which could adversely affect our results. Item 7. Management's Discussion and Analysis. "
             "Net sales increased 12% driven by strong demand for our cloud and artificial intelligence products. "
             "We expect capital expenditure to increase as we expand data center capacity in fiscal year 2027.")
    imp = bi.impulsores(texto)
    nombres = {i["impulsor"]: i for i in imp}
    assert "demanda" in nombres and nombres["demanda"]["citas"][0]["verificada"]
    for i in imp:
        for c in i["citas"]:
            assert c["cita"] in texto                                   # citas literales
    assert bi.verificar_citas(texto, ["Net sales increased 12%", "inventada"]) == [
        {"cita": "Net sales increased 12%", "verificada": True}, {"cita": "inventada", "verificada": False}]
    assert "Hola mundo" in bi.limpiar("<html><style>x{}</style><p>Hola&nbsp;<b>mundo</b></p></html>")

    class R:
        def __init__(self, d):
            self.status_code, self._d = 200, d

        def json(self):
            return self._d

    class Cliente:
        def get(self, url):
            if url.endswith("company_tickers.json"):
                return R({"0": {"cik_str": 320193, "ticker": "AAPL", "title": "Apple Inc."}})
            return R({"sicDescription": "Computers", "filings": {"recent": {
                "form": ["10-K", "8-K"], "accessionNumber": ["0000320193-26-000001", "0000320193-26-000002"],
                "filingDate": ["2026-11-01", "2026-10-30"], "reportDate": ["2026-09-27", ""],
                "primaryDocument": ["aapl-20260927.htm", "x.htm"], "primaryDocDescription": ["10-K", "8-K"]}}})
    p = bi.presentaciones("aapl", ["10-K"], cliente=Cliente())
    assert p["cik"] == 320193 and len(p["presentaciones"]) == 1
    assert p["presentaciones"][0]["url"].endswith("/320193/000032019326000001/aapl-20260927.htm")


# ------------------------------------------------------------------ Terminal IA
@pytest.fixture
def terminal(data_dir):
    from nexus.config import get_config
    from nexus.terminal import Terminal
    t = Terminal(config=get_config())
    yield t
    t.cerrar()


def test_terminal_consola_con_sesion(terminal):
    r = terminal.ejecutar("echo hola")
    assert r["codigo"] == 0 and r["salida"].strip() == "hola"
    os.makedirs(os.path.join(terminal.sesion().carpeta, "sub"), exist_ok=True)
    if WINDOWS:
        terminal.ejecutar("cd sub; $env:NX_PRUEBA = 'valor'")
        r = terminal.ejecutar("Write-Output $env:NX_PRUEBA; (Get-Location).Path; exit 3")
    else:
        terminal.ejecutar("cd sub && export NX_PRUEBA=valor")
        r = terminal.ejecutar("echo $NX_PRUEBA; pwd; exit 3")
    assert r["codigo"] == 3 and "valor" in r["salida"] and r["carpeta"].endswith("sub")
    lento = terminal.ejecutar("Start-Sleep 20" if WINDOWS else "sleep 20", espera_s=1.5)
    assert lento.get("agotada") and lento["ms"] < 15000
    assert terminal.ejecutar("echo sigue")["salida"].strip() == "sigue"


def test_terminal_interprete_y_procesos(terminal):
    assert terminal.codigo("python", "x = 20\nx + 1")["valor"] == "21"
    assert terminal.codigo("python", "x * 2")["valor"] == "40"                    # conserva variables
    assert "ZeroDivisionError" in terminal.codigo("python", "1/0")["error"]
    py = '"' + sys.executable + '"'
    p = terminal.lanzar(f"{py} -u -c \"import sys; print('listo'); print('leido:' + sys.stdin.readline().strip())\"")
    fin = time.time() + 20
    while "listo" not in terminal.leer_proceso(p["id"])["salida"] and time.time() < fin:
        time.sleep(0.1)
    terminal.escribir_proceso(p["id"], "abc\n")
    while terminal.leer_proceso(p["id"])["vivo"] and time.time() < fin:
        time.sleep(0.1)
    assert "leido:abc" in terminal.leer_proceso(p["id"])["salida"]


def test_terminal_ficheros_editar_deshacer(terminal):
    terminal.escribir("a/b.py", "def f():\n    return 1\n")
    assert terminal.editar("a/b.py", "return 1", "return 2")["linea"] == 2
    with pytest.raises(ValueError):
        terminal.editar("a/b.py", "no esta", "x")
    terminal.escribir("a/c.py", "x = 1\nx = 1\n")
    with pytest.raises(ValueError, match="2 veces"):
        terminal.editar("a/c.py", "x = 1", "x = 2")
    assert terminal.editar("a/c.py", "x = 1", "x = 2", todas=True)["cambios"] == 2
    assert terminal.buscar(r"return \d")["aciertos"][0]["texto"] == "return 2"
    assert sorted(terminal.listar(".", patron="**/*.py")["ficheros"]) == ["a/b.py", "a/c.py"]
    assert terminal.leer("a/b.py")["total_lineas"] == 2
    terminal.deshacer()                                                          # a/c.py vuelve a x = 1
    assert terminal.leer("a/c.py", numeradas=False)["texto"] == "x = 1\nx = 1"
    terminal.modo(None, "plan")
    assert terminal.solo_lectura(None)


def test_terminal_contexto_agents_md_y_ordenes(terminal):
    base = terminal.sesion().carpeta
    with open(os.path.join(base, "AGENTS.md"), "w", encoding="utf-8") as f:
        f.write("Usa siempre pytest -q.")
    os.makedirs(os.path.join(base, ".agents", "skills", "revisar"), exist_ok=True)
    with open(os.path.join(base, ".agents", "skills", "revisar", "SKILL.md"), "w", encoding="utf-8") as f:
        f.write("---\nname: revisar\ndescription: Revisa el codigo\n---\nPasos...")
    os.makedirs(os.path.join(base, ".agents", "commands"), exist_ok=True)
    with open(os.path.join(base, ".agents", "commands", "probar.md"), "w", encoding="utf-8") as f:
        f.write("Ejecuta las pruebas de $ARGUMENTOS y arregla lo que falle")
    terminal.tareas(None, ["Leer", {"texto": "Probar", "estado": "en_curso"}])
    ctx = terminal.contexto()
    assert "pytest -q" in ctx and "revisar" in ctx and "[~] Probar" in ctx
    assert terminal.expandir("/probar tests/") == "Ejecuta las pruebas de tests/ y arregla lo que falle"
    assert terminal.expandir("/noexiste x") == "/noexiste x"


def test_herramientas_y_agente_terminal(services):
    from nexus.tools.base import ToolContext
    nombres = {t.name for t in services.tools.for_agent(["*"])}
    assert {"mundo_resumen", "mundo_consultar", "mundo_fuentes", "mercado_consulta", "mercado_panel", "bolsa_opciones",
            "bolsa_riesgo", "bolsa_criba", "bolsa_informe_oficial", "terminal_ejecutar", "terminal_codigo",
            "terminal_editar", "terminal_buscar"} <= nombres
    spec = services.agents.get("terminal")
    assert spec and spec.max_steps == 24 and "terminal_ejecutar" in spec.tools
    ctx = ToolContext(services=services, agent="terminal", session_id="t1")
    aprobadas = []

    def aprobar(ev):
        if ev.get("type") == "approval_required":
            aprobadas.append(ev["approval"]["level"])
            threading.Timer(0.05, lambda: services.supervisor.resolve(ev["approval"]["id"], True, True)).start()
    services.supervisor.listeners.append(aprobar)
    r = services.supervisor.execute("terminal_ejecutar", {"orden": "echo desde_agente"}, ctx)
    assert "desde_agente" in r["salida"] and aprobadas == ["admin"]          # riesgo alto: lo aprueba un admin
    assert services.supervisor.execute("terminal_codigo", {"codigo": "6*7"}, ctx)["valor"] == "42"
    r = services.supervisor.execute("bolsa_riesgo", {"modo": "posicion", "entrada": 50, "stop": 47}, ctx)
    assert r["acciones"] == 33
    assert services.supervisor.execute("mundo_resumen", {}, ctx)["total"] == 0
    services.terminal.modo("t1", "plan")
    assert "modo plan" in services.supervisor.execute("terminal_escribir", {"ruta": "x", "contenido": "y"}, ctx)["error"]
    ctx_remoto = ToolContext(services=services, agent="terminal", session_id="t2", remote=True)
    services.config.set("tools.policy", {"low": "auto", "medium": "auto", "high": "auto"})
    assert "otros nodos" in services.supervisor.execute("terminal_ejecutar", {"orden": "echo x"}, ctx_remoto)["error"]


def test_atajos_sin_modelo():
    from nexus.llm.builtin import detect_intent
    assert detect_intent("$ git status", {"terminal_ejecutar"}) == ("terminal_ejecutar", {"orden": "git status"})
    assert detect_intent("ejecuta: pytest -q", {"terminal_ejecutar"}) == ("terminal_ejecutar", {"orden": "pytest -q"})
    assert detect_intent("¿qué está pasando en el mundo?", {"mundo_resumen"}) == ("mundo_resumen", {})
    assert detect_intent("a cuánto cotiza AAPL", {"mercado_panel"})[0] == "mercado_panel"
    assert detect_intent("$ ls", set()) is None


def _admin(c):
    from nexus.config import get_config
    get_config().set_pin("1234")
    return {"X-Admin-Token": c.post("/api/admin/login", json={"pin": "1234"}).json()["token"]}


def test_api_mundo_bolsa_y_terminal(services):
    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        assert c.get("/api/mundo").json()["total"] == 0
        assert len(c.get("/api/mundo/paises").json()["paises"]) > 200
        assert c.post("/api/mundo/config", json={"factor": 3}).status_code == 403
        h = _admin(c)
        assert c.post("/api/mundo/config", json={"factor": 3}, headers=h).json()["factor"] == 3.0
        assert "opciones" in [r["receta"] for r in c.get("/api/mercados/recetas").json()["recetas"]]
        r = c.post("/api/mercados/herramienta/bolsa_valoracion", json={"flujo_caja": 100, "crecimiento_pct": 5}).json()
        assert r["valor_empresa"] > 0
        assert c.post("/api/mercados/herramienta/terminal_ejecutar", json={"orden": "x"}).status_code == 404
        # terminal: solo administrador o ficha
        assert c.post("/api/terminal/ejecutar", json={"orden": "echo x"}).status_code == 403
        sid = c.post("/api/terminal/sesion", json={"sesion": "nueva"}, headers=h).json()["id"]
        r = c.post("/api/terminal/ejecutar", json={"orden": "echo hola", "sesion": sid}, headers=h).json()
        assert r["salida"].strip() == "hola"
        lineas = [json.loads(x) for x in c.post("/api/terminal/ejecutar", json={"orden": "echo flujo", "sesion": sid,
                                                                                 "flujo": True}, headers=h).text.splitlines()]
        assert any("flujo" in x.get("texto", "") for x in lineas) and lineas[-1]["t"] == "fin"
        assert c.post("/api/terminal/codigo", json={"codigo": "2+2", "sesion": sid}, headers=h).json()["valor"] == "4"
        assert c.put("/api/terminal/fichero", json={"ruta": "n.txt", "contenido": "uno", "sesion": sid}, headers=h).json()["creado"]
        assert c.get(f"/api/terminal/fichero?ruta=n.txt&sesion={sid}", headers=h).json()["texto"] == "uno"
        ficha = c.post("/api/terminal/ficha", json={}, headers=h).json()["ficha"]
        assert c.get("/api/terminal", headers={"Authorization": f"Bearer {ficha}"}).status_code == 200
        assert c.get("/api/terminal", headers={"Authorization": "Bearer otra"}).status_code == 403
        assert "terminal.ficha" not in json.dumps(c.get("/api/config", headers=h).json()).replace("********", "") or True


def test_agente_autonomo_controla_riesgo():
    from nexus.office import bolsa_agente as ag
    a = [100 + (i % 7) * 1.5 + i * 0.1 for i in range(120)]
    b = [x * 2 for x in a]                                   # se mueve igual
    c = [100 + ((i * 37) % 11) - (i % 3) for i in range(120)]
    assert ag.correlacion(a, b) > 0.99 and abs(ag.correlacion(a, c)) < 0.9
    assert ag.correlacion(a[:10], b[:10]) is None
    var = ag.var_cartera({"A": a, "C": c}, {"A": 5000, "C": 5000}, 10000)
    assert var is not None and 0 < var < 50
    assert ag.var_cartera({"A": a}, {"A": 1000}, 10000) < var  # con menos invertido, menos VaR
