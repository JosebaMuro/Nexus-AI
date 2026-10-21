"""Mejorador de prompts para imagen, musica y chat.

Con LLM: reescribe el prompt siguiendo una plantilla por destino (y traduce al ingles para imagen).
Sin LLM: reglas (diccionario ES->EN, estilos, etiquetas de calidad, negativo, estructura de tarea).
"""
from __future__ import annotations

import re

STYLES = {
    "": "",
    "cinematografico": "cinematic still, dramatic lighting, shallow depth of field, film grain, anamorphic",
    "fotorrealista": "photorealistic, ultra detailed, natural lighting, 85mm lens, high dynamic range",
    "anime": "anime style, clean line art, vibrant colors, cel shading, studio quality",
    "oleo": "oil painting, thick brush strokes, rich textures, classical composition",
    "acuarela": "watercolor painting, soft washes, paper texture, delicate colors",
    "cyberpunk": "cyberpunk, neon lights, rain, reflections, futuristic city, high contrast",
    "fantasia": "epic fantasy art, magical atmosphere, volumetric light, intricate details",
    "3d": "3d render, octane render, global illumination, subsurface scattering, physically based",
    "pixel": "pixel art, 16-bit, limited palette, crisp pixels",
    "gotico": "dark gothic, moody, chiaroscuro, ornate details, eclipse light",
}
QUALITY = "highly detailed, sharp focus, masterpiece, best quality"
NEGATIVE = "blurry, low quality, lowres, deformed, bad anatomy, extra fingers, watermark, text, signature, jpeg artifacts"
ES_EN = {
    "un": "a", "una": "a", "el": "the", "la": "the", "los": "the", "las": "the", "de": "of", "del": "of the", "con": "with",
    "y": "and", "en": "in", "sobre": "on", "bajo": "under", "entre": "between", "al": "at the", "noche": "night", "dia": "day",
    "ciudad": "city", "bosque": "forest", "mar": "sea", "playa": "beach", "montana": "mountain", "montaña": "mountain",
    "cielo": "sky", "lluvia": "rain", "nieve": "snow", "niebla": "fog", "sol": "sun", "luna": "moon", "estrellas": "stars",
    "estrella": "star", "eclipse": "eclipse", "rio": "river", "lago": "lake", "castillo": "castle", "templo": "temple",
    "pagoda": "pagoda", "casa": "house", "calle": "street", "jardin": "garden", "flores": "flowers", "flor": "flower",
    "arbol": "tree", "arboles": "trees", "mujer": "woman", "hombre": "man", "nina": "girl", "niño": "boy", "nino": "boy",
    "niña": "girl", "gato": "cat", "perro": "dog", "dragon": "dragon", "caballo": "horse", "pajaro": "bird", "robot": "robot",
    "astronauta": "astronaut", "guerrero": "warrior", "samurai": "samurai", "geisha": "geisha", "retrato": "portrait",
    "paisaje": "landscape", "cristal": "crystal", "fuego": "fire", "agua": "water", "hielo": "ice", "oro": "gold",
    "plata": "silver", "rojo": "red", "roja": "red", "azul": "blue", "verde": "green", "negro": "black", "negra": "black",
    "blanco": "white", "blanca": "white", "amarillo": "yellow", "morado": "purple", "violeta": "violet", "rosa": "pink",
    "naranja": "orange", "gris": "gray", "oscuro": "dark", "oscura": "dark", "brillante": "glowing", "brillantes": "glowing",
    "antiguo": "ancient", "antigua": "ancient", "futurista": "futuristic", "magico": "magical", "mágico": "magical",
    "grande": "huge", "pequeño": "small", "pequeno": "small", "hermoso": "beautiful", "hermosa": "beautiful",
    "neon": "neon", "neón": "neon", "tormenta": "storm", "atardecer": "sunset", "amanecer": "sunrise", "volando": "flying",
    "nave": "spaceship", "espacio": "outer space", "planeta": "planet", "galaxia": "galaxy", "nebulosa": "nebula",
    "kimono": "kimono", "tatuajes": "tattoos", "tatuaje": "tattoo", "maquillaje": "makeup", "cara": "face", "ojos": "eyes",
}
MUSIC_HINTS = {
    "epica": "orchestral, epic taiko drums, brass, choir, 90 bpm, minor key, cinematic build-up",
    "triste": "solo piano, soft strings, 70 bpm, minor key, melancholic, reverb",
    "feliz": "upbeat pop, acoustic guitar, claps, 120 bpm, major key, bright",
    "calma": "ambient pads, soft piano, 75 bpm, major key, lo-fi, relaxing",
    "misterio": "dark ambient, low drones, sparse percussion, 85 bpm, phrygian mode, suspense",
    "electronica": "synthwave, analog synths, arpeggios, 124 bpm, four-on-the-floor, neon",
    "jazz": "jazz trio, upright bass, brushed drums, 110 bpm, dorian, swing",
}
TEMPLATES = {
    "image": ("Eres un experto en prompts para generadores de imagen (Stable Diffusion). Reescribe la idea del usuario "
              "como UN prompt en ingles, conciso y visual: sujeto, accion, entorno, estilo, iluminacion, color, composicion y "
              "calidad, separados por comas. Despues una linea 'NEGATIVE:' con lo que hay que evitar. Formato exacto:\n"
              "PROMPT: ...\nNEGATIVE: ..."),
    "music": ("Eres productor musical. Convierte la idea en una descripcion para un generador de musica: genero, estado de animo, "
              "instrumentos, tempo (BPM), tonalidad y estructura. Una sola linea en espanol empezando por 'PROMPT:'."),
    "chat": ("Mejora la peticion del usuario para un asistente de IA sin cambiar su intencion: aclara el objetivo, anade contexto "
             "util, el formato de respuesta deseado y restricciones. Responde solo con 'PROMPT:' seguido del prompt mejorado, en el "
             "mismo idioma del usuario."),
}


