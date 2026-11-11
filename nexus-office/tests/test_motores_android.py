"""Motores por hardware en la app Android (4.0.2): proceso auxiliar nativo, puente de ExecuTorch y motores que no
aplican en el movil. El proceso auxiliar se simula con un script que habla como ``nexus-office motor``."""
import json
import os
import stat
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from nexus import motores
from nexus.models import manager as mm
from nexus.motores import auxiliar, selector

AYUDANTE = r'''#!{py}
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
a = sys.argv[1:]
motor = a[1]
op = {{a[i][2:]: a[i + 1] for i in range(2, len(a) - 1, 2)}}
open(op["cache"] + "/args.json", "w").write(json.dumps({{"motor": motor, **op}}))
if op["acelerador"] == "npu" and motor == "litert":
    print("ERROR: sin NPU", flush=True); sys.exit(4)
class H(BaseHTTPRequestHandler):
    def log_message(self, *x): pass
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b'{{"status":"ok"}}')
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        for t in ["Hola", " desde ", motor, " (" + body["messages"][-1]["content"] + ")"]:
            d = {{"choices": [{{"index": 0, "delta": {{"content": t}}, "finish_reason": None}}]}}
            self.wfile.write(("data: " + json.dumps(d) + "\n\n").encode())
        self.wfile.write(b"data: [DONE]\n\n")
HTTPServer(("127.0.0.1", int(op["puerto"])), H).serve_forever()
'''


# el proceso auxiliar solo existe en Android (POSIX): en Windows no se puede ejecutar el script que lo simula
solo_posix = pytest.mark.skipif(sys.platform == "win32", reason="el proceso auxiliar es de Android (POSIX)")


@pytest.fixture
def libs(tmp_path, monkeypatch):
    d = tmp_path / "lib"
    d.mkdir()
    f = d / auxiliar.AYUDANTE
    f.write_text(AYUDANTE.format(py=sys.executable))
    f.chmod(f.stat().st_mode | stat.S_IEXEC)
    for n in ("liblitert-lm.so", "libGenie.so", "libQnnHtp.so", "libQnnSystem.so", "libonnxruntime-genai.so",
              "libonnxruntime.so"):
        (d / n).write_bytes(b"")
    monkeypatch.setenv("NEXUS_ANDROID", "1")
    monkeypatch.setenv("NEXUS_NATIVE_LIB_DIR", str(d))
    monkeypatch.delenv("NEXUS_PUENTE_URL", raising=False)
    return d


def test_disponible_en_android(libs, monkeypatch):
    for m in ("litert", "genie", "onnx"):
        assert motores.disponible(m) == (True, ""), m
    assert motores.disponible("executorch")[0] is False   # sin puente
    monkeypatch.setenv("NEXUS_PUENTE_URL", "http://127.0.0.1:1")
    (libs / "libexecutorch.so").write_bytes(b"")
    assert motores.disponible("executorch") == (True, "")
    for m in ("tensorrt", "openvino", "mlc"):
        ok, motivo = motores.disponible(m)
        assert not ok and motivo == motores.NO_ANDROID[m]
        assert motores.aplica(m)[0] is False
    (libs / "libQnnHtp.so").unlink()
    ok, motivo = motores.disponible("genie")
    assert not ok and "libQnnHtp.so" in motivo


def test_sin_android_no_cambia(monkeypatch):
    monkeypatch.delenv("NEXUS_ANDROID", raising=False)
    if hasattr(sys, "getandroidapilevel"):
        pytest.skip("Python de Android")
    assert motores.aplica("tensorrt")[0] is True or sys.platform == "darwin"


@solo_posix
def test_auxiliar_genera_y_cierra(libs, tmp_path):
    m = motores.abrir("genie", tmp_path, "npu", motores.Params(n_ctx=2048))
    try:
        assert isinstance(m, auxiliar.Auxiliar) and m.acelerador == "npu"
        args = json.loads((auxiliar.paths.sub("cache") / "motores" / "genie" / "args.json").read_text())
        assert args["motor"] == "genie" and args["lib"].endswith("libGenie.so") and args["ctx"] == "2048"
        assert args["dispatch"] == str(libs)
        txt = "".join(m.generar([{"role": "user", "content": "hola"}], motores.Params(), lambda: False))
        assert txt == "Hola desde genie (hola)"
    finally:
        m.cerrar()
    assert m.proc.poll() is not None


