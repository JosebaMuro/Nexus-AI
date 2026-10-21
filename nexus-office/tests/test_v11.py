"""Novedades 1.1: mejorador de prompts, gestor de modelos abierto, drivers y recursos del avatar."""
import json

from nexus import paths
from nexus.hardware import build_profile
from nexus.hardware.drivers import evaluate
from nexus.models.manager import annotate, best_mmproj
from nexus.prompt_enhancer import enhance, enhance_rules, translate_rules
from test_hardware import hw


def test_prompt_enhancer_rules():
    r = enhance_rules("una geisha bajo un eclipse con una pagoda", "image", "gotico")
    assert r["prompt"].startswith("a geisha under an eclipse with a pagoda") and "gothic" in r["prompt"] and "blurry" in r["negative"]
    m = enhance_rules("música épica para una batalla", "music")
    assert m["mood"] == "epica" and "bpm" in m["prompt"]
    c = enhance_rules("explicame la fotosintesis", "chat")
    assert "Objetivo" in c["prompt"] and "Formato" in c["prompt"]
    assert translate_rules("un gato negro") == "a cat black"   # traduccion palabra a palabra (sin LLM)
    assert enhance(None, "", "image")["prompt"] == ""


def test_prompt_enhancer_tool_with_builtin(services):
    r = services.supervisor.execute("enhance_prompt", {"text": "ciudad de neón bajo la lluvia", "target": "image"},
                                    __import__("nexus.tools", fromlist=["ToolContext"]).ToolContext(services=services))
    assert "city" in r["prompt"] and r["engine"] == "reglas"


def test_model_annotations_and_mmproj():
    prof = build_profile(hw(8, cores=2))
    files = [{"path": "m-Q4_K_M.gguf", "size": 2.4e9}, {"path": "m-Q8_0.gguf", "size": 4.3e9}, {"path": "m-F16.gguf", "size": 8e9},
             {"path": "mmproj-F16.gguf", "size": 8e8}, {"path": "mmproj-Q8_0.gguf", "size": 4e8},
             {"path": "big-00001-of-00002.gguf", "size": 1e9}]
    out = {f["path"]: f for f in annotate(files, prof, "huggingface", "org/Model-4B-GGUF")}
    assert out["m-Q4_K_M.gguf"].get("recommended") and out["m-Q4_K_M.gguf"]["quant"] == "Q4_K_M"
    assert out["m-F16.gguf"]["fit"] == "pesado" and out["mmproj-F16.gguf"]["kind"] == "proyector"
    assert out["big-00001-of-00002.gguf"]["split"] and out["m-Q4_K_M.gguf"].get("slow")   # 4B > 3B en esta CPU
    assert best_mmproj(files)["path"] == "mmproj-Q8_0.gguf"


def test_driver_evaluation():
    # versiones con el formato de Windows: se evaluan como Windows en cualquier sistema
    old = evaluate({"name": "Intel UHD", "vendor": "intel", "version": "31.0.101.4146", "date": "2023-01-10"}, "cpu", windows=True)
    assert old["status"] == "actualizar" and len(old["reasons"]) == 2 and old["options"]
    nv = evaluate({"name": "RTX", "vendor": "nvidia", "version": "31.0.15.3699", "date": ""}, "cuda", windows=True)   # 536.99
    assert nv["status"] == "actualizar" and any("CUDA" in r for r in nv["reasons"])
    ok = evaluate({"name": "RTX", "vendor": "nvidia", "version": "32.0.15.6094", "date": ""}, "cuda", windows=True)    # 560.94
    assert ok["status"] == "ok"


def test_driver_evaluation_linux():
    nv = evaluate({"name": "RTX", "vendor": "nvidia", "version": "535.104.05", "date": ""}, "cuda", windows=False)
    assert nv["status"] == "actualizar" and any("550.54" in r for r in nv["reasons"])
    ok = evaluate({"name": "RTX", "vendor": "nvidia", "version": "550.54.14", "date": ""}, "cuda", windows=False)
    assert ok["status"] == "ok"
    mesa = evaluate({"name": "Mesa (AMD/Intel)", "vendor": "mesa", "version": "24.0.5", "date": ""}, "vulkan", windows=False)
    assert mesa["status"] == "ok"
    unknown = evaluate({"name": "Intel UHD (i915)", "vendor": "intel", "version": "", "date": ""}, "cpu", windows=False)
    assert unknown["status"] == "desconocido"


def test_lan_access_requires_key(services):
    from fastapi.testclient import TestClient

    from nexus.server import create_app
    app = create_app(services)
    with TestClient(app) as local:
        info = local.get("/api/lan").json()           # solo desde el propio equipo
        key = info["key"]
        assert key and local.get("/api/status").status_code == 200
        remote = TestClient(app, client=("192.168.1.50", 50000))
        assert remote.get("/api/ping").json()["nexus"] is True                    # descubrimiento abierto
        assert remote.get("/api/status").status_code == 401                       # sin clave: bloqueado
        assert remote.get("/").status_code == 401 and "clave" in remote.get("/").text.lower()
        r = remote.get(f"/?key={key}")
        assert r.status_code == 200 and "nexus_key" in r.cookies                  # con clave: entra y queda recordada
        assert remote.get("/api/status").status_code == 200                       # la cookie autentica lo siguiente
        assert remote.get("/api/lan").status_code == 403                          # la clave nunca se entrega a remotos
        pkg = remote.get("/api/package/termux")
        assert pkg.status_code == 200 and pkg.content[:2] == bytes([0x1F, 0x8B])   # tar.gz para Termux


def test_geisha_assets_present():
    g = paths.WEB / "assets" / "geisha"
    rig = json.loads((g / "rig.json").read_text())
    assert len(rig["head"]["rows"]) > 40 and len(rig["body"]["rows"]) > 40
    top = rig["head"]["rows"][0][0]
    assert 1.6 < top < 1.9 and rig["body"]["rows"][-1][0] < 0.05      # figura de pie, de 1,7 m aprox.
    for f in ("face_front.png", "profile_left.png", "profile_right.png", "back_head.png", "body_front.png", "body_back.png", "background.jpg"):
        assert (g / f).exists(), f
