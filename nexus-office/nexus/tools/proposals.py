"""Propuestas de herramientas hechas por los agentes, con supervision humana.

Flujo: agente propone -> analisis estatico (AST) -> cola de revision -> el administrador
revisa/edita el codigo, puede probarlo en el sandbox -> aprueba (se instala en
data/tools/community y se recarga el registro) o rechaza (el motivo se guarda como leccion).
"""
from __future__ import annotations

import ast
import json
import re
import time
import uuid

from .. import log, paths
from .base import RISKS

L = log.get("proposals")
DANGEROUS_IMPORTS = {"subprocess": "high", "os": "medium", "shutil": "high", "socket": "medium", "ctypes": "high",
                     "multiprocessing": "high", "winreg": "high", "importlib": "high", "pickle": "medium",
                     "requests": "medium", "httpx": "medium", "urllib": "medium", "sys": "medium", "pathlib": "medium",
                     "threading": "medium", "signal": "high", "marshal": "high", "builtins": "high"}
DANGEROUS_CALLS = {"eval": "high", "exec": "high", "compile": "high", "__import__": "high", "open": "medium",
                   "globals": "high", "locals": "medium", "getattr": "medium", "setattr": "high", "delattr": "high",
                   "system": "high", "popen": "high", "remove": "high", "rmtree": "high", "unlink": "high",
                   "kill": "high"}
NAME_RE = re.compile(r"^[a-z][a-z0-9_]{2,40}$")


def _max_risk(*risks: str) -> str:
    return max(risks, key=RISKS.index) if risks else "low"


def analyze(code: str, name: str = "") -> dict:
    errors, warnings, imports, calls = [], [], [], []
    try:
        tree = ast.parse(code)
    except SyntaxError as e:
        return {"ok": False, "errors": [f"Error de sintaxis linea {e.lineno}: {e.msg}"], "warnings": [],
                "imports": [], "calls": [], "suggested_risk": "high", "manifest": None}
    manifest, has_run = None, False
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "TOOL" for t in node.targets):
            try:
                manifest = ast.literal_eval(node.value)
            except ValueError:
                errors.append("TOOL debe ser un diccionario literal")
        if isinstance(node, ast.FunctionDef) and node.name == "run":
            has_run = len(node.args.args) >= 1
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imports += [a.name.split(".")[0] for a in node.names]
        elif isinstance(node, ast.ImportFrom) and node.module:
            imports.append(node.module.split(".")[0])
        elif isinstance(node, ast.Call):
            fn = node.func
            nm = fn.id if isinstance(fn, ast.Name) else fn.attr if isinstance(fn, ast.Attribute) else ""
            if nm in DANGEROUS_CALLS:
                calls.append(nm)
        elif isinstance(node, ast.Attribute) and node.attr.startswith("__") and node.attr not in ("__name__", "__doc__"):
            warnings.append(f"acceso a atributo interno {node.attr}")
    if manifest is None:
        errors.append("Falta TOOL = {...} con name/description/parameters")
    elif not isinstance(manifest, dict) or not manifest.get("name"):
        errors.append("TOOL necesita al menos 'name' y 'description'")
    if not has_run:
        errors.append("Falta la funcion run(args, ctx)")
    if name and manifest and isinstance(manifest, dict) and manifest.get("name") != name:
        warnings.append(f"el nombre del manifiesto ({manifest.get('name')}) no coincide con '{name}'")
    risks = [DANGEROUS_IMPORTS[i] for i in imports if i in DANGEROUS_IMPORTS] + [DANGEROUS_CALLS[c] for c in calls]
    for i in sorted(set(imports)):
        if i in DANGEROUS_IMPORTS:
            warnings.append(f"importa '{i}' (riesgo {DANGEROUS_IMPORTS[i]})")
    for c in sorted(set(calls)):
        warnings.append(f"llama a '{c}' (riesgo {DANGEROUS_CALLS[c]})")
    return {"ok": not errors, "errors": errors, "warnings": warnings, "imports": sorted(set(imports)),
            "calls": sorted(set(calls)), "suggested_risk": _max_risk("low", *risks), "manifest": manifest}


