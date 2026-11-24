"""Instaladores sin conexion: lo que viene en la app, descargar una vez, importar, comprobar y repartir por la red."""
import hashlib
import json
import time
import urllib.error
import urllib.request

import pytest
from fastapi.testclient import TestClient

from nexus import instaladores as I

V = "9.9.9"


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


@pytest.fixture
def m(tmp_path, monkeypatch):
    app = tmp_path / "maletin"
    app.mkdir()
    (app / f"Nexus-Office-{V}-android-vpn.apk").write_bytes(b"apk cliente")
    (app / "rehacer-ubuntu.sh").write_text("#!/bin/sh\n")
    propia = tmp_path / "base.apk"
    propia.write_bytes(b"apk autonoma" * 100)
    monkeypatch.setenv("NEXUS_MALETIN_APP", str(app))
    monkeypatch.setenv("NEXUS_APK_PROPIA", str(propia))
    mal = I.Maletin(V)
    yield mal
    mal.parar()


def _release(mal, ficheros: dict[str, bytes]) -> None:
    info = {"tag": f"v{V}", "version": V, "url": "", "consultado": "", "aviso": "",
            "assets": {n: {"tam": len(b), "url": f"https://ejemplo/{n}"} for n, b in ficheros.items()},
            "sha256": {n: _sha(b) for n, b in ficheros.items()}}
    (I.carpeta() / "release.json").write_text(json.dumps(info))


def test_lo_que_viene_dentro_de_la_app(m):
    e = m.estado()
    por = {i["id"]: i for i in e["items"]}
    assert por["android-autonomo"]["estado"] == "listo" and por["android-autonomo"]["origen"] == "app"
    assert por["android-autonomo"]["nombre"] == f"Nexus-Office-{V}-android-autonomo.apk"
    assert por["android-cliente"]["estado"] == "listo" and por["ubuntu-minima"]["estado"] == "listo"
    assert por["windows-x64"]["estado"] == "falta"
    assert {i["plataforma"] for i in e["items"]} >= {"Windows", "macOS", "Linux x86_64", "Linux ARM64 / Raspberry Pi"}
    assert e["listos"] == 3 and e["compartir"]["activo"] is False


def test_descarga_una_vez_con_sha256(m, monkeypatch):
    datos = {f"Nexus-Office-Setup-{V}-windows-x64.exe": b"MZ" * 1000, f"Nexus-Office-{V}-macos-arm64.tar.gz": b"tgz"}
    _release(m, datos)
    from nexus import net

    def falsa(url, dest, progress=None, cancel=None, retries=3):
        nombre = url.rsplit("/", 1)[1]
        cuerpo = datos[nombre] if "macos" not in nombre else b"corrupto"
        dest.write_bytes(cuerpo)
        progress and progress(len(cuerpo), len(cuerpo))
        return dest
    monkeypatch.setattr(net, "download", falsa)
    m.descargar(["windows-x64", "macos-arm64"])
    for _ in range(100):
        if not m.descarga and not m._cola:
            break
        time.sleep(0.05)
    por = {i["id"]: i for i in m.estado()["items"]}
    assert por["windows-x64"]["estado"] == "listo" and por["windows-x64"]["verificado"] == "sha256"
    assert por["macos-arm64"]["estado"] == "falta" and "SHA256" in por["macos-arm64"]["error"]
    assert not (I.carpeta() / f"Nexus-Office-{V}-macos-arm64.tar.gz").exists()
    m.borrar("windows-x64")
    assert {i["id"]: i for i in m.estado()["items"]}["windows-x64"]["estado"] == "falta"
    with pytest.raises(ValueError):
        m.borrar("android-autonomo")   # lo de la app no se borra


def test_sin_espacio_no_empieza(m, monkeypatch):
    _release(m, {f"Nexus-Office-Setup-{V}-windows-x64.exe": b"x"})
    import shutil
    monkeypatch.setattr(shutil, "disk_usage", lambda p: shutil._ntuple_diskusage(10, 10, 1000))
    with pytest.raises(ValueError, match="espacio"):
        m.descargar(["windows-x64"])
    with pytest.raises(ValueError, match="desconocido"):
        m.descargar(["nada"])


def test_importar_de_usb_sin_conexion(m, tmp_path):
    usb = tmp_path / "usb"
    usb.mkdir()
    bueno, malo = f"nexus-office_{V}_arm64.deb", f"Nexus-Office-{V}-linux-aarch64.run"
    (usb / bueno).write_bytes(b"deb")
    (usb / malo).write_bytes(b"run roto")
    (usb / "otro.txt").write_text("no")
    (usb / "SHA256SUMS.txt").write_text(f"{_sha(b'deb')}  {bueno}\n{_sha(b'run')}  {malo}\n")
    r = m.importar(str(usb))
    assert bueno in r["copiados"] and "otro.txt" not in r["copiados"]
    assert any("no coincide" in e for e in r["errores"])
    por = {i["id"]: i for i in r["items"]}
    assert por["linux-aarch64-deb"]["estado"] == "listo" and por["linux-aarch64-deb"]["verificado"] == "sha256"
    assert por["linux-aarch64-run"]["estado"] == "falta"
    with pytest.raises(ValueError):
        m.importar(str(tmp_path / "no-existe"))


def test_repartir_por_la_red_local(m):
    est = m.compartir(True, 0)
    assert est["activo"]
    p = m._srv.server_address[1]
    base = f"http://127.0.0.1:{p}"
    pagina = urllib.request.urlopen(base + "/").read().decode()
    assert "android-autonomo.apk" in pagina and "Windows" not in pagina   # solo lo que hay
    nombre = f"Nexus-Office-{V}-android-autonomo.apk"
    todo = urllib.request.urlopen(f"{base}/f/{nombre}").read()
    assert todo == b"apk autonoma" * 100
    req = urllib.request.Request(f"{base}/f/{nombre}", headers={"Range": "bytes=12-23"})
    with urllib.request.urlopen(req) as r:
        assert r.status == 206 and r.read() == b"apk autonoma"
    for malo in ("/f/../../etc/passwd", "/f/release.json", "/api/status", "/f/verificados.json"):
        with pytest.raises(urllib.error.HTTPError) as e:
            urllib.request.urlopen(base + malo)
        assert e.value.code == 404
    assert m.compartir(False)["activo"] is False


def test_api(services, m, monkeypatch):
    monkeypatch.setattr(I, "_MALETIN", m)
    from nexus.server.app import create_app
    c = TestClient(create_app(services))
    d = c.get("/api/instaladores").json()
    assert d["listos"] == 3
    r = c.get("/api/instaladores/fichero/android-cliente")
    assert r.status_code == 200 and r.content == b"apk cliente"
    assert c.get("/api/instaladores/fichero/windows-x64").status_code == 404
    assert c.get("/api/instaladores/fichero/..%2Fx").status_code == 404
    assert c.post("/api/instaladores/compartir", json={"activo": True}).status_code == 403   # solo administradores
