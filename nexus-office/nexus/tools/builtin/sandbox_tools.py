"""Ficheros y ejecucion de codigo confinados a data/sandbox."""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

from ... import paths
from ...proc import python_cmd


def _safe(rel: str) -> Path:
    root = paths.sandbox_dir().resolve()
    p = (root / rel).resolve()
    if not p.is_relative_to(root):
        raise ValueError("ruta fuera del sandbox")
    return p


def file_list(args, ctx):
    root = paths.sandbox_dir()
    base = _safe(args["path"]) if args.get("path") else root   # p.ej. proyectos/<nombre> de un ZIP importado
    files = [p.relative_to(root).as_posix() for p in sorted(base.rglob("*")) if p.is_file()]
    offset = max(0, int(args.get("offset", 0)))
    out = {"files": files[offset:offset + 500], "total": len(files), "offset": offset}
    if offset + 500 < len(files):
        out["next_offset"] = offset + 500
    return out


READ_CHUNK = 20000


def file_read(args, ctx):
    """Ficheros grandes por partes: offset (caracter) y next_offset si queda mas."""
    p = _safe(args["path"])
    offset = max(0, int(args.get("offset", 0)))
    size = max(1000, min(int(args.get("max_chars", READ_CHUNK)), 200_000))
    with open(p, encoding="utf-8", errors="replace") as f:
        if offset:
            f.read(offset)
        content = f.read(size)
        more = bool(f.read(1))
    out = {"path": args["path"], "content": content, "offset": offset}
    if more:
        out["next_offset"] = offset + len(content)
    return out


def file_write(args, ctx):
    p = _safe(args["path"])
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(args["content"], encoding="utf-8")
    return {"path": args["path"], "bytes": p.stat().st_size}


def python_exec(args, ctx):
    code_file = paths.sandbox_dir() / "_exec.py"
    code_file.write_text(args["code"], encoding="utf-8")
    try:
        r = subprocess.run([*python_cmd(), "-I", "-X", "utf8", str(code_file)], capture_output=True, text=True,
                           timeout=int(args.get("timeout", 20)), cwd=str(paths.sandbox_dir()), encoding="utf-8",
                           errors="replace", creationflags=0x08000000 if os.name == "nt" else 0)
        return {"returncode": r.returncode, "stdout": r.stdout[-6000:], "stderr": r.stderr[-3000:]}
    except subprocess.TimeoutExpired:
        return {"error": "tiempo agotado"}
    finally:
        code_file.unlink(missing_ok=True)


TOOLS = [
    ({"name": "file_list", "description": ("Lista los ficheros del espacio de trabajo (sandbox). Los proyectos ZIP que "
                                            "sube el usuario estan en proyectos/<nombre>"),
      "parameters": {"type": "object", "properties": {"path": {"type": "string", "description": "subcarpeta opcional"},
                                                      "offset": {"type": "integer", "default": 0,
                                                                 "description": "para listas largas: next_offset"}}},
      "risk": "low", "permissions": ["filesystem"]}, file_list),
    ({"name": "file_read", "description": ("Lee un fichero de texto del sandbox. Los grandes se leen por partes: si "
                                           "devuelve next_offset, vuelve a llamar con offset para seguir"),
      "parameters": {"type": "object", "properties": {"path": {"type": "string"},
                                                      "offset": {"type": "integer", "default": 0},
                                                      "max_chars": {"type": "integer", "default": 20000}},
                     "required": ["path"]}, "risk": "low", "permissions": ["filesystem"]}, file_read),
    ({"name": "file_write", "description": "Escribe un fichero de texto en el sandbox",
      "parameters": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                     "required": ["path", "content"]}, "risk": "medium", "permissions": ["filesystem"]}, file_write),
    ({"name": "python_exec", "description": "Ejecuta codigo Python en un proceso aislado dentro del sandbox",
      "parameters": {"type": "object", "properties": {"code": {"type": "string"}, "timeout": {"type": "integer", "default": 20}},
                     "required": ["code"]}, "risk": "high", "permissions": ["code_exec"], "timeout": 120}, python_exec),
]
