"""Descubrimiento modular de herramientas: integradas (nexus/tools/builtin) y de la comunidad
(data/tools/community, aprobadas por un administrador). Los manifiestos de la comunidad se leen
por AST sin ejecutar su codigo; con sandbox activo se ejecutan en un subproceso aislado.
"""
from __future__ import annotations

import ast
import importlib
import importlib.util
import json
import pkgutil
import threading
from pathlib import Path

from .. import log, paths
from .base import RISKS, ToolSpec

L = log.get("tools")


def read_manifest(path: Path) -> dict | None:
    """Extrae el dict TOOL de un fichero sin ejecutarlo."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except (SyntaxError, OSError):
        return None
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "TOOL" for t in node.targets):
            try:
                val = ast.literal_eval(node.value)
                return val if isinstance(val, dict) else None
            except ValueError:
                return None
    return None


class ToolRegistry:
    def __init__(self, sandbox_community: bool = True) -> None:
        self.sandbox_community = sandbox_community
        self.tools: dict[str, ToolSpec] = {}
        self._lock = threading.RLock()
        self._disabled: set[str] = set(self._load_disabled())
        self.reload()

    def _disabled_file(self) -> Path:
        return paths.sub("tools") / "disabled.json"

    def _load_disabled(self) -> list[str]:
        f = self._disabled_file()
        return json.loads(f.read_text()) if f.exists() else []

    def set_enabled(self, name: str, enabled: bool) -> None:
        (self._disabled.discard if enabled else self._disabled.add)(name)
        self._disabled_file().write_text(json.dumps(sorted(self._disabled)))
        if name in self.tools:
            self.tools[name].enabled = enabled

    def reload(self) -> None:
        found: dict[str, ToolSpec] = {}
        from . import builtin
        for mod_info in pkgutil.iter_modules(builtin.__path__):
            try:
                mod = importlib.import_module(f"{builtin.__name__}.{mod_info.name}")
                mod = importlib.reload(mod)
            except Exception as e:  # noqa: BLE001
                L.error("No se pudo cargar la herramienta integrada %s: %s", mod_info.name, e)
                continue
            pairs = list(getattr(mod, "TOOLS", []))
            if hasattr(mod, "TOOL") and hasattr(mod, "run"):
                pairs.append((mod.TOOL, mod.run))
            for manifest, func in pairs:
                spec = self._spec(manifest, "builtin", mod.__file__, func)
                if spec:
                    found[spec.name] = spec
        for path in sorted(paths.community_tools_dir().glob("*.py")):
            manifest = read_manifest(path)
            if not manifest:
                L.warning("Herramienta de comunidad sin manifiesto valido: %s", path.name)
                continue
            func = None
            if not self.sandbox_community:
                spec_ = importlib.util.spec_from_file_location(f"nexus_community_{path.stem}", path)
                mod = importlib.util.module_from_spec(spec_)
                try:
                    spec_.loader.exec_module(mod)
                    func = mod.run
                except Exception as e:  # noqa: BLE001
                    L.error("Fallo cargando %s: %s", path.name, e)
                    continue
            spec = self._spec(manifest, "community", str(path), func)
            if spec and spec.name not in found:  # una de comunidad nunca sustituye a una integrada
                found[spec.name] = spec
        with self._lock:
            self.tools = found
        L.info("%d herramientas cargadas (%d de comunidad)", len(found),
               sum(1 for t in found.values() if t.source == "community"))

    def _spec(self, m: dict, source: str, path: str, func) -> ToolSpec | None:
        if not m.get("name") or not m.get("description"):
            return None
        risk = m.get("risk", "medium")
        if risk not in RISKS:
            risk = "high"
        return ToolSpec(name=m["name"], description=m["description"],
                        parameters=m.get("parameters") or {"type": "object", "properties": {}},
                        risk=risk, permissions=list(m.get("permissions", [])), version=str(m.get("version", "1.0")),
                        author=m.get("author", "nexus" if source == "builtin" else "comunidad"), source=source,
                        path=path, func=func, enabled=m["name"] not in self._disabled,
                        timeout=float(m["timeout"]) if m.get("timeout") else None)

    def get(self, name: str) -> ToolSpec | None:
        return self.tools.get(name)

    def for_agent(self, allowed: list[str]) -> list[ToolSpec]:
        if not allowed:
            return []
        every = "*" in allowed
        return [t for t in self.tools.values() if t.enabled and (every or t.name in allowed)]

    def list(self) -> list[dict]:
        return [t.public() for t in sorted(self.tools.values(), key=lambda t: (t.source, t.name))]
