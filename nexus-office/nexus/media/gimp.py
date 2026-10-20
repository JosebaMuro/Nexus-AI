"""GIMP como motor de edicion de imagen para los agentes (retoque, encuadre, texto, logos, exportacion).

Orden de busqueda del programa: ``media.gimp_path`` de la configuracion, el GIMP incluido en el paquete offline
(``bundled/runtimes/gimp``), el del sistema (PATH, Archivos de programa, /Applications, Flatpak, Snap). GIMP se
ejecuta sin ventana (modo batch) con su propio perfil en ``data/gimp`` para no tocar el del usuario:

- GIMP 3.x: Python-Fu (``python-fu-eval``) con la API de GObject y los filtros GEGL.
- GIMP 2.10: Script-Fu (``plug-in-script-fu-eval``).

Sin GIMP (Android, o un equipo donde no se puede ejecutar) el mismo trabajo se hace con Pillow: las operaciones
y sus parametros son los mismos, cambia solo la calidad de algunos filtros. Nada se descarga.

Una edicion es una lista de operaciones (``OPS``) que se resuelve primero a medidas exactas en pixeles
(``resolve``) y despues la aplica el motor elegido. Las recetas (``RECIPES``) son listas ya preparadas: foto de
carnet, formatos de redes sociales, producto con fondo blanco...
"""
from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .. import log, paths

L = log.get("gimp")

POSITIONS = ("arriba-izquierda", "arriba", "arriba-derecha", "izquierda", "centro", "derecha",
             "abajo-izquierda", "abajo", "abajo-derecha")
FORMATS = {"png": "png", "jpg": "jpg", "jpeg": "jpg", "webp": "webp", "tif": "tif", "tiff": "tif", "xcf": "xcf",
           "bmp": "bmp"}

# operacion -> {parametro: (tipo, defecto, minimo, maximo)}; tipo "color" admite "#rrggbb", nombre o [r, g, b]
OPS: dict[str, dict] = {
    "redimensionar": {"ancho": (int, 0, 0, 20000), "alto": (int, 0, 0, 20000), "porcentaje": (float, 0, 0, 1000),
                      "modo": (str, "ajustar", None, ("ajustar", "exacto", "rellenar"))},
    "recortar": {"x": (int, 0, 0, 20000), "y": (int, 0, 0, 20000), "ancho": (int, 0, 0, 20000),
                 "alto": (int, 0, 0, 20000)},
    "encuadrar": {"proporcion": (str, "1:1", None, None), "ancho": (int, 0, 0, 20000), "alto": (int, 0, 0, 20000),
                  "posicion": (str, "centro", None, POSITIONS)},
    "lienzo": {"ancho": (int, 0, 0, 20000), "alto": (int, 0, 0, 20000), "color": ("color", "#ffffff", None, None)},
    "borde": {"grosor": (int, 20, 1, 2000), "color": ("color", "#ffffff", None, None)},
    "rotar": {"grados": (float, 90, -360, 360)},
    "voltear": {"direccion": (str, "horizontal", None, ("horizontal", "vertical"))},
    "brillo_contraste": {"brillo": (float, 0, -100, 100), "contraste": (float, 0, -100, 100)},
    "niveles_auto": {},
    "balance_blancos": {},
    "saturacion": {"cantidad": (float, 15, -100, 100)},
    "tono": {"grados": (float, 0, -180, 180)},
    "gamma": {"valor": (float, 1.0, 0.1, 10)},
    "blanco_negro": {},
    "sepia": {},
    "enfocar": {"radio": (float, 2.0, 0.1, 50), "cantidad": (float, 0.6, 0, 5), "umbral": (int, 0, 0, 255)},
    "desenfocar": {"radio": (float, 5, 0.1, 200)},
    "reducir_ruido": {"fuerza": (int, 3, 1, 10)},
    "invertir": {},
    "posterizar": {"niveles": (int, 6, 2, 64)},
    "vineta": {"fuerza": (float, 45, 0, 100)},
    "texto": {"texto": (str, "", None, None), "tamano": (int, 0, 0, 2000), "color": ("color", "#ffffff", None, None),
              "posicion": (str, "abajo-derecha", None, POSITIONS), "opacidad": (float, 100, 0, 100),
              "fuente": (str, "Sans-serif", None, None)},
    "superponer": {"imagen": (str, "", None, None), "escala": (float, 20, 1, 100),
                   "posicion": (str, "abajo-derecha", None, POSITIONS), "opacidad": (float, 85, 0, 100)},
    "ppp": {"valor": (float, 300, 10, 2400)},
    "aplanar": {"color": ("color", "#ffffff", None, None)},
}
OP_HELP = {
    "redimensionar": "ancho y/o alto en px o porcentaje; modo ajustar (cabe dentro, sin deformar), exacto o rellenar",
    "recortar": "x, y, ancho, alto en px", "encuadrar": "recorta a una proporción (1:1, 4:5, 16:9, 26:32...) "
    "y, si se indica, deja ancho x alto exactos", "lienzo": "amplía el lienzo centrado con un color",
    "borde": "marco de grosor px", "rotar": "grados en sentido horario", "voltear": "horizontal o vertical",
    "brillo_contraste": "-100 a 100", "niveles_auto": "estira el contraste sin cambiar los colores",
    "balance_blancos": "corrige dominantes de color", "saturacion": "-100 a 100", "tono": "-180 a 180 grados",
    "gamma": "0,1 a 10 (más de 1 aclara las sombras)", "blanco_negro": "", "sepia": "",
    "enfocar": "máscara de enfoque: radio, cantidad, umbral", "desenfocar": "desenfoque gaussiano, radio px",
    "reducir_ruido": "fuerza 1-10", "invertir": "negativo", "posterizar": "niveles 2-64",
    "vineta": "oscurece los bordes, fuerza 0-100", "texto": "texto, tamano px, color, posicion, opacidad, fuente",
    "superponer": "pega otra imagen (logo) de la carpeta de trabajo: imagen, escala % del ancho, posicion, opacidad",
    "ppp": "resolución de impresión", "aplanar": "quita la transparencia sobre un color",
}

