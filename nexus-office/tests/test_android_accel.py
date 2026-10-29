"""CPU, GPU y NPU en la app Android autonoma (nexus/hardware/android_accel.py) y su uso para acelerar el modelo."""
import json
import os
import stat
import sys
from types import SimpleNamespace

import pytest

from nexus import android_tune as at
from nexus.hardware import android_accel as aa

LIST_ADRENO = """ggml_vulkan: Found 1 Vulkan devices:
ggml_opencl: device: 'QUALCOMM Adreno(TM) 750 (OpenCL 3.0 Adreno(TM) 750)'
Available devices:
  Vulkan0: Adreno (TM) 750 (7623 MiB, 7623 MiB free)
  GPUOpenCL: QUALCOMM Adreno(TM) 750 (7623 MiB, 7623 MiB free)
"""
LIST_MALI = """Available devices:
  Vulkan0: Mali-G720-Immortalis MC12 (11520 MiB, 9000 MiB free)
"""


def test_lee_los_dispositivos_de_llama_server():
    devs = aa.parse_devices(LIST_ADRENO)
    assert [(d["dev"], d["backend"]) for d in devs] == [("Vulkan0", "vulkan"), ("GPUOpenCL", "opencl")]
    assert devs[1]["name"] == "QUALCOMM Adreno(TM) 750" and devs[1]["mem_mb"] == 7623
    assert aa.parse_devices("Available devices:\n  (none)\n") == []


def test_elige_opencl_en_adreno_y_vulkan_en_el_resto():
    assert aa.pick(aa.parse_devices(LIST_ADRENO))["dev"] == "GPUOpenCL"
    assert aa.pick(aa.parse_devices(LIST_MALI))["dev"] == "Vulkan0"
    # Adreno solo con Vulkan: llama.cpp va peor que en la CPU, se queda en CPU
    assert aa.pick(aa.parse_devices(LIST_ADRENO.replace("  GPUOpenCL", "  X").replace("X: QUALCOMM", "Y"))) is None
    assert aa.pick([]) is None
    assert aa.gpu_vendor("Mali-G720-Immortalis MC12") == "arm" and aa.gpu_vendor("Adreno (TM) 750") == "qualcomm"


@pytest.mark.parametrize("p,info,family,name", [
    ({"ro.soc.manufacturer": "QTI", "ro.soc.model": "SM8650", "ro.board.platform": "pineapple"}, {}, "qualcomm",
     "Qualcomm SM8650 (Snapdragon 8 Gen 3)"),
    ({"ro.board.platform": "mt6989", "ro.hardware": "mt6989"}, {"soc_manufacturer": "Mediatek", "soc_model": "MT6989"},
     "mediatek", "Mediatek MT6989"),
    ({"ro.board.platform": "zuma", "ro.hardware": "zuma"}, {"soc_manufacturer": "Google", "soc_model": "Tensor G3"},
     "google", "Google Tensor G3"),
    ({"ro.board.platform": "s5e9945"}, {"soc_manufacturer": "Samsung", "soc_model": "s5e9945"}, "samsung", "Samsung s5e9945"),
    ({"ro.board.platform": "rk3588"}, {}, "", "rk3588"),
])
def test_soc(p, info, family, name):
    s = aa.soc(p, info)
    assert s["family"] == family and s["name"] == name


def test_npu_por_soc_y_por_biblioteca():
    q = aa.soc({"ro.soc.manufacturer": "QTI", "ro.soc.model": "SM8650"}, {})
    n = aa.npu(q, lib=lambda names: "/vendor/lib64/libQnnHtp.so" if "libQnnHtp.so" in names else "")
    assert n["vendor"] == "qualcomm" and "Hexagon" in n["name"] and n["source"] == "vendor-lib"
    assert aa.npu(aa.soc({"ro.board.platform": "mt6989"}, {}), lib=lambda n: "")["name"].startswith("MediaTek APU")
    assert "Tensor G3 TPU" in aa.npu(aa.soc({"ro.board.platform": "zuma"}, {}), lib=lambda n: "")["name"]
    assert aa.npu(aa.soc({"ro.board.platform": "rk3588"}, {}), lib=lambda n: "") is None


