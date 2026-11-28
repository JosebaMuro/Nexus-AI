"""FreeLLMAPI portable para los paquetes offline de Agente IA (bundled/runtimes/freellmapi).

FreeLLMAPI (https://github.com/tashfeenahmed/freellmapi, licencia MIT) junta los planes gratis de decenas de
proveedores de IA (Groq, Gemini, Mistral, OpenRouter, Cerebras...) detras de un solo endpoint /v1 compatible con
OpenAI. Es la misma pasarela que lleva el proyecto «9000 tokens» (Infinito), y se empaqueta igual: Node.js de la
plataforma + el servidor ya compilado + su panel web, sin descargar nada al instalar.

Se construye una vez (TypeScript y panel web, independientes de la plataforma) y despues, para cada destino, se
instalan las dependencias de produccion del servidor para ese sistema y procesador (npm --os/--cpu, sin compilar:
better-sqlite3 llega precompilado con prebuild-install y sharp con su paquete @img de la plataforma). Todo funciona
desde Linux para los seis destinos de escritorio.

    python packaging/fetch_freellmapi.py --target linux-x86_64 [--ref main] [--smoke]
    python packaging/fetch_freellmapi.py --target all

Deja .cache/desktop-runtimes/<target>/freellmapi/ con:
    node/            Node.js oficial (bin/node o node.exe)
    app/             servidor (server/dist), panel (client/dist), shared y node_modules de produccion
    freellmapi.json  versiones (freellmapi, commit, node, destino)
Lo copia build.py y lo arranca nexus/freellmapi.py.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache"
DESKTOP_RT = CACHE / "desktop-runtimes"
REPO = "https://github.com/tashfeenahmed/freellmapi"
NODE_MAJOR = os.environ.get("NEXUS_NODE_MAJOR", "22")

# destino de Agente IA -> (sistema de npm, procesador de npm, nombre de Node.js en nodejs.org, formato)
DESTINOS = {
    "windows-x64": ("win32", "x64", "win-x64", "zip"),
    "windows-arm64": ("win32", "arm64", "win-arm64", "zip"),
    "linux-x86_64": ("linux", "x64", "linux-x64", "tar.xz"),
    "linux-aarch64": ("linux", "arm64", "linux-arm64", "tar.xz"),
    "macos-arm64": ("darwin", "arm64", "darwin-arm64", "tar.gz"),
    "macos-x86_64": ("darwin", "x64", "darwin-x64", "tar.gz"),
}
NPM = "npm.cmd" if os.name == "nt" else "npm"
NPMQ = ["--no-audit", "--no-fund", "--loglevel=error"]


def log(msg: str) -> None:
    print(f"[freellmapi] {msg}", flush=True)


def run(cmd: list[str], cwd: Path, env: dict | None = None) -> str:
    log("$ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, env={**os.environ, **(env or {})}, capture_output=True, text=True)
    if p.returncode:
        sys.stderr.write(p.stdout[-4000:] + p.stderr[-4000:])
        raise RuntimeError(f"fallo ({p.returncode}): {' '.join(cmd)}")
    return p.stdout


def _get(url: str, dest: Path) -> Path:
    if not dest.exists():
        log(f"descargando {url}")
        tmp = dest.with_suffix(dest.suffix + ".part")
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "agente-ia"}), timeout=120) as r, \
                open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        tmp.rename(dest)
    return dest


def node_version() -> str:
    with urllib.request.urlopen("https://nodejs.org/dist/index.json", timeout=60) as r:
        index = json.load(r)
    return next(v["version"] for v in index if v["version"].startswith(f"v{NODE_MAJOR}."))


def build_comun(ref: str) -> Path:
    """Clona FreeLLMAPI y compila servidor, cli y panel web (una vez: no depende de la plataforma)."""
    src = CACHE / "freellmapi-src"
    marca = src / ".nexus-build"
    if marca.exists() and marca.read_text().strip() == ref:
        return src
    shutil.rmtree(src, ignore_errors=True)
    src.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--depth", "1", "--branch", ref, REPO, str(src)] if ref != "main"
        else ["git", "clone", "--depth", "1", REPO, str(src)], CACHE)
    run([NPM, "ci", *NPMQ, "--ignore-scripts"], src)
    run([NPM, "run", "build"], src)
    marca.write_text(ref)
    return src


def _deref(d: Path) -> None:
    """Cambia los enlaces de los workspaces de npm (node_modules/@freellmapi/*) por copias: el .exe y el zip no
    guardan enlaces simbolicos."""
    for p in sorted(d.rglob("*")):
        if p.is_symlink():
            real = p.resolve()
            p.unlink()
            if real.is_dir():
                shutil.copytree(real, p, symlinks=False, ignore=shutil.ignore_patterns("node_modules", "src", "*.map"))
            elif real.exists():
                shutil.copy2(real, p)


def _podar(app: Path) -> None:
    """Quita lo que no hace falta para servir (tipos, fuentes, mapas, pruebas)."""
    for p in list(app.rglob("*")):
        if not p.exists():
            continue
        n = p.name
        if p.is_file() and (n.endswith((".map", ".d.ts", ".d.ts.map", ".md", ".markdown")) and n.lower() not in
                            ("license.md", "licence.md")):
            p.unlink()
        elif p.is_dir() and n in ("__tests__", "test", "tests", "docs", "example", "examples", ".github") \
                and "node_modules" in p.parts:
            shutil.rmtree(p, ignore_errors=True)


def instalar_destino(target: str, src: Path, nodev: str, out: Path | None = None) -> Path:
    npm_os, npm_cpu, node_name, fmt = DESTINOS[target]
    dest = out or DESKTOP_RT / target / "freellmapi"
    shutil.rmtree(dest, ignore_errors=True)
    app = dest / "app"
    app.mkdir(parents=True)
    # lo compilado y lo que npm necesita para instalar las dependencias del servidor
    for rel in ("package.json", "package-lock.json", "LICENSE", "server/package.json", "server/dist", "shared",
                "client/dist"):
        s = src / rel
        if not s.exists():
            continue
        t = app / rel
        t.parent.mkdir(parents=True, exist_ok=True)
        if s.is_dir():
            shutil.copytree(s, t, ignore=shutil.ignore_patterns("node_modules", "src", "*.map"))
        else:
            shutil.copy2(s, t)
    # solo los workspaces del servidor: el panel (client) ya va compilado y el cli no hace falta
    raiz = json.loads((app / "package.json").read_text())
    raiz["workspaces"] = ["shared", "server"]
    raiz.pop("devDependencies", None)
    (app / "package.json").write_text(json.dumps(raiz, indent=2))
    run([NPM, "install", *NPMQ, "--omit=dev", "--ignore-scripts", f"--os={npm_os}", f"--cpu={npm_cpu}"], app)
    # better-sqlite3 precompilado para ese sistema y esa version de Node (sin node-gyp)
    bs = app / "node_modules" / "better-sqlite3"
    pi = app / "node_modules" / "prebuild-install" / "bin.js"
    if bs.is_dir() and pi.exists():
        run(["node", str(pi), "--platform", npm_os, "--arch", npm_cpu, "--runtime", "node",
             "--target", nodev.lstrip("v")], bs)
        if not (bs / "build" / "Release" / "better_sqlite3.node").exists():
            raise RuntimeError(f"{target}: sin better_sqlite3.node precompilado")
        for sobra in ("deps", "src"):   # fuentes de SQLite y del addon: no hacen falta con el binario
            shutil.rmtree(bs / sobra, ignore_errors=True)
    else:
        raise RuntimeError(f"{target}: npm no instalo better-sqlite3")
    _deref(app)
    _podar(app)
    # Node.js oficial de la plataforma
    arch = f"node-{nodev}-{node_name}.{fmt}"
    pkg = _get(f"https://nodejs.org/dist/{nodev}/{arch}", CACHE / arch)
    tmp = Path(tempfile.mkdtemp(prefix="node-", dir=CACHE))
    try:
        if fmt == "zip":
            with zipfile.ZipFile(pkg) as z:
                z.extractall(tmp)
        else:
            with tarfile.open(pkg) as t:
                t.extractall(tmp, filter="tar") if hasattr(tarfile, "data_filter") else t.extractall(tmp)
        raiz = next(tmp.iterdir())
        node = dest / "node"
        node.mkdir()
        if fmt == "zip":
            for f in ("node.exe", "LICENSE"):
                shutil.copy2(raiz / f, node / f)
        else:
            (node / "bin").mkdir()
            shutil.copy2(raiz / "bin" / "node", node / "bin" / "node")
            os.chmod(node / "bin" / "node", 0o755)
            shutil.copy2(raiz / "LICENSE", node / "LICENSE")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    commit = ""
    try:
        commit = run(["git", "rev-parse", "HEAD"], src).strip()
    except RuntimeError:
        pass
    info = {"freellmapi": json.loads((src / "server" / "package.json").read_text())["version"], "commit": commit,
            "node": nodev, "target": target, "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "origen": REPO, "licencia": "MIT"}
    (dest / "freellmapi.json").write_text(json.dumps(info, indent=1))
    mb = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file()) >> 20
    log(f"{target}: FreeLLMAPI {info['freellmapi']} + Node {nodev} en {dest} ({mb} MB)")
    return dest


def smoke(dest: Path) -> None:
    """Arranca el servidor incluido (solo si es de esta plataforma) y comprueba /v1/models."""
    sys.path.insert(0, str(ROOT))
    os.environ.setdefault("NEXUS_DATA_DIR", tempfile.mkdtemp(prefix="nexus-fl-"))
    from nexus import freellmapi
    srv = freellmapi.FreeLLMAPI(None, raiz=dest)
    srv.puerto = 31999
    srv.arrancar()
    try:
        for _ in range(60):
            time.sleep(1)
            e = srv.estado()
            if e["responde"] and e["clave_puesta"]:
                log(f"humo: responde en {e['url']} con clave unica")
                return
        raise RuntimeError("humo: FreeLLMAPI no responde\n" + "\n".join(srv.registro(40)))
    finally:
        srv.parar()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", required=True, help="destino o «all»: " + ", ".join(DESTINOS))
    ap.add_argument("--ref", default=os.environ.get("FREELLMAPI_REF", "main"))
    ap.add_argument("--smoke", action="store_true", help="arranca el servidor (destino de esta plataforma)")
    a = ap.parse_args()
    targets = list(DESTINOS) if a.target == "all" else [a.target]
    src = build_comun(a.ref)
    nodev = node_version()
    for t in targets:
        d = instalar_destino(t, src, nodev)
        if a.smoke:
            smoke(d)


if __name__ == "__main__":
    main()
