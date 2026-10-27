"""Perfiles de oficina para pymes: agentes de oficina y de gremio, cada uno con su propio grafo."""
import yaml
from fastapi.testclient import TestClient

from nexus import office
from nexus.agents.manager import AgentSpec

WEB_TOOLS = {"web_search", "web_fetch", "url_to_pdf", "crawl_docs"}


def test_catalog_and_profiles_are_consistent(services):
    cat, profs = office.catalog(), office.profiles()
    assert len(profs) >= 10
    tools = {t.name for t in services.tools.for_agent(["*"])}
    for aid, d in cat.items():
        spec = AgentSpec.from_dict(d)
        assert spec.id == aid and spec.name and spec.specialty and spec.keywords
        assert set(spec.tools) <= tools, (aid, set(spec.tools) - tools)
        assert not set(spec.tools) & WEB_TOOLS, aid          # 100 % offline: nada que necesite internet
        assert len(office.seeds(d)) >= 5, aid
        assert all(r and s and o for s, r, o, _ in office.seeds(d))
    for pid, p in profs.items():
        assert set(p["agents"]) <= set(cat), pid
        assert any(office.is_trade(a) for a in p["agents"]), pid
        assert any(not office.is_trade(a) for a in p["agents"]), pid
    trades = {a for p in profs.values() for a in p["agents"] if office.is_trade(a)}
    assert {"herrero", "calderero", "albanil"} <= trades


def test_apply_profile_builds_the_office(services):
    s = services
    o = office.Office(s.config)
    p = o.apply("caldereria")
    assert p["active"] and {a["id"] for a in p["agents"]} >= {"calderero", "soldador", "calidad"}
    s.agents.reload()
    ids = set(s.agents.ids())
    assert ids == {"nexus", "calderero", "soldador", "calidad", "presupuestos", "administracion", "compras", "prl", "rrhh"}
    assert not s.agents.get("artista").enabled                       # desactivado, no borrado
    nexus = s.agents.get("nexus")
    assert set(nexus.can_delegate_to) == ids - {"nexus"} and not set(nexus.tools) & WEB_TOOLS
    assert nexus.name == "Oficina Calderería" and s.config.get("user.assistant_name") == "Oficina Calderería"
    assert s.config.get("office.profile") == "caldereria"
    # el YAML guardado tiene el formato de AgentManager (sin la clave de siembra)
    assert "grafo" not in yaml.safe_load((s.agents.dir / "calderero.yaml").read_text(encoding="utf-8"))
    # un agente personalizado no se pisa al volver a aplicar; con reset_agents si
    s.agents.save({**office.catalog()["soldador"], "name": "Soldador jefe"})
    o.apply("caldereria")
    s.agents.reload()
    assert s.agents.get("soldador").name == "Soldador jefe"
    o.apply("caldereria", reset_agents=True)
    s.agents.reload()
    assert s.agents.get("soldador").name == "Soldador"
    assert list((s.agents.dir / "respaldo").glob("soldador-*.yaml"))


def test_each_agent_gets_its_own_seeded_graph(services):
    s = services
    o = office.Office(s.config)
    o.apply("estructuras-metalicas")
    s.agents.reload()
    seeded = office.ensure(s)
    assert set(seeded) == {"herrero", "calderero", "soldador", "calidad", "presupuestos", "administracion", "compras",
                           "prl", "rrhh"}
    assert office.ensure(s) == {}                                     # idempotente

    def names(scope):
        return {r["name"] for r in s.db.query("SELECT name FROM entities WHERE scope=?", (scope,))}
    assert "Barandilla" in names("herrero") and "Barandilla" not in names("calderero")
    assert "Virola" in names("calderero") and "Virola" not in names("herrero") and "Virola" not in names("")
    assert "WPS" in names("soldador") and "WPS" not in names("calidad")
    facts = s.graph.for_agent("herrero").facts_about(["Barandilla"])
    assert any("0,90 m" in f for f in facts)
    view = s.graph.for_agent("calderero").to_json()
    assert {n["scope"] for n in view["nodes"]} == {"calderero"}
    anchor = s.db.one("SELECT name FROM entities WHERE scope='calderero' AND type='agente'")
    assert anchor["name"] == "Calderero"
    # lo que aprende en su trabajo va a su grafo, no al de otro gremio
    sid = s.memory.new_session()
    s.agents.run("calderero", "Hablame del Fondo Toriesferico y del Rebordeado", sid)
    assert any("toriesf" in n.lower() for n in names("calderero"))
    assert not any("toriesf" in n.lower() for n in names("herrero"))


def test_trade_routing(services):
    s = services
    o = office.Office(s.config)
    o.apply("mantenimiento-integral")
    s.agents.reload()
    cases = {"hay una fuga en el desague del bano": "fontanero", "salta el diferencial del cuadro electrico": "electricista",
             "hay que enfoscar un tabique de ladrillo": "albanil", "cambiar el bombin de la cerradura": "cerrajero",
             "el aire acondicionado no enfria": "climatizacion", "pintar la fachada con pintura de silicato": "pintor",
             "hazme un presupuesto con su margen": "presupuestos", "quiero dar de alta un aviso de un cliente": "recepcion"}
    for text, aid in cases.items():
        assert s.agents.route(text) == aid, text
    o.apply("herreria")
    s.agents.reload()
    assert s.agents.route("una barandilla de forja para la escalera") == "herrero"
    assert s.agents.route("tengo porosidad en el cordon de soldadura mig") == "soldador"


