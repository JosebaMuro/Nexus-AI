"""Novedades 1.2: interfaz adaptativa (movil, tablet, app Android)."""
import re

import pytest

from nexus import paths

ROOT = paths.WEB.parent.parent


def test_device_detector_loads_before_paint_and_app():
    html = (paths.WEB / "index.html").read_text(encoding="utf-8")
    head = html.split("</head>")[0]
    assert '<script src="/static/js/device.js"></script>' in head        # sincrono, antes del primer pintado
    assert "interactive-widget=resizes-content" in head                  # el teclado no tapa el cuadro de escritura
    assert html.count("data-pri") == 4 and 'id="rail-more"' in html and 'id="more-sheet"' in html
    assert 'id="avatar-toggle"' in html
    sw = (paths.WEB / "sw.js").read_text(encoding="utf-8")
    assert "/static/js/device.js" in sw and 'e.request.mode === "navigate"' in sw   # la portada nunca sustituye a un script


def test_layouts_defined_for_every_device():
    css = (paths.WEB / "css" / "nexus.css").read_text(encoding="utf-8")
    for sel in ('[data-layout="phone"]', '[data-layout="tablet"]', '[data-orient="landscape"]', '[data-input="touch"]', "[data-lowpower]"):
        assert sel in css, sel
    assert "max-width: 700px" not in css and "max-width: 1100px" not in css   # sustituidos por el detector
    js = (paths.WEB / "js" / "device.js").read_text(encoding="utf-8")
    assert re.search(r"NexusAndroid\\/\[\\d\.\]\+ \\\(\(phone\|tablet\)", js)    # lee la forma que envia la APK


def test_android_app_reports_form_factor_and_handles_back():
    src = ROOT / "android" / "src" / "ai" / "nexus" / "office" / "client" / "MainActivity.java"
    if not src.exists():
        pytest.skip("el codigo de la app Android solo va en el proyecto completo y el paquete python")
    java = src.read_text(encoding="utf-8")
    assert '" NexusAndroid/" + version() + " (" + form() + "; sw"' in java
    assert "smallestScreenWidthDp >= 600" in java
    assert "window.__nexusBack" in java and "window.__nexusConfig" in java
    for bridge in ("public void haptic()", "public void setBars(", "public String device()"):
        assert bridge in java, bridge
    app = (paths.WEB / "js" / "app.js").read_text(encoding="utf-8")
    assert "window.__nexusBack = () =>" in app and "NexusApp.setBars" in (paths.WEB / "js" / "themes.js").read_text(encoding="utf-8")


def test_split_parts_roundtrip(tmp_path):
    import importlib.util
    if not (ROOT / "packaging" / "split_parts.py").exists():
        pytest.skip("las herramientas de empaquetado solo van en el proyecto completo y el paquete python")
    spec = importlib.util.spec_from_file_location("split_parts", ROOT / "packaging" / "split_parts.py")
    sp = importlib.util.module_from_spec(spec); spec.loader.exec_module(sp)
    src = tmp_path / "paquete.zip"; src.write_bytes(bytes(range(256)) * 41)   # 10.496 bytes
    out = tmp_path / "envio"; out.mkdir()
    digest = sp.sha256(src)
    parts = sp.split(src, out, 4096)
    assert [p.name for p in parts] == ["paquete.zip.001", "paquete.zip.002", "paquete.zip.003"]
    assert sp.verify_join(parts, digest) and not sp.verify_join(parts[:2], digest)
    bat, sh = sp.join_scripts(src, parts, digest, out)
    assert "copy /b paquete.zip.001+paquete.zip.002+paquete.zip.003" in bat.read_text(encoding="utf-8")
    assert digest in sh.read_text(encoding="utf-8") and b"\r\n" not in sh.read_bytes()


def test_requirements_reference_complete():
    """requirements.txt: pip lo lee entero y referencia runtimes, Python portable y los pesos de cada perfil."""
    from nexus.models.recommended import RECOMMENDED
    text = (ROOT / "requirements.txt").read_text(encoding="utf-8")
    pip_lines = [l.split("#")[0].strip() for l in text.splitlines() if l.strip() and not l.startswith("#")]
    spec = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*(\[[\w,]+\])?\s*(>=|==|~=|<=|<|>)\s*[\w.*]+$")   # formato PEP 508 simple
    assert all(spec.match(l) for l in pip_lines), [l for l in pip_lines if not spec.match(l)]
    assert {re.split(r"[<>=~]", l)[0] for l in pip_lines} >= {"fastapi", "uvicorn", "numpy", "cryptography", "pypdf"}
    for ref in ("python-build-standalone", "ggml-org/llama.cpp", "leejet/stable-diffusion.cpp", "faster-whisper", "piper-tts"):
        assert ref in text, ref
    for r in RECOMMENDED:
        assert r["file"] in text and r["repo"] in text, r["id"]
    assert text.count("# [") >= 10 and "NVIDIA" in text and "Apple Silicon" in text and "Termux" in text
