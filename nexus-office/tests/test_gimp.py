"""GIMP como herramienta de los agentes: operaciones, motores (GIMP 3, GIMP 2.10, Pillow), herramientas sobre la
carpeta de trabajo y paquete de conocimiento (manual en el RAG + grafo).

Las pruebas con GIMP real se saltan si no esta instalado. NEXUS_GIMP_TEST=ruta1|ruta2 anade mas ejecutables
(p. ej. un GIMP 3 y un GIMP 2.10 a la vez)."""
import base64
import io
import os
from pathlib import Path

import pytest
from PIL import Image, ImageDraw

from nexus.media import gimp


class Cfg(dict):
    def get(self, k, d=None):
        return dict.get(self, k, d)


def _photo(path: Path, size=(800, 600), alpha=False) -> Path:
    im = Image.new("RGBA" if alpha else "RGB", size, (150, 110, 80, 255) if alpha else (150, 110, 80))
    d = ImageDraw.Draw(im)
    d.ellipse((size[0] // 4, size[1] // 5, size[0] * 3 // 4, size[1] * 4 // 5), fill=(230, 190, 60))
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path)
    return path


ALL_OPS = [{"op": "redimensionar", "ancho": 640}, {"op": "balance_blancos"}, {"op": "niveles_auto"},
           {"op": "brillo_contraste", "brillo": 10, "contraste": 15}, {"op": "saturacion", "cantidad": 20},
           {"op": "tono", "grados": 10}, {"op": "gamma", "valor": 1.2}, {"op": "enfocar"},
           {"op": "desenfocar", "radio": 2}, {"op": "reducir_ruido"}, {"op": "vineta"},
           {"op": "texto", "texto": "Año 2026 · Peluquería Núñez"}, {"op": "borde", "grosor": 12, "color": "negro"},
           {"op": "ppp", "valor": 300}]
GEO_OPS = [{"op": "rotar", "grados": 90}, {"op": "voltear"}, {"op": "encuadrar", "proporcion": "4:5", "ancho": 400},
           {"op": "lienzo", "ancho": 500, "alto": 600, "color": "#eeeeee"}, {"op": "blanco_negro"},
           {"op": "posterizar", "niveles": 4}, {"op": "invertir"}, {"op": "sepia"}]


# ------------------------------------------------------------------ validacion y geometria
def test_normalize_defaults_aliases_and_errors():
    ops = gimp.normalize([{"op": "resize", "ancho": "300"}, "byn", {"op": "texto", "texto": "Hola", "color": "rojo"}])
    assert [o["op"] for o in ops] == ["redimensionar", "blanco_negro", "texto"]
    assert ops[0]["ancho"] == 300 and ops[2]["color"] == (220, 30, 40) and ops[2]["posicion"] == "abajo-derecha"
    assert gimp.normalize([{"op": "saturacion", "cantidad": 500}])[0]["cantidad"] == 100   # limites
    for bad in ([{"op": "magia"}], [{"op": "texto"}], [{"op": "lienzo", "color": "#zz"}],
                [{"op": "voltear", "direccion": "diagonal"}], []):
        with pytest.raises(gimp.EditError):
            gimp.normalize(bad)


def test_resolve_sizes():
    prim, size = gimp.resolve(gimp.normalize([{"op": "redimensionar", "ancho": 400, "alto": 400}]), 800, 600)
    assert size == (400, 300) and prim == [{"op": "escalar", "w": 400, "h": 300}]   # ajustar: cabe sin deformar
    _, size = gimp.resolve(gimp.normalize([{"op": "redimensionar", "ancho": 400, "alto": 400, "modo": "exacto"}]),
                           800, 600)
    assert size == (400, 400)
    prim, size = gimp.resolve(gimp.normalize(gimp.RECIPES["foto_carnet"]["ops"]), 3000, 4000)
    assert size == (307, 378)
    crop = next(p for p in prim if p["op"] == "recortar")
    assert abs(crop["w"] / crop["h"] - 26 / 32) < 0.01 and crop["x"] == (3000 - crop["w"]) // 2
    _, size = gimp.resolve(gimp.normalize([{"op": "rotar", "grados": 90}, {"op": "borde", "grosor": 10}]), 800, 600)
    assert size == (620, 820)
    prim, _ = gimp.resolve(gimp.normalize([{"op": "superponer", "imagen": "logo.png", "escala": 25,
                                            "posicion": "arriba-izquierda"}]), 1000, 800, lambda _: (200, 100))
    assert (prim[0]["w"], prim[0]["h"], prim[0]["x"], prim[0]["y"]) == (250, 125, 24, 24)


# ------------------------------------------------------------------ motores
def _engines():
    out = [("pillow", None)]
    cmds = [c for c in os.environ.get("NEXUS_GIMP_TEST", "").split("|") if c]
    found = gimp.find(Cfg())
    if found:
        cmds.append(found["cmd"][0] if len(found["cmd"]) == 1 else "|".join(found["cmd"]))
    for c in dict.fromkeys(cmds):
        info = gimp.probe(c.split("|") if "|" in c else [c])
        if info:
            out.append((f"gimp{info['version']}", c))
    return out


@pytest.mark.parametrize("name,cmd", _engines())
def test_engine_all_operations(data_dir, name, cmd):
    src = _photo(data_dir / "in" / "foto.jpg")
    logo = Image.new("RGBA", (200, 100), (0, 0, 0, 0))
    ImageDraw.Draw(logo).ellipse((0, 0, 200, 100), fill=(200, 20, 20, 255))
    logo.save(data_dir / "in" / "logo.png")
    eng = gimp.GimpEngine(Cfg({"media.gimp": "pil" if cmd is None else "gimp", "media.gimp_path": cmd or ""}))
    ops = ALL_OPS + [{"op": "superponer", "imagen": str(data_dir / "in" / "logo.png"), "escala": 15}]
    res = eng.edit(src, ops, data_dir / "out" / "todo.jpg", quality=88)
    assert res["motor"].lower().startswith("gimp" if cmd else "pillow") and "nota" not in res
    with Image.open(data_dir / "out" / "todo.jpg") as im:
        assert im.size == (664, 504) == tuple(res["tamano"]) and im.mode == "RGB"
        assert round(im.info["dpi"][0]) == 300
    res = eng.edit(src, GEO_OPS, data_dir / "out" / "geo.png")
    with Image.open(data_dir / "out" / "geo.png") as im:
        assert im.size == (500, 600) and im.mode == "RGB"   # sin transparencia de origen: se aplana
    # un giro libre deja esquinas transparentes: se conservan en PNG
    eng.edit(src, [{"op": "rotar", "grados": 15}], data_dir / "out" / "giro.png")
    with Image.open(data_dir / "out" / "giro.png") as im:
        assert im.mode == "RGBA" and im.getpixel((0, 0))[3] == 0
    if cmd:   # el proyecto editable de GIMP
        eng.edit(src, [{"op": "redimensionar", "porcentaje": 50}], data_dir / "out" / "proyecto.xcf")
        assert (data_dir / "out" / "proyecto.xcf").read_bytes()[:8] == b"gimp xcf"
    else:
        with pytest.raises(gimp.EditError):
            eng.edit(src, [{"op": "invertir"}], data_dir / "out" / "proyecto.xcf")


@pytest.mark.parametrize("name,cmd", [e for e in _engines() if e[1]])
def test_gimp_script_and_failures(data_dir, name, cmd):
    src = _photo(data_dir / "in" / "foto.png")
    eng = gimp.GimpEngine(Cfg({"media.gimp_path": cmd}))
    major = eng.info()["major"]
    code = ("img = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(ENTRADA))\n"
            "img.scale(100, 50)\nimg.flatten()\n"
            "Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, img, Gio.File.new_for_path(SALIDA), None)\n") if major >= 3 else \
        ("(let* ((img (car (gimp-file-load RUN-NONINTERACTIVE ENTRADA ENTRADA))))"
         "(gimp-image-scale img 100 50)"
         "(file-png-save-defaults RUN-NONINTERACTIVE img (car (gimp-image-flatten img)) SALIDA SALIDA))")
    res = eng.run_script(code, src, data_dir / "out.png")
    assert res["ok"] and res["fichero"]
    with Image.open(data_dir / "out.png") as im:
        assert im.size == (100, 50)
    bad = eng.run_script("esto no es codigo (", src, data_dir / "nada.png", timeout=120)
    assert not bad["ok"] and not bad["fichero"]
    # si GIMP falla en una edicion (fuente imposible no, imagen superpuesta rota si), se hace con Pillow y se avisa
    broken = data_dir / "roto.png"
    broken.write_bytes(b"no es una imagen")
    with pytest.raises(gimp.EditError):
        eng.edit(src, [{"op": "superponer", "imagen": str(broken)}], data_dir / "x.png")


def test_engine_without_gimp_status(monkeypatch):
    monkeypatch.setattr(gimp, "find", lambda *a, **k: None)
    st = gimp.GimpEngine(Cfg()).status()
    assert st["gimp"] is False and st["motor"] == "pillow" and "Pillow" in st["nota"]
    assert set(st["recetas"]) == set(gimp.RECIPES) and "texto" in st["operaciones"]


def test_bundled_gimp_is_preferred(tmp_path, monkeypatch):
    fake = tmp_path / "bundled" / "runtimes" / "gimp" / "bin"
    fake.mkdir(parents=True)
    exe = fake / ("gimp-console-3.0.exe" if os.name == "nt" else "gimp-console-3.0")
    exe.write_text("")
    monkeypatch.setattr(gimp.paths, "ROOT", tmp_path)
    monkeypatch.setattr(gimp, "_bundled_dir", lambda: tmp_path / "bundled" / "runtimes" / "gimp")
    if os.name != "nt" and not __import__("sys").platform == "darwin":
        (tmp_path / "bundled" / "runtimes" / "gimp" / "nexus-gimp-console").write_text("")
    cands = gimp.candidates(Cfg())
    assert cands[0][1] == "incluido"


# ------------------------------------------------------------------ herramientas de los agentes
def test_tools_on_workspace(services, data_dir):
    from nexus import paths
    from nexus.tools import ToolContext
    from nexus.tools.builtin import gimp_tools
    services.config.set("media.gimp", "pil", save=False)   # rapido y determinista
    _photo(paths.sandbox_dir() / "adjuntos" / "tarta.jpg", (1200, 900))
    _photo(paths.sandbox_dir() / "imagenes" / "logo.png", (300, 150), alpha=True)
    events = []
    ctx = ToolContext(services=services, agent="disenador-grafico", emit=events.append)
    res = services.supervisor.execute("imagen_editar", {
        "imagen": "adjuntos/tarta.jpg", "receta": "redes_vertical",
        "operaciones": [{"op": "texto", "texto": "Pastelería Lola"},
                        {"op": "superponer", "imagen": "imagenes/logo.png", "escala": 15}]}, ctx)
    assert res["ruta"] == "imagenes/tarta_editada.jpg" and res["tamano"] == [1080, 1350], res
    assert res["url"].startswith("/outputs/images/") and (paths.outputs_dir() / res["url"][9:]).exists()
    assert any(e["type"] == "media" for e in events)
    again = services.supervisor.execute("imagen_editar", {"imagen": "ultima", "operaciones": [{"op": "sepia"}]}, ctx)
    assert again["original"] == "imagenes/tarta_editada.jpg" and again["ruta"] == "imagenes/tarta_editada_editada.jpg"
    third = services.supervisor.execute("imagen_editar", {"imagen": "adjuntos/tarta.jpg", "receta": "redes_vertical"},
                                        ctx)
    assert third["ruta"] == "imagenes/tarta_editada_2.jpg"   # nunca pisa un fichero
    info = services.supervisor.execute("imagen_info", {"imagen": "imagenes/tarta_editada.jpg"}, ctx)
    assert info["ancho"] == 1080 and info["formato"] == "JPEG" and "analisis" in info
    for bad in ("../../config.yaml", "/etc/passwd", "adjuntos/no-existe.jpg"):
        out = services.supervisor.execute("imagen_editar", {"imagen": bad, "operaciones": ["invertir"]}, ctx)
        assert "error" in out
    out = services.supervisor.execute("imagen_editar", {"imagen": "adjuntos/tarta.jpg", "operaciones": ["invertir"],
                                                        "salida": "../../fuera.png"}, ctx)
    assert "error" in out and not (data_dir / "fuera.png").exists()
    assert gimp_tools.TOOLS[-1][0]["risk"] == "high"   # gimp_script: codigo libre, lo aprueba un administrador
    st = services.supervisor.execute("gimp_estado", {}, ctx)
    assert st["motor"] == "pillow"


def test_chat_images_are_saved_to_workspace(services):
    from nexus import paths
    from nexus.agents.manager import store_images
    buf = io.BytesIO()
    Image.new("RGB", (10, 10), "red").save(buf, "JPEG")
    saved = store_images(["data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode(), "basura"])
    assert len(saved) == 1 and saved[0].startswith("adjuntos/") and saved[0].endswith(".jpg")
    assert (paths.sandbox_dir() / saved[0]).exists()


# ------------------------------------------------------------------ agentes y paquete de conocimiento
def test_catalog_and_profiles_use_the_designer():
    from nexus import office
    cat, profs = office.catalog(), office.profiles()
    assert "gimp_script" in cat["disenador-grafico"]["tools"]
    for pid in ("agencia-marketing", "imprenta-artes-graficas", "estudio-fotografia"):
        assert "disenador-grafico" in profs[pid]["agents"]
    pack = office.packs()["gimp"]
    for aid in pack["agentes"]:
        assert aid in cat or (Path(office.DEFAULTS_DIR) / f"{aid}.yaml").exists()
    assert len(list(pack["dir"].glob("*.md"))) >= 9 and len(pack["grafo"]) >= 100
    assert all(len(t) == 4 for t in office.seeds({"grafo": pack["grafo"]}))


def test_knowledge_pack_installs_in_rag_and_graph(services, monkeypatch):
    from nexus import office
    monkeypatch.delenv("NEXUS_NO_KNOWLEDGE_PACKS", raising=False)
    office.Office(services.config).apply("estudio-fotografia")
    services.agents.reload()
    # una instalacion anterior: el fotografo ya existia sin las herramientas de GIMP
    from dataclasses import asdict
    spec = services.agents.get("fotografo")
    services.agents.save({**asdict(spec), "tools": [t for t in spec.tools if t not in ("imagen_editar", "imagen_info",
                                                                                         "gimp_estado")]})
    done = office.install_packs(services)
    assert {"gimp:disenador-grafico", "gimp:fotografo"} <= set(done) and done["gimp:fotografo"] >= 9
    assert "imagen_editar" in services.agents.get("fotografo").tools
    docs = [d for d in services.rag.list(agent="disenador-grafico") if d["agent"] == "disenador-grafico"]
    assert len(docs) >= 9 and all(d["meta"]["paquete"] == "gimp" for d in docs)
    assert not [d for d in services.rag.list(agent="") if d["meta"].get("paquete")]   # no va al grafo general
    hits = services.rag.search("atajo para clonar y fijar el origen", k=5, agent="disenador-grafico")
    assert any("Ctrl+clic" in h["text"] for h in hits)
    g = services.graph.for_agent("disenador-grafico")
    facts = " ".join(g.facts_about(["Foto de DNI"], limit=10))
    assert "32 × 26 mm" in facts
    assert office.install_packs(services) == {}   # idempotente
    # una version nueva del paquete sustituye los documentos (no se duplican)
    pk = office.packs()["gimp"]
    monkeypatch.setattr(office, "packs", lambda: {"gimp": {**pk, "version": pk["version"] + 1}})
    office.install_packs(services)
    docs2 = [d for d in services.rag.list(agent="disenador-grafico") if d["agent"] == "disenador-grafico"]
    assert len(docs2) == len(docs) and all(d["meta"]["version"] == pk["version"] + 1 for d in docs2)
    # el paquete no impide la siembra del catalogo (usa «conoce», no «domina»)
    assert office.Office(services.config).seed_graphs(services.graph, ["disenador-grafico"])["disenador-grafico"] > 0


def test_packs_can_be_disabled(services, monkeypatch):
    from nexus import office
    monkeypatch.delenv("NEXUS_NO_KNOWLEDGE_PACKS", raising=False)
    services.config.set("office.knowledge_packs", False, save=False)
    assert office.install_packs(services) == {}
