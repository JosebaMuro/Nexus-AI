"""La imagen tiene que depender del prompt: prompts distintos dan imagenes distintas y con lo que dice el texto.

Antes, sin modelo de imagen (y siempre en Android) se dibujaba un campo de flujo con la paleta del prompt: dos
prompts sin palabras de color daban el mismo «fractal». Y con sd.cpp el prompt en espanol llegaba sin traducir.
"""
import os
import stat
import sys

import numpy as np
import pytest

from nexus.media import scene
from nexus.media.image import ImageEngine

N = 192


def arr(prompt, seed=1):
    return np.asarray(scene.compose(prompt, seed, N), dtype=np.float32)


def brightness(a):
    return float(a.mean())


def frac(a, cond):
    return float(cond(a[..., 0], a[..., 1], a[..., 2]).mean())


def test_parse_entiende_espanol_e_ingles():
    s = scene.parse("Dos gatos negros bajo la luna en una ciudad")
    assert s["time"] == "night" and s["places"] == ["city"] and "moon" in s["sky"]
    assert s["subjects"] == [{"kind": "cat", "color": scene.COLORS["negro"], "count": 2}]
    s = scene.parse("a red car on the beach at sunset")
    assert s["time"] == "sunset" and s["places"] == ["beach"]
    assert s["subjects"][0]["kind"] == "car" and s["subjects"][0]["color"] == scene.COLORS["red"]
    s = scene.parse("Castillo en la montaña con nieve y un árbol")
    assert s["places"] == ["mountain"] and "snow" in s["weather"]
    assert [x["kind"] for x in s["subjects"]] == ["castle", "tree"]
    assert scene.parse("un ramo de rosas rojas")["subjects"][0]["kind"] == "flower"
    assert scene.parse("un gato rosa")["subjects"][0]["color"] == scene.COLORS["rosa"]


def test_prompts_distintos_dan_imagenes_distintas():
    prompts = ["un gato negro bajo la luna", "una casa en la playa al atardecer", "a red car in a city at night",
               "castillo en la montaña con nieve", "dos perros en un campo de flores amarillas",
               "un astronauta en el espacio", "un barco en el mar con tormenta", "robot en el desierto al amanecer"]
    imgs = [arr(p) for p in prompts]
    for i in range(len(imgs)):
        for j in range(i + 1, len(imgs)):
            diff = np.abs(imgs[i] - imgs[j]).mean()
            assert diff > 20, f"«{prompts[i]}» y «{prompts[j]}» se parecen demasiado ({diff:.1f})"


def test_mismo_prompt_y_semilla_es_reproducible():
    assert np.array_equal(arr("un faro en la costa de noche", 5), arr("un faro en la costa de noche", 5))


