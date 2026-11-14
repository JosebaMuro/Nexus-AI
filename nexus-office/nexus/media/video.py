"""Video a partir de una secuencia de imagenes, sin ffmpeg: cada fotograma se compone con Pillow (encuadre, zoom
lento tipo Ken Burns y fundido entre imagenes) y se guarda como JPEG dentro de un AVI (Motion JPEG). Lo abren VLC,
el reproductor de Windows, Android y los editores de video.
"""
from __future__ import annotations

import io
import struct
import time
from dataclasses import dataclass
from pathlib import Path

FORMATOS = {"16:9": (1280, 720), "720p": (1280, 720), "1080p": (1920, 1080), "9:16": (720, 1280), "1:1": (1080, 1080),
            "4:3": (1024, 768)}


@dataclass
class Opts:
    w: int = 1280
    h: int = 720
    fps: int = 24
    segundos: float = 3.0       # por imagen
    transicion: float = 0.8     # segundos de fundido entre imagenes (0 = corte)
    kenburns: bool = True       # zoom y desplazamiento lentos
    calidad: int = 85           # JPEG

    def formato(self, nombre: str) -> "Opts":
        if nombre in FORMATOS:
            self.w, self.h = FORMATOS[nombre]
        return self


class AviWriter:
    """Cabecera AVI + fotogramas JPEG (00dc) + indice idx1."""

    def __init__(self, out: Path, w: int, h: int, fps: int) -> None:
        self.w, self.h, self.fps = w, h, fps
        self.f = open(out, "wb")
        self.index: list[tuple[int, int]] = []
        self.max_frame = 0
        self.f.write(self._cabecera(0, 0))
        self.movi_pos = self.f.tell() - 4   # posicion de 'movi'

    def _cabecera(self, frames: int, movi_size: int) -> bytes:
        avih = struct.pack("<14I", 1_000_000 // self.fps, self.max_frame * self.fps, 0, 0x10, frames, 0, 1,
                           self.max_frame, self.w, self.h, 0, 0, 0, 0)
        strh = b"vids" + b"MJPG" + struct.pack("<IHHIIIIIIII", 0, 0, 0, 0, 1, self.fps, 0, frames, self.max_frame,
                                               0xFFFFFFFF, 0) + struct.pack("<4H", 0, 0, self.w, self.h)
        strf = struct.pack("<IiiHH4sIiiII", 40, self.w, self.h, 1, 24, b"MJPG", self.w * self.h * 3, 0, 0, 0, 0)
        strl = b"strl" + b"strh" + struct.pack("<I", len(strh)) + strh + b"strf" + struct.pack("<I", len(strf)) + strf
        hdrl = b"hdrl" + b"avih" + struct.pack("<I", len(avih)) + avih + b"LIST" + struct.pack("<I", len(strl)) + strl
        idx_size = 8 + 16 * frames if frames else 0
        riff_size = 4 + 8 + len(hdrl) + 8 + 4 + movi_size + idx_size
        return (b"RIFF" + struct.pack("<I", riff_size) + b"AVI " + b"LIST" + struct.pack("<I", len(hdrl)) + hdrl +
                b"LIST" + struct.pack("<I", 4 + movi_size) + b"movi")

    def frame(self, jpeg: bytes) -> None:
        pos = self.f.tell() - self.movi_pos
        self.f.write(b"00dc" + struct.pack("<I", len(jpeg)) + jpeg + (b"\0" if len(jpeg) % 2 else b""))
        self.index.append((pos, len(jpeg)))
        self.max_frame = max(self.max_frame, len(jpeg))

    def close(self) -> dict:
        movi_size = self.f.tell() - self.movi_pos - 4
        self.f.write(b"idx1" + struct.pack("<I", 16 * len(self.index)))
        for pos, size in self.index:
            self.f.write(b"00dc" + struct.pack("<III", 0x10, pos, size))
        total = self.f.tell()
        self.f.seek(0)
        self.f.write(self._cabecera(len(self.index), movi_size))
        self.f.close()
        return {"frames": len(self.index), "seconds": round(len(self.index) / self.fps, 2), "width": self.w,
                "height": self.h, "fps": self.fps, "bytes": total}


