"""Paridad de imagen_editar / imagen_info / gimp_estado entre la edicion C++ y la Python (sin GIMP: motor integrado
frente a Pillow). Uso: python tests/compare_gimp.py RUTA/nexus-tests [RUTA/nexus-office]

Crea las mismas imagenes en dos carpetas de datos, ejecuta las mismas llamadas en las dos ediciones y compara las
respuestas (menos url y bytes, que dependen del codificador) y los pixeles de cada resultado (diferencia media).
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY_ROOT = HERE.parent.parent / "nexus-office"


def make_images(sandbox: Path) -> None:
    from PIL import Image, ImageDraw
    (sandbox / "adjuntos").mkdir(parents=True, exist_ok=True)
    w, h = 640, 480
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (int(255 * x / w), int(255 * y / h), (x * y) % 256 // 2 + 60)
    d = ImageDraw.Draw(img)
    d.ellipse((200, 120, 440, 360), fill=(230, 200, 40))
    img.save(sandbox / "adjuntos/foto.jpg", quality=92, dpi=(72, 72))
    exif = Image.Exif()
    exif[0x0112] = 6
    exif[0x010F] = "Nexus"
    exif[0x0110] = "Prueba 1"
    img.save(sandbox / "adjuntos/girada.jpg", quality=92, exif=exif.tobytes())
    logo = Image.new("RGBA", (200, 100), (0, 0, 0, 0))
    ImageDraw.Draw(logo).rectangle((10, 10, 190, 90), fill=(30, 90, 200, 200))
    logo.save(sandbox / "adjuntos/logo.png", dpi=(300, 300))
    img.convert("P", palette=Image.ADAPTIVE, colors=32).save(sandbox / "adjuntos/anim.gif")
    img.save(sandbox / "adjuntos/web.webp", quality=85)
    img.save(sandbox / "adjuntos/plano.bmp")


def calls() -> list[dict]:
    E = "imagen_editar"
    c = [{"tool": "gimp_estado", "args": {}}]
    for f in ("foto.jpg", "girada.jpg", "logo.png", "anim.gif", "web.webp", "plano.bmp"):
        c.append({"tool": "imagen_info", "args": {"imagen": f"adjuntos/{f}"}})
    single = [
        {"op": "redimensionar", "ancho": 320}, {"op": "redimensionar", "porcentaje": 50},
        {"op": "redimensionar", "ancho": 300, "alto": 300, "modo": "rellenar"},
        {"op": "redimensionar", "ancho": 300, "alto": 200, "modo": "exacto"},
        {"op": "recortar", "x": 100, "y": 50, "ancho": 200, "alto": 150},
        {"op": "encuadrar", "proporcion": "4:5"}, {"op": "encuadrar", "proporcion": "16:9", "posicion": "arriba"},
        {"op": "lienzo", "ancho": 800, "alto": 800, "color": "rojo"}, {"op": "borde", "grosor": 15, "color": "#000"},
        {"op": "rotar", "grados": 90}, {"op": "rotar", "grados": 180}, {"op": "rotar", "grados": -90},
        {"op": "voltear"}, {"op": "voltear", "direccion": "vertical"},
        {"op": "brillo_contraste", "brillo": 20, "contraste": 10}, {"op": "niveles_auto"}, {"op": "balance_blancos"},
        {"op": "saturacion", "cantidad": 40}, {"op": "tono", "grados": 60}, {"op": "gamma", "valor": 1.8},
        {"op": "blanco_negro"}, {"op": "sepia"}, {"op": "enfocar"}, {"op": "desenfocar", "radio": 6},
        {"op": "reducir_ruido", "fuerza": 3}, {"op": "reducir_ruido", "fuerza": 8}, {"op": "invertir"},
        {"op": "posterizar", "niveles": 4}, {"op": "vineta", "fuerza": 60}, {"op": "ppp", "valor": 150},
        {"op": "superponer", "imagen": "adjuntos/logo.png", "escala": 30, "posicion": "arriba-izquierda"},
        {"op": "texto", "texto": "Rebajas 30%", "color": "blanco", "posicion": "abajo"},
        {"op": "texto", "texto": "Ñandú", "tamano": 60, "opacidad": 50, "posicion": "centro"},
    ]
    for i, op in enumerate(single):
        c.append({"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [op], "formato": "png",
                                      "salida": f"op{i:02d}"}})
    for r in ("foto_carnet", "redes_cuadrado", "redes_vertical", "historia", "banner_web", "producto_fondo_blanco",
              "mejora_rapida", "web_ligera", "impresion_a4"):
        c.append({"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "receta": r}})
    c += [
        {"tool": E, "args": {"imagen": "adjuntos/girada.jpg", "operaciones": ["blanco_negro"]}},
        {"tool": E, "args": {"imagen": "adjuntos/logo.png", "operaciones": [{"op": "aplanar", "color": "azul"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/logo.png", "operaciones": [{"op": "rotar", "grados": 30}]}},
        {"tool": E, "args": {"imagen": "adjuntos/anim.gif", "operaciones": [{"op": "escalar", "ancho": 100}]}},
        {"tool": E, "args": {"imagen": "adjuntos/web.webp", "operaciones": ["invertir"], "formato": "webp"}},
        {"tool": E, "args": {"imagen": "adjuntos/plano.bmp", "operaciones": ["sepia"]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "girar", "grados": 45}],
                             "formato": "jpg", "calidad": 70}},
        {"tool": E, "args": {"imagen": "ultima", "operaciones": ["nitidez"], "salida": "sub/x.png"}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": ["invertir"], "salida": "op00"}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "volar"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "lienzo", "color": "chillón"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "voltear", "direccion": "x"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "rotar", "grados": "mucho"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "texto"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "superponer"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "redimensionar"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": [{"op": "encuadrar", "proporcion": "a"}]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": []}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "receta": "nada"}},
        {"tool": E, "args": {"imagen": "adjuntos/no.jpg", "operaciones": ["invertir"]}},
        {"tool": E, "args": {"imagen": "../fuera.jpg", "operaciones": ["invertir"]}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": ["invertir"], "salida": "../../x.png"}},
        {"tool": E, "args": {"imagen": "adjuntos/foto.jpg", "operaciones": ["invertir"], "formato": "xcf"}},
        {"tool": "gimp_script", "args": {"codigo": "x"}},
    ]
    return c


def run_python(data: Path, cs: list[dict]) -> list:
    code = r"""
