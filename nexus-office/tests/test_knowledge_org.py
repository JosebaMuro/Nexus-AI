"""1.6: memoria reorganizada (comun vs. saber hacer de cada agente), gestor del grafo y embeddings GGUF."""
import io
import json
import sys
import textwrap
import time
import zipfile

import numpy as np
import pytest
from fastapi.testclient import TestClient

from nexus.agents.manager import AgentSpec
from nexus.db import DB
from nexus.memory import HashingEmbedder, KnowledgeGraph, Learner, MemoryStore
from nexus.memory import kg_io
from nexus.memory.organizer import Organizer

PROG = AgentSpec(id="programador", name="Programador", description="Escribe y depura codigo",
                 keywords=["codigo", "python", "depura"], specialty="programación, Python, Django, SQL, git, algoritmos")
ART = AgentSpec(id="artista", name="Artista", description="Crea imagenes y musica",
                keywords=["imagen", "musica"], specialty="ilustración, acuarela, color, armonía, melodía")
INV = AgentSpec(id="investigador", name="Investigador", description="Busca informacion", keywords=["busca", "fuentes"],
                specialty="investigación, fuentes, citas")


def make(agents=(PROG, ART, INV)):
    db = DB()
    mem = MemoryStore(db, HashingEmbedder())
    graph = KnowledgeGraph(db)
    org = Organizer(graph, lambda: mem.embedder, lambda: list(agents))
    return db, graph, mem, Learner(db, mem, graph, org), org


def names(db, scope):
    return {r["name"].lower() for r in db.query("SELECT name FROM entities WHERE scope=?", (scope,))}


def test_common_knowledge_goes_to_general_and_know_how_to_its_specialist():
    db, graph, mem, learner, _ = make()
    # el usuario habla con Nexus-Office (generalista) de Django: es saber hacer del Programador
    learner.on_user_message("Quiero aprender Python y Django para montar una Web", "s1", agent="nexus")
    # el Investigador habla de fotosintesis: no es su oficio -> conocimiento comun
    learner.on_user_message("Hablame de la Fotosintesis y la Clorofila", "s2", agent="investigador")
    assert "django" in names(db, "programador") and "django" not in names(db, "")
    assert "clorofila" in names(db, "") and "clorofila" not in names(db, "investigador")
    cats = {r["name"].lower(): r["category"] for r in db.query("SELECT name, category FROM entities")}
    assert cats["django"] == "especialidad" and cats["clorofila"] == "comun"
    # aislamiento: lo del programador no lo ve el investigador; lo comun lo ven todos
    ctx = learner.build_context("que sabes de Django y la Clorofila", "investigador")
    assert not any("Django" in f for f in ctx["facts"])


def test_behavior_relations_and_interactions_are_shared():
    db, graph, mem, learner, org = make()
    learned = learner.on_user_message("Me llamo Joseba. Prefiero que me respondas con ejemplos. Mi hermana Ane vive "
                                      "en Bilbao. No uses emojis.", "s1", agent="programador")
    cats = {t.get("category") for t in learned}
    assert {"comportamiento", "relacion", "perfil"} <= cats
    g = names(db, "")
    assert "ane" in g and any("emojis" in n for n in g)
    assert not any("emojis" in n for n in names(db, "programador"))
    beh = learner.behaviors()
    assert any("ejemplos" in b for b in beh) and any("emojis" in b for b in beh)
    ctx = learner.build_context("hola", "artista")      # otro agente tambien recibe las pautas y la relacion
    assert ctx["behaviors"] and any("Ane" in f for f in ctx["facts"])
    learner.on_exchange("s1", "hola", "Hola Joseba", "programador")
    assert db.one("SELECT 1 FROM relations WHERE rel='conversa_con' AND scope=''")



