"""Plantillas de chat para los motores que no la aplican solos (Genie, ExecuTorch y respaldo de los demas).

Se elige por los tokens especiales del tokenizer del modelo (Llama 3, ChatML de Qwen, Phi, Granite, Mistral, Gemma).
Si el modelo trae ``chat_template.jinja`` (o ``chat_template`` en tokenizer_config.json) y jinja2 esta instalado,
se usa esa plantilla, que es la exacta del modelo.
"""
from __future__ import annotations

import json
from pathlib import Path


def _leer(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def detectar(carpeta: Path) -> str:
    t = _leer(carpeta / "tokenizer.json")[:2_000_000] + _leer(carpeta / "tokenizer_config.json")
    if "<|start_header_id|>" in t:
        return "llama3"
    if "<|im_start|>" in t:
        return "chatml"
    if "<|start_of_role|>" in t:
        return "granite"
    if "<start_of_turn>" in t:
        return "gemma"
    if "<|assistant|>" in t:
        return "phi"
    if "[INST]" in t:
        return "mistral"
    return "simple"


def plantilla_jinja(carpeta: Path) -> str:
    j = _leer(carpeta / "chat_template.jinja")
    if j:
        return j
    try:
        cfg = json.loads(_leer(carpeta / "tokenizer_config.json") or "{}")
    except ValueError:
        return ""
    tpl = cfg.get("chat_template", "")
    if isinstance(tpl, list):   # varias plantillas con nombre: la «default»
        tpl = next((x.get("template", "") for x in tpl if x.get("name") == "default"), "")
    return tpl if isinstance(tpl, str) else ""


def con_jinja(carpeta: Path, msgs: list[dict]) -> str | None:
    tpl = plantilla_jinja(carpeta)
    if not tpl:
        return None
    try:
        import jinja2   # noqa: PLC0415
        env = jinja2.Environment(trim_blocks=True, lstrip_blocks=True)
        cfg = {}
        try:
            cfg = json.loads(_leer(carpeta / "tokenizer_config.json") or "{}")
        except ValueError:
            pass

        def tok(v):
            return v.get("content", "") if isinstance(v, dict) else (v or "")
        return env.from_string(tpl).render(messages=msgs, add_generation_prompt=True, enable_thinking=False,
                                           bos_token=tok(cfg.get("bos_token")), eos_token=tok(cfg.get("eos_token")))
    except Exception:  # noqa: BLE001 - plantilla con extensiones que jinja2 no conoce: se usa la propia
        return None


def formatear(tipo: str, msgs: list[dict]) -> str:
    out = ""
    first_user = True
    pending_sys = ""
    for m in msgs:
        r, c = m["role"], m["content"]
        if tipo == "llama3":
            if not out:
                out = "<|begin_of_text|>"
            out += f"<|start_header_id|>{r}<|end_header_id|>\n\n{c}<|eot_id|>"
        elif tipo == "chatml":
            out += f"<|im_start|>{r}\n{c}<|im_end|>\n"
        elif tipo == "granite":
            out += f"<|start_of_role|>{r}<|end_of_role|>{c}<|end_of_text|>\n"
        elif tipo == "phi":
            out += f"<|{r}|>\n{c}<|end|>\n"
        elif tipo == "gemma":
            if r == "system":   # Gemma no tiene papel de sistema: va delante del primer mensaje del usuario
                pending_sys += c + "\n\n"
                continue
            out += f"<start_of_turn>{'model' if r == 'assistant' else 'user'}\n{pending_sys if r == 'user' else ''}{c}<end_of_turn>\n"
            if r == "user":
                pending_sys = ""
        elif tipo == "mistral":
            if r == "system":
                pending_sys += c + "\n\n"
            elif r == "user":
                out += ("<s>" if first_user else "") + f"[INST] {pending_sys}{c} [/INST]"
                pending_sys, first_user = "", False
            else:
                out += f" {c}</s>"
        else:
            out += ("Usuario: " if r == "user" else "" if r == "system" else "Asistente: ") + c + "\n\n"
    out += {"llama3": "<|start_header_id|>assistant<|end_header_id|>\n\n", "chatml": "<|im_start|>assistant\n",
            "granite": "<|start_of_role|>assistant<|end_of_role|>", "phi": "<|assistant|>\n",
            "gemma": "<start_of_turn>model\n", "mistral": "", "simple": "Asistente: "}.get(tipo, "")
    return out


def prompt(carpeta: Path, msgs: list[dict]) -> str:
    return con_jinja(carpeta, msgs) or formatear(detectar(carpeta), msgs)


FINALES = ("<|im_end|>", "<|eot_id|>", "<|end|>", "<end_of_turn>", "</s>", "<|end_of_text|>", "<|endoftext|>")


def limpiar(texto: str) -> str:
    """Quita marcas de fin de turno que algun runtime deja pasar."""
    for f in FINALES:
        texto = texto.replace(f, "")
    return texto