import json, sys
from nexus.tools.builtin import gimp_tools as g
class S: config = None
class C: services = S(); emit = None
out = []
for c in json.load(open(sys.argv[1])):
    try:
        out.append({"ok": getattr(g, c["tool"])(c["args"], C())})
    except Exception as e:
        out.append({"error": f"{type(e).__name__}: {e}"})
print(json.dumps(out, default=str))
"""
    f = data / "calls.json"
    f.write_text(json.dumps(cs))
    env = dict(os.environ, NEXUS_DATA_DIR=str(data))
    r = subprocess.run([sys.executable, "-c", code, str(f)], capture_output=True, text=True, cwd=PY_ROOT, env=env)
    if r.returncode:
        sys.exit(r.stderr)
    return json.loads(r.stdout.strip().splitlines()[-1])


def run_cpp(exe: str, data: Path, cs: list[dict]) -> list:
    f = data / "calls.json"
    f.write_text(json.dumps(cs))
    env = dict(os.environ, NEXUS_DATA_DIR=str(data))
    if len(sys.argv) > 2:
        env["NEXUS_ROOT"] = str(Path(sys.argv[2]).resolve().parent)
    r = subprocess.run([exe, "--tool-calls", str(f)], capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(r.stderr)
    return json.loads(r.stdout.strip().splitlines()[-1])


def pixel_diff(a: Path, b: Path):
    from PIL import Image
    with Image.open(a) as x, Image.open(b) as y:
        if x.size != y.size:
            return f"tamano {x.size} vs {y.size}"
        if x.mode != y.mode and "A" in x.mode + y.mode:
            return f"modo {x.mode} vs {y.mode}"
        x, y = x.convert("RGB"), y.convert("RGB")
        dx, dy = x.tobytes(), y.tobytes()
        return round(sum(abs(p - q) for p, q in zip(dx, dy)) / len(dx), 2)


def main() -> None:
    exe = sys.argv[1]
    tmp = Path(tempfile.mkdtemp(prefix="cmpgimp-"))
    dpy, dcpp = tmp / "py", tmp / "cpp"
    for d in (dpy, dcpp):
        make_images(d / "sandbox")
    cs = calls()
    py = run_python(dpy, cs)
    cp = run_cpp(exe, dcpp, cs)
    same = 0
    bad_px = []
    for c, a, b in zip(cs, py, cp):
        strip = lambda r: {k: v for k, v in (r.get("ok") or {}).items() if k not in ("url", "bytes")} \
            if "ok" in r else r
        ra, rb = strip(a), strip(b)
        label = c["tool"] + " " + json.dumps(c["args"].get("operaciones") or c["args"].get("receta")
                                             or c["args"].get("imagen") or "", ensure_ascii=False)[:70]
        if ra == rb:
            same += 1
        else:
            print("DIFERENTE", label)
            print("  py :", json.dumps(ra, ensure_ascii=False)[:600])
            print("  cpp:", json.dumps(rb, ensure_ascii=False)[:600])
        if c["tool"] == "imagen_editar" and "ok" in a and "ok" in b:
            pa, pb = dpy / "sandbox" / a["ok"]["ruta"], dcpp / "sandbox" / b["ok"]["ruta"]
            d = pixel_diff(pa, pb)
            if not isinstance(d, float) or d > 6:
                bad_px.append((label, d))
    print(f"{same}/{len(cs)} respuestas iguales")
    for label, d in bad_px:
        print("PIXELES", label, d)
    print(f"{len(bad_px)} resultados con pixeles distintos (media > 6 de 255)")


if __name__ == "__main__":
    main()
