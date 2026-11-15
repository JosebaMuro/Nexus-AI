"""4.2: arranque con el hardware guardado, nucleos de rendimiento, estaticos comprimidos."""
import gzip
import importlib
import json
import threading
import time

from fastapi.testclient import TestClient

from nexus import hardware

det = importlib.import_module("nexus.hardware.detect")


def _esperar_revision():
    for t in threading.enumerate():
        if t.name == "nexus-hw-detect":
            t.join(10)


def test_deteccion_con_cache_y_revision_en_segundo_plano(monkeypatch):
    llamadas = []
    base = det.detect(probe_runtimes=False)

    def falso(ollama_url="", probe_runtimes=True):
        llamadas.append(1)
        return base
    monkeypatch.setattr(hardware, "detect", falso)
    assert hardware.detectar(rapida=True) is base and hardware.ESTADO["origen"] == "deteccion" and len(llamadas) == 1
    hw2 = hardware.detectar(rapida=True)                 # misma huella: arranca con lo guardado
    assert hardware.ESTADO["origen"] == "cache" and hw2.cpu_name == base.cpu_name
    _esperar_revision()                                  # y revisa en segundo plano
    assert len(llamadas) == 2 and hardware.ESTADO["cambio"] is False
    f = hardware._archivo_huella()
    f.write_text(json.dumps({"huella": "otra", "ts": time.time()}))
    hardware.detectar(rapida=True)                       # huella distinta: detecta antes de arrancar
    assert hardware.ESTADO["origen"] == "deteccion"
    f.write_text(json.dumps({"huella": hardware.huella(), "ts": time.time() - hardware.CADUCA_S - 5}))
    hardware.detectar(rapida=True)                       # caducada: tambien
    assert hardware.ESTADO["origen"] == "deteccion"
    hardware.detectar(rapida=False)
    assert hardware.ESTADO["origen"] == "deteccion"


def test_cambio_de_hardware_se_avisa(monkeypatch):
    base = det.detect(probe_runtimes=False)
    hardware.detectar(rapida=False)
    otro = det.HardwareInfo.from_dict({**base.to_dict(), "gpus": [{"vendor": "nvidia", "name": "RTX nueva", "vram_gb": 12}]})
    monkeypatch.setattr(hardware, "detect", lambda ollama_url="", probe_runtimes=True: otro)
    hardware.detectar(rapida=True)
    _esperar_revision()
    assert hardware.ESTADO["cambio"] and "reiniciar" in hardware.ESTADO["detalle"]
    assert hardware.load_cached().gpus[0].name == "RTX nueva"


def test_nucleos_de_rendimiento(tmp_path):
    (tmp_path / "cpu_core").mkdir()
    (tmp_path / "cpu_atom").mkdir()
    (tmp_path / "cpu_core" / "cpus").write_text("0-11\n")
    for c in range(12):
        t = tmp_path / "system" / "cpu" / f"cpu{c}" / "topology"
        t.mkdir(parents=True)
        (t / "core_id").write_text(str(c // 2))
        (t / "physical_package_id").write_text("0")
    assert det._rapidos_linux(tmp_path) == 6
    (tmp_path / "cpu_atom").rmdir()
    assert det._rapidos_linux(tmp_path) == 0             # sin nucleos de eficiencia no es hibrida
    hw = det.detect(probe_runtimes=False)
    d = hw.to_dict()
    d.update(cores_physical=14, cores_logical=20, cores_rapidos=6)
    assert hardware.build_profile(det.HardwareInfo.from_dict(d))["llm"]["n_threads"] == 6
    d.update(cores_rapidos=0)
    assert hardware.build_profile(det.HardwareInfo.from_dict(d))["llm"]["n_threads"] == 14
    viejo = {k: v for k, v in d.items() if k != "cores_rapidos"}   # hardware.json de una version anterior
    assert det.HardwareInfo.from_dict(viejo).cores_rapidos == 0


def test_alto_rendimiento_no_hace_nada_fuera_de_windows():
    from nexus import proc
    import os
    assert proc.alto_rendimiento() is (False if os.name != "nt" else proc.alto_rendimiento())


def test_estaticos_comprimidos(services):
    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        r = c.get("/static/js/app.js", headers={"Accept-Encoding": "gzip"})
        assert r.status_code == 200 and r.headers.get("content-encoding") == "gzip" and "Agente" in r.text
        etag = r.headers.get("etag")
        assert etag and int(r.headers["content-length"]) < len(r.content)   # httpx ya lo descomprime
        assert c.get("/static/js/app.js", headers={"Accept-Encoding": "gzip", "If-None-Match": etag}).status_code == 304
        sin = c.get("/static/js/app.js", headers={"Accept-Encoding": "identity"})
        assert sin.headers.get("content-encoding") is None and sin.text == r.text
        crudo = c.get("/static/css/" + "app.css", headers={"Accept-Encoding": "gzip"})
        assert crudo.status_code in (200, 404)
        assert gzip.decompress(gzip.compress(b"x")) == b"x"