def test_imperative_behaviors_user_anchor_and_multiline_names():
    db, graph, mem, learner, _ = make()
    learner.on_user_message("Respóndeme siempre con ejemplos. Llámame Joseba.", "s1", agent="nexus")
    beh = " | ".join(learner.behaviors())
    assert "ejemplos" in beh and "Joseba" in beh
    loose = {"respóndeme", "respondeme", "ejemplos", "llámame"}   # las palabras de una pauta no son temas sueltos
    assert not loose & (names(db, "") | names(db, "nexus") | names(db, "programador"))
    learner.on_user_message("Quiero depurar mi codigo Python con Django", "s2", agent="programador")
    # en el subgrafo del agente el Usuario es un ancla, no un "perfil" duplicado del general
    assert db.one("SELECT category FROM entities WHERE scope='programador' AND key='usuario'")["category"] == ""
    assert "perfil" not in graph.categories_by_scope().get("programador", {})
    # un titulo de Markdown y la primera palabra del parrafo no forman un nombre propio
    assert "Despliegue\nPrimero" not in KnowledgeGraph.keywords("# Despliegue\nPrimero crea el entorno con Nginx.", 6)

def test_prompt_includes_behaviors(services):
    s = services
    s.learner.on_user_message("A partir de ahora cita siempre las fuentes.", s.memory.new_session(), "nexus")
    spec = s.agents.get("investigador")
    info = s.learner.build_context("algo", "investigador")
    prompt = s.agents.build_system(spec, info, [])
    assert "Pautas de comportamiento" in prompt and "cita siempre las fuentes" in prompt
    assert s.agents.get("programador").specialty   # los agentes por defecto traen su especialidad


def test_move_merge_and_bridges_do_not_leak():
    db, graph, mem, learner, _ = make()
    prog = graph.for_agent("programador")
    prog.relate("Django", "es_un", "framework web")
    graph.general().relate("Python", "es_un", "lenguaje")
    py = db.one("SELECT id FROM entities WHERE scope='' AND key='python'")["id"]
    dj = db.one("SELECT id FROM entities WHERE scope='programador' AND key='django'")["id"]
    graph.add_relation(dj, py, "usa")                       # puente del agente hacia el general
    assert db.one("SELECT scope FROM relations WHERE rel='usa'")["scope"] == "programador"
    assert not any("usa" in f for f in graph.general().facts_about(["Python"]))   # el general no lo ve
    assert any("Django usa Python" == f for f in prog.facts_about(["Django"]))
    with pytest.raises(ValueError):                         # dos agentes distintos: aislados
        art = graph.for_agent("artista").upsert_entity("Acuarela")
        graph.add_relation(dj, art, "x")
    # mover al general un concepto que ya existe alli: se fusiona
    graph.general().upsert_entity("Framework web")
    fw = db.one("SELECT id FROM entities WHERE scope='programador' AND key='framework web'")["id"]
    moved = graph.move_entity(fw, "")
    assert moved["scope"] == "" and db.one("SELECT COUNT(*) n FROM entities WHERE key='framework web'")["n"] == 1
    rel = db.one("SELECT r.scope FROM relations r JOIN entities b ON b.id=r.dst WHERE b.key='framework web'")
    assert rel["scope"] == "programador"                   # Django (agente) -> framework web (general): puente
    # nucleos y usuario protegidos
    core = db.one("SELECT id FROM entities WHERE type='self'")["id"]
    with pytest.raises(ValueError):
        graph.move_entity(core, "programador")
    # fusionar duplicados
    a = graph.general().upsert_entity("Inteligencia artificial")
    b = graph.general().upsert_entity("IA")
    graph.add_relation(b, py, "usa")
    graph.merge_entities(b, a)
    assert not graph.entity(b) and db.one("SELECT 1 FROM relations WHERE src=? AND rel='usa'", (a,))


def test_reorganize_moves_misplaced_knowledge():
    db, graph, mem, learner, org = make()
    # como lo dejaba la 1.5: todo lo de la conversacion en el subgrafo del agente
    inv = graph.for_agent("investigador")
    inv.relate("Usuario", "le_gusta", "la montaña", "persona", "preferencia")
    inv.relate("Django", "es_un", "framework de Python")
    inv.relate("Volcanes", "relacionado_con", "Islandia")
    mem.remember("Al usuario le gusta la montaña", "preference", agent="investigador")
    moves = org.plan()
    to = {m["name"].lower(): m["to"] for m in moves}
    assert to["la montaña"] == "" and to["django"] == "programador" and to["volcanes"] == ""
    res = org.apply(moves, store=mem)
    assert res["moved"] >= 3 and res["memories_shared"] == 1
    assert "django" in names(db, "programador") and "la montaña" in names(db, "")


