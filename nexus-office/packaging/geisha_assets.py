"""Genera los recursos del avatar 3D "geisha" a partir de la hoja de personaje.

    venv/python.exe packaging/geisha_assets.py [ruta_hoja.jpg]

1. Recorta los planos de la hoja (frente, perfiles, nuca, cuerpo de frente y de espaldas...).
2. Segmenta cada plano separando la figura del fondo (inundacion desde los bordes).
3. Mide las siluetas fila a fila y guarda un "rig" JSON: con la anchura (vista frontal) y la
   profundidad (vista de perfil) de cada altura se construye en el navegador un casco visual 3D.
4. Exporta las texturas RGBA (fondo transparente) que el shader proyecta desde cada vista.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "nexus" / "web" / "assets" / "geisha"
SHEET_BOXES = {"face_front": (0, 0, 282, 350), "background": (286, 0, 560, 350), "profile_left": (0, 372, 184, 628),
               "profile_right": (189, 372, 350, 628), "back_head": (378, 372, 560, 628), "torso_front": (0, 651, 280, 920),
               "torso_side": (320, 651, 560, 920), "kimono_front": (0, 943, 280, 1176), "obi_back": (283, 943, 560, 1176),
               "hands_left": (0, 1199, 280, 1366), "hands_right": (282, 1199, 560, 1366), "body_front": (2, 1393, 279, 1851),
               "body_back": (288, 1393, 558, 1851)}


def crop_sheet(sheet: Path) -> None:
    src = Image.open(sheet).convert("RGB")
    OUT.mkdir(parents=True, exist_ok=True)
    for k, b in SHEET_BOXES.items():
        im = src.crop(b)
        im.resize((im.width * 2, im.height * 2), Image.LANCZOS).save(OUT / f"{k}.jpg", quality=92)
    src.save(OUT / "character_sheet.jpg", quality=95)


def dilate(m: np.ndarray, r: int = 1) -> np.ndarray:
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def erode(m: np.ndarray, r: int = 1) -> np.ndarray:
    return ~dilate(~m, r)


def segment(img: Image.Image) -> np.ndarray:
    """Silueta solida de la figura. El fondo es gris poco saturado con vineta vertical: se modela por fila
    con los pixeles de los bordes y se clasifica como figura lo muy oscuro (pelo, kimono), lo claro (piel,
    naipes) o lo saturado (rojos). Cada fila se rellena entre sus extremos y los extremos se suavizan."""
    a = np.asarray(img.convert("RGB")).astype(np.float32)
    lum, sat = a.mean(2), a.max(2) - a.min(2)
    h, w = lum.shape
    band = max(12, int(w * 0.16))  # bandas exteriores; percentil alto porque la vineta aclara hacia el centro
    edge = np.concatenate([lum[:, 4:band], lum[:, -band:-4]], 1)
    bg = np.percentile(edge, 80, axis=1)
    k = 31  # suavizado vertical robusto del fondo
    pad = np.pad(bg, k // 2, mode="edge")
    bg = np.array([np.median(pad[i:i + k]) for i in range(h)])
    bg = np.clip(bg, 12, 70)[:, None]
    fg = (lum < bg * 0.55) | (lum > bg + 24) | (sat > 34)
    fg[:, :3] = fg[:, -3:] = False
    rows = []
    cx = w / 2
    for y in range(h):
        xs = np.flatnonzero(fg[y])
        if len(xs) < 6:
            rows.append(None)
            continue
        gaps = np.flatnonzero(np.diff(xs) > 14)
        starts, ends = np.r_[xs[0], xs[gaps + 1]], np.r_[xs[gaps], xs[-1]]
        segs = [(s0, e0, (fg[y, s0:e0 + 1]).sum()) for s0, e0 in zip(starts, ends)]
        segs = [sg for sg in segs if sg[2] >= 4]
        if not segs:
            rows.append(None)
            continue
        # se une el segmento central con los vecinos importantes (brazos/mangas)
        main = min(segs, key=lambda sg: 0 if sg[0] <= cx <= sg[1] else min(abs(sg[0] - cx), abs(sg[1] - cx)))
        x0, x1 = main[0], main[1]
        for sg in segs:
            if sg[2] > 25 and (abs(sg[1] - x0) < 40 or abs(sg[0] - x1) < 40):
                x0, x1 = min(x0, sg[0]), max(x1, sg[1])
        rows.append([int(x0), int(x1)])
    # suavizado de extremos (mediana) y relleno de huecos verticales cortos
    for i in range(h):
        if rows[i] is None:
            near = [rows[j] for j in range(max(0, i - 6), min(h, i + 7)) if rows[j]]
            rows[i] = [int(np.median([r[0] for r in near])), int(np.median([r[1] for r in near]))] if len(near) > 3 else None
    sm = []
    for i in range(h):
        win = [rows[j] for j in range(max(0, i - 13), min(h, i + 14)) if rows[j]]
        sm.append([int(np.median([r[0] for r in win])), int(np.median([r[1] for r in win]))] if rows[i] and win else None)
    mask = np.zeros((h, w), bool)
    for y, r in enumerate(sm):
        if r:
            mask[y, r[0]:r[1] + 1] = True
    return mask


def rgba(img: Image.Image, mask: np.ndarray, feather: float = 1.5) -> Image.Image:
    alpha = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(feather))
    out = img.convert("RGBA")
    out.putalpha(alpha)
    return out


def extents(mask: np.ndarray, open_r: int = 6) -> list[tuple[int, int] | None]:
    """Extremos izquierdo/derecho por fila, tras eliminar estructuras finas (horquillas, cadenas)."""
    solid = mask
    rows = []
    for y in range(solid.shape[0]):
        xs = np.flatnonzero(solid[y])
        rows.append((int(xs[0]), int(xs[-1])) if len(xs) else None)
    return rows


def fill_notches(rows: list, k: int = 61, limit: int = 10**9) -> list:
    """x0: apertura (quita picos hacia dentro); x1: cierre. Rellena muescas de menos de k filas.
    Solo por encima de 'limit' (zona del pelo): por debajo hay estrechamientos reales (cuello)."""
    idx = [i for i, r in enumerate(rows) if r and i < limit]
    if not idx:
        return rows
    x0 = np.array([rows[i][0] for i in idx], float)
    x1 = np.array([rows[i][1] for i in idx], float)

    def filt(v, fn):
        pad = np.pad(v, k // 2, mode="edge")
        return np.array([fn(pad[i:i + k]) for i in range(len(v))])
    x0 = filt(filt(x0, np.min), np.max)
    x1 = filt(filt(x1, np.max), np.min)
    out = list(rows)
    for j, i in enumerate(idx):
        out[i] = (int(x0[j]), int(x1[j]))
    return out


def sample(rows, n: int, y0: int, y1: int) -> list:
    out = []
    for i in range(n):
        y = int(y0 + (y1 - y0) * i / (n - 1))
        r = rows[y] or next((rows[y + d] for d in range(1, 40) if 0 <= y + d < len(rows) and rows[y + d]), None)
        out.append([y, r[0], r[1]] if r else [y, None, None])
    return out


def first_last(mask: np.ndarray) -> tuple[int, int]:
    ys = np.flatnonzero(mask.any(1))
    return int(ys[0]), int(ys[-1])


def build() -> dict:
    rig: dict = {"views": {}}
    for name in ("face_front", "profile_left", "profile_right", "back_head", "body_front", "body_back", "torso_front"):
        img = Image.open(OUT / f"{name}.jpg")
        mask = segment(img)
        rgba(img, mask).save(OUT / f"{name}.png", optimize=True)
        Image.fromarray((mask * 255).astype(np.uint8)).save(OUT / f"{name}_mask.png")
        top, bottom = first_last(mask)
        rig["views"][name] = {"w": img.width, "h": img.height, "top": top, "bottom": bottom,
                              "rows": sample(fill_notches(extents(mask), limit={"face_front": 330, "back_head": 250}.get(name, 230)) if name in ("face_front", "profile_left", "profile_right", "back_head") else extents(mask), 96, top, bottom)}
        print(f"{name:14s} {img.size} figura y={top}..{bottom}")
    return rig


# ----------------------------------------------------------------------------- rig en coordenadas de mundo
# Referencias medidas sobre la hoja (pixeles de los recortes a 2x). Cabeza: coronilla (sin el adorno), barbilla y
# punto mas estrecho del cuello en cada vista. Cuerpo: cuello y suelo. Unidades de mundo: metros, suelo en y=0.
HEAD = {"front": {"top": 62, "chin": 510, "neck": 530},
        "profile": {"top": 43, "chin": 398, "neck": 430},
        "back": {"top": 48, "neck": 388}}
BODY = {"neck": 158, "floor": 910, "neck_world": 1.40, "head_neck_world": 1.43}


def _ext(view: dict, py: float):
    rows = view["rows"]
    best = min(rows, key=lambda r: abs(r[0] - py))
    return (best[1], best[2]) if best[1] is not None else None


def build_rig(raw: dict) -> dict:
    V = raw["views"]
    ff, pl, bh, bf, bb = V["face_front"], V["profile_left"], V["back_head"], V["body_front"], V["body_back"]
    # escala del cuerpo y de la cabeza (la anchura del pelo en el cuerpo fija la escala de la cabeza)
    kb = (BODY["neck_world"]) / (BODY["floor"] - BODY["neck"])
    hair_body = max((r[2] - r[1]) for r in bf["rows"] if r[1] is not None and r[0] < BODY["neck"] - 10)
    hair_front = max((r[2] - r[1]) for r in ff["rows"] if r[1] is not None and r[0] < HEAD["front"]["chin"])
    kh = hair_body * kb / hair_front
    sp = (HEAD["front"]["chin"] - HEAD["front"]["top"]) / (HEAD["profile"]["chin"] - HEAD["profile"]["top"])
    sbk = (HEAD["front"]["neck"] - HEAD["front"]["top"]) / (HEAD["back"]["neck"] - HEAD["back"]["top"])
    neck_front = _ext(ff, HEAD["front"]["neck"])
    xc = (neck_front[0] + neck_front[1]) / 2
    neck_prof = _ext(pl, HEAD["profile"]["neck"])
    zc = (neck_prof[0] + neck_prof[1]) / 2
    neck_prof_r = _ext(V["profile_right"], HEAD["profile"]["neck"])
    zc_r = (neck_prof_r[0] + neck_prof_r[1]) / 2
    neck_back = _ext(bh, HEAD["back"]["neck"])
    xcb = (neck_back[0] + neck_back[1]) / 2
    yN = BODY["head_neck_world"]
    head_rows = []
    for r in ff["rows"]:
        py = r[0]
        if r[1] is None or py > HEAD["front"]["neck"] + 30:
            continue
        prow = HEAD["profile"]["top"] + (py - HEAD["front"]["top"]) / sp
        pe = _ext(pl, prow)
        if pe is None:
            continue
        y = yN + (HEAD["front"]["neck"] - py) * kh
        x0, x1 = (r[1] - xc) * kh, (r[2] - xc) * kh
        z1, z0 = (zc - pe[0]) * sp * kh, (zc - pe[1]) * sp * kh   # cara (izquierda en el perfil) hacia +z
        if py < 75:  # cresta del adorno superior: pieza plana en el frente del mono
            z0, z1 = 0.0, 0.035
        elif py < 330 and (z1 - z0) < 0.85 * (x1 - x0):  # zona del pelo: el perfil recortado subestima el volumen
            mid, half = (z0 + z1) / 2, 0.425 * (x1 - x0)
            z0, z1 = min(z0, mid - half), max(z1, mid + half)
        head_rows.append([round(y, 4), round(x0, 4), round(x1, 4), round(z0, 4), round(z1, 4)])
    head_rows.sort(key=lambda r: -r[0])
    neck_body_f = _ext(bf, BODY["neck"])
    xbf = (neck_body_f[0] + neck_body_f[1]) / 2
    neck_body_b = _ext(bb, BODY["neck"])
    xbb = (neck_body_b[0] + neck_body_b[1]) / 2
    body_rows = []
    for r in bf["rows"]:
        py = r[0]
        if r[1] is None or py < BODY["neck"] - 6:
            continue
        be = _ext(bb, py)
        x0 = (r[1] - xbf) * kb
        x1 = (r[2] - xbf) * kb
        if be:  # union con la silueta trasera (espejada)
            x0, x1 = min(x0, -(be[1] - xbb) * kb), max(x1, -(be[0] - xbb) * kb)
        w = x1 - x0
        y = (BODY["floor"] - py) * kb
        half = min(0.17, max(0.05, 0.22 * w))
        if y > 1.36:  # el cuello del cuerpo queda dentro del cuello de la cabeza
            half = min(half, 0.034)
            x0, x1 = max(x0, -0.042), min(x1, 0.042)
        back = -half
        if 300 <= py <= 430:  # lazo del obi en la espalda
            back -= 0.07 * np.sin(np.pi * (py - 300) / 130)
        body_rows.append([round(y, 4), round(x0, 4), round(x1, 4), round(back, 4), round(half * 0.9, 4)])
    body_rows.sort(key=lambda r: -r[0])

    def proj(view: dict, scale_h: float, center_px: float, sv: float, ref_py: float, ref_y: float, flip: bool) -> list:
        # u = h * a + b ;  v = y * c + d   (h: coordenada horizontal de la vista en metros)
        a = (1 / scale_h) / view["w"] * (-1 if flip else 1)
        b = center_px / view["w"]
        c = (1 / sv) / view["h"]
        d = 1 - ref_py / view["h"] - ref_y * c
        return [a, b, c, d]

    return {
        "k_head": kh, "k_body": kb,
        "head": {"rows": head_rows,
                 "front": proj(ff, kh, xc, kh, HEAD["front"]["neck"], yN, False),
                 "left": proj(pl, sp * kh, zc, sp * kh, HEAD["profile"]["neck"], yN, True),     # h = z, la cara a la izquierda
                 "right": proj(V["profile_right"], sp * kh, zc_r, sp * kh, HEAD["profile"]["neck"], yN, False),
                 "back": proj(bh, sbk * kh, xcb, sbk * kh, HEAD["back"]["neck"], yN, True),
                 "eyes": [[(225 - xc) * kh, yN + (HEAD["front"]["neck"] - 352) * kh], [(339 - xc) * kh, yN + (HEAD["front"]["neck"] - 352) * kh]],
                 "mouth": [(282 - xc) * kh, yN + (HEAD["front"]["neck"] - 470) * kh],
                 "pins": [[(45 - xc) * kh, yN + (HEAD["front"]["neck"] - 95) * kh, (165 - xc) * kh, yN + (HEAD["front"]["neck"] - 125) * kh],
                          [(520 - xc) * kh, yN + (HEAD["front"]["neck"] - 90) * kh, (400 - xc) * kh, yN + (HEAD["front"]["neck"] - 122) * kh],
                          [(45 - xc) * kh, yN + (HEAD["front"]["neck"] - 160) * kh, (170 - xc) * kh, yN + (HEAD["front"]["neck"] - 165) * kh],
                          [(520 - xc) * kh, yN + (HEAD["front"]["neck"] - 160) * kh, (395 - xc) * kh, yN + (HEAD["front"]["neck"] - 165) * kh]]},
        "body": {"rows": body_rows,
                 "front": proj(bf, kb, xbf, kb, BODY["floor"], 0.0, False),
                 "back": proj(bb, kb, xbb, kb, BODY["floor"], 0.0, True)},
    }


if __name__ == "__main__":
    crop_sheet(Path(sys.argv[1]) if len(sys.argv) > 1 else OUT / "character_sheet.jpg")
    raw = build()
    rig = build_rig(raw)
    (OUT / "rig.json").write_text(json.dumps(rig))
    for f in OUT.glob("*_mask.png"):
        f.unlink()
    print(f"rig.json escrito: {len(rig['head']['rows'])} anillos de cabeza, {len(rig['body']['rows'])} de cuerpo, "
          f"altura {rig['head']['rows'][0][0]:.2f} m")
