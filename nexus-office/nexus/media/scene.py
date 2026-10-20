"""Compositor de escenas: imagen sin modelo neuronal que representa lo que dice el prompt.

Lee el texto (espanol o ingles) y saca la hora (dia, noche, atardecer...), el clima, el lugar (mar, playa, montana,
bosque, ciudad, espacio...), los sujetos (gato, casa, arbol, barco, persona...) con su color y cuantos hay, y dibuja
esa escena con formas vectoriales. No compite con stable-diffusion.cpp, pero «un gato negro bajo la luna» da un gato
negro bajo la luna y no un dibujo abstracto que se parece en todos los prompts.
"""
from __future__ import annotations

import math
import re
import unicodedata
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def plain(t: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFKD", t.lower()) if not unicodedata.combining(c))


COLORS = {
    "rojo": (210, 40, 45), "roja": (210, 40, 45), "red": (210, 40, 45), "azul": (40, 90, 210), "blue": (40, 90, 210),
    "verde": (40, 160, 70), "green": (40, 160, 70), "amarillo": (245, 205, 40), "amarilla": (245, 205, 40),
    "yellow": (245, 205, 40), "naranja": (245, 130, 30), "orange": (245, 130, 30), "morado": (130, 60, 190),
    "morada": (130, 60, 190), "purple": (130, 60, 190), "violeta": (150, 80, 210), "violet": (150, 80, 210),
    "rosa": (240, 110, 170), "pink": (240, 110, 170), "negro": (28, 28, 32), "negra": (28, 28, 32), "black": (28, 28, 32),
    "blanco": (240, 240, 235), "blanca": (240, 240, 235), "white": (240, 240, 235), "gris": (130, 130, 135),
    "gray": (130, 130, 135), "grey": (130, 130, 135), "marron": (120, 75, 40), "brown": (120, 75, 40),
    "dorado": (230, 185, 60), "dorada": (230, 185, 60), "gold": (230, 185, 60), "golden": (230, 185, 60),
    "plateado": (190, 195, 205), "plateada": (190, 195, 205), "silver": (190, 195, 205), "cian": (0, 200, 220),
    "cyan": (0, 200, 220), "turquesa": (30, 200, 190), "turquoise": (30, 200, 190),
}
# palabra -> (categoria, clave). Plurales: se prueba tambien sin la «s»/«es» final.
WORDS = {
    # hora del dia
    "noche": ("time", "night"), "night": ("time", "night"), "nocturno": ("time", "night"), "nocturna": ("time", "night"),
    "medianoche": ("time", "night"), "midnight": ("time", "night"), "luna": ("sky", "moon"), "moon": ("sky", "moon"),
    "atardecer": ("time", "sunset"), "sunset": ("time", "sunset"), "ocaso": ("time", "sunset"),
    "crepusculo": ("time", "sunset"), "dusk": ("time", "sunset"), "amanecer": ("time", "sunrise"),
    "sunrise": ("time", "sunrise"), "alba": ("time", "sunrise"), "dawn": ("time", "sunrise"), "dia": ("time", "day"),
    "day": ("time", "day"), "mediodia": ("time", "day"), "soleado": ("time", "day"), "sunny": ("time", "day"),
    # clima
    "lluvia": ("weather", "rain"), "rain": ("weather", "rain"), "lloviendo": ("weather", "rain"),
    "rainy": ("weather", "rain"), "nieve": ("weather", "snow"), "snow": ("weather", "snow"), "nevando": ("weather", "snow"),
    "snowy": ("weather", "snow"), "invierno": ("weather", "snow"), "winter": ("weather", "snow"),
    "niebla": ("weather", "fog"), "fog": ("weather", "fog"), "bruma": ("weather", "fog"), "mist": ("weather", "fog"),
    "foggy": ("weather", "fog"), "tormenta": ("weather", "storm"), "storm": ("weather", "storm"),
    "rayo": ("weather", "storm"), "lightning": ("weather", "storm"), "nube": ("sky", "cloud"), "cloud": ("sky", "cloud"),
    "nublado": ("sky", "cloud"), "cloudy": ("sky", "cloud"),
    # lugar
    "mar": ("place", "sea"), "sea": ("place", "sea"), "oceano": ("place", "sea"), "ocean": ("place", "sea"),
    "ola": ("place", "sea"), "wave": ("place", "sea"), "playa": ("place", "beach"), "beach": ("place", "beach"),
    "costa": ("place", "beach"), "coast": ("place", "beach"), "arena": ("place", "beach"),
    "montana": ("place", "mountain"), "mountain": ("place", "mountain"), "monte": ("place", "mountain"),
    "sierra": ("place", "mountain"), "pico": ("place", "mountain"), "peak": ("place", "mountain"),
    "volcan": ("place", "mountain"), "bosque": ("place", "forest"), "forest": ("place", "forest"),
    "selva": ("place", "forest"), "jungle": ("place", "forest"), "woods": ("place", "forest"),
    "ciudad": ("place", "city"), "city": ("place", "city"), "edificio": ("place", "city"), "building": ("place", "city"),
    "rascacielo": ("place", "city"), "skyscraper": ("place", "city"), "calle": ("place", "city"),
    "street": ("place", "city"), "urbano": ("place", "city"), "urban": ("place", "city"), "town": ("place", "city"),
    "espacio": ("place", "space"), "space": ("place", "space"), "galaxia": ("place", "space"),
    "galaxy": ("place", "space"), "universo": ("place", "space"), "universe": ("place", "space"),
    "nebulosa": ("place", "space"), "nebula": ("place", "space"), "cosmos": ("place", "space"),
    "desierto": ("place", "desert"), "desert": ("place", "desert"), "duna": ("place", "desert"),
    "dune": ("place", "desert"), "lago": ("place", "lake"), "lake": ("place", "lake"), "rio": ("place", "lake"),
    "river": ("place", "lake"), "estanque": ("place", "lake"), "pond": ("place", "lake"),
    "jardin": ("place", "garden"), "garden": ("place", "garden"), "parque": ("place", "garden"),
    "park": ("place", "garden"), "campo": ("place", "field"), "field": ("place", "field"), "prado": ("place", "field"),
    "meadow": ("place", "field"), "pradera": ("place", "field"), "colina": ("place", "field"), "hill": ("place", "field"),
    # cielo
    "sol": ("sky", "sun"), "sun": ("sky", "sun"), "estrella": ("sky", "star"), "star": ("sky", "star"),
    "estrellado": ("sky", "star"), "starry": ("sky", "star"), "planeta": ("sky", "planet"), "planet": ("sky", "planet"),
    "pajaro": ("subj", "bird"), "bird": ("subj", "bird"), "ave": ("subj", "bird"), "gaviota": ("subj", "bird"),
    "seagull": ("subj", "bird"), "cohete": ("subj", "rocket"), "rocket": ("subj", "rocket"), "nave": ("subj", "rocket"),
    "spaceship": ("subj", "rocket"), "ovni": ("subj", "ufo"), "ufo": ("subj", "ufo"),
    # sujetos
    "gato": ("subj", "cat"), "gata": ("subj", "cat"), "gatito": ("subj", "cat"), "cat": ("subj", "cat"),
    "kitten": ("subj", "cat"), "perro": ("subj", "dog"), "perra": ("subj", "dog"), "perrito": ("subj", "dog"),
    "cachorro": ("subj", "dog"), "dog": ("subj", "dog"), "puppy": ("subj", "dog"), "casa": ("subj", "house"),
    "house": ("subj", "house"), "cabana": ("subj", "house"), "cabin": ("subj", "house"), "hogar": ("subj", "house"),
    "home": ("subj", "house"), "castillo": ("subj", "castle"), "castle": ("subj", "castle"),
    "palacio": ("subj", "castle"), "palace": ("subj", "castle"), "arbol": ("subj", "tree"), "tree": ("subj", "tree"),
    "pino": ("subj", "pine"), "pine": ("subj", "pine"), "flor": ("subj", "flower"), "flower": ("subj", "flower"),
    "rosa_flor": ("subj", "flower"), "tulipan": ("subj", "flower"), "tulip": ("subj", "flower"),
    "girasol": ("subj", "flower"), "sunflower": ("subj", "flower"), "coche": ("subj", "car"), "car": ("subj", "car"),
    "auto": ("subj", "car"), "carro": ("subj", "car"), "automovil": ("subj", "car"), "barco": ("subj", "boat"),
    "boat": ("subj", "boat"), "velero": ("subj", "boat"), "sailboat": ("subj", "boat"), "ship": ("subj", "boat"),
    "barca": ("subj", "boat"), "pez": ("subj", "fish"), "peces": ("subj", "fish"), "fish": ("subj", "fish"),
    "persona": ("subj", "person"), "person": ("subj", "person"), "gente": ("subj", "person"),
    "people": ("subj", "person"), "hombre": ("subj", "person"), "man": ("subj", "person"), "men": ("subj", "person"),
    "mujer": ("subj", "person"), "woman": ("subj", "person"), "women": ("subj", "person"), "nino": ("subj", "child"),
    "nina": ("subj", "child"), "child": ("subj", "child"), "children": ("subj", "child"), "kid": ("subj", "child"),
    "boy": ("subj", "child"), "girl": ("subj", "child"), "chico": ("subj", "person"), "chica": ("subj", "person"),
    "astronauta": ("subj", "astronaut"), "astronaut": ("subj", "astronaut"), "robot": ("subj", "robot"),
    "androide": ("subj", "robot"), "android": ("subj", "robot"), "faro": ("subj", "lighthouse"),
    "lighthouse": ("subj", "lighthouse"), "corazon": ("subj", "heart"), "heart": ("subj", "heart"),
    "mariposa": ("subj", "butterfly"), "butterfly": ("subj", "butterfly"), "cactus": ("subj", "cactus"),
    "seta": ("subj", "mushroom"), "hongo": ("subj", "mushroom"), "mushroom": ("subj", "mushroom"),
    "caballo": ("subj", "horse"), "horse": ("subj", "horse"), "vaca": ("subj", "cow"), "cow": ("subj", "cow"),
    "abstracto": ("style", "abstract"), "abstract": ("style", "abstract"), "fractal": ("style", "abstract"),
    "patron": ("style", "abstract"), "pattern": ("style", "abstract"),
}
NUMBERS = {"un": 1, "una": 1, "uno": 1, "a": 1, "an": 1, "one": 1, "dos": 2, "two": 2, "tres": 3, "three": 3,
           "cuatro": 4, "four": 4, "cinco": 5, "five": 5, "varios": 3, "varias": 3, "several": 3, "muchos": 5,
           "muchas": 5, "many": 5, "some": 3, "unos": 2, "unas": 2}
SKY_SUBJECTS = {"bird", "rocket", "ufo", "butterfly"}
WATER_SUBJECTS = {"boat", "fish"}
DEFAULT_COLOR = {
    "cat": (225, 135, 55), "dog": (165, 115, 70), "house": (232, 205, 165), "castle": (150, 150, 162),
    "tree": (45, 140, 60), "pine": (30, 110, 60), "flower": (230, 60, 90), "car": (200, 40, 40), "boat": (240, 240, 235),
    "fish": (250, 140, 30), "person": (60, 110, 200), "child": (240, 170, 40), "astronaut": (238, 238, 240),
    "robot": (175, 182, 195), "lighthouse": (240, 240, 235), "heart": (220, 40, 70), "butterfly": (250, 150, 40),
    "cactus": (60, 150, 70), "mushroom": (210, 40, 40), "horse": (130, 80, 45), "cow": (245, 245, 240),
    "bird": (40, 40, 50), "rocket": (235, 235, 240), "ufo": (170, 180, 195),
}
SIZE = {"house": 0.30, "castle": 0.36, "tree": 0.34, "pine": 0.36, "lighthouse": 0.46, "person": 0.24, "child": 0.17,
        "astronaut": 0.24, "robot": 0.24, "cat": 0.13, "dog": 0.15, "horse": 0.24, "cow": 0.2, "car": 0.13,
        "flower": 0.12, "cactus": 0.22, "mushroom": 0.1, "heart": 0.2, "boat": 0.18, "fish": 0.09, "bird": 0.05,
        "rocket": 0.22, "ufo": 0.12, "butterfly": 0.07}


def _lookup(tok: str):
    for t in (tok, tok[:-1] if tok.endswith("s") else None, tok[:-2] if tok.endswith("es") else None):
        if t and t in WORDS:
            return WORDS[t]
    return None


def _color(tok: str):
    return COLORS.get(tok) or (COLORS.get(tok[:-1]) if tok.endswith("s") else None)


def parse(prompt: str) -> dict:
    """Descompone el prompt en hora, clima, lugar, elementos del cielo y sujetos (con color y cantidad)."""
    toks = re.findall(r"[a-z0-9]+", plain(prompt))
    scene = {"time": None, "weather": [], "places": [], "sky": [], "subjects": [], "colors": [], "abstract": False}
    for i, tok in enumerate(toks):
        c = _color(tok)
        if c:
            scene["colors"].append(c)
        # «rosa» es color salvo que vaya como sustantivo («una rosa», «rosas rojas»)
        if tok in ("rosa", "rosas") and i and toks[i - 1] in ("una", "unas", "las", "la", "de", "con") and \
                not (i + 1 < len(toks) and toks[i + 1] in WORDS):
            hit = WORDS["rosa_flor"]
        else:
            hit = _lookup(tok)
        if not hit:
            continue
        cat, key = hit
        if cat == "time":
            scene["time"] = scene["time"] or key
        elif cat == "weather":
            scene["weather"].append(key)
        elif cat == "place":
            if key not in scene["places"]:
                scene["places"].append(key)
        elif cat == "sky":
            if key not in scene["sky"]:
                scene["sky"].append(key)
        elif cat == "style":
            scene["abstract"] = True
        else:
            color = None   # espanol: «gato negro»; ingles: «black cat»; se mira tambien dos palabras atras/adelante
            for j in (i + 1, i - 1, i + 2, i - 2):
                if 0 <= j < len(toks) and _color(toks[j]) and not (j > i and _lookup(toks[j - 1]) and j - 1 != i):
                    color = _color(toks[j])
                    break
            count = 1
            for j in (i - 1, i - 2):
                if 0 <= j < len(toks) and toks[j] in NUMBERS:
                    count = NUMBERS[toks[j]]
                    break
                if 0 <= j < len(toks) and toks[j].isdigit():
                    count = min(int(toks[j]), 6)
                    break
            else:
                if tok.endswith("s") and _lookup(tok) and tok not in WORDS:   # plural sin numero
                    count = 2
            if key in ("person", "child") and tok in ("gente", "people", "children"):
                count = max(count, 3)
            scene["subjects"].append({"kind": key, "color": color, "count": count})
    if scene["time"] is None:
        scene["time"] = "night" if "moon" in scene["sky"] or "star" in scene["sky"] or "space" in scene["places"] \
            else "day"
    return scene


# ---------------------------------------------------------------------------------------------- utilidades de color
def _mix(a, b, t):
    return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))


