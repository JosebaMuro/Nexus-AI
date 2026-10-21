from nexus.db import DB
from nexus.memory import HashingEmbedder, KnowledgeGraph, Learner, MemoryStore


def make():
    db = DB()
    mem = MemoryStore(db, HashingEmbedder())
    graph = KnowledgeGraph(db)
    return db, mem, graph, Learner(db, mem, graph)


def test_recall_ranks_related_memory_first():
    _, mem, _, _ = make()
    mem.remember("Mi perro se llama Toby y es un golden retriever", "fact")
    mem.remember("La reunion del trabajo es el martes", "fact")
    mem.remember("Me encanta la pizza napolitana", "preference")
    top = mem.recall("como se llama mi perro", k=1)
    assert top and "Toby" in top[0]["content"]


def test_dedupe_bumps_importance():
    _, mem, _, _ = make()
    a = mem.remember("El usuario vive en Bilbao", "fact", 0.5)
    b = mem.remember("El usuario vive en Bilbao", "fact", 0.5)
    assert a == b


def test_graph_extraction_spanish():
    _, _, graph, learner = make()
    learned = learner.on_user_message("Hola, me llamo Joseba, vivo en Bilbao y me gusta la astronomía", "s1")
    rels = {(t["r"], t["o"]) for t in learned}
    assert ("se_llama", "Joseba") in rels and ("vive_en", "Bilbao") in rels
    assert any(r == "le_gusta" and "astronom" in o for r, o in rels)
    assert any("Joseba" in f for f in graph.user_facts())
    assert not any(t["r"] == "le_gusta" for t in learner.on_user_message("no me gusta el brocoli", "s1"))


def test_explicit_remember_and_feedback_learning():
    db, mem, _, learner = make()
    learner.on_user_message("Recuerda que mi cumpleaños es el 3 de mayo", "s1")
    assert any("3 de mayo" in m["content"] for m in mem.list("fact"))
    sid = mem.new_session()
    mem.add_message(sid, "user", "explicame la fotosintesis")
    mid = mem.add_message(sid, "assistant", "La fotosintesis convierte luz en energia quimica.", "nexus")
    learner.feedback(mid, 1)
    assert mem.list("example")
    learner.feedback(mid, -1, "demasiado corto")
    assert any("demasiado corto" in m["content"] for m in mem.list("lesson"))


def test_graph_json_has_self_and_user():
    _, _, graph, _ = make()
    g = graph.to_json()
    types = {n["type"] for n in g["nodes"]}
    assert "self" in types and "persona" in types
