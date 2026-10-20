"""wheelhouse/: instalar con wheels locales primero, nunca compilar y respetar el modo offline (sin red ni pip real)."""
import subprocess

import pytest

from nexus import wheelhouse as wh


class Run:
    def __init__(self, codes):
        self.codes, self.calls = list(codes), []

    def __call__(self, cmd, check=False, **kw):
        self.calls.append(cmd)
        code = self.codes.pop(0) if self.codes else 0
        if check and code:
            raise subprocess.CalledProcessError(code, cmd)
        return subprocess.CompletedProcess(cmd, code)


@pytest.fixture
def house(tmp_path, monkeypatch):
    for k in ("NEXUS_OFFLINE", "NEXUS_ALLOW_BUILD", "TERMUX_VERSION"):
        monkeypatch.delenv(k, raising=False)
    (tmp_path / "piper_tts-1.8.0-cp39-abi3-manylinux_2_28_x86_64.whl").write_bytes(b"x")
    return tmp_path


def test_usa_wheelhouse_sin_red_y_sin_compilar(house, monkeypatch):
    run = Run([0])
    monkeypatch.setattr(wh.subprocess, "run", run)
    wh.pip_install(["piper-tts"], dest=house)
    assert len(run.calls) == 1
    cmd = run.calls[0]
    assert "--no-index" in cmd and str(house) in cmd and "--only-binary=:all:" in cmd


def test_si_falta_algo_completa_desde_internet(house, monkeypatch):
    run = Run([1, 0])
    monkeypatch.setattr(wh.subprocess, "run", run)
    wh.pip_install(["faster-whisper"], dest=house)
    second = run.calls[1]
    assert "--no-index" not in second and "--find-links" in second and "--only-binary=:all:" in second


def test_modo_offline_no_sale_a_internet(house, monkeypatch):
    run = Run([1])
    monkeypatch.setattr(wh.subprocess, "run", run)
    monkeypatch.setenv("NEXUS_OFFLINE", "1")
    with pytest.raises(RuntimeError, match="offline"):
        wh.pip_install(["faster-whisper"], dest=house)
    assert len(run.calls) == 1
    with pytest.raises(RuntimeError, match="sin wheelhouse"):
        wh.pip_install(["x"], dest=house / "vacio")


def test_compilar_solo_si_se_permite(monkeypatch):
    monkeypatch.delenv("TERMUX_VERSION", raising=False)
    monkeypatch.delenv("NEXUS_ALLOW_BUILD", raising=False)
    if not wh.is_android():
        assert wh.binary_args() == ["--only-binary=:all:"]
    monkeypatch.setenv("NEXUS_ALLOW_BUILD", "1")
    assert wh.binary_args() == ["--prefer-binary"]


def test_requirements_y_etiquetas_linux():
    reqs = wh.requirements()
    assert any(r.startswith("fastapi") for r in reqs) and not any("#" in r for r in reqs)
    variants = wh._variants("linux-x86_64")
    assert [v[0] for v in variants] == ["glibc 2.39", "glibc 2.28"]
    new, old = variants[0][1], variants[1][1]
    assert "manylinux_2_34_x86_64" in new and "manylinux_2_34_x86_64" not in old
    assert "manylinux2014_x86_64" in old and all("aarch64" not in p for p in new)


def test_manifiesto_detecta_wheels_danadas(house):
    wh.write_manifest(house, ["linux-x86_64"])
    assert wh.verify(house) == []
    (house / "piper_tts-1.8.0-cp39-abi3-manylinux_2_28_x86_64.whl").write_bytes(b"roto")
    assert wh.verify(house) == ["piper_tts-1.8.0-cp39-abi3-manylinux_2_28_x86_64.whl"]


def test_cli_wheels_no_importa_el_servidor():
    from nexus.__main__ import main
    with pytest.raises(SystemExit) as e:
        main(["wheels", "--help"])
    assert e.value.code == 0


def test_vendor_pyyaml_puro_para_windows_arm64():
    import zipfile
    whl = wh.VENDOR / "pyyaml-6.0.3-py3-none-any.whl"
    names = zipfile.ZipFile(whl).namelist()
    assert "yaml/__init__.py" in names
    assert not any(n.endswith((".so", ".pyd")) for n in names)


def test_vendor_va_en_todas_las_instalaciones(house, monkeypatch):
    run = Run([0])
    monkeypatch.setattr(wh.subprocess, "run", run)
    wh.pip_install(["pyyaml"], dest=house)
    assert str(wh.VENDOR) in run.calls[0]


def test_sin_wheel_explica_que_no_compila(house, monkeypatch):
    monkeypatch.setattr(wh.subprocess, "run", Run([1, 1]))
    with pytest.raises(RuntimeError, match="no se compila"):
        wh.pip_install(["piper-tts"], dest=house)


def _fake_wheel(dest, name, ver, tag, requires=()):
    import zipfile
    f = dest / f"{name}-{ver}-{tag}.whl"
    meta = "Metadata-Version: 2.1\nName: {}\nVersion: {}\n{}\n\nDescripcion\nRequires-Dist: falso\n".format(
        name, ver, "".join(f"Requires-Dist: {r}\n" for r in requires))
    with zipfile.ZipFile(f, "w") as z:
        z.writestr(f"{name}-{ver}.dist-info/METADATA", meta)
    return f


def test_closure_uses_target_platform_markers(tmp_path):
    """pip download evalua los marcadores con este equipo: en Windows faltaria colorama (click) sin esta comprobacion."""
    from nexus import wheelhouse as w
    _fake_wheel(tmp_path, "click", "8.5.0", "py3-none-any", ['colorama; platform_system == "Windows"'])
    _fake_wheel(tmp_path, "pyttsx3", "2.99", "py3-none-any", ['pyobjc>=2.4; platform_system == "Darwin"'])
    _fake_wheel(tmp_path, "pillow", "12.3.0", "cp311-cp311-macosx_10_10_x86_64")
    assert w.closure_missing("linux-x86_64", ["click", "pyttsx3"], tmp_path, w._manylinux("x86_64")) == []
    assert w.closure_missing("windows-x64", ["click", "pyttsx3"], tmp_path, ["win_amd64"]) == ["colorama"]
    assert w.closure_missing("macos-x86_64", ["pyttsx3"], tmp_path, w.TARGETS["macos-x86_64"]["platforms"]) == ["pyobjc>=2.4"]
    # macOS: una wheel para una version anterior del sistema tambien vale
    assert w.closure_missing("macos-x86_64", ["pillow"], tmp_path, w.TARGETS["macos-x86_64"]["platforms"]) == []
    got = []

    def fetch(spec):
        got.append(spec)
        _fake_wheel(tmp_path, "colorama", "0.4.6", "py2.py3-none-any")
        return True
    assert w.closure_missing("windows-x64", ["click"], tmp_path, ["win_amd64"], fetch) == [] and got == ["colorama"]
