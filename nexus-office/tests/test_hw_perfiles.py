"""Perfiles de hardware y eleccion de GPU cuando hay varias (llama-server --list-devices, llvmpipe, dos tarjetas)."""
import sys

from nexus.hardware import gpu_devices, hw_profiles, profiles
from nexus.hardware.detect import GPU
from test_hardware import hw

SALIDA = """ggml_vulkan: Found 3 Vulkan devices:
Available devices:
  Vulkan0: Intel(R) UHD Graphics 770 (ADL-S GT1) (15872 MiB, 15000 MiB free)
  Vulkan1: NVIDIA GeForce RTX 3060 (12288 MiB, 11520 MiB free)
  Vulkan2: llvmpipe (LLVM 15.0.7, 256 bits) (32000 MiB, 32000 MiB free)
"""


def test_parse_y_pick():
    devs = gpu_devices.parse(SALIDA)
    assert [d["id"] for d in devs] == ["Vulkan0", "Vulkan1", "Vulkan2"]
    assert devs[2]["software"] and devs[0]["integrated"] and not devs[1]["integrated"]
    assert gpu_devices.pick(devs)["id"] == "Vulkan1"            # la dedicada, aunque la integrada tenga mas memoria
    assert gpu_devices.pick(devs, "Intel UHD 770")["id"] == "Vulkan0"
    assert gpu_devices.pick(devs, "vulkan0")["id"] == "Vulkan0"
    assert gpu_devices.pick(devs, "llvmpipe")["id"] == "Vulkan1"  # nunca la de software
    assert gpu_devices.pick([devs[2]]) is None
    assert gpu_devices.needs_device_flag(devs) and not gpu_devices.needs_device_flag(devs[1:2])


def test_elegir_gpu_cambia_el_perfil(monkeypatch):
    monkeypatch.setattr(sys, "platform", "linux")
    h = hw(gpus=[GPU("intel", "Intel UHD Graphics 770", 0, integrated=True), GPU("nvidia", "RTX 3060", 12),
                 GPU("amd", "Radeon RX 6600", 8)])
    try:
        profiles.select_gpu("")
        assert profiles._best_gpu(h).name == "RTX 3060"
        profiles.select_gpu("RX 6600")
        assert profiles._best_gpu(h).name == "Radeon RX 6600"
        h.runtimes = {"llama_devices": gpu_devices.parse(SALIDA)}
        profiles.select_gpu("Vulkan0")
        assert profiles._best_gpu(h).name == "Intel UHD Graphics 770"
    finally:
        profiles.select_gpu("")


class Cfg(dict):
    def get(self, k, d=None):
        return super().get(k, d)

    def set(self, k, v, save=True):
        self[k] = v


def test_perfiles_crud_y_superponer():
    cfg = Cfg()
    ids = [p["id"] for p in hw_profiles.lista(cfg)]
    assert {"auto", "rendimiento", "ahorro", "cpu", "grande"} <= set(ids)
    p = hw_profiles.guardar(cfg, {"nombre": "Mi Portátil", "acelerador": "gpu", "gpu": "RTX 3060", "capas_gpu": "20",
                                  "kv": "ram", "contexto": 4096, "motor": "raro"})
    assert p["id"] == "mi-portátil" or p["id"].startswith("mi-port")
    assert p["capas_gpu"] == 20 and p["motor"] == "auto" and hw_profiles.get(cfg, p["id"])
    prof = {"llm": {"n_ctx": 8192, "max_ctx": 16384, "gpu_layers": -1, "n_threads": 8, "n_batch": 512}}
    hw_profiles.superponer(prof, p)
    assert prof["llm"]["gpu_layers"] == 20 and prof["llm"]["kv_ram"] and prof["llm"]["n_ctx"] == 4096
    assert prof["llm"]["gpu_select"] == "RTX 3060" and prof["perfil_hw"]["id"] == p["id"]
    ahorro = hw_profiles.get(cfg, "ahorro")
    prof2 = {"llm": {"n_ctx": 8192, "gpu_layers": 0, "n_threads": 8, "n_batch": 512}}
    hw_profiles.superponer(prof2, ahorro)
    assert prof2["llm"]["n_threads"] == 4 and prof2["llm"]["n_ctx"] == 2048
    res = hw_profiles.activar(p["id"], cfg)       # sin servicios: queda para el arranque
    assert cfg["hardware.perfil"] == p["id"] and cfg["hardware.gpu"] == "RTX 3060"
    assert res["perfil"]["id"] == p["id"]
    assert hw_profiles.borrar(cfg, p["id"]) and cfg["hardware.perfil"] == "auto"
    assert not hw_profiles.borrar(cfg, "rendimiento")    # los de serie no se borran
    profiles.select_gpu("")


def test_api_perfiles(services):
    from fastapi.testclient import TestClient
    from nexus.server.app import create_app
    old = dict(services.profile)
    try:
        with TestClient(create_app(services)) as client:
            _api(client)
    finally:
        services.profile.clear()
        services.profile.update(old)
        for k in ("hardware.perfil", "hardware.gpu", "hardware.accel"):
            services.config.set(k, "auto" if k != "hardware.gpu" else "")


def _api(client):
    r = client.get("/api/hw/perfiles").json()
    assert r["activo"] and any(p["id"] == "rendimiento" for p in r["perfiles"]) and "gpus" in r
    r = client.put("/api/hw/perfiles/prueba", json={"nombre": "Prueba", "acelerador": "cpu", "contexto": 2048})
    assert r.status_code == 200 and r.json()["id"] == "prueba"
    r = client.post("/api/hw/perfiles/prueba/activar")
    assert r.status_code == 200, r.text
    assert r.json()["llm"]["n_ctx"] <= 4096
    assert client.get("/api/accel").json()["perfil_hw"] == "prueba"
    assert client.post("/api/hw/perfiles/auto/activar").status_code == 200
    assert client.delete("/api/hw/perfiles/prueba").status_code == 200
    assert client.post("/api/hw/perfiles/nada/activar").status_code == 404
    r = client.post("/api/hw/gpu", json={"gpu": ""})
    assert r.status_code == 200 and "acelerador" in r.json()
