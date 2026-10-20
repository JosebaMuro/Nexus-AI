"""Grafo general + un subgrafo aislado por agente, y RAG importado al grafo general o al de un agente."""
import sqlite3

import pytest
from fastapi.testclient import TestClient

from nexus.db import DB
from nexus.memory import HashingEmbedder, KnowledgeGraph, Learner, MemoryStore

VOLCANES = ("El Teide es un volcan de Tenerife que mide 3718 metros. Es el pico mas alto de Espana. "
            "Su ultima erupcion fue en 1909 por el Chinyero.")
RECETA = "La paella valenciana lleva arroz, pollo, conejo, judia verde y garrofon. Se cocina con azafran."


def make():
    db = DB()
    mem = MemoryStore(db, HashingEmbedder())
    graph = KnowledgeGraph(db)
    return db, graph, Learner(db, mem, graph)


def scope_names(db, scope):
    return {r["name"].lower() for r in db.query("SELECT name FROM entities WHERE scope=?", (scope,))}


def test_each_agent_learns_in_its_own_isolated_graph():
    db, graph, learner = make()
    learner.on_user_message("Me llamo Joseba. Hablame de la Fotosintesis y la Clorofila", "s1", agent="investigador")
    learner.on_exchange("s1", "", "La Fotosintesis convierte la luz en energia gracias a la Clorofila.", "investigador")
    learner.on_user_message("Quiero aprender Python y Django", "s2", agent="programador")
    inv, prog = scope_names(db, "investigador"), scope_names(db, "programador")
    assert "clorofila" in inv and "clorofila" not in prog
    assert "django" in prog and "django" not in inv
    # lo que se sabe del usuario es comun: esta en el general y lo ven todos
    assert any("Joseba" in f for f in graph.user_facts())
    ctx_prog = learner.build_context("que sabes de la Clorofila", "programador")
    assert any("Joseba" in f for f in ctx_prog["facts"])
    assert not any("Clorofila" in f for f in ctx_prog["facts"])
    # cada subgrafo tiene su ancla (el agente) y su propio nodo de usuario
    assert db.one("SELECT 1 FROM entities WHERE scope='investigador' AND type='agente'")


def test_agent_view_and_full_view():
    db, graph, learner = make()
    learner.on_user_message("Hablame de la Astronomia y las Galaxias", "s1", agent="investigador")
    own = graph.for_agent("investigador").to_json()
    assert {n["scope"] for n in own["nodes"]} == {"investigador"}
    assert sum(1 for n in own["nodes"] if n["type"] == "self") == 1        # el agente es el nucleo de su vista
    full = graph.to_json(scope="*")
    scopes = {n["scope"] for n in full["nodes"]}
    assert scopes == {"", "investigador"}
    core = next(n["id"] for n in full["nodes"] if n["type"] == "self")
    anchor = next(n["id"] for n in full["nodes"] if n["type"] == "agente")
    assert {"source": core, "target": anchor, "rel": "subgrafo", "weight": 3.0} in full["links"]
    ids = {n["id"] for n in full["nodes"]}
    assert all(link["source"] in ids and link["target"] in ids for link in full["links"])
    st = graph.stats()
    assert st["scopes"]["investigador"]["entities"] > 0 and st["entities"] == sum(v["entities"] for v in st["scopes"].values())


def test_cores_and_user_cannot_be_deleted_and_scope_drop():
    db, graph, learner = make()
    g = graph.for_agent("artista")
    g.relate("Acuarela", "es_un", "tecnica")
    for row in db.query("SELECT id FROM entities WHERE type IN ('self','agente') OR key='usuario'"):
        graph.delete_entity(row["id"])
    assert db.one("SELECT COUNT(*) n FROM entities WHERE type IN ('self','agente')")["n"] == 2
    assert graph.drop_scope("artista") > 0
    assert not scope_names(db, "artista") and scope_names(db, "")
    with pytest.raises(ValueError):
        graph.drop_scope("")


