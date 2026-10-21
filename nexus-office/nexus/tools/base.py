"""Definiciones comunes del sistema de herramientas.

Una herramienta es un modulo Python con:

    TOOL = {"name": ..., "description": ..., "parameters": {JSON schema}, "risk": "low|medium|high",
            "permissions": ["network", "filesystem", "code_exec", "system"], "version": "1.0", "author": "..."}

    def run(args: dict, ctx) -> dict | str: ...

Un modulo tambien puede declarar varias con ``TOOLS = [(TOOL_DICT, funcion), ...]``.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

RISKS = ("low", "medium", "high")
PERMISSIONS = ("network", "filesystem", "code_exec", "system", "memory", "media", "agents")


@dataclass
class ToolSpec:
    name: str
    description: str
    parameters: dict = field(default_factory=lambda: {"type": "object", "properties": {}})
    risk: str = "low"
    permissions: list[str] = field(default_factory=list)
    version: str = "1.0"
    author: str = "nexus"
    source: str = "builtin"          # builtin | community
    path: str = ""
    func: Callable | None = None
    enabled: bool = True
    timeout: float | None = None      # segundos; None = tools.timeout_s de la configuracion

    def public(self) -> dict:
        return {k: getattr(self, k) for k in ("name", "description", "parameters", "risk", "permissions",
                                               "version", "author", "source", "enabled")}


@dataclass
class ToolContext:
    services: Any = None
    agent: str = ""
    session_id: str = ""
    emit: Callable[[dict], None] | None = None
    remote: bool = False             # peticion originada por otro nodo cooperativo
    depth: int = 0                   # profundidad de delegacion entre agentes


_TYPES = {"string": str, "integer": int, "number": (int, float), "boolean": bool, "object": dict, "array": list}


def validate_args(schema: dict, args: dict) -> tuple[dict, list[str]]:
    """Valida y convierte tipos simples. Devuelve (args_limpios, errores)."""
    errors: list[str] = []
    props = schema.get("properties", {})
    clean = {}
    for key in schema.get("required", []):
        if key not in args or args[key] in (None, ""):
            errors.append(f"falta el parametro '{key}'")
    for key, value in (args or {}).items():
        spec = props.get(key)
        if spec is None:
            continue  # se ignoran parametros desconocidos
        typ = spec.get("type", "string")
        try:
            if typ == "integer" and not isinstance(value, int):
                value = int(float(value))
            elif typ == "number" and not isinstance(value, (int, float)):
                value = float(value)
            elif typ == "boolean" and not isinstance(value, bool):
                value = str(value).lower() in ("1", "true", "si", "sí", "yes")
            elif typ == "string" and not isinstance(value, str):
                value = str(value)
        except (TypeError, ValueError):
            errors.append(f"'{key}' debe ser {typ}")
            continue
        if "enum" in spec and value not in spec["enum"]:
            errors.append(f"'{key}' debe ser uno de {spec['enum']}")
        if typ in _TYPES and not isinstance(value, _TYPES[typ]):
            errors.append(f"'{key}' debe ser {typ}")
        clean[key] = value
    for key, spec in props.items():
        if key not in clean and "default" in spec:
            clean[key] = spec["default"]
    return clean, errors
