"""Creacion multimedia para los agentes."""
from __future__ import annotations

import re

NON_EN = re.compile(r"\b(de|del|una|un|con|el|la|los|las|y|en|sobre|muy)\b|[áéíóúñ]", re.I)


def english_prompt(services, prompt: str) -> str:
    """Los modelos de difusion entienden mejor el ingles: se traduce con el LLM si lo hay."""
    if not NON_EN.search(prompt) or not services.llm.has_real_llm():
        return prompt
    try:
        out = services.llm.complete("Translate this image description into a concise English prompt for an image "
                                    f"generator. Reply with the prompt only.\n\n{prompt}", max_tokens=80, temperature=0.2)
        out = out.strip().strip('"').splitlines()[0].strip() if out.strip() else ""
        return out if 3 < len(out) < 400 else prompt
    except Exception:  # noqa: BLE001
        return prompt


def generate_image(args, ctx):
    negative = args.get("negative", "")
    if ctx.services.config.get("media.auto_enhance_prompts", True):
        from ...prompt_enhancer import enhance
        if ctx.emit:
            ctx.emit({"type": "progress", "text": "Mejorando el prompt"})
        enh = enhance(ctx.services, args["prompt"], "image", args.get("style", ""))
        prompt, negative = enh["prompt"], negative or enh["negative"]
    else:
        prompt = english_prompt(ctx.services, args["prompt"])
    res = ctx.services.images.generate(prompt, negative, seed=args.get("seed"))
    res["prompt"] = prompt
    if ctx.emit:
        ctx.emit({"type": "media", "kind": "image", "url": res["url"], "engine": res["engine"]})
    return {k: v for k, v in res.items() if k != "path"}  # el modelo no necesita rutas locales


def compose_music(args, ctx):
    res = ctx.services.music.generate(args["prompt"], float(args.get("seconds", 20)))
    if ctx.emit:
        ctx.emit({"type": "media", "kind": "audio", "url": res["url"], "engine": res["engine"]})
    return {k: v for k, v in res.items() if k != "path"}


def enhance_prompt(args, ctx):
    from ...prompt_enhancer import enhance
    return enhance(ctx.services, args["text"], args.get("target", "image"), args.get("style", ""))


TOOLS = [
    ({"name": "enhance_prompt", "description": "Mejora un prompt para imagen, musica o chat (estilo, detalle, calidad, traduccion)",
      "parameters": {"type": "object", "properties": {"text": {"type": "string"},
                                                      "target": {"type": "string", "enum": ["image", "music", "chat"], "default": "image"},
                                                      "style": {"type": "string"}}, "required": ["text"]},
      "risk": "low", "permissions": ["media"], "timeout": 120}, enhance_prompt),
    ({"name": "generate_image", "description": "Genera una imagen a partir de una descripcion (prompt en ingles da mejores resultados)",
      "parameters": {"type": "object", "properties": {"prompt": {"type": "string"}, "negative": {"type": "string"},
                                                      "seed": {"type": "integer"}}, "required": ["prompt"]},
      "risk": "low", "permissions": ["media"], "timeout": 900}, generate_image),
    ({"name": "compose_music", "description": "Compone una pieza musical (estado de animo/estilo en el prompt) y devuelve WAV y MIDI",
      "parameters": {"type": "object", "properties": {"prompt": {"type": "string"},
                                                      "seconds": {"type": "number", "default": 20}}, "required": ["prompt"]},
      "risk": "low", "permissions": ["media"], "timeout": 300}, compose_music),
]
