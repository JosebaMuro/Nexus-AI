"""Ejecutor aislado de herramientas de la comunidad (se lanza con ``python -I``).

Lee ``{"path": ..., "args": {...}}`` por stdin y escribe ``{"ok": bool, "result"|"error": ...}`` por stdout.
"""
import importlib.util
import json
import sys


def main() -> None:
    req = json.loads(sys.stdin.read())
    spec = importlib.util.spec_from_file_location("community_tool", req["path"])
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
        result = mod.run(req.get("args", {}), None)
        out = {"ok": True, "result": result}
    except Exception as e:  # noqa: BLE001
        out = {"ok": False, "error": f"{type(e).__name__}: {e}"}
    sys.stdout.write("\n@@NEXUS_RESULT@@" + json.dumps(out, ensure_ascii=False, default=str))


if __name__ == "__main__":
    main()
