"""Motores de inferencia por hardware: formatos, matriz por chip, selector, banco y backend con un motor falso."""
import json

import pytest

from nexus import motores
from nexus.motores import backend as mb, banco, chips, plantillas, selector
from nexus.motores.litert import texto_trozo


# ------------------------------------------------------------------ formatos
def test_detectar_formato(tmp_path):
    (tmp_path / "onnx").mkdir()
    (tmp_path / "onnx" / "genai_config.json").write_text("{}")
    (tmp_path / "ov").mkdir()
    (tmp_path / "ov" / "openvino_model.xml").write_text("")
    (tmp_path / "ov" / "openvino_tokenizer.xml").write_text("")
    (tmp_path / "mlc").mkdir()
    (tmp_path / "mlc" / "mlc-chat-config.json").write_text("{}")
    (tmp_path / "genie").mkdir()
    (tmp_path / "genie" / "genie_config.json").write_text("{}")
    (tmp_path / "trt").mkdir()
    (tmp_path / "trt" / "rank0.engine").write_text("")
    (tmp_path / "m.litertlm").write_text("")
    (tmp_path / "m.pte").write_text("")
    f = motores.detectar_formato
    assert f(tmp_path / "onnx")["format"] == "onnx-genai"
    assert f(tmp_path / "ov")["format"] == "openvino"
    assert f(tmp_path / "mlc")["format"] == "mlc"
    assert f(tmp_path / "genie")["format"] == "genie"
    assert f(tmp_path / "trt")["format"] == "tensorrt"
    assert f(tmp_path / "m.litertlm")["format"] == "litertlm"
    assert f(tmp_path / "m.pte")["format"] == "executorch"
    assert f(tmp_path / "nada") is None
    assert motores.motor_de_formato("onnx-genai") == "onnx" and motores.motor_de_formato("gguf") == "llamaserver"


def test_classify_reconoce_formatos_de_motor(tmp_path):
    from nexus.models.manager import classify
    d = tmp_path / "Qwen2.5-1.5B-Instruct-int4-ov"
    d.mkdir()
    (d / "config.json").write_text("{}")
    (d / "openvino_model.xml").write_text("")
    (d / "openvino_tokenizer.xml").write_text("")
    assert classify(d) == {"format": "openvino", "kind": "llm"}


# ------------------------------------------------------------------ chips y matriz
def _hw(npus=(), gpus=(), cpu="Intel Core i5"):
    return {"npus": [dict(n) for n in npus], "gpus": [dict(g) for g in gpus], "cpu_name": cpu, "runtimes": {}}


@pytest.mark.parametrize("hw,esperado", [
    (_hw(npus=[{"vendor": "qualcomm", "name": "Snapdragon X Elite NPU"}]), "snapdragon"),
    (_hw(npus=[{"vendor": "google", "name": "Tensor G3 TPU"}]), "tensor"),
    (_hw(npus=[{"vendor": "mediatek", "name": "Dimensity 9300 APU"}]), "mediatek"),
    (_hw(gpus=[{"vendor": "nvidia", "name": "RTX 4070", "integrated": False}]), "nvidia"),
    (_hw(gpus=[{"vendor": "amd", "name": "RX 7800", "integrated": False}]), "amd"),
    (_hw(npus=[{"vendor": "intel", "name": "Intel AI Boost"}]), "intel"),
    (_hw(gpus=[{"vendor": "apple", "name": "M3 GPU", "integrated": True}], cpu="Apple M3"), "apple"),
    (_hw(cpu="AMD Ryzen 5 3600"), "cpu"),
])
def test_chip(hw, esperado):
    assert chips.chip(hw, {}) == esperado