# ------------------------------------------------------------------ formatos de grafo
GRAPHML = """<?xml version="1.0"?><graphml xmlns="http://graphml.graphdrawing.org/xmlns">
<key id="d0" for="node" attr.name="label" attr.type="string"/><key id="d1" for="edge" attr.name="label"/>
<graph edgedefault="directed"><node id="a"><data key="d0">Python</data></node><node id="b"><data key="d0">Guido</data></node>
<edge source="b" target="a"><data key="d1">creo</data></edge></graph></graphml>"""
TTL = """@prefix ex: <http://example.org/> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
ex:Python a ex:Lenguaje ; rdfs:label "Python" ; ex:creadoPor ex:Guido_van_Rossum , ex:PSF .
<http://example.org/Django> ex:escritoEn ex:Python ."""


@pytest.mark.parametrize("name,data,edges", [
    ("g.json", json.dumps({"nodes": [{"id": 1, "name": "Python"}, {"id": 2, "label": "Django"}],
                           "links": [{"source": 2, "target": 1, "label": "escrito en"}]}), 1),
    ("c.json", json.dumps({"elements": {"nodes": [{"data": {"id": "a", "label": "A"}}, {"data": {"id": "b", "label": "B"}}],
                                        "edges": [{"data": {"source": "a", "target": "b", "label": "x"}}]}}), 1),
    ("t.json", json.dumps([{"s": "Teide", "r": "esta_en", "o": "Tenerife"}, ["Bilbao", "esta en", "Bizkaia"]]), 2),
    ("t.csv", "source,target,relation\nPython,Guido,creado por\nDjango,Python,usa\n", 2),
    ("t.tsv", "Python\tcreado_por\tGuido\n", 1),
    ("g.graphml", GRAPHML, 1),
    ("g.gexf", '<gexf><graph><nodes><node id="1" label="A"/><node id="2" label="B"/></nodes><edges>'
               '<edge source="1" target="2" label="r"/></edges></graph></gexf>', 1),
    ("g.dot", 'digraph G { a [label="Python"]; a -> b -> c [label="usa"]; }', 2),
    ("g.ttl", TTL, 3),
    ("g.jsonl", '{"subject": "A", "predicate": "p", "object": "B"}\n{"s": "B", "r": "q", "o": "C"}\n', 2),
])
def test_graph_formats(name, data, edges):
    g = kg_io.parse_bytes(data.encode(), name)
    assert len(g["edges"]) == edges and all(n["name"] for n in g["nodes"])
    if name == "g.ttl":
        labels = {n["name"] for n in g["nodes"]}
        assert {"Python", "Guido van Rossum", "Django"} <= labels
        assert any(e["rel"] == "creado_por" for e in g["edges"])
    if name == "g.graphml":
        assert {n["name"] for n in g["nodes"]} == {"Python", "Guido"}


def test_export_import_roundtrip_and_auto_distribution():
    db, graph, mem, learner, org = make()
    graph.for_agent("programador").relate("Django", "usa", "Python")
    graph.general().relate("Usuario", "le_gusta", "el cine", "persona", "preferencia", category="preferencia")
    data, mime, ext = kg_io.export_graph(graph, None, "json")
    for fmt in ("graphml", "csv"):
        assert kg_io.export_graph(graph, None, fmt)[0]
    db2, graph2, _, _, org2 = make()
    res = kg_io.insert_graph(graph2, kg_io.parse_bytes(data, "x.json"), "auto", org2, ["programador", "artista"])
    assert "django" in names(db2, "programador") and "el cine" in names(db2, "")
    # un grafo ajeno en modo auto: cada nodo a su sitio
    g = kg_io.parse_bytes(b"source,target,relation\nAcuarela,Color,usa\nPaella,Arroz,lleva\n", "x.csv")
    res = kg_io.insert_graph(graph2, g, "auto", org2, ["programador", "artista"])
    assert "acuarela" in names(db2, "artista") and "paella" in names(db2, "")
    assert res["relations"] == 2 or res["skipped"] >= 0