class ProposalManager:
    def __init__(self, registry, supervisor, learner=None) -> None:
        self.registry, self.supervisor, self.learner = registry, supervisor, learner
        self.root = paths.proposals_dir()

    def _dir(self, pid: str):
        return self.root / pid

    def _save(self, prop: dict) -> dict:
        d = self._dir(prop["id"])
        d.mkdir(parents=True, exist_ok=True)
        (d / "tool.py").write_text(prop["code"], encoding="utf-8")
        (d / "proposal.json").write_text(json.dumps({k: v for k, v in prop.items() if k != "code"}, indent=1,
                                                    ensure_ascii=False), encoding="utf-8")
        return prop

    def create(self, name: str, description: str, code: str, parameters: dict | None = None, risk: str = "medium",
               rationale: str = "", author: str = "") -> dict:
        name = re.sub(r"[^a-z0-9_]", "_", name.lower().strip())[:40]
        if not NAME_RE.match(name):
            raise ValueError("nombre invalido (minusculas, numeros y _; 3-40 caracteres)")
        if "TOOL" not in code:  # el agente solo envio la funcion: completamos el manifiesto
            manifest = {"name": name, "description": description,
                        "parameters": parameters or {"type": "object", "properties": {}},
                        "risk": risk if risk in RISKS else "medium", "permissions": [], "version": "1.0", "author": author}
            code = f"TOOL = {json.dumps(manifest, ensure_ascii=False, indent=4)}\n\n\n{code}"
        a = analyze(code, name)
        prop = {"id": time.strftime("%Y%m%d%H%M%S") + "-" + uuid.uuid4().hex[:4], "name": name,
                "description": description, "declared_risk": risk, "rationale": rationale, "author": author,
                "created": time.time(), "status": "pendiente", "analysis": {k: v for k, v in a.items() if k != "manifest"},
                "code": code, "review": ""}
        if self.registry.get(name) and self.registry.get(name).source == "builtin":
            prop["analysis"]["errors"].append("ya existe una herramienta integrada con ese nombre")
            prop["analysis"]["ok"] = False
        L.info("Nueva propuesta de herramienta '%s' por %s (riesgo sugerido %s)", name, author, a["suggested_risk"])
        return self._save(prop)

    def get(self, pid: str) -> dict | None:
        d = self._dir(pid)
        if not (d / "proposal.json").exists():
            return None
        prop = json.loads((d / "proposal.json").read_text(encoding="utf-8"))
        prop["code"] = (d / "tool.py").read_text(encoding="utf-8")
        return prop

    def list(self, status: str | None = None) -> list[dict]:
        out = []
        for d in sorted(self.root.iterdir(), reverse=True):
            p = self.get(d.name) if d.is_dir() else None
            if p and (not status or p["status"] == status):
                out.append(p)
        return out

    def update_code(self, pid: str, code: str) -> dict:
        prop = self.get(pid)
        prop["code"] = code
        prop["analysis"] = {k: v for k, v in analyze(code, prop["name"]).items() if k != "manifest"}
        return self._save(prop)

    def test(self, pid: str, args: dict) -> dict:
        """Ejecuta la propuesta en el sandbox aislado sin instalarla."""
        prop = self.get(pid)
        if not prop["analysis"]["ok"]:
            return {"error": "la propuesta tiene errores: " + "; ".join(prop["analysis"]["errors"])}
        try:
            return {"result": self.supervisor._run_sandboxed(self._dir(pid) / "tool.py", args, 20)}
        except Exception as e:  # noqa: BLE001
            return {"error": str(e)}

    def approve(self, pid: str, reviewer: str = "admin", note: str = "") -> dict:
        prop = self.get(pid)
        if not prop:
            raise KeyError(pid)
        a = analyze(prop["code"], prop["name"])
        if not a["ok"]:
            raise ValueError("No se puede aprobar: " + "; ".join(a["errors"]))
        manifest = a["manifest"]
        if RISKS.index(manifest.get("risk", "high")) < RISKS.index(a["suggested_risk"]):
            note = (note + f" | riesgo elevado a {a['suggested_risk']} por el analisis").strip(" |")
            prop["code"] = re.sub(r"([\"']risk[\"']\s*:\s*)[\"']\w+[\"']",
                                  lambda m: f'{m.group(1)}"{a["suggested_risk"]}"', prop["code"], count=1)
            if not analyze(prop["code"], prop["name"])["ok"]:
                raise ValueError("No se pudo ajustar el riesgo del manifiesto automaticamente")
        (paths.community_tools_dir() / f"{prop['name']}.py").write_text(prop["code"], encoding="utf-8")
        prop.update(status="aprobada", review=note, reviewer=reviewer, reviewed=time.time())
        self._save(prop)
        self.registry.reload()
        L.info("Propuesta %s aprobada e instalada como herramienta '%s'", pid, prop["name"])
        return prop

    def reject(self, pid: str, reason: str, reviewer: str = "admin") -> dict:
        prop = self.get(pid)
        prop.update(status="rechazada", review=reason, reviewer=reviewer, reviewed=time.time())
        self._save(prop)
        if self.learner:  # el sistema aprende por que se rechazo
            self.learner.memory.remember(f"La herramienta propuesta '{prop['name']}' fue rechazada: {reason}", "lesson",
                                         0.8, source="revision-herramientas", agent=prop["author"].replace("agente:", ""))
        return prop

    def uninstall(self, name: str) -> bool:
        f = paths.community_tools_dir() / f"{name}.py"
        if f.exists():
            f.unlink()
            self.registry.reload()
            return True
        return False
