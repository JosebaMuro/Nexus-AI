"""Paquetes offline con GPU incluida: eleccion del runtime, extras locales y limpieza al final de la instalacion."""
import os
import sys
from pathlib import Path

import pytest

from nexus import cleanup, runtimes, wheelhouse

pytestmark = pytest.mark.skipif(os.name == "nt", reason="ejecutables de prueba en sh")

PROFILE_NVIDIA = {"accelerator": "cuda", "id": "cuda-nvidia-dgpu"}
PROFILE_CPU = {"accelerator": "cpu", "id": "cpu-cpu-only"}


def fake_exe(path: Path, ok: bool) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!/bin/sh\nexit {0 if ok else 1}\n")
    path.chmod(0o755)
    return path


@pytest.fixture
def bundled(tmp_path, monkeypatch):
    b = tmp_path / "bundled" / "runtimes"
    monkeypatch.setattr(runtimes, "BUNDLED", b)
    monkeypatch.setattr(runtimes, "_USABLE", {})
    monkeypatch.setattr(runtimes, "_has_avx2", lambda: True)
    monkeypatch.setattr(runtimes, "_os", lambda: "linux")
    monkeypatch.setattr(runtimes.RuntimeManager, "installed", lambda self: {"llama": [], "sd": []})
    monkeypatch.setattr(runtimes.RuntimeManager, "find_exe", lambda self, rt, v: None)
    monkeypatch.setattr(runtimes.shutil, "which", lambda name: None)
    return b


def test_bundled_gpu_runtime_is_preferred_and_falls_back_without_driver(bundled):
    fake_exe(bundled / "llama" / "cpu" / "llama-server", True)
    cuda = fake_exe(bundled / "llama" / "cuda" / "bin" / "llama-server", False)    # sin libcuda.so.1: no arranca
    vulkan = fake_exe(bundled / "llama" / "vulkan" / "llama-server", True)
    rm = runtimes.RuntimeManager()
    assert rm.exe("llama", PROFILE_NVIDIA) == (vulkan, "vulkan")                  # cuda falla -> vulkan
    runtimes._USABLE.clear()
    fake_exe(cuda, True)
    assert rm.exe("llama", PROFILE_NVIDIA) == (cuda, "cuda")
    assert rm.exe("llama", PROFILE_CPU) == (bundled / "llama" / "cpu" / "llama-server", "cpu")


def test_optional_extras_install_when_their_wheels_are_bundled(tmp_path):
    for name in ("torch_directml-0.2.5-cp311-cp311-win_amd64.whl", "transformers-4.0-py3-none-any.whl",
                 "scipy-1.17-cp311-cp311-win_amd64.whl"):
        (tmp_path / name).write_bytes(b"")
    assert wheelhouse.has_local(["torch-directml", "transformers", "scipy"], tmp_path)
    assert not wheelhouse.has_local(["torch"], tmp_path)
    assert "onnxruntime-qnn" in wheelhouse.extras_for("windows-x64")
    assert "torch" in wheelhouse.extras_for("macos-arm64") and wheelhouse.extras_for("linux-x86_64") == wheelhouse.EXTRAS


def test_cleanup_removes_unused_and_keeps_what_was_used(tmp_path, monkeypatch, bundled):
    wh, rt = tmp_path / "wheelhouse", tmp_path / "runtime"
    wh.mkdir()
    rt.mkdir()
    (rt / "cpython-3.11.17-x86_64-unknown-linux-gnu-install_only.tar.gz").write_bytes(b"x" * 10)
    used = wh / "pytest-{}-py3-none-any.whl".format(pytest.__version__)    # instalado en este entorno
    unused = wh / "paquete_que_no_existe-9.9-py3-none-any.whl"
    used.write_bytes(b"u" * 100)
    unused.write_bytes(b"n" * 1000)
    cpu = fake_exe(bundled / "llama" / "cpu" / "llama-server", True)
    fake_exe(bundled / "llama" / "cuda" / "bin" / "llama-server", False)          # este equipo no tiene NVIDIA
    fake_exe(bundled / "llama" / "vulkan" / "llama-server", True)
    for name, val in (("ROOT", tmp_path), ("WHEELHOUSE", wh), ("RUNTIME", rt), ("BUNDLED", bundled)):
        monkeypatch.setattr(cleanup, name, val)
    from nexus import hardware
    monkeypatch.setattr(hardware, "load_cached", lambda: None)
    monkeypatch.setattr(hardware, "detect", lambda **kw: None)
    monkeypatch.setattr(hardware, "build_profile", lambda hw: PROFILE_NVIDIA)
    monkeypatch.setattr(wheelhouse, "write_manifest", lambda *a, **k: None)
    pl = cleanup.plan()
    assert unused in pl["unused"] and used in pl["used"]
    assert bundled / "llama" / "cuda" in pl["unused"]
    assert {bundled / "llama" / "vulkan", bundled / "llama" / "cpu"} == set(pl["runtimes_kept"])
    monkeypatch.setattr(sys, "stdin", None)                 # sin terminal y sin --yes: no se borra nada
    cleanup.run()
    assert unused.exists()
    report = cleanup.run(yes=True)                          # --yes: borra lo no usado y guarda lo usado
    assert not unused.exists() and not (bundled / "llama" / "cuda").exists()
    assert used.exists() and cpu.exists() and (bundled / "llama" / "vulkan").exists()
    assert "wheelhouse/" + used.name in report["kept_for_reinstall"]
    fake_exe(bundled / "llama" / "cuda" / "bin" / "llama-server", False)
    cleanup.run(yes=True, keep=False)                       # --no-keep: tambien wheelhouse/ y runtime/
    assert not wh.exists() and not rt.exists() and cpu.exists()


def test_cleanup_conserva_lo_que_puede_pedir_el_selector(tmp_path, monkeypatch, bundled):
    """La NPU Intel usa llama-server con OpenVINO: la limpieza no lo borra aunque el perfil elija Vulkan."""
    from nexus.hardware.detect import GPU, NPU
    from test_hardware import hw
    fake_exe(bundled / "llama" / "cpu" / "llama-server", True)
    fake_exe(bundled / "llama" / "vulkan" / "llama-server", True)
    fake_exe(bundled / "llama" / "openvino" / "llama-server", True)
    fake_exe(bundled / "llama" / "cuda" / "llama-server", False)
    for name, val in (("ROOT", tmp_path), ("WHEELHOUSE", tmp_path / "wh"), ("RUNTIME", tmp_path / "rt"), ("BUNDLED", bundled)):
        monkeypatch.setattr(cleanup, name, val)
    from nexus import hardware
    monkeypatch.setattr(sys, "platform", "linux")
    h = hw(gpus=[GPU("intel", "Arc", 0, True)], npus=[NPU("intel", "Intel AI Boost")])
    monkeypatch.setattr(hardware, "load_cached", lambda: h)
    pl = cleanup.plan()
    assert bundled / "llama" / "openvino" in pl["runtimes_kept"] and bundled / "llama" / "vulkan" in pl["runtimes_kept"]
    assert bundled / "llama" / "cuda" in pl["unused"]
