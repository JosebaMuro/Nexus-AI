"""Edicion profesional de imagenes con GIMP (nexus/media/gimp.py) sobre la carpeta de trabajo de los agentes.

Las imagenes se leen y se guardan en data/sandbox (la misma carpeta que file_list/file_read/file_write): las
adjuntas en el chat estan en adjuntos/, las editadas van a imagenes/. Tambien se aceptan las imagenes generadas
(/outputs/images/...) y «ultima» para la imagen mas reciente. Sin GIMP el trabajo se hace con Pillow.
"""
from __future__ import annotations

import re
import shutil
import time
from pathlib import Path

from ... import paths
from ...media import gimp

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp", ".gif", ".xcf", ".psd"}
PREVIEW_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif"}
LAST = {"", "ultima", "última", "last", "la ultima", "la última"}


def engine(services) -> gimp.GimpEngine:
    eng = getattr(services, "gimp", None)
    if eng is None:
        eng = gimp.GimpEngine(services.config)
        try:
            services.gimp = eng
        except AttributeError:
            pass
    return eng


def _inside(p: Path, root: Path) -> bool:
    return p.resolve().is_relative_to(root.resolve())


def latest_image() -> Path | None:
    roots = [paths.sandbox_dir() / "imagenes", paths.sandbox_dir() / "adjuntos", paths.sub("outputs", "images")]
    files = [f for r in roots if r.is_dir() for f in r.rglob("*") if f.is_file() and f.suffix.lower() in IMAGE_EXT
             and not f.name.startswith(".")]
    return max(files, key=lambda f: f.stat().st_mtime) if files else None


def resolve_input(ref: str) -> Path:
    """Ruta de la carpeta de trabajo, URL /outputs/... de una imagen generada, o «ultima»."""
    ref = (ref or "").strip().strip('"').replace("\\", "/")
    if ref.lower() in LAST:
        p = latest_image()
        if not p:
            raise gimp.EditError("no hay ninguna imagen en la carpeta de trabajo")
        return p
    if ref.startswith("/outputs/") or ref.startswith("outputs/"):
        root = paths.outputs_dir()
        p = (root / ref.split("outputs/", 1)[1]).resolve()
    else:
        root = paths.sandbox_dir()
        p = (root / ref.lstrip("/")).resolve()
    if not _inside(p, root):
        raise gimp.EditError("la imagen tiene que estar en la carpeta de trabajo")
    if not p.is_file():
        raise gimp.EditError(f"no existe la imagen {ref} (usa file_list para ver la carpeta de trabajo)")
    if p.suffix.lower() not in IMAGE_EXT:
        raise gimp.EditError(f"{p.name} no es una imagen")
    return p


def workspace_path(p: Path) -> str:
    try:
        return p.resolve().relative_to(paths.sandbox_dir().resolve()).as_posix()
    except ValueError:
        return "/outputs/" + p.resolve().relative_to(paths.outputs_dir().resolve()).as_posix()


def output_path(src: Path, salida: str, fmt: str) -> Path:
    root = paths.sandbox_dir()
    salida = (salida or "").strip().replace("\\", "/").lstrip("/")
    if salida:
        rel = salida if "/" in salida else f"imagenes/{salida}"
    else:
        rel = f"imagenes/{src.stem}_editada"
    rel = re.sub(r"[^\w./ -]+", "_", rel)
    p = (root / rel).resolve()
    if not _inside(p, root):
        raise gimp.EditError("la salida tiene que quedar en la carpeta de trabajo")
    ext = gimp.FORMATS.get(p.suffix.lower().lstrip("."))
    if fmt or not ext:
        p = p.with_suffix("." + (gimp.FORMATS.get(fmt, "") or ext or "png").replace("jpg", "jpg"))
    base, n = p, 2
    while p.exists():   # nunca se pisa un fichero (tampoco el original)
        p = base.with_name(f"{base.stem}_{n}{base.suffix}")
        n += 1
    p.parent.mkdir(parents=True, exist_ok=True)
    return p


def _preview(p: Path) -> str | None:
    if p.suffix.lower() not in PREVIEW_EXT:
        return None
    dst = paths.sub("outputs", "images") / f"{int(time.time())}_{p.name}"
    shutil.copy2(p, dst)
    return f"/outputs/images/{dst.name}"


def imagen_editar(args, ctx):
    src = resolve_input(args.get("imagen", ""))
    ops = args.get("operaciones") or []
    fmt = (args.get("formato") or "").lower()
    quality = args.get("calidad") or 90
    receta = (args.get("receta") or "").strip()
    if receta:
        if receta not in gimp.RECIPES:
            raise gimp.EditError(f"receta desconocida: {receta}. Disponibles: {', '.join(gimp.RECIPES)}")
        r = gimp.RECIPES[receta]
        ops = list(r["ops"]) + list(ops)
        fmt = fmt or r.get("formato", "")
        quality = args.get("calidad") or r.get("calidad", 90)
    if not fmt and src.suffix.lower() in (".xcf", ".psd", ".gif"):
        fmt = "png"
    out = output_path(src, args.get("salida", ""), fmt)
    if ctx and ctx.emit:
        ctx.emit({"type": "progress", "text": f"Editando {src.name}"})
    res = engine(ctx.services).edit(src, ops, out, quality=int(quality),
                                    overlay_resolver=lambda ref: resolve_input(ref))
    res.update(original=workspace_path(src), ruta=workspace_path(out), bytes=out.stat().st_size)
    url = _preview(out)
    if url:
        res["url"] = url
        if ctx and ctx.emit:
            ctx.emit({"type": "media", "kind": "image", "url": url, "engine": res["motor"]})
    return res


