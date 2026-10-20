"""Imagen: generacion (stable-diffusion.cpp > compositor de escenas sin modelo) y analisis
(LLM multimodal > estadisticas con PIL)."""
from __future__ import annotations

import base64
import io
import os
import re
import subprocess
import time
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageStat

from .. import log, paths
from . import scene

L = log.get("image")
COLOR_WORDS = {
    "rojo": (255, 40, 60), "red": (255, 40, 60), "azul": (40, 120, 255), "blue": (40, 120, 255),
    "verde": (40, 220, 120), "green": (40, 220, 120), "amarillo": (255, 220, 40), "yellow": (255, 220, 40),
    "morado": (170, 60, 255), "purple": (170, 60, 255), "violeta": (170, 60, 255), "rosa": (255, 90, 200),
    "pink": (255, 90, 200), "naranja": (255, 140, 30), "orange": (255, 140, 30), "cian": (0, 240, 255),
    "cyan": (0, 240, 255), "neon": (0, 255, 200), "oro": (255, 200, 80), "gold": (255, 200, 80),
    "fuego": (255, 90, 20), "fire": (255, 90, 20), "mar": (20, 140, 200), "ocean": (20, 140, 200),
    "bosque": (30, 160, 70), "forest": (30, 160, 70), "noche": (30, 30, 90), "night": (30, 30, 90),
    "atardecer": (255, 120, 80), "sunset": (255, 120, 80), "espacio": (90, 60, 200), "space": (90, 60, 200),
}


def _slug(t: str) -> str:
    import unicodedata
    t = "".join(c for c in unicodedata.normalize("NFKD", t.lower()) if not unicodedata.combining(c))
    return re.sub(r"[^a-z0-9]+", "-", t)[:40].strip("-") or "imagen"