def _fake_gpu_exe(libs, output):
    exe = libs / aa.GPU_EXE
    exe.write_text(f"#!/bin/sh\ncat <<'EOF'\n{output}EOF\n")
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    return exe


@pytest.mark.skipif(sys.platform == "win32", reason="script de shell")
def test_sonda_completa_con_cache(tmp_path, monkeypatch):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    (tmp_path / "nexus").mkdir()
    monkeypatch.setenv("NEXUS_DEVICE_INFO", str(tmp_path / "device_info.json"))
    (tmp_path / "device_info.json").write_text(json.dumps({"soc_manufacturer": "QTI", "soc_model": "SM8650",
                                                           "gl_renderer": "Adreno (TM) 750", "vulkan_version": "1.3"}))
    monkeypatch.setattr(aa, "props", lambda: {"ro.build.fingerprint": "f1"})
    monkeypatch.setattr(aa, "vendor_lib", lambda names, dirs=aa.VENDOR_LIBS: "")
    libs = tmp_path / "lib"
    libs.mkdir()
    exe = _fake_gpu_exe(libs, LIST_ADRENO)
    r = aa.detect(str(libs))
    assert r["gpu"]["use"]["dev"] == "GPUOpenCL" and r["gpu"]["name"] == "QUALCOMM Adreno(TM) 750"
    assert r["soc"]["family"] == "qualcomm" and r["npu"]["vendor"] == "qualcomm"
    exe.write_text("#!/bin/sh\nexit 1\n")   # otro binario (otro tamano): se repite la sonda
    assert aa.detect(str(libs))["gpu"]["use"] is None
    _fake_gpu_exe(libs, LIST_ADRENO)
    assert aa.detect(str(libs))["gpu"]["use"]["dev"] == "GPUOpenCL"
    # la GPU falla al arrancar llama-server: queda apuntado y se usa la CPU
    aa.mark_gpu_broken("se cayo")
    r = aa.detect(str(libs))
    assert r["gpu"]["use"] is None and r["gpu"]["broken"] == "se cayo"


def test_sin_binario_gpu(tmp_path, monkeypatch):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    (tmp_path / "nexus").mkdir()
    monkeypatch.setattr(aa, "props", lambda: {"ro.hardware.egl": "mali"})
    monkeypatch.setenv("NEXUS_DEVICE_INFO", str(tmp_path / "no.json"))
    r = aa.detect(str(tmp_path))
    assert r["gpu"]["binary"] is False and r["gpu"]["use"] is None and r["gpu"]["name"] == "ARM Mali"


def _hw(monkeypatch, backend="opencl", device="GPUOpenCL", ram=12):
    from nexus.hardware.detect import GPU, NPU, HardwareInfo
    monkeypatch.setenv("NEXUS_ANDROID", "1")
    monkeypatch.setattr(at, "core_layout", lambda *a: {"total": 8, "fast": 4, "source": "cpuinfo"})
    return HardwareInfo(os="Linux", os_version="", arch="aarch64", cpu_name="Qualcomm SM8650", cores_physical=8,
                        cores_logical=8, cpu_flags=["neon"], ram_gb=ram, disk_free_gb=50,
                        gpus=[GPU(vendor="qualcomm", name="Adreno 750", integrated=True, source="android-opencl")],
                        npus=[NPU(vendor="qualcomm", name="Qualcomm Hexagon NPU", runtime_hint="QNN", source="soc")],
                        runtimes={"android_accel": {"gpu_backend": backend, "gpu_device": device}})