def imagen_info(args, ctx):
    from PIL import Image
    from ...media.image import ImageEngine
    src = resolve_input(args.get("imagen", ""))
    out = {"imagen": workspace_path(src), "bytes": src.stat().st_size}
    try:
        with Image.open(src) as im:
            out.update(formato=im.format, modo=im.mode, ancho=im.width, alto=im.height,
                       transparencia=im.mode in ("RGBA", "LA", "PA") or "transparency" in im.info)
            if im.info.get("dpi"):
                dpi = [round(float(v)) for v in im.info["dpi"]]
                out["ppp"] = dpi
                out["tamano_impresion_cm"] = [round(im.width / dpi[0] * 2.54, 1), round(im.height / dpi[1] * 2.54, 1)]
            exif = im.getexif() or {}
            names = {0x010F: "fabricante", 0x0110: "camara", 0x0132: "fecha", 0x0112: "orientacion",
                     0x013B: "autor", 0x8298: "copyright"}
            meta = {v: str(exif[k])[:80] for k, v in names.items() if k in exif}
            if meta:
                out["exif"] = meta
            stats = ImageEngine.describe_stats(im.convert("RGB"))
            out["analisis"] = {k: stats[k] for k in ("dominant_colors", "brightness", "summary") if k in stats}
    except Exception as e:  # noqa: BLE001
        out["nota"] = f"Pillow no lee este formato ({e}); GIMP si puede editarlo"
    return out


def gimp_estado(args, ctx):
    return engine(ctx.services).status(refresh=bool(args.get("buscar_de_nuevo")))


def gimp_script(args, ctx):
    src = resolve_input(args["imagen"]) if args.get("imagen") else None
    out = output_path(src or Path("script"), args["salida"], "") if args.get("salida") else None
    res = engine(ctx.services).run_script(args["codigo"], src, out, timeout=float(args.get("segundos", 300)))
    if out and out.exists():
        res["ruta"] = workspace_path(out)
        url = _preview(out)
        if url:
            res["url"] = url
            if ctx and ctx.emit:
                ctx.emit({"type": "media", "kind": "image", "url": url, "engine": res["motor"]})
    return res


_IMG = {"type": "string", "description": "imagen de la carpeta de trabajo (p. ej. adjuntos/foto.jpg, "
        "imagenes/logo.png), una URL /outputs/images/... o «ultima»"}
_OPS_DOC = "; ".join(f"{k} ({v})" if v else k for k, v in gimp.OP_HELP.items())
TOOLS = [
    ({"name": "imagen_editar",
      "description": ("Edita una imagen como en GIMP (lo usa si está instalado o incluido; si no, Pillow) y guarda el "
                      "resultado en la carpeta de trabajo (imagenes/). Encadena operaciones en orden: " + _OPS_DOC +
                      ". O usa una receta: " + ", ".join(gimp.RECIPES) + ". Nunca sobrescribe el original"),
      "parameters": {"type": "object", "properties": {
          "imagen": _IMG,
          "operaciones": {"type": "array", "items": {"type": "object"},
                          "description": 'lista ordenada, p. ej. [{"op": "encuadrar", "proporcion": "4:5"}, '
                                         '{"op": "texto", "texto": "Rebajas", "posicion": "abajo"}]'},
          "receta": {"type": "string", "enum": [""] + list(gimp.RECIPES), "default": ""},
          "formato": {"type": "string", "enum": ["", "jpg", "png", "webp", "tif", "xcf", "bmp"], "default": "",
                      "description": "vacío = el de la receta o el del original; xcf = proyecto de GIMP"},
          "calidad": {"type": "integer", "description": "calidad JPEG/WebP 10-100 (90 por defecto)"},
          "salida": {"type": "string", "description": "nombre o ruta del resultado en la carpeta de trabajo"}},
          "required": ["imagen"]},
      "risk": "low", "permissions": ["media", "filesystem"], "timeout": 600}, imagen_editar),
    ({"name": "imagen_info",
      "description": "Datos de una imagen de la carpeta de trabajo: tamaño, formato, ppp y tamaño de impresión, "
                     "transparencia, EXIF (cámara, fecha, autor), colores dominantes y brillo",
      "parameters": {"type": "object", "properties": {"imagen": _IMG}, "required": ["imagen"]},
      "risk": "low", "permissions": ["media", "filesystem"]}, imagen_info),
    ({"name": "gimp_estado",
      "description": "Dice si GIMP está disponible (versión, origen, intérprete de scripts) y lista las operaciones, "
                     "recetas y formatos de imagen_editar",
      "parameters": {"type": "object", "properties": {"buscar_de_nuevo": {"type": "boolean", "default": False}}},
      "risk": "low", "permissions": ["media"]}, gimp_estado),
    ({"name": "gimp_script",
      "description": ("Ejecuta un script propio dentro de GIMP para lo que imagen_editar no cubre: Python-Fu con la "
                      "API de GIMP 3 (Gimp, Gio, Gegl ya importados) o Script-Fu en GIMP 2.10. Las variables ENTRADA "
                      "y SALIDA traen las rutas de la imagen y del resultado. Consulta antes el manual de GIMP con "
                      "rag_search. Necesita GIMP y la aprobación de un administrador"),
      "parameters": {"type": "object", "properties": {
          "codigo": {"type": "string"}, "imagen": _IMG,
          "salida": {"type": "string", "description": "nombre del resultado en la carpeta de trabajo, con extensión"},
          "segundos": {"type": "integer", "default": 300}}, "required": ["codigo"]},
      "risk": "high", "permissions": ["media", "filesystem", "code_exec"], "timeout": 900}, gimp_script),
]