def _shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


SKIES = {
    "day": [(70, 140, 225), (170, 210, 245)],
    "sunset": [(55, 45, 115), (230, 105, 85), (255, 190, 110)],
    "sunrise": [(110, 140, 215), (250, 175, 150), (255, 215, 160)],
    "night": [(6, 8, 28), (30, 36, 85)],
    "storm": [(55, 60, 72), (105, 110, 125)],
    "space": [(2, 2, 10), (14, 10, 34)],
}
LIGHT = {"day": 1.0, "sunrise": 0.85, "sunset": 0.75, "night": 0.42, "storm": 0.62, "space": 0.8}


class _Draw(ImageDraw.ImageDraw):
    """ImageDraw que acepta cajas con las esquinas en cualquier orden (las figuras se dibujan simetricas)."""

    @staticmethod
    def _box(xy):
        x0, y0, x1, y1 = xy
        return [min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)]

    def rectangle(self, xy, *a, **k):
        return super().rectangle(self._box(xy), *a, **k)

    def ellipse(self, xy, *a, **k):
        return super().ellipse(self._box(xy), *a, **k)

    def rounded_rectangle(self, xy, *a, **k):
        return super().rounded_rectangle(self._box(xy), *a, **k)

    def chord(self, xy, *a, **k):
        return super().chord(self._box(xy), *a, **k)


