"""Modo live por voz: respuestas cortas, sin razonamiento largo, y por el websocket del chat."""
from fastapi.testclient import TestClient


def _capture(s):
    seen = {}

    def fake(msgs, spec, **kw):
        seen.update(kw, system=msgs[0]["content"])
        yield "Vale."
    return seen, fake


def test_live_pide_respuesta_corta_y_sin_razonar(services):
    s = services
    seen, fake = _capture(s)
    real, s.llm.stream = s.llm.stream, fake
    try:
        s.agents.run("nexus", "cuéntame algo del mar", s.memory.new_session(), live=True)
        assert seen["reasoning"] == "off" and seen["max_tokens"] <= 400
        assert "voz" in seen["system"].lower()
        seen.clear()
        s.config.set("live.reasoning", True)
        s.agents.run("nexus", "cuéntame algo del mar", s.memory.new_session(), live=True)
        assert "reasoning" not in seen and seen["reasoning_budget"] == 256
        seen.clear()
        s.agents.run("nexus", "cuéntame algo del mar", s.memory.new_session())
        assert "reasoning" not in seen and "reasoning_budget" not in seen
    finally:
        s.llm.stream = real
        s.config.set("live.reasoning", False)


def test_modelo_solo_thinking_acorta_en_vez_de_apagar(services, monkeypatch):
    from nexus.llm.manager import LLMManager
    m = LLMManager(services.config, services.models.catalog, services.profile, None, services.runtimes)
    seen = {}

    def fake_stream(messages, model="", **opts):
        seen.clear(); seen.update(opts)
        yield "ok"
    monkeypatch.setattr(m, "select", lambda spec="default": ("llamaserver", ""))
    monkeypatch.setattr(m, "reasoning_model", lambda b, mo: True)
    monkeypatch.setattr(m.backends["llamaserver"], "stream_chat", fake_stream)
    monkeypatch.setattr(m, "thinking_only", lambda b, mo: False)
    "".join(m.stream([{"role": "user", "content": "hola"}], reasoning="off"))
    assert seen["reasoning"] == "off" and "extra_body" not in seen          # Qwen3.5: enable_thinking=false
    monkeypatch.setattr(m, "thinking_only", lambda b, mo: True)
    "".join(m.stream([{"role": "user", "content": "hola"}], reasoning="off"))
    assert "reasoning" not in seen and seen["extra_body"]["reasoning_budget_tokens"] == 256


def test_websocket_chat_live(services):
    from nexus.server.app import create_app
    s = services
    seen, fake = _capture(s)
    real, s.llm.stream = s.llm.stream, fake
    try:
        with TestClient(create_app(s)) as c, c.websocket_connect("/ws") as ws:
            ws.send_json({"type": "chat", "text": "explícame la marea", "agent": "nexus", "live": True})
            for _ in range(200):
                if ws.receive_json()["type"] == "done":
                    break
        assert seen.get("reasoning") == "off"
    finally:
        s.llm.stream = real
