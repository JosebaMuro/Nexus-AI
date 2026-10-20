"""Selector de inicio: motor llama y CPU/GPU/NPU elegidos a mano, aplicados al perfil y comprobados en el registro."""
import sys

from fastapi.testclient import TestClient

from nexus.hardware import accel_choice as ac
from nexus.hardware.detect import GPU, NPU
from test_hardware import hw


def test_plan_por_acelerador(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    nv = hw(gpus=[GPU("nvidia", "RTX 4060", 8)], npus=[NPU("intel", "Intel AI Boost")])
    assert ac.plan(nv, "cpu")["variant"] == "cpu"
    g = ac.plan(nv, "gpu")
    assert g["override"] == "cuda" and g["variant"] == "cuda" and g["ok"]
    n = ac.plan(nv, "npu")
    assert n["variant"] == "openvino" and n["env"] == {"GGML_OPENVINO_DEVICE": "NPU"} and n["ok"]
    amd = hw(gpus=[GPU("amd", "Radeon 780M", 2, integrated=True)], npus=[NPU("amd", "Ryzen AI NPU")])
    p = ac.plan(amd, "npu")      # la NPU de AMD no la usa llama.cpp: se dice y se va a la GPU
    assert not p["ok"] and "NPU" in p["note"] and p["override"] == "vulkan"
    assert ac.plan(hw(), "gpu") == {**ac.plan(hw(), "gpu"), "override": "cpu", "ok": False}


def test_build_for_ajusta_el_perfil(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    h = hw(gpus=[GPU("nvidia", "RTX 4060", 8)], npus=[NPU("intel", "Intel AI Boost")])
    prof, _ = ac.build_for(h, "gpu")
    assert prof["accelerator"] == "cuda" and prof["llm"]["gpu_layers"] == -1
    prof, _ = ac.build_for(h, "cpu")
    assert prof["llm"]["gpu_layers"] == 0
    prof, _ = ac.build_for(h, "npu")
    assert prof["llm"]["device_env"] == {"GGML_OPENVINO_DEVICE": "NPU"} and prof["llm"]["single_slot"]
    assert prof["llm"]["n_ctx"] <= 4096 and "llm" in prof["npu"]["offload"]


def test_android_npu_y_gpu(monkeypatch):
    monkeypatch.setenv("NEXUS_ANDROID", "1")
    devs = [{"dev": "HTP0", "backend": "hexagon", "name": "Hexagon"},
            {"dev": "GPUOpenCL", "backend": "opencl", "name": "QUALCOMM Adreno(TM) 750"}]
    h = hw(gpus=[GPU("qualcomm", "Adreno 750", 0, True)], runtimes={"android_accel": {"devices": devs}})
    assert ac.plan(h, "npu")["device"] == "HTP0"
    assert ac.plan(h, "gpu")["device"] == "GPUOpenCL"
    sin_npu = hw(gpus=[GPU("arm", "Mali", 0, True)],
                 runtimes={"android_accel": {"devices": [{"dev": "Vulkan0", "backend": "vulkan", "name": "Mali-G715"}]}})
    p = ac.plan(sin_npu, "npu")
    assert p["device"] == "Vulkan0" and not p["ok"] and "NPU" in p["note"]


def test_registro_de_llama_server():
    log = ("ggml_vulkan: 0 = NVIDIA GeForce RTX 4060 (NVIDIA) | uma: 0 | fp16: 1\n"
           "llama_model_load_from_file_impl: using device Vulkan0 (NVIDIA GeForce RTX 4060) - 7800 MiB free\n"
           "load_tensors: offloaded 37/37 layers to GPU\n")
    e = ac.effective_from_log(log)
    assert e["device"] == "GPU" and e["offloaded"] == "37/37" and "Vulkan0" in e["detail"]
    assert ac.effective_from_log("load_tensors: offloaded 0/37 layers to GPU\n")["device"] == "CPU"
    assert ac.effective_from_log("using device HTP0 (Hexagon) - 2048 MiB free\noffloaded 29/29 layers to GPU")["device"] \
        == "NPU (Hexagon)"
    assert ac.effective_from_log("OpenVINO backend: device NPU\n", {"GGML_OPENVINO_DEVICE": "NPU"})["device"] == "NPU (OpenVINO)"


def test_motor_integrado_desde_el_paquete(services, monkeypatch, tmp_path):
    from nexus import runtimes as rt
    monkeypatch.setattr(rt, "llama_python_ready", lambda: False)
    monkeypatch.setattr(rt, "BUNDLED", tmp_path)
    assert ac._engine_state(services, "llamacpp") == (False, "este paquete no lo incluye")
    (tmp_path / "llama-python").mkdir()
    (tmp_path / "llama-python" / "llama_cpp_python-0.3.36-cp311-cp311-linux_x86_64.whl").write_bytes(b"x")
    ok, note = ac._engine_state(services, "llamacpp")
    assert "se instala al elegirlo" in note
    monkeypatch.setattr(rt, "install_llama_python", lambda: "roto")
    import pytest
    with pytest.raises(ValueError, match="roto"):
        ac.apply(services, "llamacpp", "auto")


def test_motor_elegido_va_primero(services):
    s = services
    calls = []
    real = s.llm.is_available
    s.llm.is_available = lambda n: calls.append(n) or n in ("ollama", "builtin")
    try:
        s.config.set("llm.engine", "ollama")
        assert s.llm.select()[0] == "ollama" and calls[0] == "ollama"
        s.config.set("llm.engine", "llamaserver")     # no disponible: el siguiente que funcione
        assert s.llm.select()[0] == "ollama"
    finally:
        s.llm.is_available = real
        s.config.set("llm.engine", "auto")


def test_api_selector(services):
    from nexus.server.app import create_app
    s = services
    old = dict(s.profile)
    try:
        with TestClient(create_app(s)) as c:
            d = c.get("/api/accel").json()
            assert {e["id"] for e in d["engines"]} == set(ac.ENGINES) and {a["id"] for a in d["accels"]} == set(ac.ACCELS)
            assert d["ask"] is True and d["hardware"]["cpu"]
            r = c.post("/api/accel", json={"engine": "auto", "accel": "cpu", "remember": True}).json()
            assert r["accel"] == "cpu" and r["variant"] == "cpu" and r["profile"]["gpu_layers"] == 0
            d = c.get("/api/accel").json()
            assert d["ask"] is False and d["accel"] == "cpu" and s.config.get("hardware.llama_variant") == "cpu"
            assert c.post("/api/accel", json={"accel": "turbo"}).status_code == 400
            t = c.post("/api/accel/test").json()
            assert t["ok"] and t["backend"] == "builtin"
    finally:
        s.config.set("hardware.accel", "auto")
        s.config.set("hardware.llama_variant", "")
        s.config.set("startup.ask_accel", True)
        s.profile.clear()
        s.profile.update(old)
        s.llm.rebuild()