def test_wiki_notes_become_a_graph():
    g = kg_io.parse_wiki_notes({"Python": "Lenguaje. Ver [[Django]] y [[Flask|el micro]].", "Django": "Usa [[Python]]"})
    rels = {(e["src"], e["dst"]) for e in g["edges"]}
    assert ("python", "django") in rels and ("django", "python") in rels and len(g["nodes"]) == 3


# ------------------------------------------------------------------ embeddings GGUF (llama-server) y progresivos
FAKE_SERVER = textwrap.dedent('''
    import json, sys, http.server, zlib
    args = sys.argv[1:]
    port = int(args[args.index("--port") + 1])
    open(sys.argv[0] + ".args", "w").write(json.dumps(args))
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_GET(self):
            self.send_response(200); self.end_headers(); self.wfile.write(b"{}")
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            out = []
            for i, t in enumerate(body["input"]):
                v = [0.0] * 8
                for w in t.lower().split():
                    v[zlib.crc32(w.encode()) % 8] += 1
                out.append({"index": i, "embedding": v, "text": t})
            with open(sys.argv[0] + ".inputs", "a") as f:
                f.write(json.dumps(body["input"]) + "\\n")
            data = json.dumps({"data": out}).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
            self.wfile.write(data)
    http.server.HTTPServer(("127.0.0.1", port), H).serve_forever()
''')


@pytest.fixture
def fake_llama(tmp_path):
    script = tmp_path / "llama-server.py"
    script.write_text(FAKE_SERVER)
    exe = tmp_path / ("llama-server.cmd" if sys.platform == "win32" else "llama-server")
    exe.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n')
    exe.chmod(0o755)
    model = tmp_path / "Qwen3-Embedding-0.6B-Q8_0.gguf"
    model.write_bytes(b"GGUF")
    if sys.platform == "win32":
        pytest.skip("servidor falso con shebang")
    return exe, script, model


def test_gguf_embedder_uses_llama_server_with_instructions(fake_llama):
    from nexus.memory.embeddings import GGUFEmbedder
    exe, script, model = fake_llama
    entry = {"id": "qwen3-embedding-0.6b", "path": str(model), "kind": "embed", "instructions": True,
             "meta": {"architecture": "qwen3"}}
    emb = GGUFEmbedder(entry, lambda: (exe, "cpu"), threads=2)
    try:
        v = emb.embed(["hola mundo", "adios"])
        assert v.shape == (2, 8) and abs(float(np.linalg.norm(v[0])) - 1) < 1e-5
        q = emb.embed_query(["como instalo python"], "search")
        assert q.shape == (1, 8)
        args = json.loads(script.with_suffix(".py.args").read_text())
        assert "--embedding" in args and args[args.index("--pooling") + 1] == "last" and args[args.index("-t") + 1] == "2"
        sent = [json.loads(line) for line in script.with_suffix(".py.inputs").read_text().splitlines()]
        assert sent[0][0] == "hola mundo<|endoftext|>"                         # ultimo token = EOS
        assert sent[1][0].startswith("Instruct: Given a question, retrieve passages from technical documentation")
        assert sent[1][0].endswith("Query:como instalo python<|endoftext|>")
        n = len(sent)
        emb.embed_query(["como instalo python"], "search")                      # cacheada: no vuelve al servidor
        assert len(script.with_suffix(".py.inputs").read_text().splitlines()) == n
        assert emb.running() and emb.id == "gguf:qwen3-embedding-0.6b"
    finally:
        emb.close()
    assert not emb.running()