def test_perfil_android_con_gpu(monkeypatch):
    from nexus.hardware.profiles import build_profile
    p = build_profile(_hw(monkeypatch))
    assert p["accelerator"] == "opencl" and p["llm"]["gpu_layers"] == -1 and p["llm"]["device"] == "GPUOpenCL"
    assert p["llm"]["max_model_b"] == 4 and p["llm"]["n_ctx"] <= 4096   # mismos limites de RAM del movil
    assert p["npu"]["present"] and not p["npu"]["ready"] and "GPU" in p["npu"]["hint"]
    cpu = build_profile(_hw(monkeypatch, backend="", device=""))
    assert cpu["accelerator"] == "cpu" and cpu["llm"]["gpu_layers"] == 0 and cpu["llm"]["device"] == ""
    assert "CPU" in cpu["npu"]["hint"]


class _Proc:
    def __init__(self, cmd, env, alive):
        self.cmd, self.env, self.alive = cmd, env, alive

    def poll(self):
        return None if self.alive else 1

    def terminate(self):
        self.alive = False

    def wait(self, t=None):
        return 0


def test_llama_server_usa_la_gpu_y_cae_a_cpu_si_falla(tmp_path, monkeypatch):
    from nexus.llm import llamaserver as ls
    libs = tmp_path / "lib"
    libs.mkdir()
    for n in ("libllama-server-i8mm.so", aa.GPU_EXE):
        (libs / n).write_bytes(b"\x7fELF")
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA", str(libs / "libllama-server-i8mm.so"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_CPU", str(libs / "libllama-server-i8mm.so"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_GPU", str(libs / aa.GPU_EXE))
    started = []
    gpu_works = {"ok": True}

    def popen(cmd, **kw):
        gpu = cmd[0].endswith(aa.GPU_EXE)
        p = _Proc(cmd, kw.get("env"), alive=not gpu or gpu_works["ok"])
        started.append(p)
        return p

    monkeypatch.setattr(ls, "popen", popen)
    monkeypatch.setattr(ls.httpx, "get", lambda *a, **k: SimpleNamespace(status_code=200))
    profile = {"llm": {"gpu_layers": -1, "device": "GPUOpenCL"}}
    rt = SimpleNamespace(exe=lambda *a: (libs / "libllama-server-i8mm.so", "embedded"))
    b = ls.LlamaServerBackend(rt, lambda m: (str(tmp_path / "m.gguf"), {"id": "m"}), lambda: profile)
    b.ensure("", {"n_ctx": 2048, "gpu_layers": -1, "device": "GPUOpenCL"})
    cmd = started[-1].cmd
    assert cmd[0].endswith(aa.GPU_EXE) and cmd[cmd.index("--device") + 1] == "GPUOpenCL" and "999" in cmd
    assert str(libs) in started[-1].env["LD_LIBRARY_PATH"]
    b.ensure("", {"n_ctx": 2048, "gpu_layers": 0, "device": ""})            # CPU: binario musl, sin GPU
    cmd = started[-1].cmd
    assert cmd[0].endswith("i8mm.so") and "--device" not in cmd and cmd[cmd.index("-ngl") + 1] == "0"
    assert started[-1].env is None
    # driver roto: el de GPU muere al arrancar -> se apunta y sigue en CPU
    gpu_works["ok"] = False
    (tmp_path / "nexus" / "logs").mkdir(parents=True, exist_ok=True)
    (tmp_path / "nexus" / "logs" / "llama-server.log").write_bytes(b"vk error")
    b.stop()
    b.ensure("", {"n_ctx": 2048, "gpu_layers": -1, "device": "GPUOpenCL"})
    assert started[-2].cmd[0].endswith(aa.GPU_EXE) and started[-1].cmd[0].endswith("i8mm.so")
    assert profile["llm"]["gpu_layers"] == 0 and profile["llm"]["device"] == ""
    assert "GPUOpenCL" in json.loads((tmp_path / "nexus" / "android_accel.json").read_text())["gpu_broken"]


def _tuner(tmp_path, monkeypatch):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    (tmp_path / "nexus").mkdir(exist_ok=True)
    monkeypatch.setenv("NEXUS_DEVICE_STATE", str(tmp_path / "device_state.json"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_CPU", "/libs/libllama-server-i8mm.so")
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_GPU", "/libs/" + aa.GPU_EXE)
    mon = SimpleNamespace(llm_calls=[], settings={"n_threads": 4})
    s = SimpleNamespace(profile={"llm": {"n_threads": 4, "n_ctx": 4096, "gpu_layers": -1, "device": "Vulkan0"}, "ram_gb": 12},
                        monitor=mon, llm=SimpleNamespace(inflight=0), tuning_reasons=[])
    s.models = SimpleNamespace(active=lambda: {"llm": "qwen3-vl-4b-thinking"}, catalog=SimpleNamespace(get=lambda k: None),
                               recommended=lambda: [])
    return s, at.LiveTuner(s, layout={"fast": 4, "total": 8})


def _answer(s, tps, n=at.MIN_SAMPLES):
    for _ in range(n):
        s.monitor.llm_calls.append({"backend": "llamaserver", "tokens": 100, "seconds": 100 / tps + 1, "ttft": 1.0})


@pytest.mark.parametrize("gpu_tps,cpu_tps,winner", [(12.0, 7.0, "gpu"), (4.0, 7.0, "cpu")])
def test_autoajuste_compara_gpu_y_cpu(tmp_path, monkeypatch, gpu_tps, cpu_tps, winner):
    s, t = _tuner(tmp_path, monkeypatch)
    r = t.step()
    assert r["gpu"] == "Vulkan0" and r["accel"]["mode"] == "gpu" and s.profile["llm"]["gpu_layers"] == -1
    _answer(s, gpu_tps)
    t.step()
    assert s.profile["llm"]["gpu_layers"] == 0 and s.profile["llm"]["device"] == ""   # ahora mide la CPU
    _answer(s, cpu_tps)
    r = t.step()
    assert r["accel"]["choice"] == winner
    assert (s.profile["llm"]["device"] == "Vulkan0") == (winner == "gpu")
    assert r["llama_variant"] == ("gpu:Vulkan0" if winner == "gpu" else "libllama-server-i8mm.so")
    # despues mide los hilos con lo elegido, y lo recuerda en el siguiente arranque
    assert r["exploring"] and s.profile["llm"]["n_threads"] == 3
    s2, t2 = _tuner(tmp_path, monkeypatch)
    assert t2.step()["accel"]["choice"] == winner
    assert (s2.profile["llm"]["gpu_layers"] == -1) == (winner == "gpu")


def test_autoajuste_sin_gpu_no_compara(tmp_path, monkeypatch):
    s, t = _tuner(tmp_path, monkeypatch)
    s.profile["llm"].update(gpu_layers=0, device="")
    t2 = at.LiveTuner(s, layout={"fast": 4, "total": 8})
    r = t2.step()
    assert r["gpu"] == "" and not r["accel"] and s.profile["llm"]["n_threads"] == 3


def test_detect_en_android_rellena_gpu_y_npu(tmp_path, monkeypatch):
    import importlib
    d = importlib.import_module("nexus.hardware.detect")
    monkeypatch.setenv("NEXUS_ANDROID", "1")
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path))
    monkeypatch.setattr(aa, "detect", lambda: {
        "soc": {"name": "Qualcomm SM8650 (Snapdragon 8 Gen 3)", "family": "qualcomm"},
        "gpu": {"name": "QUALCOMM Adreno(TM) 750", "vendor": "qualcomm", "use": {"dev": "GPUOpenCL", "backend": "opencl",
                "mem_mb": 7623}, "devices": [], "broken": "", "binary": True},
        "npu": {"vendor": "qualcomm", "name": "Qualcomm Hexagon NPU", "runtime_hint": "QNN", "source": "soc"}})
    hw = d.detect(probe_runtimes=False)
    assert hw.cpu_name.startswith("Qualcomm SM8650")
    assert [(g.vendor, g.source) for g in hw.gpus] == [("qualcomm", "android-opencl")]
    assert hw.npus[0].name == "Qualcomm Hexagon NPU"
    assert hw.runtimes["android_accel"]["gpu_device"] == "GPUOpenCL"
    assert os.environ["NEXUS_ANDROID"] == "1"


# ------------------------------------------------------------------ NPU Hexagon (Snapdragon 8 Gen 2 o posterior)
LIST_SNAPDRAGON = LIST_ADRENO + "  HTP0: Hexagon (0 MiB, 0 MiB free)\n"


def test_prefiere_la_npu_hexagon_y_si_no_la_gpu():
    devs = aa.parse_devices(LIST_SNAPDRAGON)
    assert devs[-1]["dev"] == "HTP0" and devs[-1]["backend"] == "hexagon"
    assert aa.parse_devices("  HTP0:1: Hexagon (0 MiB, 0 MiB free)\n")[0]["dev"] == "HTP0:1"
    assert aa.pick(devs)["dev"] == "HTP0"
    assert aa.pick(devs, npu=False)["dev"] == "GPUOpenCL"
    assert aa.pick(devs, gpu=False)["dev"] == "HTP0" and aa.pick(devs, npu=False, gpu=False) is None
    assert aa.is_npu_dev("HTP0") and not aa.is_npu_dev("GPUOpenCL") and not aa.is_npu_dev("")
    env = aa.gpu_env("/data/app/lib/arm64")
    assert env["ADSP_LIBRARY_PATH"].split(";")[0] == "/data/app/lib/arm64"


@pytest.mark.skipif(sys.platform == "win32", reason="script de shell")
def test_sonda_con_npu_y_vuelta_a_la_gpu(tmp_path, monkeypatch):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    (tmp_path / "nexus").mkdir()
    monkeypatch.setenv("NEXUS_DEVICE_INFO", str(tmp_path / "device_info.json"))
    (tmp_path / "device_info.json").write_text(json.dumps({"soc_manufacturer": "QTI", "soc_model": "SM8650"}))
    monkeypatch.setattr(aa, "props", lambda: {"ro.build.fingerprint": "f1"})
    monkeypatch.setattr(aa, "vendor_lib", lambda names, dirs=aa.VENDOR_LIBS: "/vendor/lib64/libcdsprpc.so")
    libs = tmp_path / "lib"
    libs.mkdir()
    _fake_gpu_exe(libs, LIST_SNAPDRAGON)
    r = aa.detect(str(libs))
    assert r["gpu"]["use"]["dev"] == "HTP0" and r["gpu"]["name"] == "QUALCOMM Adreno(TM) 750"
    assert r["npu"]["usable"] and r["npu"]["device"] == "HTP0" and "Hexagon" in r["npu"]["runtime_hint"]
    aa.mark_gpu_broken("la NPU no arranca", "HTP0")
    r = aa.detect(str(libs))
    assert r["gpu"]["use"]["dev"] == "GPUOpenCL" and not r["npu"]["usable"] and r["gpu"]["broken"] == ""
    aa.mark_gpu_broken("ni la GPU", "GPUOpenCL")
    assert aa.detect(str(libs))["gpu"]["use"] is None


def test_llama_server_npu_cae_a_gpu_y_despues_a_cpu(tmp_path, monkeypatch):
    from nexus.llm import llamaserver as ls
    libs = tmp_path / "lib"
    libs.mkdir()
    for n in ("libllama-server-i8mm.so", aa.GPU_EXE):
        (libs / n).write_bytes(b"\x7fELF")
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA", str(libs / "libllama-server-i8mm.so"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_CPU", str(libs / "libllama-server-i8mm.so"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA_GPU", str(libs / aa.GPU_EXE))
    (tmp_path / "nexus" / "logs").mkdir(parents=True)
    (tmp_path / "nexus" / "logs" / "llama-server.log").write_bytes(b"htp error")
    works = {"HTP0": False, "GPUOpenCL": True}
    started = []

    def popen(cmd, **kw):
        dev = cmd[cmd.index("--device") + 1] if "--device" in cmd else ""
        p = _Proc(cmd, kw.get("env"), alive=works.get(dev, True))
        started.append(p)
        return p

    monkeypatch.setattr(ls, "popen", popen)
    monkeypatch.setattr(ls.httpx, "get", lambda *a, **k: SimpleNamespace(status_code=200))
    monkeypatch.setattr(aa, "detect", lambda *a, **k: {"gpu": {"use": {"dev": "GPUOpenCL", "backend": "opencl"}}})
    profile = {"llm": {"gpu_layers": -1, "device": "HTP0"}}
    rt = SimpleNamespace(exe=lambda *a: (libs / "libllama-server-i8mm.so", "embedded"))
    b = ls.LlamaServerBackend(rt, lambda m: (str(tmp_path / "m.gguf"), {"id": "m"}), lambda: profile)
    b.ensure("", {"n_ctx": 2048, "gpu_layers": -1, "device": "HTP0"})
    assert started[0].cmd[started[0].cmd.index("--device") + 1] == "HTP0"
    assert started[-1].cmd[started[-1].cmd.index("--device") + 1] == "GPUOpenCL"
    assert str(libs) in started[-1].env["ADSP_LIBRARY_PATH"]
    assert profile["llm"] == {"gpu_layers": -1, "device": "GPUOpenCL"}
    assert "HTP0" in json.loads((tmp_path / "nexus" / "android_accel.json").read_text())["npu_broken"]
    # y si la GPU tampoco arranca, a la CPU
    works["GPUOpenCL"] = False
    b.stop()
    b.ensure("", {"n_ctx": 2048, "gpu_layers": -1, "device": "HTP0"})
    assert started[-1].cmd[0].endswith("i8mm.so") and profile["llm"] == {"gpu_layers": 0, "device": ""}


def test_perfil_android_con_npu(monkeypatch):
    from nexus.hardware.profiles import build_profile
    p = build_profile(_hw(monkeypatch, backend="hexagon", device="HTP0"))
    assert p["accelerator"] == "hexagon" and p["llm"]["gpu_layers"] == -1 and p["llm"]["device"] == "HTP0"
    assert p["npu"]["ready"] and p["npu"]["offload"] == ["llm"] and "NPU" in p["npu"]["hint"]
    assert p["embeddings"]["device"] != "npu"


def test_autoajuste_mide_la_gpu_si_la_npu_fallo(tmp_path, monkeypatch):
    s, t = _tuner(tmp_path, monkeypatch)
    s.profile["llm"]["device"] = "HTP0"
    t = at.LiveTuner(s, layout={"fast": 4, "total": 8})
    assert t.step()["gpu"] == "HTP0"
    s.profile["llm"]["device"] = "GPUOpenCL"     # llamaserver paso a la GPU
    assert t.step()["gpu"] == "GPUOpenCL" and s.profile["llm"]["device"] == "GPUOpenCL"


@pytest.mark.skipif(sys.platform == "win32", reason="script de shell")
def test_sonda_sin_npu_si_el_backend_hexagon_se_cae(tmp_path, monkeypatch):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    (tmp_path / "nexus").mkdir()
    monkeypatch.setenv("NEXUS_DEVICE_INFO", str(tmp_path / "no.json"))
    monkeypatch.setattr(aa, "props", lambda: {"ro.build.fingerprint": "f1"})
    libs = tmp_path / "lib"
    libs.mkdir()
    (libs / "libggml-hexagon.so").write_bytes(b"\x7fELF")
    exe = libs / aa.GPU_EXE    # aborta salvo con GGML_HEXAGON_DEVICES=0
    exe.write_text("#!/bin/sh\n[ \"$GGML_HEXAGON_DEVICES\" = 0 ] || { echo 'ggml-hex: abort'; exit 134; }\n"
                   f"cat <<'EOF'\n{LIST_ADRENO}EOF\n")
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    r = aa.detect(str(libs))
    assert r["gpu"]["use"]["dev"] == "GPUOpenCL" and "abort" in r["gpu"]["npu_broken"]
    assert aa.gpu_env(str(libs), npu=False)["GGML_HEXAGON_DEVICES"] == "0"
    assert "GGML_HEXAGON_DEVICES" not in aa.gpu_env(str(libs))