def test_la_imagen_refleja_la_hora():
    noche, dia = arr("una casa en el campo de noche"), arr("una casa en el campo de dia")
    assert brightness(noche[: N // 3]) < 60 < brightness(dia[: N // 3])
    tarde = arr("una casa en el campo al atardecer")[: N // 3]
    assert tarde[..., 0].mean() > tarde[..., 2].mean()       # cielo calido al atardecer


def test_la_imagen_refleja_el_lugar():
    mar = arr("el mar en calma")[int(N * 0.65):]
    assert mar[..., 2].mean() > mar[..., 0].mean() + 40       # agua azul abajo
    desierto = arr("un desierto")[int(N * 0.75):]
    assert desierto[..., 0].mean() > desierto[..., 2].mean() + 50   # arena
    nieve = arr("una montaña con nieve")[int(N * 0.75):]
    assert nieve.mean() > 200                                 # suelo blanco
    espacio = arr("el espacio")
    assert brightness(espacio) < 70


def test_la_imagen_refleja_el_color_del_sujeto():
    def red(r, g, b):
        return (r > 150) & (g < 80) & (b < 80)

    def blue(r, g, b):
        return (b > 150) & (r < 80) & (g < 120)
    rojo, azul = arr("un coche rojo en la ciudad"), arr("un coche azul en la ciudad")
    assert frac(rojo, red) > 0.01 and frac(azul, red) < frac(rojo, red) / 3
    assert frac(azul, blue) > 0.01 and frac(rojo, blue) < frac(azul, blue) / 3


def test_la_imagen_contiene_el_sujeto():
    """El sujeto se dibuja en el suelo con su color: un gato naranja aparece en la mitad de abajo, y con el mismo
    fondo (mismo lugar y semilla) la diferencia con un perro esta solo donde van los animales."""
    def orange(r, g, b):
        return (r > 200) & (g > 110) & (g < 160) & (b < 80)
    gato, perro = arr("un gato naranja en un prado", 3), arr("un perro en un prado", 3)
    assert frac(gato[N // 2:], orange) > 0.01 and frac(perro, orange) < 0.001
    d = np.abs(gato - perro).sum(axis=2) > 60
    ys, _ = np.nonzero(d)
    assert len(ys) > N * N * 0.01 and ys.min() > N * 0.4   # el cielo y el horizonte no cambian


def test_generate_sin_modelo_usa_la_escena(data_dir):
    eng = ImageEngine(_Cfg(), _Catalog([]), _Runtimes(None), _profile)
    a = eng.generate("un gato negro bajo la luna", seed=1)
    b = eng.generate("una casa en la playa al atardecer", seed=1)
    assert a["engine"] == b["engine"] == "escena"
    assert "modelo de imagen" in a["note"]
    from PIL import Image
    ia, ib = (np.asarray(Image.open(r["path"]), dtype=np.float32) for r in (a, b))
    assert np.abs(ia - ib).mean() > 20
    assert eng.generate("un fractal abstracto morado")["engine"] == "procedural"
    assert "No he reconocido" in eng.generate("xyzzy qwerty")["note"]


@pytest.mark.skipif(os.name == "nt", reason="sd-cli falso como script de shell")
def test_sdcpp_recibe_el_prompt_en_ingles(data_dir, tmp_path):
    """stable-diffusion.cpp (CLIP) no entiende espanol: el prompt llega traducido."""
    log = tmp_path / "args.txt"
    fake = tmp_path / "sd-cli"
    fake.write_text(f"""#!{sys.executable}
import sys
from PIL import Image
a = sys.argv
open({str(log)!r}, "w").write(a[a.index("-p") + 1])
Image.new("RGB", (8, 8)).save(a[a.index("-o") + 1])
""")
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    model = tmp_path / "sdxs.gguf"
    model.write_bytes(b"GGUF")
    entry = {"id": "sdxs-512", "kind": "image", "format": "gguf", "path": str(model), "gen": {"steps": 1, "size": 512}}
    eng = ImageEngine(_Cfg(), _Catalog([entry]), _Runtimes((fake, "cpu")), _profile)
    res = eng.generate("un gato negro bajo la luna en el bosque", seed=3)
    assert res["engine"] == "sd.cpp (cpu)"
    sent = log.read_text()
    assert "cat" in sent and "black" in sent and "moon" in sent and "forest" in sent and "gato" not in sent
    assert res["prompt"] == sent
    # si sd.cpp falla se dice por que y la imagen sigue el prompt
    fake.write_text("#!/bin/sh\necho 'error: modelo corrupto' >&2\nexit 1\n")
    res = eng.generate("un gato negro bajo la luna", seed=3)
    assert res["engine"] == "escena" and "modelo corrupto" in res["note"]


class _Cfg:
    def get(self, key, default=None):
        return default


class _Catalog:
    def __init__(self, items):
        self.items = items

    def get(self, mid):
        return next((m for m in self.items if m["id"] == mid), None)

    def list(self):
        return self.items


class _Runtimes:
    def __init__(self, exe):
        self._exe = exe

    def exe(self, runtime, profile):
        return self._exe


def _profile():
    return {"image": {"resolution": 512}, "llm": {"n_threads": 2}}
