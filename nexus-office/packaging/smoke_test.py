"""Prueba de humo de un Nexus-Office ya instalado desde su instalador: arranca, descarga un modelo pequeno y genera
texto con cada opcion del selector de inicio (motor llama-server y llama.cpp integrado, en CPU), y comprueba que
los runtimes incluidos estan y arrancan.

    python packaging/smoke_test.py <carpeta instalada> [--python <python del venv>] [--modelo qwen3.5-0.8b]

Sale con error si algo falla. Lo usa .github/workflows/entrega.yml con cada instalador.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

PORT = 8799


def call(path: str, body: dict | None = None, timeout: float = 900) -> dict:
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method="POST" if body is not None else "GET",
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("home", type=Path)
    ap.add_argument("--python", default="")
    ap.add_argument("--modelo", default="qwen3.5-0.8b")
    a = ap.parse_args()
    home = a.home.resolve()
    py = a.python or str(next(p for p in (home / "venv" / "python.exe", home / "venv" / "Scripts" / "python.exe",
                                          home / "venv" / "bin" / "python3") if p.exists()))
    env = {**os.environ, "NEXUS_DATA_DIR": tempfile.mkdtemp(prefix="nexus-humo-"), "PYTHONUTF8": "1"}
    fails: list[str] = []

    def check(ok: bool, what: str) -> None:
        print(("OK    " if ok else "FALLO ") + what, flush=True)
        if not ok:
            fails.append(what)

    rt = subprocess.run([py, "-c", "import json; from nexus import runtimes as r; print(json.dumps({"
                         "'llama': r.bundled_variants('llama'), 'python': [p.name for p in r.bundled_llama_python()],"
                         "'openvino': str(r.bundled_exe('llama', 'openvino') or '')}))"],
                        cwd=home, env=env, capture_output=True, text=True)
    print(rt.stdout, rt.stderr[-2000:])
    bundled = json.loads(rt.stdout.strip().splitlines()[-1]) if rt.returncode == 0 else {}
    check(bool(bundled.get("llama")), f"llama-server incluido: {bundled.get('llama')}")
    check(bool(bundled.get("python")) or bool(os.environ.get("NEXUS_SIN_INTEGRADO")),
          f"motor integrado incluido: {bundled.get('python')}")
    if os.environ.get("NEXUS_CON_OPENVINO"):
        check("openvino" in bundled.get("llama", []) and bool(bundled.get("openvino")),
              f"llama-server OpenVINO incluido y arranca: {bundled.get('openvino')}")

    r = subprocess.run([py, "-m", "nexus", "pull", a.modelo], cwd=home, env=env, capture_output=True, text=True)
    print(r.stdout[-1500:], r.stderr[-1500:])
    check(r.returncode == 0 and "completado" in r.stdout, f"modelo {a.modelo} descargado")

    log = open(Path(env["NEXUS_DATA_DIR"]) / "servidor.log", "w")
    srv = subprocess.Popen([py, "-m", "nexus", "run", "--no-browser", "--port", str(PORT)], cwd=home, env=env,
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(240):
            try:
                opts = call("/api/accel", timeout=5)
                break
            except OSError:
                if srv.poll() is not None:
                    break
                time.sleep(1)
        else:
            opts = None
        check(bool(opts), "servidor arrancado")
        if not opts:
            return 1
        print(json.dumps({k: opts[k] for k in ("engines", "accels", "hardware")}, ensure_ascii=False, indent=1))
        engines = {e["id"]: e for e in opts["engines"]}
        wanted = ["llamaserver"] + ([] if os.environ.get("NEXUS_SIN_INTEGRADO") else ["llamacpp"])
        for eng in wanted:
            check(engines[eng]["available"], f"selector: {eng} disponible ({engines[eng]['note']})")
            try:
                res = call("/api/accel", {"engine": eng, "accel": "cpu", "remember": True})
                t = call("/api/accel/test", {})
                print(json.dumps({k: t.get(k) for k in ("ok", "backend", "text", "seconds", "tokens_s", "error", "effective")},
                                 ensure_ascii=False))
                check(t.get("ok") and t.get("backend") == eng and bool(t.get("text")),
                      f"{eng} en CPU genera texto ({t.get('tokens_s')} tok/s): {res.get('note') or 'sin avisos'}")
            except Exception as e:  # noqa: BLE001
                check(False, f"{eng}: {e}")
        for acc in ("gpu", "npu"):     # sin GPU ni NPU en el equipo de pruebas: debe decirlo y seguir funcionando
            res = call("/api/accel", {"engine": "auto", "accel": acc, "remember": True})
            t = call("/api/accel/test", {})
            check(t.get("ok"), f"{acc} pedida en un equipo sin {acc.upper()}: responde igual ({res.get('note')}; {t.get('backend')}: {t.get('error') or t.get('text')})")
        call("/api/accel", {"engine": "auto", "accel": "auto", "remember": False})
    finally:
        srv.terminate()
        try:
            srv.wait(30)
        except subprocess.TimeoutExpired:
            srv.kill()
        log.close()
        print((Path(env["NEXUS_DATA_DIR"]) / "servidor.log").read_text(errors="replace")[-3000:])
    print(f"\n{len(fails)} fallos" + (": " + "; ".join(fails) if fails else ""))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
