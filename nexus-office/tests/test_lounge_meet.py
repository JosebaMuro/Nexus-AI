"""Convocar reunion: no se solapan, con muchos agentes se limita la ronda y el LLM contesta sin razonar."""
import threading

from fastapi.testclient import TestClient


def test_reunion_ocupada_y_limite_de_presentaciones(services):
    s = services
    s.config.set("lounge.max_intros", 2)
    seen = []
    real_has, real_complete = s.llm.has_real_llm, s.llm.complete
    s.llm.has_real_llm = lambda: True
    s.llm.complete = lambda prompt, spec="default", **kw: seen.append(kw) or "Hola, soy yo."
    try:
        out = s.lounge.meet()
        intros = [m for m in s.lounge.history(400) if m["kind"] == "intro"]
        assert "proposals" in out and 1 <= len(intros) <= 2 and intros[0]["agent"] == "nexus"
        assert seen and all(k.get("reasoning") == "off" for k in seen)
        gate = threading.Event()
        s.llm.complete = lambda *a, **k: gate.wait(5) and "" or "ok"
        t = threading.Thread(target=s.lounge.meet); t.start()
        for _ in range(100):
            if s.lounge.busy:
                break
            threading.Event().wait(0.02)
        assert s.lounge.busy and s.lounge.meet() == {"busy": True}
        from nexus.server.app import create_app
        with TestClient(create_app(s)) as c:
            assert c.post("/api/lounge/meet", json={"topic": ""}).json() == {"started": False, "busy": True}
            assert c.get("/api/lounge").json()["busy"] is True
            gate.set(); t.join(5)
        assert not s.lounge.busy
    finally:
        s.llm.has_real_llm, s.llm.complete = real_has, real_complete
        s.config.set("lounge.max_intros", 8)