def test_matriz_respaldo_y_plataformas():
    for fam in chips.FAMILIAS:
        for plat in ("windows-x86_64", "linux-x86_64", "linux-arm64", "android-arm64", "macos-arm64", "windows-arm64"):
            m = chips.matriz(fam, plat)
            motores_ = [p["motor"] for p in m["orden"]]
            assert "llamaserver" in motores_, (fam, plat)
            assert all(x in motores.MOTORES or x == "llamaserver" for x in motores_)
            if plat.startswith("android"):
                assert "tensorrt" not in motores_ and "openvino" not in motores_
            if plat.startswith("macos"):
                assert "tensorrt" not in motores_ and "genie" not in motores_
    assert chips.matriz("nvidia", "linux-x86_64")["orden"][0]["motor"] == "tensorrt"
    assert chips.matriz("intel", "windows-x86_64")["orden"][0] == {"motor": "openvino", "acelerador": "NPU",
                                                                   "motivo": "OpenVINO GenAI en la NPU de Core Ultra"}
    assert chips.matriz("snapdragon", "android-arm64")["orden"][0]["motor"] == "genie"
    assert chips.matriz("amd", "windows-x86_64")["orden"][0]["acelerador"] == "vitisai"


# ------------------------------------------------------------------ selector y banco
def _cat():
    return [{"id": "qwen-gguf", "kind": "llm", "format": "gguf"},
            {"id": "qwen-ov", "kind": "llm", "format": "openvino"},
            {"id": "phi-onnx", "kind": "llm", "format": "onnx-genai"},
            {"id": "voz", "kind": "tts", "format": "onnx"}]


def test_selector_matriz_y_banco(monkeypatch):
    monkeypatch.setattr(selector, "disponible", lambda m: (m in ("llamaserver", "openvino", "onnx"), ""))
    monkeypatch.setattr(selector, "plataforma", lambda: "windows-x86_64")
    hw = _hw(npus=[{"vendor": "intel", "name": "Intel AI Boost"}])
    c = selector.candidatos(_cat(), hw, {}, banco={"resultados": {}})
    assert [x["id"] for x in c] == ["qwen-ov", "phi-onnx", "qwen-gguf"]   # orden de la matriz para Intel
    # medido: llama.cpp va mas rapido en este equipo -> manda la medida
    medido = {"resultados": {"llamaserver:qwen-gguf": {"tps": 30}, "openvino:qwen-ov": {"tps": 12}}}
    c = selector.candidatos(_cat(), hw, {}, banco=medido)
    assert c[0]["id"] == "qwen-gguf" and c[0]["tps"] == 30
    assert selector.orden_aceleradores("openvino", {}, hw, {})[:2] == ["NPU", "GPU"]
    assert selector.orden_aceleradores("openvino", {}, hw, {}, forzado="CPU")[0] == "CPU"
    assert selector.orden_aceleradores("genie", {}, hw, {}) == ["npu"]


def test_selector_prefiere_el_modelo_mayor_si_va_comodo(monkeypatch):
    """Elegir solo por tokens/s dejaria siempre el modelo mas pequeno: entre los que van comodos gana el mayor."""
    monkeypatch.setattr(selector, "disponible", lambda m: (True, ""))
    monkeypatch.setattr(selector, "plataforma", lambda: "linux-x86_64")
    cat = [{"id": "peq", "kind": "llm", "format": "gguf", "size_mb": 1000},
           {"id": "grande", "kind": "llm", "format": "gguf", "size_mb": 5000},
           {"id": "enorme", "kind": "llm", "format": "gguf", "size_mb": 20000},
           {"id": "peq-ov", "kind": "llm", "format": "openvino", "size_mb": 1100}]
    medido = {"resultados": {"llamaserver:peq": {"tps": 40}, "llamaserver:grande": {"tps": 12},
                             "llamaserver:enorme": {"tps": 2}, "openvino:peq-ov": {"tps": 55}}}
    orden = [c["id"] for c in selector.candidatos(cat, _hw(), {}, banco=medido)]
    # grande va comodo (12 >= 8); los dos pequenos compiten por velocidad; el enorme (2 tok/s) al final
    assert orden == ["grande", "peq-ov", "peq", "enorme"]
    lentos = {"resultados": {"llamaserver:peq": {"tps": 6}, "llamaserver:grande": {"tps": 3}}}
    assert selector.candidatos(cat[:2], _hw(), {}, banco=lentos)[0]["id"] == "peq"   # ninguno comodo: el mas rapido


