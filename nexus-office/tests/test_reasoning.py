"""Modelos con vision y razonamiento: catalogo, separacion del razonamiento y presupuesto de tokens."""
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from nexus.llm.backends import OpenAICompatBackend, ThinkSplitter, is_reasoning_model, split_stream


def test_catalog_llms_have_vision_and_reasoning():
    from nexus.models.recommended import RECOMMENDED
    llms = [r for r in RECOMMENDED if r["kind"] == "llm" and not r.get("sector_only")]   # los de sector son solo sugerencias
    assert llms and all(r["vision"] and r["reasoning"] and r.get("mmproj") for r in llms)
    # Qwen3.5 razona por defecto (razonamiento hibrido); Qwen3-VL solo en sus variantes «Thinking»
    assert all(r["id"].startswith("qwen3.5-") or ("Thinking" in r["repo"] and "Thinking" in r["file"]) for r in llms)
    assert all(is_reasoning_model(r) for r in llms)


@pytest.mark.parametrize("ram", [3.5, 6, 8, 16, 32])
def test_best_llm_always_reasons(ram):
    from nexus.hardware.detect import HardwareInfo
    from nexus.hardware.profiles import build_profile
    from nexus.models.recommended import recommend
    hw = HardwareInfo(os="linux", os_version="", arch="x86_64", cpu_name="i3-1115G4", cores_physical=2,
                      cores_logical=4, cpu_flags=["avx2"], ram_gb=ram, disk_free_gb=50, gpus=[], npus=[], runtimes={},
                      python="3.11")
    best = [r for r in recommend(build_profile(hw)) if r["kind"] == "llm" and r.get("best")]
    assert len(best) == 1 and best[0]["reasoning"] and best[0]["vision"]


def chunks(text, n=3):
    return [text[i:i + n] for i in range(0, len(text), n)]


def run(pieces, mode):
    sp, thought = ThinkSplitter(mode), []
    visible = "".join(split_stream(iter(pieces), mode, thought.append, sp))
    return visible, "".join(thought)


def test_splitter_explicit_tags_split_across_chunks():
    v, t = run(chunks("Hola <think>pienso esto</think>Respuesta final"), "tags")
    assert v == "Hola Respuesta final" and t == "pienso esto"


def test_splitter_detect_template_opened_think():
    # la plantilla abrio <think> en el prompt: la salida trae el razonamiento y solo el cierre
    v, t = run(chunks("Primero miro la imagen. Luego cuento.\n</think>\n\nHay 3 gatos."), "detect")
    assert v == "Hay 3 gatos." and t.startswith("Primero miro")


def test_splitter_detect_without_reasoning_is_visible():
    long = "Respuesta normal sin razonamiento. " * 10
    v, t = run(chunks(long), "detect")
    assert v == long and t == ""


def test_splitter_tags_mode_passes_lt_signs():
    v, _ = run(chunks("a < b y <tool>{\"name\": \"x\"}</tool>"), "tags")
    assert v == "a < b y <tool>{\"name\": \"x\"}</tool>"


class _SSE(BaseHTTPRequestHandler):
    deltas: list = []
    bodies: list = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{"data": [{"id": "qwen3-vl-2b-thinking"}]}')

    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        _SSE.bodies.append(json.loads(self.rfile.read(n)))
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        for d in _SSE.deltas:
            self.wfile.write(f"data: {json.dumps({'choices': [{'delta': d}]})}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")


@pytest.fixture
def server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), _SSE)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    yield f"http://127.0.0.1:{srv.server_address[1]}/v1"
    srv.shutdown()


def test_openai_reasoning_content_goes_apart(server):
    _SSE.deltas = [{"reasoning_content": "Veo un "}, {"reasoning_content": "gato."}, {"content": "Es un gato."}]
    b = OpenAICompatBackend(server)
    got = []
    out = "".join(b.stream_chat([{"role": "user", "content": "¿qué es?"}], on_reasoning=got.append, think_mode="detect"))
    assert out == "Es un gato." and "".join(got) == "Veo un gato." and b.last_reasoning == "Veo un gato."
    assert b.last_stats["tokens"] == 3          # el razonamiento cuenta para los tokens/s


def test_openai_budget_exhausted_shows_reasoning(server):
    _SSE.deltas = [{"reasoning_content": "Calculo paso a paso"}]
    out = "".join(OpenAICompatBackend(server).stream_chat([{"role": "user", "content": "x"}], think_mode="detect"))
    assert "agotado" in out and "Calculo paso a paso" in out


