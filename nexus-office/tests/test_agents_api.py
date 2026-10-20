from fastapi.testclient import TestClient


def test_builtin_agent_pipeline(services):
    s = services
    sid = s.memory.new_session()
    events = []
    r = s.agents.run("auto", "Hola, me llamo Ane y vivo en Donostia", sid, emit=events.append)
    assert any(e["type"] == "learned" for e in events)
    r = s.agents.run("auto", "cuánto es 2^8 + 1", sid, emit=events.append)
    assert "257" in r["answer"]
    r = s.agents.run("auto", "compón una música relajante", sid)
    assert r["agent"] == "artista" and "/outputs/music/" in r["answer"]
    assert s.agents.route("busca noticias en internet") == "investigador"


def test_fabricated_images_are_removed(services):
    s = services

    class Liar:
        def stream(self, msgs, spec, **kw):
            yield "Aqui esta: ![img](https://example.com/fake.jpg)"
    real = s.llm.stream
    s.llm.stream = Liar().stream
    try:
        r = s.agents.run("nexus", "describe el cielo", s.memory.new_session())
    finally:
        s.llm.stream = real
    assert "example.com" not in r["answer"]


def test_lounge_detects_gap_and_new_agent_routes(services):
    s = services
    sid = s.memory.new_session()
    for q in ["dime cosas de los volcanes activos", "los volcanes de islandia", "erupciones de volcanes famosas"]:
        s.agents.run("auto", q, sid)
    s.lounge.meet(quick=True)
    props = s.lounge.proposals()
    assert props and "volcan" in props[0]["spec"]["id"]
    s.lounge.resolve_proposal(props[0]["id"], True)
    assert s.agents.route("háblame de volcanes") == props[0]["spec"]["id"]
    kinds = {m["kind"] for m in s.lounge.history()}
    assert {"task", "proposal", "intro", "summary"} <= kinds


def test_api_modes_and_endpoints(services):
    from nexus.server import create_app
    with TestClient(create_app(services)) as c:
        st = c.get("/api/status").json()
        assert st["admin"] is False and st["llm"]["backend"]
        assert c.put("/api/config", json={"user.response_style": "breve"}).status_code == 200
        assert c.put("/api/config", json={"llm.temperature": 0.1}).status_code == 403
        assert c.get("/api/agents").status_code == 200 and c.put("/api/agents", json={"id": "x"}).status_code == 403
        tok = c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]
        h = {"X-Admin-Token": tok}
        assert c.put("/api/config", json={"llm.temperature": 0.1}, headers=h).status_code == 200
        assert c.post("/api/admin/login", json={"pin": "0000"}).status_code == 401
        assert "llm" in c.get("/api/config", headers=h).json()
        m = c.post("/api/music/generate", json={"prompt": "feliz", "seconds": 6}).json()
        assert m["url"].endswith(".wav") and c.get(m["url"]).status_code == 200
        assert c.get("/api/lounge").json()["agents"]
        assert c.get("/api/models/recommended").json()["items"]
        with c.websocket_connect("/ws") as ws:
            assert ws.receive_json()["type"] == "hello"
            ws.send_json({"type": "chat", "text": "cuánto es 6*7", "agent": "auto"})
            done = None
            for _ in range(200):
                e = ws.receive_json()
                if e["type"] == "done":
                    done = e
                    break
            assert done and "42" in done["text"]
