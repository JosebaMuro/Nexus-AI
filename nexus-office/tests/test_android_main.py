"""Arranque de la app autonoma (android-autonomo) simulado en el PC: mismo codigo que ejecuta el Python de la APK."""
import os
import socket
import time

import httpx


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_android_main_serves_and_uses_embedded_llama(tmp_path, monkeypatch):
    from nexus import android_main, parallel
    from nexus.config import reset_config
    libs = tmp_path / "lib"
    libs.mkdir()
    (libs / "libllama-server.so").write_bytes(b"\x7fELF")
    for k in ("NEXUS_ANDROID", "NEXUS_RUNTIME_LLAMA", "NEXUS_RUNTIME_LLAMA_CPU", "NEXUS_RUNTIME_LLAMA_GPU",
              "NEXUS_ANDROID_LIBS", "TMPDIR", "HOME"):   # se restauran al acabar el test
        monkeypatch.setenv(k, os.environ.get(k, "x"))
        if k != "HOME":
            monkeypatch.delenv(k)
    reset_config()
    port = free_port()
    t = android_main.start_in_thread(str(tmp_path / "files"), str(libs), port)
    try:
        for _ in range(120):
            try:
                r = httpx.get(f"http://127.0.0.1:{port}/api/ping", timeout=1)
                if r.status_code == 200:
                    break
            except httpx.HTTPError:
                time.sleep(0.25)
        assert r.json()["nexus"] is True
        assert os.environ["NEXUS_DATA_DIR"] == str(tmp_path / "files" / "nexus")
        assert os.environ["NEXUS_RUNTIME_LLAMA"].endswith("libllama-server.so")
        assert not parallel.processes_available()                      # Android: sin procesos hijo
        from nexus.runtimes import RuntimeManager
        assert RuntimeManager().exe("llama", {"accelerator": "cpu"}) == (libs / "libllama-server.so", "embedded")
        st = httpx.get(f"http://127.0.0.1:{port}/api/status", timeout=10).json()
        assert st["version"]
    finally:
        android_main.stop()
        t.join(10)
    assert not t.is_alive()


def test_android_profile_recommends_only_mobile_llms(monkeypatch):
    from nexus.hardware.detect import HardwareInfo
    from nexus.hardware.profiles import build_profile
    from nexus.models.recommended import recommend
    monkeypatch.setenv("NEXUS_ANDROID", "1")

    def best(ram):
        hw = HardwareInfo(os="linux", os_version="", arch="aarch64", cpu_name="ARM", cores_physical=8, cores_logical=8,
                          cpu_flags=[], ram_gb=ram, disk_free_gb=50, gpus=[], npus=[], runtimes={}, python="3.12")
        recs = recommend(build_profile(hw))
        assert {r["kind"] for r in recs} == {"llm", "embed"}   # sin imagen ni Piper: no van en la APK
        return [r["id"] for r in recs if r.get("best") and r["kind"] == "llm"]

    assert best(3.6) == ["qwen3-vl-2b-thinking"]
    assert best(7.4) == ["qwen3-vl-2b-thinking"]
    assert best(11.3) == ["qwen3-vl-4b-thinking"]


def test_python_cmd_con_manejador_de_sigsys(monkeypatch):
    import sys
    from nexus.proc import python_cmd
    monkeypatch.setenv("NEXUS_PY_LOADER", "/libs/libnexusld.so")
    monkeypatch.setenv("NEXUS_PY_LIBRARY_PATH", "/rt/py/lib:/libs")
    monkeypatch.setenv("NEXUS_PY_PRELOAD", "/libs/libnexussys.so")
    assert python_cmd() == ["/libs/libnexusld.so", "--library-path", "/rt/py/lib:/libs",
                            "--preload", "/libs/libnexussys.so", sys.executable]
    monkeypatch.delenv("NEXUS_PY_PRELOAD")
    assert "--preload" not in python_cmd()
