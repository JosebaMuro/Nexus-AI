"""Protocolo de herramientas basado en texto: funciona con cualquier modelo/backend.

El modelo emite:  <tool>{"name": "calculator", "args": {"expression": "2+2"}}</tool>
Y recibe:         <tool_result name="calculator">{"result": 4}</tool_result>
"""
from __future__ import annotations

import json
import re

TOOL_RE = re.compile(r"<tool>\s*(\{.*?\})\s*</tool>", re.S)
FENCE_RE = re.compile(r"```(?:json|tool)?\s*(\{\s*\"(?:name|tool)\".*?\})\s*```", re.S)
RESULT_RE = re.compile(r'<tool_result name="([^"]+)">(.*?)</tool_result>', re.S)


def parse_tool_call(text: str) -> tuple[str, dict, str] | None:
    """Devuelve (nombre, args, texto_previo) o None."""
    for rx in (TOOL_RE, FENCE_RE):
        m = rx.search(text)
        if not m:
            continue
        raw = m.group(1)
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            try:  # modelos pequenos a veces usan comillas simples
                data = json.loads(raw.replace("'", '"'))
            except json.JSONDecodeError:
                continue
        name = data.get("name") or data.get("tool")
        if name:
            args = data.get("args") or data.get("arguments") or data.get("parameters") or {}
            return str(name), args if isinstance(args, dict) else {"input": args}, text[:m.start()].strip()
    return None


def format_result(name: str, result) -> str:
    body = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False, default=str)
    if len(body) > 6000:
        body = body[:6000] + "...[recortado]"
    return f'<tool_result name="{name}">{body}</tool_result>'


def tool_instructions(tools: list[dict]) -> str:
    if not tools:
        return ""
    lines = ["Tienes herramientas. Para usar una, responde SOLO con una linea asi:",
             '<tool>{"name": "NOMBRE", "args": {...}}</tool>',
             "Recibiras el resultado en <tool_result>. Despues responde al usuario con normalidad.",
             "Usa herramientas solo cuando aporten algo. Herramientas disponibles:"]
    for t in tools:
        props = t.get("parameters", {}).get("properties", {})
        args = ", ".join(f"{k}: {v.get('type', 'string')}" for k, v in props.items())
        lines.append(f"- {t['name']}({args}): {t['description']}")
    return "\n".join(lines)
