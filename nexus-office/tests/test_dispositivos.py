"""Dispositivos: luces NexusLED (contra el emulador del firmware real), modos, reloj y avatar del Pico W."""
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

FW = Path(__file__).resolve().parents[2] / "firmware"


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@pytest.fixture
def emulador(tmp_path):
    if os.name == "nt" or not shutil.which(os.environ.get("CXX", "g++")) or not (FW / "emulator").exists():
        pytest.skip("necesita g++ y la carpeta firmware/")
    exe = tmp_path / "nexusled-emu"
    subprocess.run([os.environ.get("CXX", "g++"), "-O1", "-std=c++17", "-o", str(exe),
                    str(FW / "emulator" / "nexusled_emu.cpp")], check=True, timeout=240)
    http, udp = free_port(), free_port(socket.SOCK_DGRAM)
    (tmp_path / "emu").mkdir()
    p = subprocess.Popen([str(exe), "--http", str(http), "--udp", str(udp), "--leds", "30", "--bind", "127.0.0.1",
                          "--data", str(tmp_path / "emu"), "--name", "Tira prueba"], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    import httpx
    for _ in range(50):
        try:
            httpx.get(f"http://127.0.0.1:{http}/api/info", timeout=0.5)
            break
        except httpx.HTTPError:
            time.sleep(0.1)
    yield {"http": http, "udp": udp}
    p.terminate()
    p.wait(5)


@pytest.fixture
def client(services):
    from nexus.devices import hub
    from nexus.server.app import create_app
    hub.reset()
    with TestClient(create_app(services)) as c:
        yield c
    hub.reset()


def test_leds_con_el_emulador_del_firmware(client, emulador):
    found = client.get(f"/api/dispositivos/leds/buscar?ip=127.0.0.1:{emulador['udp']}").json()["encontrados"]
    assert found and found[0]["name"] == "Tira prueba" and found[0]["leds"] == 30
    dev = client.post("/api/dispositivos/leds", json={"host": f"127.0.0.1:{emulador['http']}"}).json()
    assert dev["leds"] == 30 and dev["name"] == "Tira prueba"
    from nexus.devices import hub
    h = hub.get()
    h.leds.devices[0]["udp_port"] = emulador["udp"]
    # estado de Nexus por UDP (rapido) -> el firmware lo refleja en su estado
    client.post("/api/dispositivos/estado", json={"estado": "thinking", "nivel": 0.5})
    time.sleep(0.4)
    st = client.get(f"/api/dispositivos/leds/{dev['id']}/api/state").json()
    assert st["nexus"]["status"] == "thinking"
    # control del efecto a traves de la pasarela
    fx = client.get(f"/api/dispositivos/leds/{dev['id']}/api/effects").json()
    assert len(fx) >= 10 or len(fx.get("effects", [])) >= 10
    r = client.post(f"/api/dispositivos/leds/{dev['id']}/api/state", json={"on": True, "bri": 90}).json()
    assert r["bri"] == 90
    # un modo baja el brillo de las tiras
    client.post("/api/dispositivos/modo", json={"modo": "silencio"})
    assert client.get(f"/api/dispositivos/leds/{dev['id']}/api/state").json()["bri"] == 25
    assert client.get("/api/dispositivos").json()["modo"] == "silencio"
    client.delete(f"/api/dispositivos/leds/{dev['id']}")
    assert client.get("/api/dispositivos").json()["leds"] == []


def test_modos_y_reloj(client, services):
    assert services.config.get("user.voice_enabled") is True
    client.post("/api/dispositivos/modo", json={"modo": "silencio", "origen": "reloj"})
    assert services.config.get("user.voice_enabled") is False
    assert client.post("/api/dispositivos/modo", json={"modo": "nada"}).status_code == 400
    w = client.get("/api/dispositivos/reloj?cliente=reloj-1").json()
    assert w["modo"] == "silencio" and any(m["id"] == "normal" for m in w["modos"])
    assert any(a["id"] == "nexus" for a in w["agentes"])
    from nexus.agents.activity import ACTIVITY
    ACTIVITY.set("nexus", "tool", "Buscar precios", "web_search")
    w = client.get("/api/dispositivos/reloj").json()
    a = next(a for a in w["agentes"] if a["id"] == "nexus")
    assert a["estado"] == "tool" and a["escena"] == "browsing" and a["tarea"] == "Buscar precios"
    assert w["agentes"][0]["id"] == "nexus"      # los que trabajan van primero
    assert any(c["id"] == "reloj-1" for c in client.get("/api/dispositivos").json()["clientes"])
    ACTIVITY.set("nexus", "idle")


def test_pico_imagenes_por_estado(client):
    m = client.get("/api/dispositivos/pico/manifiesto").json()
    assert "thinking" in m["estados"] and m["version"]
    b = client.get("/api/dispositivos/pico/thinking.bin").content
    assert len(b) == 16 * 250                       # 122 px -> 16 bytes por fila, 250 filas
    assert 0 < sum(bin(x).count("1") for x in b) < len(b) * 8   # ni todo blanco ni todo negro
    assert b != client.get("/api/dispositivos/pico/speaking.bin").content
    png = client.get("/api/dispositivos/pico/idle.png")
    assert png.headers["content-type"] == "image/png" and png.content[:4] == b"\x89PNG"
    st = client.get("/api/dispositivos/estado?cliente=pico&tipo=pico").json()
    assert st["pico_version"] == m["version"]


def test_actividad_de_los_agentes_en_el_chat(services):
    from nexus.agents.activity import ACTIVITY
    events = []
    services.agents.run("nexus", "Hola", "s1", emit=events.append)
    st = ACTIVITY.get("nexus")
    assert st["estado"] == "idle" and st["tareas"] >= 1 and st["tarea"] == "Hola"


def test_firmware_del_pico_contra_el_servidor(client, tmp_path):
    """El codigo del Pico W (firmware/nexuspico/nexuspico.py) en el PC: descarga las imagenes, cambia con el estado
    y vuelve a descargarlas si cambia la version."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("nexuspico", FW / "nexuspico" / "nexuspico.py")
    np_ = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(np_)
    pintado = []

    def get(ruta):
        r = client.get(ruta)
        return r.status_code, r.content

    n = np_.Nucleo(get, lambda buf, motivo: pintado.append((motivo, buf)), "pico-test", str(tmp_path / "img"))
    assert n.paso() == "idle" and n.version and len(pintado[-1][1]) == 4000
    assert (tmp_path / "img" / "thinking.bin").stat().st_size == 4000
    assert client.get("/api/dispositivos/pico/idle.bin").content == pintado[-1][1]
    assert n.paso() == "idle" and len(pintado) == 1          # sin cambios no repinta (la tinta es lenta)
    client.post("/api/dispositivos/estado", json={"estado": "thinking"})
    assert n.paso() == "thinking" and pintado[-1][0] == "thinking"
    assert any(c["id"] == "pico-test" for c in client.get("/api/dispositivos").json()["clientes"])
    # sin conexion: al tercer fallo avisa en la pantalla, y despues se recupera
    caido = np_.Nucleo(lambda r: (_ for _ in ()).throw(OSError("caido")), lambda b, m: pintado.append((m, b)),
                       "pico-test", str(tmp_path / "img"))
    for _ in range(3):
        assert caido.paso() is None
    assert pintado[-1][1] is None and "sin conexion" in pintado[-1][0]
    # el giro del propio Pico coincide con el de Nexus
    from nexus.devices import hub
    img = hub.get().pico_imagen("speaking")
    w, h = img.size
    land = bytearray(((w + 7) // 8) * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                land[y * ((w + 7) // 8) + x // 8] |= 0x80 >> (x % 8)
    assert bytes(np_.girar(land, w, h)) == client.get("/api/dispositivos/pico/speaking.bin").content


def test_grabar_firmware_desde_nexus(client, monkeypatch):
    from nexus.devices import firmware as fw
    info = client.get("/api/firmware").json()
    assert info["carpeta"] and "esp32s3" in info["modelos_luces"] and info["nexus"]["key"]
    f = fw.script()
    hecho = {}
    monkeypatch.setattr(f, "flashear_esp32", lambda tipo, puerto, modelo, log: (log("grabando"), hecho.update(tipo=tipo))
                        and {"ok": True, "puerto": "/dev/ttyFAKE", "chip": "esp32c3"})
    monkeypatch.setattr(f, "configurar_esp32", lambda tipo, puerto, cfg, log: hecho.update(cfg=cfg, puerto=puerto) or {"ok": True})
    j = client.post("/api/firmware/grabar", json={"tipo": "reloj", "config": {"wifi": {"ssid": "casa", "pass": "x"}}}).json()
    for _ in range(50):
        r = client.get(f"/api/firmware/trabajo/{j['id']}").json()
        if r["estado"] != "en_curso":
            break
        time.sleep(0.05)
    assert r["estado"] == "hecho", r
    assert hecho["tipo"] == "reloj" and hecho["puerto"] == "/dev/ttyFAKE"
    assert hecho["cfg"]["wifi"]["ssid"] == "casa" and "key" in hecho["cfg"].get("nexus", {"key": ""})
    assert client.post("/api/firmware/grabar", json={"tipo": "tostadora"}).status_code == 400