class Painter:
    def __init__(self, scene: dict, seed: int, size: int) -> None:
        self.scene, self.W = scene, size * 2       # se dibuja al doble y se reduce: bordes suaves
        self.rng = np.random.default_rng(seed)
        places = scene["places"]
        self.place = places[0] if places else ("garden" if any(s["kind"] == "flower" for s in scene["subjects"])
                                               else "field")
        self.mode = "space" if self.place == "space" else ("storm" if "storm" in scene["weather"] else scene["time"])
        self.light = LIGHT[self.mode]
        self.horizon = int(self.W * (0.55 if self.place in ("sea", "beach", "lake") else 0.62))
        if self.place == "space":
            self.horizon = self.W
        self.img = Image.new("RGB", (self.W, self.W))
        self.d = _Draw(self.img)
        self.glow = Image.new("RGB", (self.W, self.W))
        self.g = _Draw(self.glow)

    def lit(self, c):
        """Color de un objeto bajo la luz de la escena (de noche, oscuro y azulado)."""
        c = _shade(c, self.light)
        return _mix(c, (20, 30, 70), 0.25) if self.mode == "night" else c

    # ------------------------------------------------------------------------------------------------ fondo
    def sky(self):
        stops, W = SKIES[self.mode], self.W
        arr = np.zeros((W, W, 3), dtype=np.float32)
        t = np.linspace(0, 1, W)[:, None]
        if len(stops) == 2:
            col = (1 - t) * np.array(stops[0]) + t * np.array(stops[1])
        else:
            a, b, c = (np.array(s, dtype=np.float32) for s in stops)
            col = np.where(t < 0.5, (1 - t * 2) * a + t * 2 * b, (2 - t * 2) * b + (t * 2 - 1) * c)
        arr[:] = col[:, None, :]
        if self.mode == "space":   # nebulosa con el color del prompt
            tint = np.array(self.scene["colors"][0] if self.scene["colors"] else (110, 60, 200), dtype=np.float32)
            yy, xx = np.mgrid[0:W, 0:W] / W
            cx, cy = self.rng.uniform(0.3, 0.7, 2)
            n = np.exp(-(((xx - cx) * 1.6) ** 2 + ((yy - cy) * 2.6) ** 2) * 3) * (0.6 + 0.4 * np.sin(xx * 9 + yy * 5))
            arr += n[..., None] * tint * 0.55
        self.img.paste(Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)))

    def stars(self, n):
        for _ in range(n):
            x, y = self.rng.uniform(0, self.W), self.rng.uniform(0, self.horizon * 0.9)
            r = self.rng.uniform(1, 3.2)
            self.d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 235))

    def sun(self, x, y, r):
        col = {"sunset": (255, 150, 70), "sunrise": (255, 200, 120)}.get(self.mode, (255, 230, 120))
        self.g.ellipse([x - r * 2.2, y - r * 2.2, x + r * 2.2, y + r * 2.2], fill=_shade(col, 0.55))
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=col)

    def moon(self, x, y, r):
        self.g.ellipse([x - r * 2, y - r * 2, x + r * 2, y + r * 2], fill=(70, 80, 110))
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=(245, 242, 220))
        for dx, dy, rr in ((-0.3, -0.2, 0.18), (0.25, 0.15, 0.12), (-0.05, 0.35, 0.1)):
            cx, cy = x + dx * r, y + dy * r
            self.d.ellipse([cx - rr * r, cy - rr * r, cx + rr * r, cy + rr * r], fill=(215, 212, 195))

    def cloud(self, x, y, s):
        col = self.lit((245, 245, 250)) if self.mode != "storm" else (85, 88, 100)
        for dx, dy, r in ((0, 0, 0.5), (0.45, 0.1, 0.38), (-0.45, 0.12, 0.36), (0.2, -0.25, 0.4), (-0.2, -0.2, 0.32)):
            cx, cy, rr = x + dx * s, y + dy * s, r * s
            self.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=col)

    def planet(self, x, y, r):
        base = self.scene["colors"][-1] if self.scene["colors"] else (220, 150, 90)
        self.g.ellipse([x - r * 1.5, y - r * 1.5, x + r * 1.5, y + r * 1.5], fill=_shade(base, 0.35))
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=base)
        for k in range(3):
            yy = y - r * 0.5 + k * r * 0.45
            w = math.sqrt(max(r * r - (yy - y) ** 2, 0))
            self.d.line([(x - w * 0.95, yy), (x + w * 0.95, yy)], fill=_shade(base, 0.8), width=int(r * 0.12))
        self.d.ellipse([x - r * 1.8, y - r * 0.35, x + r * 1.8, y + r * 0.35], outline=(235, 225, 200),
                       width=max(2, int(r * 0.08)))

    def ground(self):
        W, H, rng, place = self.W, self.horizon, self.rng, self.place
        snow = "snow" in self.scene["weather"]
        if place == "space":
            return
        if place in ("sea", "beach", "lake"):
            water = self.lit((30, 110, 175) if place != "lake" else (50, 120, 150))
            bottom = H if place == "sea" else int(H + (W - H) * (0.45 if place == "beach" else 0.55))
            if place == "lake":   # orilla con hierba detras del agua
                self.hills(H - int(W * 0.06), self.lit((60, 130, 60)), 0.03)
            self.d.rectangle([0, H, W, W], fill=water)
            for k in range(40):   # olas
                y = H + (k / 40) ** 1.5 * (W - H)
                x = rng.uniform(-50, W)
                w = 20 + (y - H) * 0.4
                self.d.line([(x, y), (x + w, y)], fill=_shade(water, 1.35), width=max(2, int((y - H) * 0.02) + 2))
            if place == "beach":
                sand = self.lit((235, 210, 150) if not snow else (240, 240, 245))
                pts = [(0, bottom)] + [(x, bottom + math.sin(x / W * 6 + 1) * W * 0.02) for x in range(0, W + 1, 16)]
                self.d.polygon(pts + [(W, W), (0, W)], fill=sand)
                self.d.line(pts, fill=(250, 250, 245), width=6)
            self.water_line = (H, bottom)
            return
        if place == "mountain":
            for layer, (shade, peaks) in enumerate(((0.7, 4), (1.0, 3))):
                pts = [(0, H)]
                for k in range(peaks * 2 + 1):
                    x = k * W / (peaks * 2)
                    top = k % 2 == 1
                    y = H - (rng.uniform(0.28, 0.42) if top else rng.uniform(0.08, 0.16)) * W * (1.15 - layer * 0.25)
                    pts.append((x, y))
                pts.append((W, H))
                rock = self.lit(_shade((110, 112, 130), shade))
                self.d.polygon(pts, fill=rock)
                for k in range(1, len(pts) - 1, 2):   # nieve en las cumbres
                    (x0, y0), (x1, y1), (x2, y2) = pts[k - 1], pts[k], pts[k + 1]
                    f = 0.28
                    self.d.polygon([(x1, y1), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f),
                                    (x1 + (x0 - x1) * f, y1 + (y0 - y1) * f)], fill=self.lit((245, 245, 250)))
            self.hills(H, self.lit((240, 240, 245) if snow else (70, 135, 65)), 0.02)
            return
        if place == "desert":
            self.hills(H - int(W * 0.04), self.lit((215, 170, 100)), 0.05)
            self.hills(H + int(W * 0.08), self.lit((235, 195, 120)), 0.04)
            return
        if place == "city":
            x = -10
            while x < W:
                w = rng.uniform(0.07, 0.14) * W
                h = rng.uniform(0.18, 0.5) * W
                body = self.lit(_shade((80, 90, 115), rng.uniform(0.8, 1.2)))
                self.d.rectangle([x, H - h, x + w, H], fill=body)
                lit_win = self.mode in ("night", "sunset", "storm")
                for wy in np.arange(H - h + 12, H - 10, 22):
                    for wx in np.arange(x + 8, x + w - 10, 18):
                        on = lit_win and rng.random() < 0.55
                        c = (255, 215, 120) if on else self.lit((150, 175, 205))
                        self.d.rectangle([wx, wy, wx + 9, wy + 12], fill=c)
                        if on:
                            self.g.rectangle([wx - 4, wy - 4, wx + 13, wy + 16], fill=(90, 70, 30))
                x += w + rng.uniform(2, 10)
            self.d.rectangle([0, H, W, W], fill=self.lit((60, 62, 70) if not snow else (225, 228, 235)))
            for k in range(6):   # marcas de la calle
                xx = k * W / 6 + 20
                self.d.rectangle([xx, H + (W - H) * 0.55, xx + W / 14, H + (W - H) * 0.58], fill=self.lit((230, 220, 150)))
            return
        green = (240, 240, 245) if snow else (75, 150, 60) if place != "garden" else (70, 160, 70)
        self.hills(H - int(W * 0.05), self.lit(_shade(green, 0.8)), 0.04)
        self.hills(H, self.lit(green), 0.025)
        if place == "garden":
            cols = self.scene["colors"] or [(230, 60, 90), (250, 200, 40), (150, 80, 210), (245, 245, 240)]
            for k in range(18):
                self.flower(rng.uniform(0, W), H + (W - H) * rng.uniform(0.15, 0.95), W * rng.uniform(0.05, 0.08),
                            cols[k % len(cols)])
        if place == "forest":
            for k in range(14):
                x = (k + rng.uniform(-0.3, 0.3)) * W / 13
                self.pine(x, H + rng.uniform(-0.02, 0.03) * W, W * rng.uniform(0.22, 0.32),
                          _shade((30, 110, 60), rng.uniform(0.7, 1.0)))

    def hills(self, base, col, amp):
        W = self.W
        ph = self.rng.uniform(0, 6.3)
        pts = [(x, base - (math.sin(x / W * 5 + ph) * 0.5 + 0.5) * amp * W) for x in range(0, W + 1, 12)]
        self.d.polygon(pts + [(W, W), (0, W)], fill=col)

    # ------------------------------------------------------------------------------------------------ sujetos
    def pine(self, x, y, h, col=None):
        col = self.lit(col or (30, 110, 60))
        self.d.rectangle([x - h * 0.04, y - h * 0.2, x + h * 0.04, y], fill=self.lit((90, 60, 35)))
        for k in range(3):
            top = y - h * (1 - k * 0.22)
            w = h * (0.22 + k * 0.08)
            self.d.polygon([(x, top), (x - w, top + h * 0.45), (x + w, top + h * 0.45)], fill=_shade(col, 1 - k * 0.08))
        if "snow" in self.scene["weather"]:
            self.d.polygon([(x, y - h), (x - h * 0.1, y - h * 0.82), (x + h * 0.1, y - h * 0.82)], fill=(250, 250, 255))

    def tree(self, x, y, h, col):
        self.d.rectangle([x - h * 0.06, y - h * 0.5, x + h * 0.06, y], fill=self.lit((100, 65, 35)))
        for dx, dy, r in ((0, -0.72, 0.3), (-0.2, -0.55, 0.22), (0.2, -0.55, 0.22), (0, -0.92, 0.2)):
            cx, cy, rr = x + dx * h, y + dy * h, r * h
            self.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=self.lit(col))

    def house(self, x, y, h, col):
        w = h * 1.1
        self.d.rectangle([x - w / 2, y - h * 0.6, x + w / 2, y], fill=self.lit(col))
        self.d.polygon([(x - w * 0.62, y - h * 0.58), (x, y - h), (x + w * 0.62, y - h * 0.58)],
                       fill=self.lit((165, 55, 45)))
        self.d.rectangle([x - w * 0.1, y - h * 0.3, x + w * 0.1, y], fill=self.lit((110, 70, 40)))
        win = (255, 215, 120) if self.mode in ("night", "sunset", "storm") else self.lit((160, 200, 235))
        for wx in (x - w * 0.32, x + w * 0.2):
            self.d.rectangle([wx, y - h * 0.45, wx + w * 0.13, y - h * 0.32], fill=win)
            if win[0] == 255:
                self.g.rectangle([wx - 8, y - h * 0.45 - 8, wx + w * 0.13 + 8, y - h * 0.32 + 8], fill=(110, 80, 30))

    def castle(self, x, y, h, col):
        c = self.lit(col)
        w = h * 1.2
        self.d.rectangle([x - w / 2, y - h * 0.55, x + w / 2, y], fill=c)
        for tx in (x - w / 2, x + w / 2 - w * 0.22, x - w * 0.11):
            th = h if tx == x - w * 0.11 else h * 0.8
            self.d.rectangle([tx, y - th, tx + w * 0.22, y], fill=_shade(c, 0.92))
            for k in range(3):
                bx = tx + k * w * 0.08
                self.d.rectangle([bx, y - th - h * 0.06, bx + w * 0.05, y - th], fill=_shade(c, 0.92))
            self.d.polygon([(tx - 4, y - th - h * 0.06), (tx + w * 0.11, y - th - h * 0.3),
                            (tx + w * 0.22 + 4, y - th - h * 0.06)], fill=self.lit((60, 70, 150)))
        self.d.chord([x - w * 0.1, y - h * 0.32, x + w * 0.1, y + h * 0.05], 180, 360, fill=self.lit((60, 40, 30)))
        self.d.rectangle([x - w * 0.1, y - h * 0.14, x + w * 0.1, y], fill=self.lit((60, 40, 30)))

    def lighthouse(self, x, y, h, col):
        w0, w1 = h * 0.16, h * 0.1
        self.d.polygon([(x - w0, y), (x - w1, y - h * 0.8), (x + w1, y - h * 0.8), (x + w0, y)], fill=self.lit(col))
        for k in (0.2, 0.5):
            yy, ww = y - h * k, w0 - (w0 - w1) * k
            self.d.polygon([(x - ww, yy), (x - ww * 0.95, yy - h * 0.12), (x + ww * 0.95, yy - h * 0.12), (x + ww, yy)],
                           fill=self.lit((200, 40, 40)))
        self.d.rectangle([x - w1, y - h * 0.92, x + w1, y - h * 0.8], fill=(255, 225, 120))
        self.d.polygon([(x - w1 * 1.2, y - h * 0.92), (x, y - h), (x + w1 * 1.2, y - h * 0.92)], fill=self.lit((60, 60, 70)))
        if self.mode in ("night", "storm", "sunset"):
            self.g.polygon([(x, y - h * 0.86), (x + self.W, y - h * 1.05), (x + self.W, y - h * 0.65)], fill=(90, 80, 40))
            self.g.ellipse([x - h * 0.15, y - h, x + h * 0.15, y - h * 0.72], fill=(160, 140, 60))

    def cat(self, x, y, h, col):
        c = self.lit(col)
        self.d.ellipse([x - h * 0.45, y - h * 0.55, x + h * 0.35, y], fill=c)                  # cuerpo
        self.d.line([(x + h * 0.3, y - h * 0.15), (x + h * 0.6, y - h * 0.35), (x + h * 0.55, y - h * 0.7)],
                    fill=c, width=max(3, int(h * 0.09)), joint="curve")                       # cola
        hx, hy, r = x - h * 0.35, y - h * 0.7, h * 0.25
        self.d.ellipse([hx - r, hy - r, hx + r, hy + r], fill=c)                               # cabeza
        for s in (-1, 1):
            self.d.polygon([(hx + s * r * 0.9, hy - r * 0.2), (hx + s * r * 0.75, hy - r * 1.45),
                            (hx + s * r * 0.15, hy - r * 0.75)], fill=c)                       # orejas
            ex = hx + s * r * 0.4
            self.d.ellipse([ex - r * 0.16, hy - r * 0.2, ex + r * 0.16, hy + r * 0.12], fill=(120, 220, 90))
            self.d.ellipse([ex - r * 0.05, hy - r * 0.16, ex + r * 0.05, hy + r * 0.08], fill=(10, 10, 10))

    def dog(self, x, y, h, col):
        c = self.lit(col)
        self.d.rounded_rectangle([x - h * 0.45, y - h * 0.62, x + h * 0.35, y - h * 0.28], radius=h * 0.15, fill=c)
        for lx in (-0.38, -0.2, 0.12, 0.28):
            self.d.rectangle([x + lx * h, y - h * 0.35, x + lx * h + h * 0.09, y], fill=c)
        self.d.line([(x + h * 0.33, y - h * 0.55), (x + h * 0.55, y - h * 0.8)], fill=c, width=max(3, int(h * 0.07)))
        hx, hy, r = x - h * 0.5, y - h * 0.8, h * 0.22
        self.d.ellipse([hx - r, hy - r, hx + r, hy + r], fill=c)
        self.d.ellipse([hx - r * 1.5, hy - r * 0.1, hx - r * 0.4, hy + r * 0.6], fill=c)          # hocico
        self.d.ellipse([hx + r * 0.2, hy - r * 0.6, hx + r * 0.75, hy + r * 0.9], fill=_shade(c, 0.7))  # oreja
        self.d.ellipse([hx - r * 0.35, hy - r * 0.35, hx - r * 0.1, hy - r * 0.1], fill=(15, 15, 15))
        self.d.ellipse([hx - r * 1.6, hy - r * 0.05, hx - r * 1.25, hy + r * 0.25], fill=(15, 15, 15))

    def horse(self, x, y, h, col, cow=False):
        c = self.lit(col)
        self.d.ellipse([x - h * 0.5, y - h * 0.75, x + h * 0.45, y - h * 0.35], fill=c)
        for lx in (-0.42, -0.28, 0.2, 0.33):
            self.d.rectangle([x + lx * h, y - h * 0.45, x + lx * h + h * 0.08, y], fill=c)
        if cow:
            for sx, sy in ((-0.2, -0.6), (0.15, -0.5)):
                self.d.ellipse([x + sx * h, y + sy * h, x + sx * h + h * 0.18, y + sy * h + h * 0.12], fill=(30, 30, 30))
            self.d.ellipse([x - h * 0.75, y - h * 0.8, x - h * 0.45, y - h * 0.5], fill=c)
            self.d.ellipse([x - h * 0.8, y - h * 0.65, x - h * 0.55, y - h * 0.48], fill=self.lit((240, 170, 170)))
            return
        self.d.polygon([(x - h * 0.4, y - h * 0.68), (x - h * 0.55, y - h * 1.05), (x - h * 0.42, y - h * 1.1),
                        (x - h * 0.25, y - h * 0.7)], fill=c)                                        # cuello
        self.d.polygon([(x - h * 0.55, y - h * 1.05), (x - h * 0.82, y - h * 0.9), (x - h * 0.78, y - h * 0.82),
                        (x - h * 0.48, y - h * 0.9)], fill=c)                                        # cabeza
        self.d.line([(x - h * 0.43, y - h * 1.08), (x - h * 0.25, y - h * 0.75)], fill=_shade(c, 0.5),
                    width=max(3, int(h * 0.05)))                                                     # crin
        self.d.line([(x + h * 0.45, y - h * 0.65), (x + h * 0.58, y - h * 0.3)], fill=_shade(c, 0.5),
                    width=max(3, int(h * 0.05)))

    def person(self, x, y, h, col, helmet=False):
        skin = self.lit((230, 185, 150))
        c = self.lit(col)
        self.d.rectangle([x - h * 0.12, y - h * 0.38, x - h * 0.03, y], fill=self.lit((50, 55, 80)) if not helmet else c)
        self.d.rectangle([x + h * 0.03, y - h * 0.38, x + h * 0.12, y], fill=self.lit((50, 55, 80)) if not helmet else c)
        self.d.rounded_rectangle([x - h * 0.17, y - h * 0.75, x + h * 0.17, y - h * 0.35], radius=h * 0.06, fill=c)
        self.d.line([(x - h * 0.16, y - h * 0.7), (x - h * 0.28, y - h * 0.42)], fill=c, width=max(3, int(h * 0.07)))
        self.d.line([(x + h * 0.16, y - h * 0.7), (x + h * 0.28, y - h * 0.42)], fill=c, width=max(3, int(h * 0.07)))
        r = h * 0.12
        if helmet:
            self.d.ellipse([x - r * 1.4, y - h * 0.9 - r * 1.4, x + r * 1.4, y - h * 0.9 + r * 1.4], fill=c)
            self.d.ellipse([x - r, y - h * 0.9 - r * 0.7, x + r, y - h * 0.9 + r * 0.7], fill=(40, 60, 110))
            self.d.rectangle([x + h * 0.15, y - h * 0.72, x + h * 0.25, y - h * 0.45], fill=_shade(c, 0.85))
            return
        self.d.ellipse([x - r, y - h * 0.88 - r, x + r, y - h * 0.88 + r], fill=skin)
        self.d.chord([x - r * 1.05, y - h * 0.88 - r * 1.1, x + r * 1.05, y - h * 0.88 + r * 0.6], 180, 360,
                     fill=self.lit((70, 45, 30)))

    def robot(self, x, y, h, col):
        c = self.lit(col)
        self.d.rectangle([x - h * 0.13, y - h * 0.3, x - h * 0.04, y], fill=_shade(c, 0.8))
        self.d.rectangle([x + h * 0.04, y - h * 0.3, x + h * 0.13, y], fill=_shade(c, 0.8))
        self.d.rounded_rectangle([x - h * 0.22, y - h * 0.68, x + h * 0.22, y - h * 0.28], radius=h * 0.04, fill=c)
        self.d.rectangle([x - h * 0.08, y - h * 0.58, x + h * 0.08, y - h * 0.45], fill=(80, 200, 240))
        for s in (-1, 1):
            self.d.rectangle([x + s * h * 0.22, y - h * 0.65, x + s * h * 0.3, y - h * 0.38], fill=_shade(c, 0.85))
        self.d.rounded_rectangle([x - h * 0.15, y - h * 0.92, x + h * 0.15, y - h * 0.7], radius=h * 0.03, fill=c)
        for s in (-1, 1):
            ex = x + s * h * 0.07
            self.d.ellipse([ex - h * 0.035, y - h * 0.85, ex + h * 0.035, y - h * 0.78], fill=(90, 230, 255))
            self.g.ellipse([ex - h * 0.07, y - h * 0.88, ex + h * 0.07, y - h * 0.75], fill=(30, 100, 130))
        self.d.line([(x, y - h * 0.92), (x, y - h)], fill=c, width=max(2, int(h * 0.02)))
        self.d.ellipse([x - h * 0.03, y - h * 1.03, x + h * 0.03, y - h * 0.97], fill=(240, 60, 60))

    def car(self, x, y, h, col):
        c = self.lit(col)
        w = h * 2.4
        self.d.rounded_rectangle([x - w / 2, y - h * 0.75, x + w / 2, y - h * 0.2], radius=h * 0.15, fill=c)
        self.d.polygon([(x - w * 0.28, y - h * 0.72), (x - w * 0.16, y - h * 1.15), (x + w * 0.2, y - h * 1.15),
                        (x + w * 0.32, y - h * 0.72)], fill=c)
        self.d.polygon([(x - w * 0.22, y - h * 0.75), (x - w * 0.13, y - h * 1.07), (x + w * 0.17, y - h * 1.07),
                        (x + w * 0.26, y - h * 0.75)], fill=self.lit((170, 210, 235)))
        for wx in (x - w * 0.3, x + w * 0.3):
            self.d.ellipse([wx - h * 0.25, y - h * 0.45, wx + h * 0.25, y + h * 0.05], fill=(25, 25, 28))
            self.d.ellipse([wx - h * 0.1, y - h * 0.3, wx + h * 0.1, y - h * 0.1], fill=(170, 170, 175))
        if self.mode in ("night", "storm"):
            self.g.polygon([(x - w / 2, y - h * 0.55), (x - w * 1.3, y - h * 0.9), (x - w * 1.3, y)], fill=(110, 100, 50))

    def flower(self, x, y, h, col):
        self.d.line([(x, y), (x, y - h * 0.7)], fill=self.lit((50, 140, 50)), width=max(2, int(h * 0.06)))
        self.d.ellipse([x, y - h * 0.4, x + h * 0.25, y - h * 0.3], fill=self.lit((50, 140, 50)))
        cx, cy, r = x, y - h * 0.78, h * 0.13
        for k in range(6):
            a = k * math.pi / 3
            px, py = cx + math.cos(a) * r * 1.3, cy + math.sin(a) * r * 1.3
            self.d.ellipse([px - r, py - r, px + r, py + r], fill=self.lit(col))
        self.d.ellipse([cx - r * 0.8, cy - r * 0.8, cx + r * 0.8, cy + r * 0.8], fill=self.lit((250, 210, 60)))

    def cactus(self, x, y, h, col):
        c, w = self.lit(col), h * 0.1
        self.d.rounded_rectangle([x - w, y - h, x + w, y], radius=w, fill=c)
        self.d.rounded_rectangle([x - w * 3.2, y - h * 0.7, x - w * 1.6, y - h * 0.35], radius=w * 0.8, fill=c)
        self.d.rectangle([x - w * 3.2, y - h * 0.45, x - w * 0.5, y - h * 0.35], fill=c)
        self.d.rounded_rectangle([x + w * 1.6, y - h * 0.8, x + w * 3.2, y - h * 0.45], radius=w * 0.8, fill=c)
        self.d.rectangle([x + w * 0.5, y - h * 0.55, x + w * 3.2, y - h * 0.45], fill=c)

    def mushroom(self, x, y, h, col):
        self.d.rectangle([x - h * 0.12, y - h * 0.55, x + h * 0.12, y], fill=self.lit((240, 230, 210)))
        self.d.chord([x - h * 0.45, y - h * 1.0, x + h * 0.45, y - h * 0.2], 180, 360, fill=self.lit(col))
        for dx, dy in ((-0.2, -0.75), (0.12, -0.82), (0.25, -0.67)):
            self.d.ellipse([x + dx * h - h * 0.05, y + dy * h - h * 0.05, x + dx * h + h * 0.05, y + dy * h + h * 0.05],
                           fill=(250, 250, 245))

    def heart(self, x, y, h, col):
        c = self.lit(col) if self.mode != "night" else col
        r = h * 0.27
        self.g.ellipse([x - h * 0.6, y - h * 1.1, x + h * 0.6, y + h * 0.1], fill=_shade(col, 0.4))
        self.d.ellipse([x - 2 * r, y - h * 0.9, x, y - h * 0.9 + 2 * r], fill=c)
        self.d.ellipse([x, y - h * 0.9, x + 2 * r, y - h * 0.9 + 2 * r], fill=c)
        self.d.polygon([(x - 2 * r + 2, y - h * 0.9 + r * 1.3), (x + 2 * r - 2, y - h * 0.9 + r * 1.3), (x, y)], fill=c)

    def boat(self, x, y, h, col):
        self.d.polygon([(x - h * 0.6, y - h * 0.22), (x + h * 0.6, y - h * 0.22), (x + h * 0.42, y), (x - h * 0.42, y)],
                       fill=self.lit((120, 70, 40)))
        self.d.line([(x, y - h * 0.22), (x, y - h * 1.1)], fill=self.lit((90, 60, 35)), width=max(2, int(h * 0.04)))
        self.d.polygon([(x + h * 0.03, y - h * 1.05), (x + h * 0.03, y - h * 0.3), (x + h * 0.5, y - h * 0.3)],
                       fill=self.lit(col))
        self.d.polygon([(x - h * 0.03, y - h * 0.9), (x - h * 0.03, y - h * 0.32), (x - h * 0.38, y - h * 0.32)],
                       fill=_shade(self.lit(col), 0.9))

    def fish(self, x, y, h, col):
        c = self.lit(col)
        self.d.ellipse([x - h * 0.6, y - h * 0.3, x + h * 0.4, y + h * 0.3], fill=c)
        self.d.polygon([(x + h * 0.3, y), (x + h * 0.75, y - h * 0.35), (x + h * 0.75, y + h * 0.35)], fill=c)
        self.d.ellipse([x - h * 0.42, y - h * 0.1, x - h * 0.28, y + h * 0.04], fill=(15, 15, 15))

    def bird(self, x, y, h, col):
        c = col if self.mode == "day" else _shade(col, 0.5)
        self.d.line([(x - h, y - h * 0.4), (x - h * 0.4, y - h * 0.5), (x, y)], fill=c, width=max(3, int(h * 0.15)),
                    joint="curve")
        self.d.line([(x, y), (x + h * 0.4, y - h * 0.5), (x + h, y - h * 0.4)], fill=c, width=max(3, int(h * 0.15)),
                    joint="curve")

    def butterfly(self, x, y, h, col):
        c = self.lit(col) if self.mode != "night" else col
        for s in (-1, 1):
            self.d.ellipse([min(x, x + s * h * 0.6), y - h * 0.55, max(x, x + s * h * 0.6), y + h * 0.05], fill=c)
            self.d.ellipse([min(x, x + s * h * 0.45), y, max(x, x + s * h * 0.45), y + h * 0.4], fill=_shade(c, 0.8))
        self.d.line([(x, y - h * 0.5), (x, y + h * 0.4)], fill=(30, 30, 30), width=max(2, int(h * 0.08)))

    def rocket(self, x, y, h, col):
        c = self.lit(col) if self.mode != "space" else col
        w = h * 0.16
        self.g.polygon([(x - w, y), (x + w, y), (x, y + h * 0.7)], fill=(160, 90, 20))
        self.d.polygon([(x - w * 0.7, y), (x + w * 0.7, y), (x, y + h * 0.45)], fill=(255, 170, 40))
        self.d.rounded_rectangle([x - w, y - h * 0.75, x + w, y], radius=w * 0.5, fill=c)
        self.d.polygon([(x - w, y - h * 0.72), (x, y - h), (x + w, y - h * 0.72)], fill=(210, 40, 45))
        for s in (-1, 1):
            self.d.polygon([(x + s * w, y - h * 0.25), (x + s * w * 2, y + h * 0.02), (x + s * w, y)], fill=(210, 40, 45))
        self.d.ellipse([x - w * 0.5, y - h * 0.58, x + w * 0.5, y - h * 0.58 + w], fill=(80, 170, 230))

    def ufo(self, x, y, h, col):
        self.g.polygon([(x - h * 0.3, y), (x + h * 0.3, y), (x + h * 0.8, y + h * 3), (x - h * 0.8, y + h * 3)],
                       fill=(60, 110, 70))
        self.d.ellipse([x - h * 0.35, y - h * 0.55, x + h * 0.35, y + h * 0.05], fill=(150, 220, 240))
        self.d.ellipse([x - h, y - h * 0.25, x + h, y + h * 0.25], fill=col)
        for k in range(5):
            lx = x - h * 0.7 + k * h * 0.35
            self.d.ellipse([lx - h * 0.06, y - h * 0.05, lx + h * 0.06, y + h * 0.07], fill=(255, 230, 90))

    def draw_subject(self, kind, x, y, h, col):
        fn = {"cat": self.cat, "dog": self.dog, "house": self.house, "castle": self.castle, "tree": self.tree,
              "pine": self.pine, "flower": self.flower, "car": self.car, "boat": self.boat, "fish": self.fish,
              "person": self.person, "child": self.person, "robot": self.robot, "lighthouse": self.lighthouse,
              "heart": self.heart, "cactus": self.cactus, "mushroom": self.mushroom, "bird": self.bird,
              "rocket": self.rocket, "ufo": self.ufo, "butterfly": self.butterfly}.get(kind)
        if kind == "astronaut":
            return self.person(x, y, h, col, helmet=True)
        if kind in ("horse", "cow"):
            return self.horse(x, y, h, col, cow=kind == "cow")
        fn(x, y, h, col)

    # ------------------------------------------------------------------------------------------------ escena
    def paint(self) -> Image.Image:
        sc, W, H, rng = self.scene, self.W, self.horizon, self.rng
        self.sky()
        if self.mode in ("night", "space") or "star" in sc["sky"]:
            self.stars(260 if self.mode == "space" else 120)
        cx = W * rng.uniform(0.62, 0.82)
        top = min(H, W * 0.62)
        if "moon" in sc["sky"] or (self.mode == "night" and "sun" not in sc["sky"]):
            self.moon(cx, top * rng.uniform(0.22, 0.32), W * 0.07)
        elif self.mode in ("sunset", "sunrise"):
            self.sun(cx, H - W * 0.06, W * 0.09)
        elif "sun" in sc["sky"] or (self.mode == "day" and "cloud" not in sc["sky"]):
            self.sun(cx, top * 0.25, W * 0.065)
        if "planet" in sc["sky"] or self.mode == "space":
            self.planet(W * rng.uniform(0.25, 0.4), W * rng.uniform(0.3, 0.45), W * 0.13)
        if "cloud" in sc["sky"] or self.mode == "storm" or "rain" in sc["weather"]:
            for _ in range(5 if self.mode == "storm" or "rain" in sc["weather"] else 3):
                self.cloud(rng.uniform(0, W), rng.uniform(0.08, 0.3) * W, W * rng.uniform(0.12, 0.2))
        elif self.mode == "day":
            for _ in range(2):
                self.cloud(rng.uniform(0, W * 0.5), rng.uniform(0.1, 0.25) * W, W * rng.uniform(0.07, 0.1))
        self.ground()

        # sujetos: en el suelo de izquierda a derecha, en el cielo arriba y en el agua sobre las olas
        items = []
        for s in sc["subjects"]:
            items += [s] * max(1, min(s["count"], 6))
        if not items and self.place in ("field", "garden") and not sc["sky"]:
            items = [{"kind": "tree", "color": None, "count": 1}] * 2
        ground = [s for s in items if s["kind"] not in SKY_SUBJECTS and not (s["kind"] in WATER_SUBJECTS
                                                                               and self.place in ("sea", "beach", "lake"))]
        water = [s for s in items if s["kind"] in WATER_SUBJECTS and s not in ground]
        flying = [s for s in items if s["kind"] in SKY_SUBJECTS]
        ground.sort(key=lambda s: -SIZE.get(s["kind"], 0.2))   # lo grande detras
        floor = H if self.place not in ("sea", "beach", "lake") else getattr(self, "water_line", (H, H))[1]
        if self.place == "sea" and ground:   # en alta mar lo que va en tierra se apoya en una isla/roca
            floor = int(H + (W - H) * 0.55)
            self.d.ellipse([W * 0.05, floor - W * 0.04, W * 0.95, floor + W * 0.1], fill=self.lit((205, 185, 135)))
        n = len(ground)
        for k, s in enumerate(ground):
            slot = (k + 0.5) / n if n else 0.5
            x = W * (0.12 + 0.76 * slot) + rng.uniform(-0.04, 0.04) * W
            y = floor + (W - floor) * (0.25 + 0.5 * ((k * 0.37) % 1)) if n > 1 else floor + (W - floor) * 0.45
            base = SIZE.get(s["kind"], 0.2)
            h = W * base * ((1.6 if base < 0.2 else 1.15) if n == 1 else 1.0) * rng.uniform(0.92, 1.08)
            col = s["color"] or DEFAULT_COLOR.get(s["kind"], (200, 200, 200))
            if s["kind"] in ("flower", "tree") and not s["color"] and sc["colors"]:
                col = sc["colors"][k % len(sc["colors"])]
            self.shadow(x, y, h, s["kind"])
            self.draw_subject(s["kind"], x, y, h, col)
        for k, s in enumerate(water):
            y = H + (W - H) * (0.2 + 0.25 * (k % 2)) if s["kind"] == "boat" else H + (W - H) * rng.uniform(0.5, 0.9)
            self.draw_subject(s["kind"], W * (0.2 + 0.6 * (k + 0.5) / len(water)), y, W * SIZE[s["kind"]],
                              s["color"] or DEFAULT_COLOR[s["kind"]])
        for k, s in enumerate(flying):
            self.draw_subject(s["kind"], W * (0.15 + 0.7 * (k + 0.5) / len(flying)) + rng.uniform(-30, 30),
                              W * rng.uniform(0.12, 0.35), W * SIZE[s["kind"]], s["color"] or DEFAULT_COLOR[s["kind"]])

        self.weather()
        glow = self.glow.filter(ImageFilter.GaussianBlur(self.W / 40))
        mixed = np.asarray(self.img, dtype=np.int16) + np.asarray(glow, dtype=np.int16)
        img = Image.fromarray(np.clip(mixed, 0, 255).astype(np.uint8))
        if "fog" in sc["weather"]:
            img = Image.blend(img, Image.new("RGB", img.size, (200, 205, 212)), 0.35)
        return img.resize((self.W // 2, self.W // 2), Image.LANCZOS)

    def shadow(self, x, y, h, kind):
        if self.mode in ("night", "space") or kind in ("fish", "boat"):
            return
        w = h * (0.7 if kind not in ("car", "castle", "house") else 1.3)
        sh = Image.new("L", self.img.size, 0)
        ImageDraw.Draw(sh).ellipse([x - w * 0.5, y - h * 0.04, x + w * 0.5, y + h * 0.05], fill=90)
        self.img.paste((0, 0, 0), mask=sh.filter(ImageFilter.GaussianBlur(4)))

    def weather(self):
        W, rng, wx = self.W, self.rng, self.scene["weather"]
        if "rain" in wx or "storm" in wx:
            for _ in range(500):
                x, y = rng.uniform(-40, W), rng.uniform(0, W)
                self.d.line([(x, y), (x + 8, y + 34)], fill=(175, 190, 215), width=2)
        if "storm" in wx:
            x, y = rng.uniform(0.2, 0.8) * W, W * 0.12
            pts = [(x, y)]
            for _ in range(6):
                x += rng.uniform(-40, 40)
                y += W * 0.06
                pts.append((x, y))
            self.g.line(pts, fill=(160, 160, 220), width=14)
            self.d.line(pts, fill=(250, 250, 255), width=5)
        if "snow" in wx:
            for _ in range(350):
                x, y, r = rng.uniform(0, W), rng.uniform(0, W), rng.uniform(2, 6)
                self.d.ellipse([x - r, y - r, x + r, y + r], fill=(250, 250, 255))


def compose(prompt: str, seed: int = 0, size: int = 512) -> Image.Image:
    """Dibuja la escena que describe el prompt (determinista para un mismo prompt y semilla)."""
    scene = parse(prompt)
    # el azar depende de la escena (lugar, hora, clima, cielo) y de la semilla: cambiar solo el sujeto no mueve el fondo
    key = repr((scene["places"], scene["time"], scene["weather"], scene["sky"]))
    return Painter(scene, zlib.crc32(key.encode()) ^ seed, size).paint()


def understood(prompt: str) -> bool:
    """True si el compositor ha reconocido algo concreto (lugar, sujeto, cielo, hora o clima) en el prompt."""
    s = parse(prompt)
    toks = re.findall(r"[a-z0-9]+", plain(prompt))
    return bool(s["places"] or s["subjects"] or s["sky"] or s["weather"] or
                any(_lookup(t) and _lookup(t)[0] == "time" for t in toks))