class ImageEngine:
    def __init__(self, config, catalog, runtimes, profile_getter, llm=None) -> None:
        self.config, self.catalog, self.runtimes, self.profile_getter, self.llm = config, catalog, runtimes, profile_getter, llm

    def _model(self) -> dict | None:
        mid = self.config.get("media.image_model", "")
        entry = self.catalog.get(mid) if mid else None
        if entry is None:
            entry = next((m for m in self.catalog.list() if m["kind"] == "image"), None)
        return entry

    def status(self) -> dict:
        model = self._model()
        sd = self.runtimes.exe("sd", self.profile_getter())
        return {"model": model["id"] if model else None, "runtime": sd[1] if sd else None,
                "engine": "sd.cpp" if model and sd else "escena"}

    # ------------------------------------------------------------------ generacion
    def generate(self, prompt: str, negative: str = "", size: int | None = None, steps: int | None = None,
                 seed: int | None = None, idea: str = "") -> dict:
        """``idea`` es el texto original del usuario (si ``prompt`` ya viene mejorado): el compositor lo lee mejor."""
        seed = seed if seed is not None else int(time.time()) % 100000
        out = paths.sub("outputs", "images") / f"{int(time.time())}_{_slug(idea or prompt)}.png"
        model, sd = self._model(), self.runtimes.exe("sd", self.profile_getter())
        why = ("No hay modelo de imagen descargado" if not model else
               "No hay motor stable-diffusion.cpp para este equipo" if not sd else
               f"El modelo {model['id']} no es compatible con sd.cpp ({model['format']})")
        if model and sd and model["format"] in ("gguf", "safetensors", "ckpt"):
            try:
                return self._sdcpp(sd, model, self.english(prompt), negative, size, steps, seed, out)
            except Exception as e:  # noqa: BLE001
                L.warning("sd.cpp fallo (%s); usando el compositor de escenas", e)
                why = f"stable-diffusion.cpp fallo: {str(e).strip()[-200:]}"
        text = idea or prompt
        if scene.parse(text)["abstract"]:
            img, engine = self.procedural(text, seed, size or 512), "procedural"
        else:
            img, engine = scene.compose(text, seed, size or 512), "escena"
        img.save(out)
        note = (f"{why}: imagen compuesta sin IA a partir de las palabras del prompt. Para imagenes neuronales "
                "descarga un modelo de imagen en Modelos (p. ej. SDXS-512 en CPU).")
        if engine == "escena" and not scene.understood(text):
            note = "No he reconocido ningun elemento del prompt (lugar, objeto, animal, hora o clima). " + note
        return {"engine": engine, "url": f"/outputs/images/{out.name}", "path": str(out), "seed": seed, "note": note}

    def english(self, prompt: str) -> str:
        """Los modelos de difusion (CLIP) solo entienden ingles: un prompt en espanol se traduce (con el LLM si lo
        hay y, si no, con el diccionario del mejorador de prompts)."""
        from ..prompt_enhancer import looks_spanish, translate_rules
        if not looks_spanish(prompt):
            return prompt
        if self.llm is not None:
            try:
                if self.llm.has_real_llm():
                    t = self.llm.complete("Translate this image description into a concise English prompt for an image "
                                          f"generator. Reply with the prompt only.\n\n{prompt}", max_tokens=120,
                                          temperature=0.2)
                    t = t.strip().strip('"').splitlines()[0].strip() if t.strip() else ""
                    if 3 < len(t) < 600:
                        return t
            except Exception as e:  # noqa: BLE001
                L.info("Traduccion con LLM no disponible (%s): se usa el diccionario", e)
        return translate_rules(prompt)

    def _sdcpp(self, sd, model, prompt, negative, size, steps, seed, out: Path) -> dict:
        exe, variant = sd
        gen = model.get("gen") or {}
        size = size or gen.get("size") or self.profile_getter()["image"]["resolution"]
        steps = steps or gen.get("steps") or 4
        threads = self.profile_getter()["llm"]["n_threads"] if variant == "cpu" else 0
        cmd = [str(exe), "-m", model["path"], "-p", prompt, "--steps", str(steps), "--cfg-scale", str(gen.get("cfg", 1.0)),
               "-W", str(size), "-H", str(size), "-s", str(seed), "-o", str(out)]
        if negative:
            cmd += ["-n", negative]
        if threads:
            cmd += ["-t", str(max(threads, os.cpu_count() or 1))]
        t0 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, cwd=str(exe.parent),
                           creationflags=0x08000000 if os.name == "nt" else 0, encoding="utf-8", errors="replace")
        if r.returncode != 0 or not out.exists():
            raise RuntimeError((r.stderr or r.stdout)[-400:])
        return {"engine": f"sd.cpp ({variant})", "model": model["id"], "url": f"/outputs/images/{out.name}",
                "path": str(out), "seed": seed, "steps": steps, "size": size, "seconds": round(time.time() - t0, 1),
                "prompt": prompt}

    @staticmethod
    def procedural(prompt: str, seed: int, size: int = 512) -> Image.Image:
        """Arte abstracto determinista (campo de flujo con la paleta del prompt): solo si se pide algo abstracto."""
        rng = np.random.default_rng(zlib.crc32(prompt.encode()) ^ seed)
        low = prompt.lower()
        palette = [c for w, c in COLOR_WORDS.items() if w in low] or [tuple(int(x) for x in rng.integers(40, 255, 3))
                                                                      for _ in range(3)]
        bg = tuple(int(c * 0.12) for c in palette[0])
        img = Image.new("RGB", (size, size), bg)
        glow = Image.new("RGB", (size, size), (0, 0, 0))
        d, g = ImageDraw.Draw(img), ImageDraw.Draw(glow)
        freq = rng.uniform(1.5, 4.5)
        phase = rng.uniform(0, 6.28, 2)
        for i in range(260):
            x, y = rng.uniform(0, size, 2)
            col = palette[i % len(palette)]
            pts = [(x, y)]
            for _ in range(int(rng.integers(20, 70))):
                ang = np.sin(x / size * freq * 3.1 + phase[0]) * 3 + np.cos(y / size * freq * 3.1 + phase[1]) * 3
                x, y = x + np.cos(ang) * 4, y + np.sin(ang) * 4
                pts.append((x, y))
            d.line(pts, fill=col, width=1)
            g.line(pts, fill=col, width=3)
        for _ in range(int(rng.integers(3, 8))):
            cx, cy, r = *rng.uniform(0, size, 2), rng.uniform(size * 0.03, size * 0.12)
            g.ellipse([cx - r, cy - r, cx + r, cy + r], outline=palette[-1], width=2)
        glow = glow.filter(ImageFilter.GaussianBlur(6))
        mixed = np.asarray(img, dtype=np.int16) + np.asarray(glow, dtype=np.int16)
        return Image.fromarray(np.clip(mixed, 0, 255).astype(np.uint8))

    # ------------------------------------------------------------------ analisis
    def analyze(self, data: bytes, question: str = "") -> dict:
        img = Image.open(io.BytesIO(data)).convert("RGB")
        stats = self.describe_stats(img)
        if self.llm and self._vision_available():
            img.thumbnail((896, 896))
            buf = io.BytesIO()
            img.save(buf, "PNG")
            prompt = question or "Describe esta imagen con detalle en espanol."
            try:
                text = "".join(self.llm.stream([{"role": "user", "content": prompt,
                                                 "images": [base64.b64encode(buf.getvalue()).decode()]}], max_tokens=400))
                return {"engine": "vision-llm", "description": text, "stats": stats}
            except Exception as e:  # noqa: BLE001
                L.warning("Analisis con vision fallo: %s", e)
        return {"engine": "pil", "description": stats["summary"], "stats": stats}

    def _vision_available(self) -> bool:
        backend, model = self.llm.select()
        b = self.llm.backends.get(backend)
        if backend == "llamaserver":
            return b.supports_vision(model)
        return backend in ("ollama", "openai") and bool(self.config.get("media.vision_model"))

    @staticmethod
    def describe_stats(img: Image.Image) -> dict:
        small = img.copy()
        small.thumbnail((128, 128))
        q = small.quantize(5)
        pal = q.getpalette()[:15]
        counts = sorted(q.getcolors(), reverse=True)
        colors = ["#%02x%02x%02x" % tuple(pal[i * 3:i * 3 + 3]) for _, i in counts[:5]]
        brightness = sum(ImageStat.Stat(small.convert("L")).mean) / 255
        edges = np.asarray(small.convert("L").filter(ImageFilter.FIND_EDGES), dtype=np.float32).mean() / 255
        mood = "luminosa" if brightness > 0.6 else "oscura" if brightness < 0.35 else "de luz media"
        detail = "muy detallada" if edges > 0.12 else "con detalle moderado" if edges > 0.05 else "suave/minimalista"
        return {"size": img.size, "dominant_colors": colors, "brightness": round(brightness, 2),
                "edge_density": round(float(edges), 3),
                "summary": f"Imagen {img.size[0]}x{img.size[1]}, {mood}, {detail}; colores dominantes {', '.join(colors[:3])}."}
