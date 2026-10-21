"""Compara la API de la edicion C++ con la de Python, ruta por ruta (mismas claves, tipos y, si se pide, valores).

    python tests/compare_api.py http://127.0.0.1:8790 http://127.0.0.1:8791 [--values]

Las dos ediciones deben arrancar con carpetas de datos nuevas (NEXUS_DATA_DIR) para que el contenido coincida.
Sale con codigo 1 si alguna ruta difiere en la forma (o en los valores con --values).
"""
import json
import sys
import urllib.error
import urllib.request

GETS = [
    "/api/ping", "/api/status", "/api/config", "/api/hardware", "/api/monitor", "/api/accel", "/api/service",
    "/api/downloads", "/api/drivers", "/api/runtimes", "/api/sessions", "/api/lounge", "/api/memory",
    "/api/memory/search?q=gimp", "/api/graph", "/api/graph?scope=", "/api/graph?scope=artista", "/api/graph/scopes",
    "/api/kg/overview", "/api/kg/nodes", "/api/kg/nodes?q=capa", "/api/kg/nodes?scope=artista&limit=5",
    "/api/kg/memories", "/api/kg/embeddings", "/api/kg/jobs", "/api/rag", "/api/rag?agent=artista",
    "/api/rag/search?q=capas%20y%20mascaras", "/api/rag/formats", "/api/outputs", "/api/prompt/styles", "/api/voice",
    "/api/agents", "/api/office/profiles", "/api/tools", "/api/proposals", "/api/models", "/api/models/recommended",
    "/api/lan",
]
# valores que dependen del momento o del proceso: solo se compara la forma
VOLATILE = {"ts", "time", "created", "last_seen", "started", "uptime_s", "cpu", "cpu_percent", "ram", "ram_used_gb",
            "ram_percent", "samples", "updated", "key", "urls", "version", "id", "session_id", "mtime", "disk_free_gb",
            "python", "score", "rank", "edition", "token", "load", "mem", "temp", "rate", "elapsed", "first_seen", "added",
            "t", "duration_s", "avg_cpu", "peak_ram_pct", "ram_pct", "proc_rss_gb", "session", "folder"}


def get(base, path):
    try:
        with urllib.request.urlopen(base + path, timeout=60) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def shape(v):
    if isinstance(v, dict):
        return {k: shape(x) for k, x in v.items()}
    if isinstance(v, list):
        return [shape(v[0])] if v else []
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "num"
    return type(v).__name__


def diff(a, b, path="", values=False, out=None):
    out = [] if out is None else out
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a.keys() | b.keys():
            if k not in a:
                out.append(f"{path}.{k}: falta en C++")
            elif k not in b:
                out.append(f"{path}.{k}: sobra en C++")
            elif k not in VOLATILE:
                diff(a[k], b[k], f"{path}.{k}", values, out)
        return out
    if isinstance(a, list) and isinstance(b, list):
        if values and len(a) != len(b):
            out.append(f"{path}: {len(a)} elementos en C++, {len(b)} en Python")
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", values, out)
            if not values:
                break
        return out
    if shape(a) != shape(b) and not (a is None or b is None):
        out.append(f"{path}: tipo {shape(a)} en C++, {shape(b)} en Python")
    elif values and a != b and not (isinstance(a, float) and isinstance(b, (int, float)) and abs(a - b) < 1e-6):
        out.append(f"{path}: {json.dumps(a, ensure_ascii=False)[:80]} en C++, {json.dumps(b, ensure_ascii=False)[:80]} en Python")
    return out


def main():
    cpp, py = sys.argv[1], sys.argv[2]
    values = "--values" in sys.argv
    bad = 0
    for p in GETS:
        sa, a = get(cpp, p)
        sb, b = get(py, p)
        d = ([f"estado {sa} en C++, {sb} en Python"] if sa != sb else []) + diff(a, b, "", values)
        print(("OK  " if not d else "DIF ") + p)
        for line in d[:25]:
            print("      " + line)
        bad += bool(d)
    print(f"\n{len(GETS) - bad}/{len(GETS)} rutas iguales")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
