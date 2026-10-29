"""Agente Profesor e IA online (nexus/llm/cloud.py) contra servidores falsos: API compatible con OpenAI y Messages
API de Anthropic en streaming, claves secretas, respaldo local sin clave y consultar_profesor."""
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from fastapi.testclient import TestClient

SEEN: list[dict] = []


class Fake(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _sse(self, events):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for ev in events:
            self.wfile.write(ev.encode())
            self.wfile.flush()

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        SEEN.append({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body})
        if "mala" in self.headers.get("Authorization", "") + self.headers.get("x-api-key", ""):
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error": {"type": "authentication_error", "message": "invalid x-api-key"}}')
            return
        if self.path.endswith("/chat/completions"):
            chunks = [{"choices": [{"delta": {"content": "Hola "}}]},
                      {"choices": [{"delta": {"content": "alumno"}}], "citations": ["https://es.wikipedia.org/x"]}]
            self._sse([f"data: {json.dumps(c)}\n\n" for c in chunks] + ["data: [DONE]\n\n"])
        elif self.path.split("?")[0].endswith("/v1/messages"):
            msg = {"id": "msg_1", "type": "message", "role": "assistant", "model": body["model"], "content": [],
                   "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 5, "output_tokens": 0}}
            evs = [("message_start", {"type": "message_start", "message": msg}),
                   ("content_block_start", {"type": "content_block_start", "index": 0,
                                            "content_block": {"type": "thinking", "thinking": "", "signature": ""}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                            "delta": {"type": "thinking_delta", "thinking": "pienso"}}),
                   ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                   ("content_block_start", {"type": "content_block_start", "index": 1,
                                            "content_block": {"type": "text", "text": ""}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 1,
                                            "delta": {"type": "text_delta", "text": "Soy Claude"}}),
                   ("content_block_stop", {"type": "content_block_stop", "index": 1}),
                   ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn",
                                                                          "stop_sequence": None},
                                      "usage": {"output_tokens": 3}}),
                   ("message_stop", {"type": "message_stop"})]
            self._sse([f"event: {n}\ndata: {json.dumps(d)}\n\n" for n, d in evs])
        else:
            self.send_response(404)
            self.end_headers()


@pytest.fixture
def fake():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    SEEN.clear()
    from nexus.llm import cloud
    old = {k: dict(v) for k, v in cloud.PROVIDERS.items()}
    url = f"http://127.0.0.1:{srv.server_address[1]}"
    cloud.PROVIDERS["gemini"]["base_url"] = url + "/v1beta/openai"
    cloud.PROVIDERS["perplexity"]["base_url"] = url
    cloud.PROVIDERS["claude"]["base_url"] = url
    yield url
    cloud.PROVIDERS.clear()
    cloud.PROVIDERS.update(old)
    srv.shutdown()


def test_profesor_sin_clave_usa_el_modelo_local(services):
    s = services
    assert s.agents.get("profesor") is not None
    assert not s.llm.backends["nube"].ready()
    assert s.llm.select("nube:")[0] != "nube"            # sin clave: el modelo local
    assert s.llm.select("default")[0] != "nube"          # y nunca como respaldo automatico
    assert s.agents.route("enséñame a calcular el IVA de una factura") == "profesor"


def test_openai_compatible_y_citas(services, fake):
    s = services
    s.config.set("nube.proveedor", "perplexity")
    s.config.set("nube.clave_perplexity", "pplx-1")
    assert s.llm.select("nube:") == ("nube", "")
    out = "".join(s.llm.stream([{"role": "system", "content": "sé breve"}, {"role": "user", "content": "hola"}],
                               "nube:"))
    assert out.startswith("Hola alumno") and "https://es.wikipedia.org/x" in out
    req = SEEN[-1]
    assert req["headers"]["authorization"] == "Bearer pplx-1" and req["body"]["model"] == "sonar"
    assert req["body"]["stream"] is True and req["body"]["messages"][0]["role"] == "system"
    # otro proveedor por especificacion, con su modelo
    s.config.set("nube.clave_gemini", "g-1")
    "".join(s.llm.stream([{"role": "user", "content": "hola"}], "nube:gemini:gemini-2.5-pro"))
    assert SEEN[-1]["path"] == "/v1beta/openai/chat/completions" and SEEN[-1]["body"]["model"] == "gemini-2.5-pro"


def test_claude_por_el_sdk(services, fake):
    s = services
    s.config.set("nube.proveedor", "claude")
    s.config.set("nube.clave_claude", "sk-ant-1")
    thoughts = []
    out = "".join(s.llm.stream([{"role": "system", "content": "eres profesor"}, {"role": "user", "content": "hola"}],
                               "nube:", on_reasoning=thoughts.append))
    assert out == "Soy Claude" and "pienso" in "".join(thoughts)
    req = SEEN[-1]
    assert req["path"].split("?")[0] == "/v1/messages" and "sk-ant-1" in (req["headers"].get("x-api-key", "") + req["headers"].get("authorization", ""))
    assert req["body"]["model"] == "claude-opus-5-5" and req["body"]["system"] == "eres profesor"
    assert req["body"]["thinking"] == {"type": "adaptive", "display": "summarized"}
    assert req["body"]["output_config"] == {"effort": "medium"} and req["body"]["fallbacks"] == "default"
    assert "server-side-fallback-2026-07-01" in req["headers"].get("anthropic-beta", "")
    assert "temperature" not in req["body"]


def test_api_nube_secretos_y_prueba(services, fake):
    from nexus.server import create_app
    s = services
    with TestClient(create_app(s)) as c:
        h = {"X-Admin-Token": c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]}
        _api_nube(c, h, s)


def _api_nube(c, h, s):
    assert c.put("/api/nube", json={"proveedor": "x"}, headers=h).status_code == 400
    assert c.put("/api/nube", json={"proveedor": "claude", "clave": "k"}).status_code in (401, 403)
    st = c.put("/api/nube", json={"proveedor": "claude", "clave": "sk-ant-9"}, headers=h).json()
    assert st["listo"] and any(p["id"] == "claude" and p["con_clave"] for p in st["proveedores"])
    assert "sk-ant-9" not in json.dumps(c.get("/api/nube").json())
    cfg = c.get("/api/config", headers=h).json()
    assert cfg["nube"]["clave_claude"] == "********"
    c.put("/api/config", json={"nube.clave_claude": "********"}, headers=h)   # la vista enmascarada no la pisa
    assert s.config.get("nube.clave_claude") == "sk-ant-9"
    r = c.post("/api/nube/probar", json={}, headers=h).json()
    assert r["ok"] and r["respuesta"] == "Soy Claude"
    s.config.set("nube.clave_claude", "mala")
    r = c.post("/api/nube/probar", json={}, headers=h).json()
    assert not r["ok"] and "clave" in r["error"]
    st = c.put("/api/nube", json={"borrar_claves": ["claude"]}, headers=h).json()
    assert not st["listo"]


def test_consultar_profesor_guarda_la_leccion(services, fake):
    from nexus.tools import ToolContext
    s = services
    s.config.set("nube.proveedor", "perplexity")
    s.config.set("nube.clave_perplexity", "pplx-1")
    ctx = ToolContext(services=s, agent="investigador", session_id=s.memory.new_session())
    r = s.supervisor.execute("consultar_profesor", {"pregunta": "¿qué es el IVA?"}, ctx)
    res = r.get("result", r)
    assert res["online"] and "Hola alumno" in res["answer"] and res["memoria"]
    mem = s.memory.list(kind="leccion", agent="investigador")
    assert mem and "IVA" in mem[0]["content"]