def _plain(t: str) -> str:
    import unicodedata
    return "".join(c for c in unicodedata.normalize("NFKD", t.lower()) if not unicodedata.combining(c))


def looks_spanish(text: str) -> bool:
    return bool(re.search(r"\b(de|una|un|con|el|la|los|las|del|que|y|en|sobre|muy)\b|[áéíóúñ¿¡]", text, re.I))


def translate_rules(text: str) -> str:
    words = re.findall(r"\w+|[^\w\s]", text.lower())
    out = []
    for w in words:
        out.append(ES_EN.get(w) or ES_EN.get(_plain(w)) or w)
    s = " ".join(out)
    s = re.sub(r"\s+([,.;:!?])", r"\1", s)
    s = re.sub(r"\b(a|the) (a|the)\b", r"\1", s)
    s = re.sub(r"\ba ([aeiou])", r"an \1", s)
    return s


def enhance_rules(text: str, target: str = "image", style: str = "") -> dict:
    text = text.strip()
    if target == "image":
        subject = translate_rules(text) if looks_spanish(text) else text
        parts = [subject, STYLES.get(style, style) or "", "dramatic lighting, rich colors, centered composition", QUALITY]
        return {"prompt": ", ".join(p for p in parts if p), "negative": NEGATIVE, "engine": "reglas"}
    if target == "music":
        from .media.music import detect_mood
        mood = detect_mood(text)
        return {"prompt": f"{text.rstrip('.')}. {MUSIC_HINTS.get(mood, '')}".strip(), "negative": "", "engine": "reglas",
                "mood": mood}
    # chat: estructura de tarea
    prompt = (f"Objetivo: {text.rstrip('.')}.\n"
              "Contexto: responde teniendo en cuenta lo que sabes de mi y de la conversacion.\n"
              "Formato: estructura la respuesta con puntos claros y un resumen final breve.\n"
              "Restricciones: si falta informacion, indica los supuestos; no inventes datos.")
    return {"prompt": prompt, "negative": "", "engine": "reglas"}


def enhance(services, text: str, target: str = "image", style: str = "", use_llm: bool = True) -> dict:
    text = (text or "").strip()
    if not text:
        return {"prompt": "", "negative": "", "engine": "vacio"}
    target = target if target in TEMPLATES else "image"
    if use_llm and services is not None and services.llm.has_real_llm():
        try:
            extra = f"\nEstilo deseado: {STYLES.get(style, style)}" if style else ""
            raw = services.llm.complete(f"{TEMPLATES[target]}{extra}\n\nIdea del usuario: {text}", max_tokens=220, temperature=0.4)
            raw = raw.replace("**", "")
            m = re.search(r"PROMPT:\s*(.+?)(?:\n\s*NEGATIVE:|\Z)", raw, re.S | re.I)
            prompt = (m.group(1) if m else raw).strip().strip('"')
            neg = re.search(r"NEGATIVE:\s*(.+)", raw, re.I)
            if len(prompt) > 8:
                out = {"prompt": prompt[:900], "negative": (neg.group(1).strip() if neg else (NEGATIVE if target == "image" else "")),
                       "engine": "llm"}
                if target == "image" and style and STYLES.get(style) and STYLES[style] not in out["prompt"]:
                    out["prompt"] += ", " + STYLES[style]
                return out
        except Exception:  # noqa: BLE001
            pass
    return enhance_rules(text, target, style)