def test_llamaserver_lote_cache_y_reintento_de_gpu(monkeypatch):
    from nexus.llm import llamaserver as ls
    assert ls.cache_ram_mb(8, False) == 819 and ls.cache_ram_mb(8, True) == 409
    assert ls.cache_ram_mb(2, True) == 256 and ls.cache_ram_mb(256, False) == 8192
    assert ls.lote_fisico(512, -1, 24) == 1024 and ls.lote_fisico(2048, -1, 16) == 2048
    assert ls.lote_fisico(512, -1, 8) == 512 and ls.lote_fisico(256, 0, 0) == 256 and ls.lote_fisico(512, 0, 24) == 512
    # escritorio: si la GPU no arranca, CPU durante GPU_RETRY_S sin volver a intentarla en cada peticion
    monkeypatch.delenv("NEXUS_RUNTIME_LLAMA_CPU", raising=False)
    b = ls.LlamaServerBackend(None, lambda m: None, lambda: {})
    vistos = []

    def falso(model, opts):
        vistos.append(opts.get("gpu_layers"))
        if opts.get("gpu_layers"):
            raise ls._GpuFailed("la GPU no arranca")
    monkeypatch.setattr(b, "_ensure", falso)
    b.ensure("m", {"gpu_layers": -1})
    assert vistos == [-1, 0] and b.gpu_retry_at > 0
    b.ensure("m", {"gpu_layers": -1})
    assert vistos == [-1, 0, 0]                      # dentro del plazo: directo a la CPU
    b.gpu_retry_at = 1.0                             # plazo vencido: se vuelve a probar la GPU
    b.ensure("m", {"gpu_layers": -1})
    assert vistos[-2:] == [-1, 0]


# ------------------------------------------------------------------ backend con un motor falso
class Falso(motores.Motor):
    id = "onnx"
    fallar = set()

    def __init__(self, modelo, acelerador, params):
        if acelerador in Falso.fallar:
            raise RuntimeError(f"sin {acelerador}")
        super().__init__(modelo, acelerador, params)

    def generar(self, messages, p, parar):
        assert messages[-1]["content"] == "hola"
        for t in ["Hola", ", ", "soy ", "Agente IA"]:
            if parar():
                return
            yield t


def test_backend_cae_al_siguiente_acelerador(monkeypatch, tmp_path):
    monkeypatch.setattr(mb, "abrir", lambda motor, ruta, acel, p: Falso(ruta, acel, p))
    monkeypatch.setattr(mb, "disponible", lambda m: (True, ""))
    Falso.fallar = {"qnn"}
    entry = {"id": "phi-onnx", "format": "onnx-genai", "path": str(tmp_path)}
    b = mb.MotorBackend("onnx", lambda mid: (entry["path"], entry), lambda m, e: ["qnn", "cpu"])
    assert b.available()
    out = "".join(b.stream_chat([{"role": "user", "content": "hola"}], model="phi-onnx", max_tokens=8))
    assert out == "Hola, soy Agente IA"
    assert b.efectivo["acelerador"] == "cpu" and b.last_stats.get("tokens")
    assert not list((tmp_path.parent / "data" / "motores").glob("*.marca")) if (tmp_path.parent / "data").exists() else True
    # todos fallan -> error con el motivo de cada uno
    Falso.fallar = {"qnn", "cpu"}
    b2 = mb.MotorBackend("onnx", lambda mid: (entry["path"], entry), lambda m, e: ["qnn", "cpu"])
    with pytest.raises(RuntimeError, match="sin qnn"):
        "".join(b2.stream_chat([{"role": "user", "content": "hola"}], model="phi-onnx"))


def test_marcas_de_caida(data_dir):
    with motores.Marca("litert", "npu"):
        pass
    assert motores.revisar_marcas() == []
    m = motores.Marca("litert", "npu")
    m.__enter__()   # el proceso «se cae» sin salir del bloque
    assert motores.revisar_marcas() == ["litert-npu"]
    assert "litert-npu" in motores.caidas()
    motores.olvidar_caidas()
    assert motores.caidas() == {}