RECIPES: dict[str, dict] = {
    "foto_carnet": {"descripcion": "DNI y pasaporte: 26 x 32 mm a 300 ppp (307 x 378 px), fondo claro",
                    "ops": [{"op": "aplanar", "color": "#ffffff"}, {"op": "encuadrar", "proporcion": "26:32",
                            "ancho": 307, "alto": 378}, {"op": "niveles_auto"},
                            {"op": "enfocar", "radio": 1.0, "cantidad": 0.4}, {"op": "ppp", "valor": 300}],
                    "formato": "jpg"},
    "redes_cuadrado": {"descripcion": "Publicación cuadrada 1080 x 1080 (Instagram, Facebook, LinkedIn)",
                       "ops": [{"op": "encuadrar", "proporcion": "1:1", "ancho": 1080, "alto": 1080},
                               {"op": "enfocar", "radio": 1.0, "cantidad": 0.35}], "formato": "jpg"},
    "redes_vertical": {"descripcion": "Publicación vertical 1080 x 1350 (4:5)",
                       "ops": [{"op": "encuadrar", "proporcion": "4:5", "ancho": 1080, "alto": 1350},
                               {"op": "enfocar", "radio": 1.0, "cantidad": 0.35}], "formato": "jpg"},
    "historia": {"descripcion": "Historia o reel 1080 x 1920 (9:16)",
                 "ops": [{"op": "encuadrar", "proporcion": "9:16", "ancho": 1080, "alto": 1920}], "formato": "jpg"},
    "banner_web": {"descripcion": "Cabecera web 1920 x 600",
                   "ops": [{"op": "encuadrar", "proporcion": "16:5", "ancho": 1920, "alto": 600}], "formato": "jpg"},
    "producto_fondo_blanco": {"descripcion": "Producto para tienda online: cuadrado 2000 px sobre blanco, "
                              "colores corregidos y enfocado",
                              "ops": [{"op": "aplanar", "color": "#ffffff"}, {"op": "balance_blancos"},
                                      {"op": "niveles_auto"}, {"op": "redimensionar", "ancho": 1800, "alto": 1800},
                                      {"op": "lienzo", "ancho": 2000, "alto": 2000, "color": "#ffffff"},
                                      {"op": "enfocar", "radio": 1.2, "cantidad": 0.5}], "formato": "jpg"},
    "mejora_rapida": {"descripcion": "Retoque general: niveles, color, algo de saturación y enfoque",
                      "ops": [{"op": "balance_blancos"}, {"op": "niveles_auto"}, {"op": "saturacion", "cantidad": 10},
                              {"op": "enfocar", "radio": 1.5, "cantidad": 0.5}]},
    "web_ligera": {"descripcion": "Imagen para web: 1600 px de lado mayor en WebP",
                   "ops": [{"op": "redimensionar", "ancho": 1600, "alto": 1600}], "formato": "webp", "calidad": 82},
    "impresion_a4": {"descripcion": "Para imprimir en A4 a 300 ppp (2480 x 3508 px), en TIFF",
                     "ops": [{"op": "redimensionar", "ancho": 2480, "alto": 3508}, {"op": "ppp", "valor": 300}],
                     "formato": "tif"},
}

NAMED = {"blanco": (255, 255, 255), "white": (255, 255, 255), "negro": (0, 0, 0), "black": (0, 0, 0),
         "rojo": (220, 30, 40), "red": (220, 30, 40), "verde": (30, 160, 70), "green": (30, 160, 70),
         "azul": (30, 90, 200), "blue": (30, 90, 200), "amarillo": (250, 210, 30), "yellow": (250, 210, 30),
         "gris": (128, 128, 128), "gray": (128, 128, 128), "grey": (128, 128, 128), "naranja": (245, 130, 30),
         "orange": (245, 130, 30), "rosa": (240, 110, 170), "morado": (120, 60, 160), "beige": (235, 222, 200),
         "transparente": (255, 255, 255)}


class EditError(ValueError):
    pass


# ------------------------------------------------------------------ validacion y resolucion
def parse_color(v) -> tuple[int, int, int]:
    if isinstance(v, (list, tuple)) and len(v) >= 3:
        return tuple(max(0, min(255, int(c))) for c in v[:3])
    s = str(v or "").strip().lower()
    if s in NAMED:
        return NAMED[s]
    m = re.fullmatch(r"#?([0-9a-f]{6}|[0-9a-f]{3})", s)
    if not m:
        raise EditError(f"color no valido: {v!r} (usa #rrggbb o un nombre como blanco, negro, rojo)")
    h = m.group(1)
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def normalize(ops) -> list[dict]:
    """Comprueba nombres y limites de cada operacion y rellena los valores por defecto."""
    if isinstance(ops, (str, dict)):
        ops = [ops]
    out = []
    for raw in ops or []:
        item = {"op": raw} if isinstance(raw, str) else dict(raw or {})
        name = str(item.pop("op", "") or item.pop("operacion", "")).strip().lower().replace(" ", "_")
        name = {"resize": "redimensionar", "crop": "recortar", "escalar": "redimensionar", "girar": "rotar",
                "brillo": "brillo_contraste", "contraste": "brillo_contraste", "byn": "blanco_negro",
                "nitidez": "enfocar", "marca_agua": "texto", "logo": "superponer", "dpi": "ppp",
                "viñeta": "vineta", "ruido": "reducir_ruido"}.get(name, name)
        if name not in OPS:
            raise EditError(f"operacion desconocida: {name!r}. Validas: {', '.join(OPS)}")
        clean = {"op": name}
        for key, (typ, default, lo, hi) in OPS[name].items():
            val = item.get(key, default)
            try:
                if typ == "color":
                    val = parse_color(val)
                elif typ is str:
                    val = str(val)
                    if isinstance(hi, tuple) and val not in hi:
                        raise EditError(f"{name}.{key} debe ser uno de {', '.join(hi)}")
                else:
                    val = typ(float(val))
                    val = max(lo, min(hi, val)) if lo is not None else val
            except (TypeError, ValueError) as e:
                raise EditError(f"{name}.{key}: valor no valido ({item.get(key)!r})") from e
            clean[key] = val
        if name == "texto" and not clean["texto"].strip():
            raise EditError("texto: falta el texto")
        if name == "superponer" and not clean["imagen"]:
            raise EditError("superponer: falta la imagen (ruta en la carpeta de trabajo)")
        out.append(clean)
    if not out:
        raise EditError("no hay operaciones")
    if len(out) > 40:
        raise EditError("como maximo 40 operaciones por edicion")
    return out


def _ratio(s: str) -> float:
    m = re.fullmatch(r"\s*(\d+(?:[.,]\d+)?)\s*[:x/]\s*(\d+(?:[.,]\d+)?)\s*", s)
    if not m:
        raise EditError(f"proporcion no valida: {s!r} (p. ej. 4:5)")
    a, b = (float(g.replace(",", ".")) for g in m.groups())
    if a <= 0 or b <= 0:
        raise EditError("proporcion no valida")
    return a / b


def _place(pos: str, W: int, H: int, w: int, h: int) -> tuple[int, int]:
    m = round(0.03 * min(W, H))
    x = m if "izquierda" in pos else W - w - m if "derecha" in pos else (W - w) // 2
    y = m if pos.startswith("arriba") else H - h - m if pos.startswith("abajo") else (H - h) // 2
    return int(x), int(y)