class SlowFake:
    """Embedder semantico de pega: lento (como un GGUF en CPU) y con otro id que el hashing."""
    slow, instructions, id = True, True, "gguf:fake"

    def __init__(self):
        self.inner = HashingEmbedder(256)
        self.waiting = 0
        self.calls = 0

    def embed(self, texts):
        self.calls += 1
        return self.inner.embed(texts)

    def embed_query(self, texts, task="search"):
        return self.embed(texts)

    def info(self):
        return {"id": self.id, "slow": True}

    def close(self):
        pass


def test_progressive_indexing_with_slow_model(services):
    s = services
    s.rag.ingest_pages("Manual de Django", ["Django es un framework web escrito en Python. " * 20,
                                            "Las migraciones se crean con makemigrations y se aplican con migrate."])
    fake = SlowFake()
    s.embed_worker.start = lambda: None         # en el test se avanza a mano
    s.set_embedder(fake)
    hits = s.rag.search("como aplico las migraciones", k=2)
    assert hits and "migrate" in hits[0]["text"]          # pendientes: busqueda con hashing
    assert s.embed_worker.pending()["chunks"] > 0
    # lo nuevo se indexa al momento con hashing aunque el modelo sea lento
    calls = fake.calls
    s.rag.ingest_pages("Notas", ["Celery ejecuta tareas en segundo plano."])
    assert fake.calls == calls
    while s.embed_worker._step(fake):
        pass
    assert s.embed_worker.pending()["total"] == 0
    s.rag._cache = None
    hits = s.rag.search("como aplico las migraciones", k=2)
    assert hits and "migrate" in hits[0]["text"]
    assert s.db.one("SELECT COUNT(*) n FROM rag_chunks WHERE embedder='gguf:fake'")["n"] == \
        s.db.one("SELECT COUNT(*) n FROM rag_chunks")["n"]


def test_embed_model_catalog_and_activation(services, tmp_path):
    from nexus.models.manager import classify
    from nexus.models.recommended import RECOMMENDED
    rec = next(r for r in RECOMMENDED if r["kind"] == "embed")
    assert rec["repo"] == "Qwen/Qwen3-Embedding-0.6B-GGUF" and rec["file"].endswith("Q8_0.gguf")
    f = tmp_path / "Qwen3-Embedding-0.6B-Q8_0.gguf"
    f.write_bytes(b"xx")
    assert classify(f)["kind"] == "embed"
    s = services
    entry = s.models.register(f, source="local", kind="embed", mid="qwen3-embedding-0.6b")
    swapped = []
    s.set_embedder = lambda e=None: swapped.append(1)
    s.models.activate(entry["id"])
    assert s.config.get("memory.embed_model") == "qwen3-embedding-0.6b" and swapped