def test_transaction_rolls_back_everything():
    db, graph, _ = make()
    before = db.one("SELECT COUNT(*) n FROM entities")["n"]
    with pytest.raises(RuntimeError):
        with db.transaction():
            graph.relate("Uno", "con", "Dos")
            raise RuntimeError("fallo")
    assert db.one("SELECT COUNT(*) n FROM entities")["n"] == before


def test_old_database_is_migrated(tmp_path):
    path = tmp_path / "v1.db"
    con = sqlite3.connect(path)
    con.executescript("""
        CREATE TABLE entities (id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT UNIQUE, name TEXT, type TEXT,
            weight REAL DEFAULT 1, mentions INTEGER DEFAULT 1, first_seen REAL, last_seen REAL);
        CREATE TABLE relations (id INTEGER PRIMARY KEY AUTOINCREMENT, src INTEGER, dst INTEGER, rel TEXT,
            weight REAL DEFAULT 1, last_seen REAL, UNIQUE(src, dst, rel));
        CREATE TABLE rag_docs (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, source TEXT, path TEXT, kind TEXT,
            pages INTEGER, chars INTEGER, added REAL, meta TEXT);
        INSERT INTO entities(id, key, name, type, weight) VALUES (7, 'bilbao', 'Bilbao', 'lugar', 3);
        INSERT INTO entities(id, key, name, type, weight) VALUES (8, 'usuario', 'Usuario', 'persona', 4);
        INSERT INTO relations(src, dst, rel) VALUES (8, 7, 'vive_en');
        INSERT INTO rag_docs(title) VALUES ('Antiguo');
    """)
    con.commit()
    con.close()
    db = DB(str(path))
    graph = KnowledgeGraph(db)
    assert db.one("SELECT scope, name FROM entities WHERE id=7") == {"scope": "", "name": "Bilbao"}
    assert graph.user_facts() == ["Usuario vive en Bilbao"]
    assert db.one("SELECT agent FROM rag_docs")["agent"] == ""
    graph.for_agent("investigador").upsert_entity("Bilbao", "lugar")   # misma clave en otro ambito: permitido
    assert db.one("SELECT COUNT(*) n FROM entities WHERE key='bilbao'")["n"] == 2
    db.close()
    DB(str(path)).close()   # reabrir una base ya migrada no hace nada


def test_rag_import_to_general_or_to_one_agent(services):
    s = services
    general = s.rag.ingest_pages("Guia de volcanes", [VOLCANES], "volcanes.txt")
    private = s.rag.ingest_pages("Recetario", [RECETA], "recetas.txt", agent="artista")
    assert general["agent"] == "" and private["agent"] == "artista"
    titles = lambda hits: {h["title"] for h in hits}   # noqa: E731
    assert "Recetario" in titles(s.rag.search("paella arroz azafran", k=5, min_score=0, agent="artista"))
    assert "Recetario" not in titles(s.rag.search("paella arroz azafran", k=5, min_score=0, agent="programador"))
    assert "Recetario" not in titles(s.rag.search("paella arroz azafran", k=5, min_score=0, agent=""))
    assert "Guia de volcanes" in titles(s.rag.search("Teide volcan", k=5, min_score=0, agent="programador"))
    assert "Recetario" in titles(s.rag.search("paella", k=5, min_score=0))           # la biblioteca ve todo
    assert {d["title"] for d in s.rag.list(agent="programador")} == {"Guia de volcanes"}
    # cada documento entra en su grafo: el general o el individual del agente
    assert s.db.one("SELECT 1 FROM entities WHERE scope='' AND type='documento' AND name='Guia de volcanes'")
    assert s.db.one("SELECT 1 FROM entities WHERE scope='artista' AND type='documento' AND name='Recetario'")
    assert not s.db.one("SELECT 1 FROM entities WHERE scope='' AND name='Recetario'")
    facts = s.graph.for_agent("artista").facts_about(["Recetario"])
    assert any("ha leido Recetario" in f for f in facts)
    # mover al general: el nodo se mueve con el
    moved = s.rag.assign(private["id"], "")
    assert moved["agent"] == ""
    assert not s.db.one("SELECT 1 FROM entities WHERE scope='artista' AND name='Recetario'")
    assert s.db.one("SELECT 1 FROM entities WHERE scope='' AND name='Recetario'")
    assert "Recetario" in titles(s.rag.search("paella", k=5, min_score=0, agent="programador"))
    s.rag.delete(moved["id"])
    assert not s.db.one("SELECT 1 FROM entities WHERE name='Recetario'")