def test_manager_reserves_reasoning_budget(services, server):
    from nexus.llm.manager import LLMManager
    _SSE.deltas, _SSE.bodies = [{"content": "ok"}], []
    services.config.set("llm.openai_base_url", server)
    services.config.set("llm.openai_model", "qwen3-vl-2b-thinking")
    services.config.set("llm.backend_order", ["openai", "builtin"])
    services.config.set("llm.max_tokens", 500)
    services.config.set("llm.reasoning_budget", 1000)
    m = LLMManager(services.config, services.models.catalog, services.profile, None, services.runtimes)
    assert m.select()[0] == "openai" and m.reasoning_model("openai", "")
    assert "".join(m.stream([{"role": "user", "content": "hola"}])) == "ok"
    assert _SSE.bodies[-1]["max_tokens"] == 1500 and "chat_template_kwargs" not in _SSE.bodies[-1]
    m.complete("resume")
    assert _SSE.bodies[-1]["max_tokens"] == 500 + 768            # tareas internas: presupuesto menor
    services.config.set("llm.reasoning", "off")
    "".join(m.stream([{"role": "user", "content": "hola"}]))
    assert _SSE.bodies[-1]["chat_template_kwargs"] == {"enable_thinking": False} and _SSE.bodies[-1]["max_tokens"] == 500


def test_complete_returns_empty_when_model_only_thought(services, server):
    from nexus.llm.manager import LLMManager
    _SSE.deltas = [{"reasoning_content": "mucho pensar"}]
    services.config.set("llm.openai_base_url", server)
    services.config.set("llm.openai_model", "qwen3-vl-2b-thinking")
    services.config.set("llm.backend_order", ["openai", "builtin"])
    m = LLMManager(services.config, services.models.catalog, services.profile, None, services.runtimes)
    assert m.complete("resume esto") == ""       # quien llama usa su alternativa sin LLM


def test_resolve_recommended_finds_renamed_files(services):
    from nexus.models.recommended import RECOMMENDED
    rec = next(r for r in RECOMMENDED if r["id"] == "qwen3-vl-2b-thinking")

    class FakeHF:
        def files(self, repo):
            return [{"path": p} for p in ("README.md", "Qwen3VL-2B-Thinking-Q8_0.gguf", "Qwen3VL-2B-Thinking-Q4_K_M-v2.gguf",
                                          "mmproj-Qwen3VL-2B-Thinking-F16.gguf")]
    services.models.sources["huggingface"] = FakeHF()
    out = services.models.resolve_recommended(rec)
    assert out["file"] == "Qwen3VL-2B-Thinking-Q4_K_M-v2.gguf" and out["mmproj"] == "mmproj-Qwen3VL-2B-Thinking-F16.gguf"

    class Offline:
        def files(self, repo):
            raise OSError("sin red")
    services.models.sources["huggingface"] = Offline()
    assert services.models.resolve_recommended(rec) == rec


def test_llamaserver_gets_native_reasoning_budget(services, monkeypatch):
    from nexus.llm.manager import LLMManager
    m = LLMManager(services.config, services.models.catalog, services.profile, None, services.runtimes)
    seen = {}

    def fake_stream(messages, model="", **opts):
        seen.update(opts)
        yield "ok"
    monkeypatch.setattr(m, "select", lambda spec="default": ("llamaserver", ""))
    monkeypatch.setattr(m, "reasoning_model", lambda b, mo: True)
    monkeypatch.setattr(m.backends["llamaserver"], "stream_chat", fake_stream)
    services.config.set("llm.reasoning_budget", 900)
    assert "".join(m.stream([{"role": "user", "content": "hola"}])) == "ok"
    assert seen["extra_body"]["reasoning_budget_tokens"] == 900 and seen["think_mode"] == "detect"


def test_bundled_llama_server_is_used_when_nothing_installed(tmp_path, monkeypatch):
    import os

    from nexus import runtimes
    exe = "llama-server.exe" if os.name == "nt" else "llama-server"
    for v in ("cpu", "cpu-compat"):
        (tmp_path / "llama" / v).mkdir(parents=True)
        (tmp_path / "llama" / v / exe).write_bytes(b"")
    monkeypatch.setattr(runtimes, "BUNDLED", tmp_path)
    rm = runtimes.RuntimeManager()
    monkeypatch.setattr(rm, "installed", lambda: {})
    monkeypatch.setattr(runtimes, "_has_avx2", lambda: True)
    assert rm.exe("llama", {"accelerator": "cpu", "id": "cpu"}) == (tmp_path / "llama" / "cpu" / exe, "cpu")
    monkeypatch.setattr(runtimes, "_has_avx2", lambda: False)      # CPU sin AVX2: la variante compatible
    assert rm.exe("llama", {"accelerator": "cpu", "id": "cpu"})[0].parent.name == "cpu-compat"