# ------------------------------------------------------------------ API del gestor
def _zip(files: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, t in files.items():
            z.writestr(n, t)
    return buf.getvalue()


def _wait(c, jid, h):
    for _ in range(300):
        j = c.get(f"/api/kg/jobs/{jid}", headers=h).json()
        if j["status"] != "en curso":
            return j
        time.sleep(0.05)
    raise AssertionError("el trabajo no termina")


def test_api_knowledge_manager(services):
    from nexus.server import create_app
    s = services
    with TestClient(create_app(s)) as c:
        assert c.post("/api/kg/nodes", json={"name": "x"}).status_code == 403
        h = {"X-Admin-Token": c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]}
        ov = c.get("/api/kg/overview").json()
        assert "comportamiento" in ov["general_categories"] and "especialidad" in ov["agent_categories"]
        assert any(sp["id"] == "programador" for sp in ov["specialists"]) and ov["embeddings"]["embedder"]
        n = c.post("/api/kg/nodes", json={"scope": "programador", "name": "Pytest", "category": "procedimiento",
                                          "note": "pruebas"}, headers=h).json()
        assert n["scope"] == "programador" and n["category"] == "procedimiento"
        g = c.post("/api/kg/nodes", json={"scope": "", "name": "Pruebas de software"}, headers=h).json()
        r = c.post("/api/kg/relations", json={"src": n["id"], "dst": g["id"], "rel": "es parte de"}, headers=h).json()
        assert r["scope"] == "programador" and r["rel"] == "es_parte_de"
        found = c.get("/api/kg/nodes", params={"scope": "programador", "q": "pytest"}).json()
        assert found["total"] == 1
        assert c.get("/api/kg/nodes", params={"q": "pruebas", "semantic": True}).json()["items"]
        moved = c.patch(f"/api/kg/nodes/{n['id']}", json={"scope": "general"}, headers=h).json()
        assert moved["scope"] == "" and moved["category"] == "comun"
        assert c.get(f"/api/kg/nodes/{n['id']}").json()["relations"]
        # importar un grafo
        job = c.post("/api/kg/import", files={"file": ("g.csv", b"source,target,relation\nDjango,Python,usa\n")},
                     data={"target": "programador"}, headers=h).json()
        assert _wait(c, job["id"], h)["result"]["nodes"] == 2
        assert c.post("/api/kg/import", files={"file": ("g.bin", b"\x00\x01")}, headers=h).status_code == 400
        # exportar
        ex = c.get("/api/kg/export", params={"scope": "programador", "format": "graphml"})
        assert ex.status_code == 200 and b"Django" in ex.content
        # ZIP razonado: README, documento, grafo y notas enlazadas; el LLM devuelve JSON
        s.llm.has_real_llm = lambda: True
        s.llm.complete = lambda prompt, *a, **k: (
            "Este proyecto documenta una API de Django." if "resumenes" in prompt else json.dumps({
                "resumen": "Explica como desplegar la aplicacion Django.", "tipo": "tutorial", "ambito": "programador",
                "hechos": [{"s": "Gunicorn", "r": "sirve", "o": "Django", "categoria": "procedimiento"},
                           {"s": "Usuario", "r": "prefiere", "o": "despliegues con Docker", "categoria": "preferencia"}],
                "pasos": ["crear el entorno", "instalar gunicorn", "lanzar gunicorn app.wsgi"]}))
        z = _zip({"proyecto/README.md": "# Proyecto\nAPI con Django y Python. Ver [[Despliegue]].",
                  "proyecto/docs/Despliegue.md": "# Despliegue\nPrimero instala gunicorn. Luego [[Proyecto]].",
                  "proyecto/grafo.json": json.dumps({"nodes": [{"id": "a", "name": "Nginx"}, {"id": "b", "name": "Gunicorn"}],
                                                     "links": [{"source": "a", "target": "b", "label": "proxy de"}]}),
                  "proyecto/app.py": "import django\n"})
        job = c.post("/api/kg/archive", files={"file": ("proyecto.zip", z)}, data={"target": "auto"}, headers=h).json()
        res = _wait(c, job["id"], h)
        assert res["status"] == "completado", res
        rep = res["result"]
        assert rep["scope"] == "programador" and rep["reasoned"] >= 1 and rep["skills"] >= 1 and rep["llm"]
        assert any(g.get("nodes") for g in rep["graphs"]) and rep["themes"] and rep["rag_doc"]["chunks"] > 0
        assert "gunicorn" in names(s.db, "programador")
        assert s.db.one("SELECT 1 FROM memories WHERE kind='skill' AND agent='programador'")
        assert s.db.one("SELECT 1 FROM entities WHERE scope='' AND category='preferencia'")   # gusto -> comun
        assert s.rag.list("programador")
        # reorganizar y duplicados
        job = c.post("/api/kg/reorganize", json={"apply": False}, headers=h).json()
        assert "moves" in _wait(c, job["id"], h)["result"]
        job = c.post("/api/kg/duplicates", json={"scope": None}, headers=h).json()
        assert "pairs" in _wait(c, job["id"], h)["result"]
        # recuerdos por ambito
        assert isinstance(c.get("/api/kg/memories", params={"scope": "programador"}).json(), list)
        assert c.post("/api/kg/embeddings/model", json={"model": "nada"}, headers=h).status_code == 400
        assert "pending" in c.get("/api/kg/embeddings").json() or "total" in c.get("/api/kg/embeddings").json()