def test_banco_mide_y_guarda(monkeypatch, data_dir, tmp_path):
    monkeypatch.setattr(mb, "abrir", lambda motor, ruta, acel, p: Falso(ruta, acel, p))
    monkeypatch.setattr(mb, "disponible", lambda m: (True, ""))
    monkeypatch.setattr(motores, "disponible", lambda m: (True, ""))
    Falso.fallar = set()
    entry = {"id": "phi-onnx", "kind": "llm", "format": "onnx-genai", "path": str(tmp_path)}

    class LLM:
        backends = {"onnx": mb.MotorBackend("onnx", lambda mid: (entry["path"], entry), lambda m, e: ["cpu"])}

    PROMPT = banco.PROMPT
    monkeypatch.setattr(banco, "PROMPT", PROMPT[:-1] + [{"role": "user", "content": "hola"}])
    data = banco.ejecutar(LLM(), [entry])
    r = data["resultados"]["onnx:phi-onnx"]
    assert r["tps"] > 0 and r["acelerador"] == "cpu" and r["muestra"] == "Hola, soy Agente IA"
    assert json.loads((data_dir / "motores" / "banco.json").read_text())["resultados"]["onnx:phi-onnx"]["tps"] > 0


def test_gestor_usa_el_motor_del_formato(services, monkeypatch, tmp_path):
    monkeypatch.setattr(mb, "abrir", lambda motor, ruta, acel, p: Falso(ruta, acel, p))
    monkeypatch.setattr(mb, "disponible", lambda m: (True, ""))
    Falso.fallar = set()
    d = tmp_path / "phi"
    d.mkdir()
    (d / "genai_config.json").write_text("{}")
    e = services.models.register(d, mid="phi-onnx", kind="llm")
    assert e["format"] == "onnx-genai"
    services.config.set("llm.default_model", "phi-onnx")
    services.llm._avail_cache.clear()
    assert services.llm.select() == ("onnx", "phi-onnx")
    out = "".join(services.llm.stream([{"role": "user", "content": "hola"}]))
    assert out == "Hola, soy Agente IA"
    est = services.llm.motor_estado()
    assert est["chip"] in chips.FAMILIAS and any(m["id"] == "onnx" for m in est["motores"])


# ------------------------------------------------------------------ plantillas y LiteRT
def test_plantillas(tmp_path):
    (tmp_path / "tokenizer_config.json").write_text(json.dumps({"added_tokens_decoder": {"1": {"content": "<|im_start|>"}}}))
    msgs = [{"role": "system", "content": "S"}, {"role": "user", "content": "U"}]
    assert plantillas.detectar(tmp_path) == "chatml"
    assert plantillas.formatear("chatml", msgs).endswith("<|im_start|>assistant\n")
    assert plantillas.formatear("gemma", msgs) == "<start_of_turn>user\nS\n\nU<end_of_turn>\n<start_of_turn>model\n"
    assert plantillas.formatear("mistral", msgs) == "<s>[INST] S\n\nU [/INST]"
    assert plantillas.limpiar("hola<|im_end|>") == "hola"


def test_litert_trozos():
    assert texto_trozo('{"role":"assistant","content":[{"type":"text","text":"Ho"},{"type":"text","text":"la"}]}') == "Hola"
    assert texto_trozo('{"content":"x"}') == "x"
    assert texto_trozo("texto suelto") == "texto suelto"


def test_mensajes_simples():
    m = motores.mensajes_simples([{"role": "user", "content": "a"}, {"role": "tool", "content": "b"},
                                  {"role": "assistant", "content": "c"}])
    assert m == [{"role": "user", "content": "a\n\nb"}, {"role": "assistant", "content": "c"}]


def test_disponible_explica_el_motivo(monkeypatch):
    monkeypatch.setattr(motores, "_modulo", lambda n: False)
    ok, motivo = motores.disponible("tensorrt")
    assert not ok and "tensorrt-llm" in motivo
    ok, motivo = motores.disponible("genie")
    assert not ok and "Genie" in motivo
    assert motores.disponible("llamaserver") == (True, "")