@solo_posix
def test_auxiliar_falla_con_su_registro(libs, tmp_path):
    with pytest.raises(RuntimeError, match="sin NPU"):
        motores.abrir("litert", tmp_path / "m_sm8750.litertlm", "npu", motores.Params())


@solo_posix
def test_entorno_npu(libs):
    env = auxiliar.entorno(libs)
    assert env["LD_LIBRARY_PATH"].split(":")[0] == str(libs)
    assert env["ADSP_LIBRARY_PATH"].split(";")[0] == str(libs)
    assert env["ORT_LIB_PATH"].endswith("libonnxruntime.so")
    assert "LD_PRELOAD" not in env


def test_sse():
    lineas = ['data: {"choices":[{"delta":{"role":"assistant"}}]}', "", 'data: {"choices":[{"delta":{"content":"a"}}]}',
              "data: [DONE]", 'data: {"choices":[{"delta":{"content":"no"}}]}']
    assert list(auxiliar._sse(iter(lineas))) == ["a"]
    with pytest.raises(RuntimeError, match="roto"):
        list(auxiliar._sse(iter(['data: {"error":{"message":"roto"}}'])))


def test_puente_executorch(libs, tmp_path, monkeypatch):
    vistos = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            vistos.append((self.path, self.headers.get("X-Nexus-Clave"), body))
            if self.path.endswith("/generar"):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write("Hola<|im_end|>".encode())
            else:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"{}")
    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    monkeypatch.setenv("NEXUS_PUENTE_URL", f"http://127.0.0.1:{srv.server_address[1]}")
    monkeypatch.setenv("NEXUS_PUENTE_CLAVE", "secreta")
    (libs / "libexecutorch.so").write_bytes(b"")
    (tmp_path / "model.pte").write_bytes(b"")
    (tmp_path / "tokenizer.json").write_text("{}")
    try:
        m = motores.abrir("executorch", tmp_path, "", motores.Params())
        assert isinstance(m, auxiliar.PuenteJava)
        txt = "".join(m.generar([{"role": "user", "content": "hola"}], motores.Params(max_tokens=50), lambda: False))
        assert txt.strip() == "Hola"
        m.cerrar()
    finally:
        srv.shutdown()
    assert [v[0] for v in vistos] == ["/executorch/cargar", "/executorch/generar", "/executorch/cerrar"]
    assert all(v[1] == "secreta" for v in vistos)
    assert vistos[0][2]["modelo"].endswith("model.pte") and "hola" in vistos[1][2]["prompt"]


def test_estado_marca_lo_que_no_aplica(libs):
    hw = {"npus": [{"vendor": "qualcomm", "name": "Hexagon"}], "cpu_name": "Qualcomm SM8750"}
    st = selector.estado([], hw, {"accelerator": "cpu", "npu": {"present": True}})
    por_id = {m["id"]: m for m in st["motores"]}
    assert por_id["genie"]["instalado"] and por_id["genie"]["aplica"]
    assert por_id["tensorrt"]["aplica"] is False and not por_id["tensorrt"]["instalado"]
    assert por_id["llamaserver"]["aplica"] is True


def test_litert_npu_solo_con_modelo_de_npu():
    hw = {"npus": [{"vendor": "qualcomm", "name": "Hexagon"}]}
    prof = {"accelerator": "cpu", "npu": {"present": True}}
    gen = selector.orden_aceleradores("litert", {"path": "/m/gemma-4-E2B-it.litertlm"}, hw, prof)
    npu = selector.orden_aceleradores("litert", {"path": "/m/gemma-4-E2B-it_qualcomm_sm8750.litertlm"}, hw, prof)
    assert "npu" not in gen and gen[-1] == "cpu"
    assert npu[0] == "npu"
    assert selector.orden_aceleradores("litert", {"path": "/m/g.litertlm"}, hw, prof, forzado="npu")[0] == "npu"


def test_elegir_litert_por_soc():
    names = ["gemma-4-E2B-it.litertlm", "gemma-4-E2B-it_qualcomm_sm8650.litertlm",
             "gemma-4-E2B-it_qualcomm_sm8750.litertlm", "gemma-4-E2B-it_mt6991.litertlm", "README.md"]
    assert mm.elegir_litert(names, ["sm8750", "sun"]) == "gemma-4-E2B-it_qualcomm_sm8750.litertlm"
    assert mm.elegir_litert(names, ["mt6991"]) == "gemma-4-E2B-it_mt6991.litertlm"
    assert mm.elegir_litert(names, ["gs201", "tensorg2"]) is None