def _encuadre(img, w: int, h: int, zoom: float, dx: float, dy: float):
    """Recorta la imagen para llenar w x h con un zoom (>= 1) y un desplazamiento (-1..1) sobre lo que sobra."""
    from PIL import Image
    iw, ih = img.size
    escala = max(w / iw, h / ih) * zoom
    cw, ch = w / escala, h / escala
    x = (iw - cw) / 2 * (1 + dx)
    y = (ih - ch) / 2 * (1 + dy)
    return img.resize((w, h), Image.BILINEAR, box=(x, y, x + cw, y + ch))


def render_avi(imagenes: list[Path], o: Opts, out: Path) -> dict:
    """Compone el video; devuelve {frames, seconds, width, height, fps, bytes}. Lanza si no hay imagenes legibles."""
    from PIL import Image
    o.w, o.h = max(64, o.w // 2 * 2), max(64, o.h // 2 * 2)
    fotos = []
    for ruta in imagenes[:120]:
        try:
            with Image.open(ruta) as im:
                im = im.convert("RGB")
                im.thumbnail((o.w * 2, o.h * 2))   # no hace falta mas resolucion que el doble del video
                fotos.append(im.copy())
        except (OSError, ValueError):
            continue
    if not fotos:
        raise ValueError("No hay ninguna imagen legible para el vídeo")
    n_img = max(1, int(round(o.segundos * o.fps)))
    n_tr = min(n_img // 2, int(round(o.transicion * o.fps))) if len(fotos) > 1 else 0
    avi = AviWriter(out, o.w, o.h, o.fps)

    def cuadro(i: int, k: int):
        """Fotograma k (0..n_img+n_tr) de la imagen i: el movimiento sigue durante el fundido."""
        if not o.kenburns:
            return _encuadre(fotos[i], o.w, o.h, 1.0, 0, 0)
        t = k / max(1, n_img + n_tr - 1)
        acerca = i % 2 == 0
        zoom = 1.0 + 0.12 * (t if acerca else 1 - t)
        return _encuadre(fotos[i], o.w, o.h, zoom, (0.6 if i % 4 < 2 else -0.6) * (t - 0.5), (0.4 if i % 3 else -0.4) * (t - 0.5))

    for i in range(len(fotos)):
        ultimo = i == len(fotos) - 1
        for k in range(n_img):
            im = cuadro(i, k)
            if i > 0 and k < n_tr:   # fundido desde la anterior
                im = Image.blend(cuadro(i - 1, n_img + k), im, (k + 1) / (n_tr + 1))
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=o.calidad)
            avi.frame(buf.getvalue())
        if ultimo:
            break
    return avi.close()


def crear_video(services, args: dict) -> dict:
    """Herramienta crear_video y POST /api/video/rapido: imagenes de outputs/ (o las ultimas generadas) -> AVI."""
    from .. import paths
    base = paths.sub("outputs").resolve()
    rutas: list[Path] = []
    for ref in args.get("imagenes") or []:
        nombre = str(ref).split("?")[0].replace("\\", "/")
        nombre = nombre.split("/outputs/", 1)[-1].lstrip("/")
        for raiz in (base, paths.sub("sandbox").resolve()):
            p = (raiz / nombre).resolve()
            if p.is_file() and str(p).startswith(str(raiz)):
                rutas.append(p)
                break
    if not rutas:   # sin lista: las ultimas imagenes generadas
        n = max(2, min(int(args.get("ultimas") or 6), 60))
        todas = sorted((p for p in base.rglob("*") if p.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp")
                        and "videos" not in p.parts), key=lambda p: p.stat().st_mtime)
        rutas = todas[-n:]
    if not rutas:
        return {"error": "No hay imágenes: genera alguna antes o indica cuáles usar"}
    o = Opts().formato(str(args.get("formato") or "16:9"))
    o.segundos = max(0.5, min(float(args.get("segundos") or 3), 20))
    o.transicion = max(0.0, min(float(args.get("transicion") if args.get("transicion") is not None else 0.8), 3))
    o.fps = max(8, min(int(args.get("fps") or 24), 30))
    o.kenburns = bool(args.get("movimiento", True))
    titulo = "".join(c if c.isalnum() else "-" for c in str(args.get("titulo") or "video").lower())[:40].strip("-") or "video"
    nombre = f"{int(time.time())}_{titulo}.avi"
    out = paths.sub("outputs", "videos") / nombre
    try:
        res = render_avi(rutas, o, out)
    except ValueError as e:
        return {"error": str(e)}
    return {**res, "url": f"/outputs/videos/{nombre}", "imagenes": len(rutas),
            "nota": "Vídeo AVI (Motion JPEG): se abre con VLC, el reproductor de Windows o Android."}