def test_agent_run_uses_only_general_and_own_documents(services):
    s = services
    s.rag.ingest_pages("Recetario", [RECETA], "recetas.txt", agent="artista")
    seen = {}
    real = s.agents.build_system

    def spy(spec, info, tools, text_tools=True):
        seen[spec.id] = [d["title"] for d in info.get("documents", [])]
        return real(spec, info, tools, text_tools)
    s.agents.build_system = spy
    sid = s.memory.new_session()
    s.agents.run("programador", "paella valenciana con arroz y azafran", sid)
    s.agents.run("artista", "paella valenciana con arroz y azafran", sid)
    assert "Recetario" not in seen["programador"] and "Recetario" in seen["artista"]


def test_deleting_agent_keeps_its_documents_in_general(services):
    s = services
    s.agents.save({"id": "chef", "name": "Chef", "keywords": ["receta"]})
    doc = s.rag.ingest_pages("Recetario", [RECETA], "recetas.txt", agent="chef")
    assert s.db.one("SELECT 1 FROM entities WHERE scope='chef' AND type='agente' AND name='Chef'")
    s.agents.delete("chef")
    assert s.rag.get(doc["id"])["agent"] == ""
    assert not s.db.one("SELECT 1 FROM entities WHERE scope='chef'")
    assert s.db.one("SELECT 1 FROM entities WHERE scope='' AND name='Recetario'")


def test_graph_and_rag_api(services):
    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        r = c.post("/api/rag/import", files=[("files", ("recetas.txt", RECETA.encode(), "text/plain"))],
                   data={"agent": "artista"})
        assert r.status_code == 200, r.text
        assert r.json()["agent"] == "artista" and r.json()["results"][0]["doc"]["agent"] == "artista"
        assert c.post("/api/rag/import", files=[("files", ("a.txt", b"hola mundo cruel", "text/plain"))],
                      data={"agent": "nadie"}).status_code == 400
        r = c.post("/api/rag/upload", files={"file": ("volcanes.txt", VOLCANES.encode(), "text/plain")})
        assert r.status_code == 200 and r.json()["agent"] == ""
        scopes = {x["id"]: x for x in c.get("/api/graph/scopes").json()}
        assert scopes[""]["name"] == "General" and scopes["artista"]["documents"] == 1
        assert scopes["artista"]["entities"] > 0
        full = c.get("/api/graph").json()
        assert any(link["rel"] == "subgrafo" for link in full["links"])
        own = c.get("/api/graph?scope=artista").json()
        assert own["nodes"] and {n["scope"] for n in own["nodes"]} == {"artista"}
        assert c.get("/api/graph?scope=nadie").status_code == 404
        docs = c.get("/api/rag?agent=programador").json()["documents"]
        assert len(docs) == 1 and docs[0]["source"] == "volcanes.txt"
        doc_id = c.get("/api/rag?agent=artista").json()["documents"][0]["id"]
        hits = c.get("/api/rag/search", params={"q": "paella", "agent": "programador"}).json()
        assert all(h["agent"] == "" for h in hits)
        r = c.patch(f"/api/rag/{doc_id}", json={"agent": "programador"})
        assert r.status_code == 200 and r.json()["agent"] == "programador"
        assert c.patch("/api/rag/99999", json={"agent": ""}).status_code == 404


def test_renamed_agent_keeps_a_single_anchor(services):
    s = services
    s.agents.save({"id": "chef", "name": "Chef"})
    s.graph.for_agent("chef").relate("Marmitako", "es_un", "guiso")
    s.agents.save({"id": "chef", "name": "Cocinero"})
    g = s.graph.for_agent("chef")
    assert g.self_name == "Cocinero"
    anchors = s.db.query("SELECT name FROM entities WHERE scope='chef' AND type='agente'")
    assert anchors == [{"name": "Cocinero"}]