def test_clear_restores_default_agents(services):
    s = services
    o = office.Office(s.config)
    o.apply("panaderia")
    o.clear()
    s.agents.reload()
    assert set(s.agents.ids()) == {"nexus", "artista", "investigador", "forjador", "programador", "profesor"}
    assert s.agents.get("nexus").name == "Nexus-Office" and s.config.get("office.profile") == ""


def test_office_api(services):
    from nexus.server.app import create_app
    s = services
    s.config.set_pin("1234")
    with TestClient(create_app(s)) as c:
        profs = c.get("/api/office/profiles").json()
        assert len(profs) >= 10 and not any(p["active"] for p in profs)
        assert c.post("/api/office/profile", json={"id": "herreria"}).status_code in (401, 403)
        tok = c.post("/api/admin/login", json={"pin": "1234"}).json()["token"]
        h = {"X-Admin-Token": tok}
        r = c.post("/api/office/profile", json={"id": "herreria"}, headers=h)
        assert r.status_code == 200, r.text
        assert "herrero" in r.json()["seeded"] and "herrero" in r.json()["agents"]
        assert next(p for p in c.get("/api/office/profiles").json() if p["id"] == "herreria")["active"]
        assert c.post("/api/office/profile", json={"id": "nadie"}, headers=h).status_code == 400
        scopes = {x["id"] for x in c.get("/api/graph/scopes").json()}
        assert {"herrero", "soldador", "presupuestos"} <= scopes
        assert c.post("/api/office/profile", json={"id": ""}, headers=h).json()["profile"] is None


def test_cli_on_a_fresh_install(data_dir, capsys):
    from nexus.__main__ import main
    main(["oficina", "jardineria"])        # antes del primer arranque: aun no hay data/agents
    from nexus.config import reset_config
    from nexus.core import Services
    reset_config()
    s = Services(detect_hardware=False)
    try:
        assert set(s.agents.ids()) == {"nexus", "jardinero", "fontanero", "recepcion", "presupuestos", "administracion",
                                       "compras", "prl"}
        assert s.db.one("SELECT 1 FROM entities WHERE scope='jardinero' AND name='Xerojardinería'")
    finally:
        s.monitor.stop()
        s.db.close()


def test_cli(services, capsys):
    from nexus.__main__ import main
    main(["oficina"])
    out = capsys.readouterr().out
    assert "herreria" in out and "caldereria" in out
    main(["oficina", "reformas"])
    assert "Oficina Reformas" in capsys.readouterr().out
    from nexus.config import get_config, reset_config
    reset_config()
    assert get_config().get("office.profile") == "reformas"


def test_office_in_the_android_app(tmp_path, monkeypatch):
    """App autonoma de Android (android-autonomo): mismo arranque que la APK, sin terminal: el perfil se elige en la
    interfaz (API) y los agentes de la oficina estrenan su grafo en el movil."""
    import os
    import shutil
    import socket
    import time

    import httpx

    from nexus import android_main, paths
    from nexus.config import get_config, reset_config
    for k in ("NEXUS_ANDROID", "NEXUS_RUNTIME_LLAMA", "TMPDIR", "HOME"):
        monkeypatch.setenv(k, os.environ.get(k, "x"))
        if k != "HOME":
            monkeypatch.delenv(k)
    reset_config()
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        port = sk.getsockname()[1]
    t = android_main.start_in_thread(str(tmp_path / "files"), "", port)
    url = f"http://127.0.0.1:{port}"
    try:
        for _ in range(120):
            try:
                if httpx.get(url + "/api/ping", timeout=1).status_code == 200:
                    break
            except httpx.HTTPError:
                time.sleep(0.25)
        get_config().set_pin("1234")
        tok = httpx.post(url + "/api/admin/login", json={"pin": "1234"}).json()["token"]
        r = httpx.post(url + "/api/office/profile", json={"id": "taller-mecanico"}, headers={"X-Admin-Token": tok},
                       timeout=30)
        assert r.status_code == 200, r.text
        assert "mecanico" in r.json()["seeded"]
        assert (paths.agents_dir() / "mecanico.yaml").exists()
        assert str(paths.agents_dir()).startswith(str(tmp_path / "files" / "nexus"))
        scopes = {x["id"]: x for x in httpx.get(url + "/api/graph/scopes").json()}
        assert scopes["mecanico"]["entities"] > 5
    finally:
        android_main.stop()
        t.join(10)
    # la APK copia nexus/ entero con estos filtros (packaging/build_apk_autonomo.py): los YAML de la oficina van dentro
    dest = tmp_path / "apk" / "nexus"
    shutil.copytree(office.HERE.parent, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    assert len(list((dest / "office" / "perfiles").glob("*.yaml"))) == len(office.profiles())
    assert len(list((dest / "office" / "agentes").glob("*.yaml"))) == len(office.catalog())


def test_modelos_sugeridos_por_sector(data_dir):
    from nexus import office
    from nexus.models.recommended import RECOMMENDED, recommend
    ids = {r["id"] for r in RECOMMENDED}
    for pid, models in office.SECTOR_MODELS.items():
        assert {m for m, _ in models} <= ids, pid
    prof = {"ram_gb": 32, "vram_gb": 12, "llm": {"gpu_layers": -1, "max_model_b": 14, "recommended_model_b": 8},
            "image": {"neural": True}}
    general = recommend(prof)
    assert not any(r.get("sector_only") for r in general)          # MedGemma no aparece sin perfil de salud
    salud = recommend(prof, office.sector_models("clinica-dental") or {"medgemma-4b": "salud"})
    med = next(r for r in salud if r["id"] == "medgemma-4b")
    assert med["sector"] and not med.get("best")
    assert next(r for r in salud if r["kind"] == "llm" and r.get("best"))["id"] != "medgemma-4b"
    assert office.sector_models("pintura") and office.sector_models("") == {}
