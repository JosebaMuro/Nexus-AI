"""Genera los recursos del reloj NexusWatch (firmware/nexuswatch/nw_assets.h):

* FONT: fuente proporcional de 16 px de alto para Latin-1 (0x20-0xFF), 1 bit por pixel, a partir de DejaVu Sans.
* AVATAR: cara del avatar de Nexus (la geisha de nexus/web/assets/geisha) en RGB565, 150x150 y con mascara.

    python firmware/tools/gen_assets.py [ruta/a/fuente.ttf]
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
GEISHA = ROOT.parent / "nexus-office" / "nexus" / "web" / "assets" / "geisha"
OUT = ROOT / "nexuswatch" / "nw_assets.h"
FONT_CANDIDATES = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf",
                   "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]
H = 16          # alto de la fuente
AV = 150        # lado del avatar


def font_table(path: str) -> tuple[list[int], list[tuple[int, int]]]:
    f = ImageFont.truetype(path, 14)
    data: list[int] = []
    index: list[tuple[int, int]] = []   # (desplazamiento en bytes, ancho)
    for cp in range(0x20, 0x100):
        ch = chr(cp)
        if 0x7F <= cp < 0xA0:
            ch = " "
        w = max(3, int(round(f.getlength(ch))) + 1)
        img = Image.new("1", (w, H), 0)
        ImageDraw.Draw(img).text((0, -1), ch, font=f, fill=1)
        index.append((len(data), w))
        bpr = (w + 7) // 8
        px = img.load()
        for y in range(H):
            for b in range(bpr):
                v = 0
                for bit in range(8):
                    x = b * 8 + bit
                    if x < w and px[x, y]:
                        v |= 0x80 >> bit
                data.append(v)
    return data, index


def avatar() -> list[int]:
    face = Image.open(GEISHA / "face_front.png").convert("RGBA")
    # recorte cuadrado centrado en la cara (el tercio superior de la imagen es el tocado)
    w, h = face.size
    side = min(w, h)
    top = int(h * 0.12)
    face = face.crop(((w - side) // 2, top, (w - side) // 2 + side, top + side)).resize((AV, AV), Image.LANCZOS)
    mask = Image.new("L", (AV, AV), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, AV - 1, AV - 1), fill=255)
    alpha = Image.composite(face.getchannel("A"), Image.new("L", (AV, AV), 0), mask)
    rgb = ImageOps.autocontrast(face.convert("RGB"), cutoff=1)
    out = []
    pa, pr = alpha.load(), rgb.load()
    for y in range(AV):
        for x in range(AV):
            if pa[x, y] < 110:
                out.append(0x0001)   # transparente
                continue
            r, g, b = pr[x, y]
            c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out.append(0x0000 if c == 0x0001 else c)
    return out


def main() -> None:
    fpath = sys.argv[1] if len(sys.argv) > 1 else next((p for p in FONT_CANDIDATES if Path(p).exists()), None)
    if not fpath:
        sys.exit("no encuentro una fuente TTF: pasala como argumento")
    data, index = font_table(fpath)
    av = avatar()
    lines = ["// Generado por firmware/tools/gen_assets.py: no editar a mano.", "#pragma once", "#include <stdint.h>",
             f"#define NW_FONT_H {H}", f"#define NW_AVATAR {AV}",
             "static const uint16_t NW_FONT_IDX[][2] = {" + ",".join(f"{{{o},{w}}}" for o, w in index) + "};",
             "static const uint8_t NW_FONT[] = {"]
    for i in range(0, len(data), 32):
        lines.append("  " + ",".join(str(v) for v in data[i:i + 32]) + ",")
    lines.append("};")
    lines.append("// cara del avatar, RGB565; 0x0001 = transparente")
    lines.append("static const uint16_t NW_AVATAR_PX[] = {")
    for i in range(0, len(av), 24):
        lines.append("  " + ",".join(f"0x{v:04X}" for v in av[i:i + 24]) + ",")
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{OUT}: fuente {len(data)} bytes, avatar {len(av) * 2} bytes")


if __name__ == "__main__":
    main()