def resolve(ops: list[dict], W: int, H: int, overlay_size=None) -> tuple[list[dict], tuple[int, int]]:
    """Convierte las operaciones en primitivas con medidas exactas para el tamano de partida W x H.

    overlay_size(ruta) -> (w, h) de una imagen a superponer. Devuelve (primitivas, tamano final)."""
    prim: list[dict] = []
    for op in ops:
        n = op["op"]
        if n == "redimensionar":
            if op["porcentaje"]:
                w, h = W * op["porcentaje"] / 100, H * op["porcentaje"] / 100
            elif op["ancho"] and op["alto"]:
                w, h = op["ancho"], op["alto"]
                if op["modo"] in ("ajustar", "rellenar"):
                    k = (min if op["modo"] == "ajustar" else max)(w / W, h / H)
                    w, h = W * k, H * k
            elif op["ancho"]:
                w, h = op["ancho"], H * op["ancho"] / W
            elif op["alto"]:
                w, h = W * op["alto"] / H, op["alto"]
            else:
                raise EditError("redimensionar: indica ancho, alto o porcentaje")
            w, h = max(1, round(w)), max(1, round(h))
            if (w, h) != (W, H):
                prim.append({"op": "escalar", "w": w, "h": h})
            W, H = w, h
        elif n == "recortar":
            x, y = min(op["x"], W - 1), min(op["y"], H - 1)
            w = min(op["ancho"] or W - x, W - x)
            h = min(op["alto"] or H - y, H - y)
            prim.append({"op": "recortar", "x": x, "y": y, "w": w, "h": h})
            W, H = w, h
        elif n == "encuadrar":
            r = _ratio(op["proporcion"])
            if W / H > r:
                w, h = max(1, round(H * r)), H
            else:
                w, h = W, max(1, round(W / r))
            pos = op["posicion"]
            x = 0 if "izquierda" in pos else W - w if "derecha" in pos else (W - w) // 2
            y = 0 if pos.startswith("arriba") else H - h if pos.startswith("abajo") else (H - h) // 2
            if (w, h) != (W, H):
                prim.append({"op": "recortar", "x": x, "y": y, "w": w, "h": h})
            W, H = w, h
            if op["ancho"] or op["alto"]:
                tw = op["ancho"] or round(op["alto"] * r)
                th = op["alto"] or round(op["ancho"] / r)
                if (tw, th) != (W, H):
                    prim.append({"op": "escalar", "w": tw, "h": th})
                W, H = tw, th
        elif n == "lienzo":
            w, h = max(W, op["ancho"] or W), max(H, op["alto"] or H)
            prim.append({"op": "lienzo", "w": w, "h": h, "x": (w - W) // 2, "y": (h - H) // 2, "color": op["color"]})
            W, H = w, h
        elif n == "borde":
            g = op["grosor"]
            prim.append({"op": "lienzo", "w": W + 2 * g, "h": H + 2 * g, "x": g, "y": g, "color": op["color"]})
            W, H = W + 2 * g, H + 2 * g
        elif n == "rotar":
            deg = op["grados"] % 360
            if deg == 0:
                continue
            prim.append({"op": "rotar", "grados": deg})
            if deg in (90, 270):
                W, H = H, W
            elif deg != 180:
                a = math.radians(deg)
                W, H = (round(abs(W * math.cos(a)) + abs(H * math.sin(a))),
                        round(abs(W * math.sin(a)) + abs(H * math.cos(a))))
        elif n == "texto":
            size = op["tamano"] or max(12, round(min(W, H) * 0.06))
            prim.append({**op, "tamano": size})
        elif n == "superponer":
            ow, oh = overlay_size(op["imagen"]) if overlay_size else (W, H)
            w = max(1, round(W * op["escala"] / 100))
            h = max(1, round(oh * w / ow))
            x, y = _place(op["posicion"], W, H, w, h)
            prim.append({**op, "w": w, "h": h, "x": x, "y": y})
        else:
            prim.append(dict(op))
        if W * H > 400_000_000:
            raise EditError("la imagen resultante seria demasiado grande")
    return prim, (W, H)


# ------------------------------------------------------------------ deteccion de GIMP
_PROBED: dict[tuple, dict | None] = {}
_VERSION = re.compile(r"version\s+(\d+)\.(\d+)(?:\.(\d+))?", re.I)


def _bundled_dir() -> Path:
    return paths.ROOT / "bundled" / "runtimes" / "gimp"


def _console_first(found: list[Path]) -> list[Path]:
    return sorted(found, key=lambda p: (0 if "console" in p.name else 1, -len(p.name)))


def candidates(config=None) -> list[tuple[list[str], str]]:
    """Ordenes posibles para lanzar GIMP: [(cmd, origen)], de la mas preferida a la menos."""
    out: list[tuple[list[str], str]] = []
    custom = (config.get("media.gimp_path", "") if config else "") or os.environ.get("NEXUS_GIMP", "")
    if custom:
        out.append((custom.split("|") if "|" in custom else [custom], "configuracion"))
    exe = ".exe" if os.name == "nt" else ""
    b = _bundled_dir()
    if b.is_dir():
        pats = ([f"**/bin/gimp-console*{exe}", f"**/bin/gimp-[0-9]*{exe}"] if os.name == "nt" else
                ["**/Contents/MacOS/gimp-console*", "**/Contents/MacOS/gimp"] if sys.platform == "darwin" else
                ["nexus-gimp-console", "**/usr/bin/gimp-console*", "**/bin/gimp-console*"])
        found = [p for pat in pats for p in b.glob(pat) if p.is_file()]
        out += [([str(p)], "incluido") for p in _console_first(found)]
    names = ["gimp-console-3.2", "gimp-console-3.0", "gimp-console-3", "gimp-console", "gimp-console-2.10",
             "gimp-3.2", "gimp-3.0", "gimp-3", "gimp", "gimp-2.10"]
    out += [([p], "sistema") for p in dict.fromkeys(shutil.which(n) for n in names) if p]
    if os.name == "nt":
        roots = [os.environ.get(v, "") for v in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA")]
        for r in filter(None, roots):
            for d in sorted(Path(r).glob("GIMP*/bin"), reverse=True) + sorted(Path(r).glob("Programs/GIMP*/bin"),
                                                                               reverse=True):
                out += [([str(p)], "sistema") for p in _console_first(list(d.glob("gimp-console*.exe")))]
    elif sys.platform == "darwin":
        for app in sorted(Path("/Applications").glob("GIMP*.app"), reverse=True) + \
                sorted((Path.home() / "Applications").glob("GIMP*.app"), reverse=True):
            out += [([str(p)], "sistema") for p in _console_first(list((app / "Contents/MacOS").glob("gimp*")))]
    else:
        for fp in ("/var/lib/flatpak/exports/bin/org.gimp.GIMP",
                   str(Path.home() / ".local/share/flatpak/exports/bin/org.gimp.GIMP")):
            if Path(fp).exists() and shutil.which("flatpak"):
                out += [(["flatpak", "run", f"--command={c}", "org.gimp.GIMP"], "flatpak")
                        for c in ("gimp-console-3.2", "gimp-console-3.0", "gimp-console-2.10")]
                break
        if Path("/snap/bin/gimp").exists():
            out.append((["/snap/bin/gimp"], "snap"))
    seen, uniq = set(), []
    for cmd, origin in out:
        if tuple(cmd) not in seen:
            seen.add(tuple(cmd))
            uniq.append((cmd, origin))
    return uniq


def _env(major: int) -> dict:
    env = dict(os.environ)
    prof = paths.sub("gimp", f"perfil-{major}")
    env["GIMP3_DIRECTORY" if major >= 3 else "GIMP2_DIRECTORY"] = str(prof)
    if os.name != "nt":
        env.setdefault("LC_ALL", "C.UTF-8")
    env.pop("PYTHONHOME", None)   # el Python de Nexus-Office no debe colarse en el Python-Fu de GIMP
    env.pop("PYTHONPATH", None)
    return env


def probe(cmd: list[str], origin: str = "") -> dict | None:
    key = tuple(cmd)
    if key in _PROBED:
        return _PROBED[key]
    info = None
    try:
        r = subprocess.run([*cmd, "--version"], capture_output=True, text=True, timeout=60, env=_env(3),
                           **_popen_flags())
        m = _VERSION.search((r.stdout or "") + (r.stderr or ""))
        if m:
            major, minor = int(m.group(1)), int(m.group(2))
            # el binario con interfaz de GIMP 3 no arranca en Linux sin pantalla: hace falta gimp-console
            gui_only = len(cmd) == 1 and bool(re.fullmatch(r"gimp(-[\d.]+)?", Path(cmd[0]).name)) and major >= 3 \
                and sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY")
            if major >= 2 and (major, minor) >= (2, 10) and not gui_only:
                info = {"cmd": cmd, "version": ".".join(g for g in m.groups() if g), "major": major,
                        "origen": origin, "interprete": "python-fu" if major >= 3 else "script-fu"}
    except (OSError, subprocess.SubprocessError) as e:
        L.debug("GIMP no arranca con %s: %s", cmd, e)
    _PROBED[key] = info
    return info


def find(config=None, refresh: bool = False) -> dict | None:
    if refresh:
        _PROBED.clear()
    for cmd, origin in candidates(config):
        info = probe(cmd, origin)
        if info:
            return info
    return None


def _popen_flags() -> dict:
    if os.name == "nt":
        return {"creationflags": 0x08000000}   # CREATE_NO_WINDOW
    return {"start_new_session": True}


def _run(cmd: list[str], major: int, timeout: float) -> tuple[int, str]:
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                         env=_env(major), cwd=str(paths.sub("gimp")), **_popen_flags())
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
            else:
                os.killpg(p.pid, 9)
        except OSError:
            p.kill()
        out, _ = p.communicate()
        raise EditError(f"GIMP no termino en {int(timeout)} s") from None
    return p.returncode, out or ""


def _clean_log(text: str) -> str:
    noise = ("Gtk-WARNING", "cannot open display", "gimp_wire_read", "resource name is invalid", "Welcome to GIMP",
             "using gegl copy", "GLib-GObject-CRITICAL", "batch command executed successfully", "Fontconfig",
             "writable data folder", "Failed to save data", "gimp_flush(): error: Broken pipe")
    return "\n".join(ln for ln in text.splitlines() if ln.strip() and not any(n in ln for n in noise))[-1500:]


# ------------------------------------------------------------------ Script-Fu (GIMP 2.10)
def _scm_str(s: str) -> str:
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _scm_col(c) -> str:
    return f"'({c[0]} {c[1]} {c[2]})"


SCM_PRELUDE = r"""
(define img (car (gimp-file-load RUN-NONINTERACTIVE nx-in nx-in)))
(define drw 0)
(define (nx-merge) (set! drw (car (gimp-image-merge-visible-layers img CLIP-TO-IMAGE))))
(nx-merge)
(gimp-context-push)
(gimp-context-set-interpolation INTERPOLATION-LOHALO)
(gimp-context-set-background '(255 255 255))
(define (nx-lienzo w h x y c)
  (gimp-image-resize img w h x y)
  (let ((bg (car (gimp-layer-new img w h RGBA-IMAGE "fondo" 100 LAYER-MODE-NORMAL))))
    (gimp-image-insert-layer img bg 0 -1)
    (gimp-image-lower-item-to-bottom img bg)
    (gimp-context-set-foreground c)
    (gimp-drawable-fill bg FILL-FOREGROUND)
    (nx-merge)))
(define (nx-vineta f)
  (let* ((W (car (gimp-image-width img))) (H (car (gimp-image-height img)))
         (l (car (gimp-layer-new img W H RGBA-IMAGE "vineta" f LAYER-MODE-NORMAL))))
    (gimp-image-insert-layer img l 0 -1)
    (gimp-image-select-ellipse img CHANNEL-OP-REPLACE (* W 0.04) (* H 0.04) (* W 0.92) (* H 0.92))
    (gimp-selection-feather img (* 0.4 (min W H)))
    (gimp-selection-invert img)
    (gimp-context-set-foreground '(0 0 0))
    (gimp-drawable-edit-fill l FILL-FOREGROUND)
    (gimp-selection-none img)
    (nx-merge)))
(define (nx-texto t s c pos op font)
  (let* ((W (car (gimp-image-width img))) (H (car (gimp-image-height img)))
         (e (gimp-text-get-extents-fontname t s PIXELS font)) (tw (car e)) (th (cadr e))
         (m (* 0.03 (min W H)))
         (x (cond ((member pos '("arriba-izquierda" "izquierda" "abajo-izquierda")) m)
                  ((member pos '("arriba-derecha" "derecha" "abajo-derecha")) (- W tw m))
                  (else (/ (- W tw) 2))))
         (y (cond ((member pos '("arriba-izquierda" "arriba" "arriba-derecha")) m)
                  ((member pos '("abajo-izquierda" "abajo" "abajo-derecha")) (- H th m))
                  (else (/ (- H th) 2)))))
    (gimp-context-set-foreground c)
    (let ((l (car (gimp-text-fontname img -1 x y t 0 TRUE s PIXELS font))))
      (gimp-layer-set-opacity l op))
    (nx-merge)))
"""


def script_fu(prim: list[dict], src: Path, out: Path, fmt: str, quality: int, alpha: bool = True) -> str:
    lines = [f"(define nx-in {_scm_str(src.as_posix())})", f"(define nx-out {_scm_str(out.as_posix())})",
             SCM_PRELUDE]
    R = "RUN-NONINTERACTIVE"
    for p in prim:
        n = p["op"]
        if n == "escalar":
            lines.append(f"(gimp-image-scale img {p['w']} {p['h']})")
        elif n == "recortar":
            lines.append(f"(gimp-image-crop img {p['w']} {p['h']} {p['x']} {p['y']})")
        elif n == "lienzo":
            lines.append(f"(nx-lienzo {p['w']} {p['h']} {p['x']} {p['y']} {_scm_col(p['color'])})")
        elif n == "rotar":
            d = p["grados"]
            if d in (90, 180, 270):
                lines.append(f"(gimp-image-rotate img ROTATE-{int(d)})")
            else:
                lines.append(f"(gimp-item-transform-rotate drw {math.radians(d):.6f} TRUE 0 0)"
                             "(gimp-image-resize-to-layers img)")
        elif n == "voltear":
            lines.append(f"(gimp-image-flip img ORIENTATION-{p['direccion'].upper()})")
        elif n == "brillo_contraste":
            lines.append(f"(gimp-drawable-brightness-contrast drw {p['brillo'] / 100:.4f} {p['contraste'] / 100:.4f})")
        elif n == "niveles_auto":
            lines.append(f"(plug-in-autostretch-hsv {R} img drw)")
        elif n == "balance_blancos":
            lines.append("(gimp-drawable-levels-stretch drw)")
        elif n == "saturacion":
            lines.append(f"(gimp-drawable-hue-saturation drw HUE-RANGE-ALL 0 0 {p['cantidad']:.2f} 0)")
        elif n == "tono":
            lines.append(f"(gimp-drawable-hue-saturation drw HUE-RANGE-ALL {p['grados']:.2f} 0 0 0)")
        elif n == "gamma":
            lines.append(f"(gimp-drawable-levels drw HISTOGRAM-VALUE 0 1 TRUE {p['valor']:.3f} 0 1 TRUE)")
        elif n == "blanco_negro":
            lines.append("(gimp-drawable-desaturate drw DESATURATE-LUMINANCE)")
        elif n == "sepia":
            lines.append("(gimp-drawable-colorize-hsl drw 32 28 0)")
        elif n == "enfocar":
            lines.append(f"(plug-in-unsharp-mask {R} img drw {p['radio']:.2f} {p['cantidad']:.3f} {p['umbral']})")
        elif n == "desenfocar":
            lines.append(f"(plug-in-gauss {R} img drw {p['radio']:.2f} {p['radio']:.2f} 0)")
        elif n == "reducir_ruido":
            lines.append(f"(plug-in-despeckle {R} img drw {max(1, round(p['fuerza'] / 2))} 1 7 248)")
        elif n == "invertir":
            lines.append("(gimp-drawable-invert drw FALSE)")
        elif n == "posterizar":
            lines.append(f"(gimp-drawable-posterize drw {p['niveles']})")
        elif n == "vineta":
            lines.append(f"(nx-vineta {p['fuerza']:.1f})")
        elif n == "texto":
            lines.append(f"(nx-texto {_scm_str(p['texto'])} {p['tamano']} {_scm_col(p['color'])} "
                         f"{_scm_str(p['posicion'])} {p['opacidad']:.1f} {_scm_str(p['fuente'])})")
        elif n == "superponer":
            lines.append(f"(let ((l (car (gimp-file-load-layer {R} img {_scm_str(Path(p['ruta']).as_posix())}))))"
                         f"(gimp-image-insert-layer img l 0 -1)(gimp-layer-scale l {p['w']} {p['h']} FALSE)"
                         f"(gimp-layer-set-offsets l {p['x']} {p['y']})(gimp-layer-set-opacity l {p['opacidad']:.1f})"
                         "(nx-merge))")
        elif n == "ppp":
            lines.append(f"(gimp-image-set-resolution img {p['valor']} {p['valor']})")
        elif n == "aplanar":
            lines.append(f"(gimp-context-set-background {_scm_col(p['color'])})"
                         "(set! drw (car (gimp-image-flatten img)))(gimp-context-set-background '(255 255 255))")
    if fmt in ("jpg", "bmp") or not alpha:
        lines.append("(set! drw (car (gimp-image-flatten img)))")
    else:
        lines.append("(nx-merge)")
    if fmt == "jpg":
        lines.append(f"(file-jpeg-save {R} img drw nx-out nx-out {quality / 100:.2f} 0 1 1 \"\" 2 1 0 2)")
    elif fmt == "png":
        lines.append(f"(file-png-save-defaults {R} img drw nx-out nx-out)")
    else:
        lines.append(f"(gimp-file-save {R} img drw nx-out nx-out)")
    lines.append("(gimp-image-delete img)(gimp-context-pop)")
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ Python-Fu (GIMP 3)
PY3 = r'''
import json, math, sys, traceback
import gi
gi.require_version("Gimp", "3.0")
gi.require_version("Gegl", "0.4")
from gi.repository import Gimp, Gio, Gegl
J = json.loads(JOB)
NONI = Gimp.RunMode.NONINTERACTIVE


def col(c):
    return Gegl.Color.new("#%02x%02x%02x" % tuple(c))


def merge():
    return img.merge_visible_layers(Gimp.MergeType.CLIP_TO_IMAGE)


def filt(d, op, props):
    f = Gimp.DrawableFilter.new(d, op, "")
    cfg = f.get_config()
    for k, v in props.items():
        cfg.set_property(k, v)
    f.update()
    d.merge_filter(f)


def place(pos, W, H, w, h):
    m = round(0.03 * min(W, H))
    x = m if "izquierda" in pos else W - w - m if "derecha" in pos else (W - w) // 2
    y = m if pos.startswith("arriba") else H - h - m if pos.startswith("abajo") else (H - h) // 2
    return int(x), int(y)


def lienzo(w, h, x, y, c):
    img.resize(w, h, x, y)
    bg = Gimp.Layer.new(img, "fondo", w, h, Gimp.ImageType.RGBA_IMAGE, 100, Gimp.LayerMode.NORMAL)
    img.insert_layer(bg, None, len(img.get_layers()))
    Gimp.context_set_foreground(col(c))
    bg.fill(Gimp.FillType.FOREGROUND)
    return merge()


def vineta(f):
    W, H = img.get_width(), img.get_height()
    lay = Gimp.Layer.new(img, "vineta", W, H, Gimp.ImageType.RGBA_IMAGE, f, Gimp.LayerMode.NORMAL)
    img.insert_layer(lay, None, 0)
    img.select_ellipse(Gimp.ChannelOps.REPLACE, W * 0.04, H * 0.04, W * 0.92, H * 0.92)
    Gimp.Selection.feather(img, 0.4 * min(W, H))
    Gimp.Selection.invert(img)
    Gimp.context_set_foreground(col((0, 0, 0)))
    lay.edit_fill(Gimp.FillType.FOREGROUND)
    Gimp.Selection.none(img)
    return merge()


def texto(p):
    W, H = img.get_width(), img.get_height()
    font = Gimp.Font.get_by_name(p["fuente"]) or Gimp.Font.get_by_name("Sans-serif") or Gimp.context_get_font()
    ext = Gimp.text_get_extents_font(p["texto"], p["tamano"], font)
    vals = [v for v in (ext if isinstance(ext, tuple) else (ext,)) if isinstance(v, int) and not isinstance(v, bool)]
    tw, th = (vals[0], vals[1]) if len(vals) >= 2 else (p["tamano"] * len(p["texto"]) // 2, p["tamano"])
    x, y = place(p["posicion"], W, H, tw, th)
    Gimp.context_set_foreground(col(p["color"]))
    lay = Gimp.text_font(img, None, x, y, p["texto"], 0, True, p["tamano"], font)
    lay.set_opacity(p["opacidad"])
    return merge()


def export(d):
    out = Gio.File.new_for_path(J["out"])
    if J["fmt"] == "jpg":
        try:
            proc = Gimp.get_pdb().lookup_procedure("file-jpeg-export")
            cfg = proc.create_config()
            for k, v in (("run-mode", NONI), ("image", img), ("file", out), ("quality", J["quality"] / 100.0)):
                cfg.set_property(k, v)
            res = proc.run(cfg)
            if res.index(0) == Gimp.PDBStatusType.SUCCESS:
                return
        except Exception:
            traceback.print_exc()
    if not Gimp.file_save(NONI, img, out, None):
        raise RuntimeError("no se pudo exportar")


img = Gimp.file_load(NONI, Gio.File.new_for_path(J["in"]))
try:
    Gimp.context_push()
    Gimp.context_set_interpolation(Gimp.InterpolationType.LOHALO)
    Gimp.context_set_background(col((255, 255, 255)))
    d = merge()
    for p in J["ops"]:
        n = p["op"]
        if n == "escalar":
            img.scale(p["w"], p["h"])
        elif n == "recortar":
            img.crop(p["w"], p["h"], p["x"], p["y"])
        elif n == "lienzo":
            d = lienzo(p["w"], p["h"], p["x"], p["y"], p["color"])
        elif n == "rotar":
            g = p["grados"]
            if g in (90, 180, 270):
                img.rotate(getattr(Gimp.RotationType, "DEGREES%d" % g))
            else:
                d.transform_rotate(math.radians(g), True, 0, 0)
                img.resize_to_layers()
        elif n == "voltear":
            img.flip(Gimp.OrientationType.HORIZONTAL if p["direccion"] == "horizontal" else Gimp.OrientationType.VERTICAL)
        elif n == "brillo_contraste":
            d.brightness_contrast(p["brillo"] / 100.0, p["contraste"] / 100.0)
        elif n == "niveles_auto":
            filt(d, "gegl:stretch-contrast", {"keep-colors": True})
        elif n == "balance_blancos":
            d.levels_stretch()
        elif n == "saturacion":
            d.hue_saturation(Gimp.HueRange.ALL, 0, 0, p["cantidad"], 0)
        elif n == "tono":
            d.hue_saturation(Gimp.HueRange.ALL, p["grados"], 0, 0, 0)
        elif n == "gamma":
            d.levels(Gimp.HistogramChannel.VALUE, 0.0, 1.0, True, p["valor"], 0.0, 1.0, True)
        elif n == "blanco_negro":
            d.desaturate(Gimp.DesaturateMode.LUMINANCE)
        elif n == "sepia":
            d.colorize_hsl(32, 28, 0)
        elif n == "enfocar":
            filt(d, "gegl:unsharp-mask", {"std-dev": p["radio"], "scale": p["cantidad"], "threshold": p["umbral"] / 255.0})
        elif n == "desenfocar":
            s = p["radio"] / 3.33
            filt(d, "gegl:gaussian-blur", {"std-dev-x": s, "std-dev-y": s})
        elif n == "reducir_ruido":
            filt(d, "gegl:noise-reduction", {"iterations": min(8, p["fuerza"])})
        elif n == "invertir":
            d.invert(False)
        elif n == "posterizar":
            d.posterize(p["niveles"])
        elif n == "vineta":
            d = vineta(p["fuerza"])
        elif n == "texto":
            d = texto(p)
        elif n == "superponer":
            lay = Gimp.file_load_layer(NONI, img, Gio.File.new_for_path(p["ruta"]))
            img.insert_layer(lay, None, 0)
            lay.scale(p["w"], p["h"], False)
            lay.set_offsets(p["x"], p["y"])
            lay.set_opacity(p["opacidad"])
            d = merge()
        elif n == "ppp":
            img.set_resolution(p["valor"], p["valor"])
        elif n == "aplanar":
            Gimp.context_set_background(col(p["color"]))
            d = img.flatten()
            Gimp.context_set_background(col((255, 255, 255)))
    d = img.flatten() if J["fmt"] in ("jpg", "bmp") or not J["alpha"] else merge()
    export(d)
    print("NEXUS-OK")
except Exception:
    print("NEXUS-ERROR")
    traceback.print_exc()
finally:
    img.delete()
    Gimp.context_pop()
'''


def python_fu(prim: list[dict], src: Path, out: Path, fmt: str, quality: int, alpha: bool = True) -> str:
    job = {"in": str(src), "out": str(out), "fmt": fmt, "quality": quality, "alpha": alpha,
           "ops": [{**p, "color": list(p["color"])} if "color" in p else p for p in prim]}
    return f"JOB = {json.dumps(json.dumps(job, ensure_ascii=False))}\n" + PY3


# ------------------------------------------------------------------ Pillow (sin GIMP)
def _font(name: str, size: int):
    from PIL import ImageFont
    cands = [name] if name and name.lower() not in ("sans-serif", "sans", "serif") else []
    cands += ["DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "arial.ttf", "Arial.ttf",
              "C:/Windows/Fonts/arial.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
              "/System/Library/Fonts/Helvetica.ttc", "/system/fonts/Roboto-Regular.ttf", "Roboto-Regular.ttf"]
    for c in cands:
        try:
            return ImageFont.truetype(c, size)
        except OSError:
            continue
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


def pil_apply(img, prim: list[dict], dpi_holder: dict):
    from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps
    img = img.convert("RGBA") if img.mode in ("RGBA", "LA", "P", "PA") or "transparency" in img.info \
        else img.convert("RGB")

    def rgb(fn):
        nonlocal img
        if img.mode == "RGBA":
            a = img.getchannel("A")
            img = fn(img.convert("RGB")).convert("RGB")
            img.putalpha(a)
        else:
            img = fn(img)

    for p in prim:
        n = p["op"]
        if n == "escalar":
            img = img.resize((p["w"], p["h"]), Image.LANCZOS)
        elif n == "recortar":
            img = img.crop((p["x"], p["y"], p["x"] + p["w"], p["y"] + p["h"]))
        elif n == "lienzo":
            bg = Image.new(img.mode, (p["w"], p["h"]), tuple(p["color"]) + ((255,) if img.mode == "RGBA" else ()))
            bg.paste(img, (p["x"], p["y"]), img if img.mode == "RGBA" else None)
            img = bg
        elif n == "rotar":
            if p["grados"] not in (90, 180, 270):
                img = img.convert("RGBA")   # como en GIMP: las esquinas nuevas quedan transparentes
            img = img.rotate(-p["grados"], Image.BICUBIC, expand=True, fillcolor=(255, 255, 255, 0)
                             if img.mode == "RGBA" else (255, 255, 255))
        elif n == "voltear":
            img = ImageOps.mirror(img) if p["direccion"] == "horizontal" else ImageOps.flip(img)
        elif n == "brillo_contraste":
            rgb(lambda i: ImageEnhance.Contrast(ImageEnhance.Brightness(i).enhance(1 + p["brillo"] / 100))
                .enhance(1 + p["contraste"] / 100))
        elif n == "niveles_auto":
            def stretch(i):
                lo, hi = _percentiles(i.convert("L"), 0.005)
                if hi - lo < 2:
                    return i
                k = 255.0 / (hi - lo)
                return i.point(lambda v: max(0, min(255, round((v - lo) * k))))
            rgb(stretch)
        elif n == "balance_blancos":
            rgb(lambda i: ImageOps.autocontrast(i, cutoff=0.5, preserve_tone=False))
        elif n == "saturacion":
            rgb(lambda i: ImageEnhance.Color(i).enhance(1 + p["cantidad"] / 100))
        elif n == "tono":
            def hue(i):
                h, s, v = i.convert("HSV").split()
                shift = round(p["grados"] / 360 * 256)
                return Image.merge("HSV", (h.point(lambda x: (x + shift) % 256), s, v)).convert("RGB")
            rgb(hue)
        elif n == "gamma":
            g = 1.0 / p["valor"]
            rgb(lambda i: i.point(lambda v: round(255 * (v / 255) ** g)))
        elif n == "blanco_negro":
            rgb(lambda i: ImageOps.grayscale(i).convert("RGB"))
        elif n == "sepia":
            rgb(lambda i: ImageOps.colorize(ImageOps.grayscale(i), "#1e140a", "#fff2dc", mid="#a07850"))
        elif n == "enfocar":
            rgb(lambda i: i.filter(ImageFilter.UnsharpMask(p["radio"], round(p["cantidad"] * 100), p["umbral"])))
        elif n == "desenfocar":
            rgb(lambda i: i.filter(ImageFilter.GaussianBlur(p["radio"] / 3.33)))
        elif n == "reducir_ruido":
            size = 3 if p["fuerza"] <= 5 else 5
            rgb(lambda i: i.filter(ImageFilter.MedianFilter(size)))
        elif n == "invertir":
            rgb(ImageOps.invert)
        elif n == "posterizar":
            step = 255 / (p["niveles"] - 1)
            rgb(lambda i: i.point(lambda v: round(round(v / step) * step)))
        elif n == "vineta":
            W, H = img.size
            mask = Image.new("L", (W, H), 255)
            ImageDraw.Draw(mask).ellipse((W * 0.04, H * 0.04, W * 0.96, H * 0.96), fill=0)
            mask = mask.filter(ImageFilter.GaussianBlur(0.4 * min(W, H) / 2))
            mask = mask.point(lambda v: round(v * p["fuerza"] / 100))
            dark = Image.new(img.mode, img.size, (0, 0, 0, 255) if img.mode == "RGBA" else (0, 0, 0))
            img = Image.composite(dark, img, mask)
        elif n == "texto":
            W, H = img.size
            font = _font(p["fuente"], p["tamano"])
            layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
            draw = ImageDraw.Draw(layer)
            box = draw.textbbox((0, 0), p["texto"], font=font)
            x, y = _place(p["posicion"], W, H, box[2] - box[0], box[3] - box[1])
            draw.text((x - box[0], y - box[1]), p["texto"], font=font,
                      fill=tuple(p["color"]) + (round(255 * p["opacidad"] / 100),))
            img = Image.alpha_composite(img.convert("RGBA"), layer).convert(img.mode)
        elif n == "superponer":
            ov = Image.open(p["ruta"]).convert("RGBA").resize((p["w"], p["h"]), Image.LANCZOS)
            if p["opacidad"] < 100:
                ov.putalpha(ov.getchannel("A").point(lambda v: round(v * p["opacidad"] / 100)))
            base = img.convert("RGBA")
            base.alpha_composite(ov, (max(0, p["x"]), max(0, p["y"])))
            img = base.convert(img.mode)
        elif n == "ppp":
            dpi_holder["dpi"] = (p["valor"], p["valor"])
        elif n == "aplanar":
            if img.mode == "RGBA":
                bg = Image.new("RGB", img.size, tuple(p["color"]))
                bg.paste(img, mask=img.getchannel("A"))
                img = bg
    return img


def _percentiles(gray, frac: float) -> tuple[int, int]:
    hist = gray.histogram()
    total = sum(hist)
    cut = total * frac
    lo, acc = 0, 0
    for i, c in enumerate(hist):
        acc += c
        if acc > cut:
            lo = i
            break
    hi, acc = 255, 0
    for i in range(255, -1, -1):
        acc += hist[i]
        if acc > cut:
            hi = i
            break
    return lo, hi


def pil_edit(prim: list[dict], src: Path, out: Path, fmt: str, quality: int, alpha: bool = True) -> None:
    from PIL import Image, ImageOps
    with Image.open(src) as im:
        im.load()
        dpi = {"dpi": im.info.get("dpi")}
        img = ImageOps.exif_transpose(im)
    img = pil_apply(img, prim, dpi)
    kw: dict = {}
    if dpi.get("dpi"):
        kw["dpi"] = tuple(float(v) for v in dpi["dpi"])
    if (fmt in ("jpg", "bmp") or not alpha) and img.mode != "RGB":
        bg = Image.new("RGB", img.size, (255, 255, 255))
        bg.paste(img, mask=img.getchannel("A") if img.mode == "RGBA" else None)
        img = bg
    if fmt == "jpg":
        kw.update(quality=quality, optimize=True, progressive=True, subsampling=0 if quality >= 90 else 2)
    elif fmt == "webp":
        kw.update(quality=quality, method=5)
    elif fmt == "png":
        kw.update(optimize=True)
    elif fmt == "tif":
        kw.update(compression="tiff_lzw")
    elif fmt == "xcf":
        raise EditError("el formato XCF (proyecto de GIMP) necesita GIMP")
    img.save(out, {"jpg": "JPEG", "tif": "TIFF"}.get(fmt, fmt.upper()), **kw)


# ------------------------------------------------------------------ motor
class GimpEngine:
    """Edicion por lotes de operaciones: GIMP si esta disponible (``media.gimp``: auto | gimp | pil)."""

    def __init__(self, config) -> None:
        self.config = config
        self._info: dict | None = None
        self._checked = 0.0

    def info(self, refresh: bool = False) -> dict | None:
        if refresh or not self._checked or time.time() - self._checked > 600:
            self._info, self._checked = find(self.config, refresh=refresh), time.time()
        return self._info

    def status(self, refresh: bool = False) -> dict:
        g = self.info(refresh)
        mode = self.config.get("media.gimp", "auto") if self.config else "auto"
        out = {"modo": mode, "gimp": bool(g), "motor": "gimp" if g and mode != "pil" else "pillow",
               "operaciones": {k: OP_HELP.get(k, "") for k in OPS},
               "recetas": {k: v["descripcion"] for k, v in RECIPES.items()},
               "formatos": sorted(set(FORMATS.values()))}
        if g:
            out.update(version=g["version"], origen=g["origen"], interprete=g["interprete"])
        else:
            out["nota"] = ("GIMP no esta disponible en este equipo: las ediciones se hacen con Pillow (mismas "
                           "operaciones; el formato XCF y los scripts de GIMP no estan disponibles). Los instaladores "
                           "de escritorio de Nexus-Office lo traen incluido; en Android no existe GIMP.")
        return out

    def size_of(self, src: Path) -> tuple[int, int, bool]:
        """(ancho, alto, tiene_transparencia) ya girada segun su EXIF."""
        try:
            from PIL import Image
            with Image.open(src) as im:
                w, h = im.size
                if (im.getexif() or {}).get(0x0112, 1) in (5, 6, 7, 8):
                    w, h = h, w
                return w, h, im.mode in ("RGBA", "LA", "PA") or "transparency" in im.info
        except Exception:  # noqa: BLE001
            g = self.info()
            if not g:
                raise EditError(f"no se puede leer la imagen {src.name}") from None
            tmp = paths.sub("gimp", "trabajo") / f"size_{os.getpid()}_{int(time.time() * 1000)}.png"
            try:
                self._gimp(g, [], src, tmp, "png", 90, 120)
                with Image.open(tmp) as im:
                    return (*im.size, im.mode == "RGBA")
            finally:
                tmp.unlink(missing_ok=True)

    def edit(self, src: Path, ops, out: Path, quality: int = 90, engine: str = "", overlay_resolver=None,
             timeout: float = 300) -> dict:
        ops = normalize(ops)
        fmt = FORMATS.get(out.suffix.lower().lstrip("."), "")
        if not fmt:
            raise EditError(f"formato de salida no admitido: {out.suffix}")
        quality = max(10, min(100, int(quality)))
        overlays = {}
        for o in ops:
            if o["op"] == "superponer":
                p = overlay_resolver(o["imagen"]) if overlay_resolver else Path(o["imagen"])
                overlays[o["imagen"]] = (p, self.size_of(p))
        W, H, alpha = self.size_of(src)
        prim, final = resolve(ops, W, H, lambda k: overlays[k][1][:2])
        # la transparencia se conserva si la imagen ya la tenia o si un giro deja esquinas vacias
        alpha = alpha or any(p["op"] == "rotar" and p["grados"] not in (90, 180, 270) for p in prim)
        for p in prim:
            if p["op"] == "superponer":
                p["ruta"] = str(overlays[p["imagen"]][0])
        mode = engine or (self.config.get("media.gimp", "auto") if self.config else "auto")
        g = self.info() if mode != "pil" else None
        if mode == "gimp" and not g:
            raise EditError("GIMP no esta disponible en este equipo")
        out.parent.mkdir(parents=True, exist_ok=True)
        note = ""
        if g:
            try:
                t0 = time.time()
                self._gimp(g, prim, src, out, fmt, quality, timeout, alpha)
                return {"motor": f"GIMP {g['version']}", "tamano": list(final), "segundos": round(time.time() - t0, 1),
                        "operaciones": [o["op"] for o in ops]}
            except EditError as e:
                if mode == "gimp" or fmt == "xcf":
                    raise
                note = f"GIMP fallo ({str(e)[:300]}); se ha hecho con Pillow"
                L.warning("%s", note)
        pil_edit(prim, src, out, fmt, quality, alpha)
        res = {"motor": "Pillow", "tamano": list(final), "operaciones": [o["op"] for o in ops]}
        if note:
            res["nota"] = note
        return res

    def _gimp(self, g: dict, prim: list[dict], src: Path, out: Path, fmt: str, quality: int, timeout: float,
              alpha: bool = True) -> None:
        work = paths.sub("gimp", "trabajo")
        stamp = f"{os.getpid()}_{int(time.time() * 1000)}"
        tmp_out = out.with_name(f".{stamp}{out.suffix}")
        needs_fonts = any(p["op"] == "texto" for p in prim)
        if g["major"] >= 3:
            script = work / f"edit_{stamp}.py"
            script.write_text(python_fu(prim, src, tmp_out, fmt, quality, alpha), encoding="utf-8")
            cmd = [*g["cmd"], "-i", "-d", *([] if needs_fonts else ["-f"]), "--quit", "--batch-interpreter",
                   "python-fu-eval", "-b", f"exec(open({str(script)!r}, encoding='utf-8').read())"]
        else:
            script = work / f"edit_{stamp}.scm"
            script.write_text(script_fu(prim, src, tmp_out, fmt, quality, alpha), encoding="utf-8")
            cmd = [*g["cmd"], "-i", "-d", *([] if needs_fonts else ["-f"]), "--batch-interpreter",
                   "plug-in-script-fu-eval", "-b", f"(load {_scm_str(script.as_posix())})", "-b", "(gimp-quit 0)"]
        try:
            code, text = _run(cmd, g["major"], timeout)
            failed = "NEXUS-ERROR" in text or "execution error" in text
            if failed or not tmp_out.exists() or tmp_out.stat().st_size == 0:
                raise EditError(_clean_log(text) or f"GIMP termino con codigo {code} sin generar la imagen")
            os.replace(tmp_out, out)
        finally:
            script.unlink(missing_ok=True)
            tmp_out.unlink(missing_ok=True)

    def run_script(self, code: str, src: Path | None, out: Path | None, timeout: float = 300) -> dict:
        """Script libre del agente: Python-Fu en GIMP 3 o Script-Fu en GIMP 2.10, con ENTRADA y SALIDA definidas."""
        g = self.info()
        if not g:
            raise EditError("GIMP no esta disponible en este equipo: usa imagen_editar (funciona sin GIMP)")
        work = paths.sub("gimp", "trabajo")
        stamp = f"{os.getpid()}_{int(time.time() * 1000)}"
        if g["major"] >= 3:
            script = work / f"script_{stamp}.py"
            head = ("import gi\ngi.require_version('Gimp', '3.0')\ngi.require_version('Gegl', '0.4')\n"
                    "from gi.repository import Gimp, Gio, Gegl\n"
                    f"ENTRADA = {str(src) if src else ''!r}\nSALIDA = {str(out) if out else ''!r}\n")
            script.write_text(head + code + "\nprint('NEXUS-OK')\n", encoding="utf-8")
            cmd = [*g["cmd"], "-i", "-d", "--quit", "--batch-interpreter", "python-fu-eval", "-b",
                   f"exec(open({str(script)!r}, encoding='utf-8').read())"]
        else:
            script = work / f"script_{stamp}.scm"
            head = (f"(define ENTRADA {_scm_str(src.as_posix() if src else '')})\n"
                    f"(define SALIDA {_scm_str(out.as_posix() if out else '')})\n")
            script.write_text(head + code + "\n", encoding="utf-8")
            cmd = [*g["cmd"], "-i", "-d", "--batch-interpreter", "plug-in-script-fu-eval", "-b",
                   f"(load {_scm_str(script.as_posix())})", "-b", "(gimp-quit 0)"]
        try:
            rc, text = _run(cmd, g["major"], timeout)
        finally:
            script.unlink(missing_ok=True)
        ok = "execution error" not in text and "Traceback" not in text and rc == 0
        return {"ok": ok, "motor": f"GIMP {g['version']}", "interprete": g["interprete"],
                "salida": _clean_log(text), "fichero": bool(out and out.exists())}
