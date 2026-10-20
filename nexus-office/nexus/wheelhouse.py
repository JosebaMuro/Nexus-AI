"""Wheels offline: descarga el wheelhouse/ y lo usa para instalar sin compilar y sin internet.

    python -m nexus wheels                          # wheels de este equipo (base + extras) en ./wheelhouse
    python -m nexus wheels --target linux-aarch64   # para otra plataforma (se pueden repetir --target)
    python -m nexus wheels --runtime                # ademas el CPython portable en ./runtime
    python -m nexus wheels --check                  # comprueba que el wheelhouse instala todo sin internet

Las wheels son siempre binarias (--only-binary=:all:) y para CPython 3.11, la version del runtime portable,
asi que nada se compila en el equipo. pip las busca en wheelhouse/ antes que en internet.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

from .paths import ROOT

WHEELHOUSE = ROOT / "wheelhouse"
VENDOR = ROOT / "vendor" / "wheels"     # wheels puras para plataformas sin binaria en PyPI (ver su LEEME.txt)
RUNTIME_DIR = ROOT / "runtime"
PYTHON_VERSION = "3.11"          # la del runtime portable (install.sh / install.ps1)
# Lo que `nexus setup --auto` puede instalar con pip (setup_extras.py). PyTorch no: pesa GB y depende de la GPU.
EXTRAS = ["piper-tts", "faster-whisper", "pyttsx3", "onnxruntime", "openvino"]
# aceleracion por plataforma, para que `nexus setup` no necesite internet (setup_extras.plan):
#   Windows: NPU Qualcomm (QNN) y GPU de cualquier fabricante para PyTorch/MusicGen (DirectML);
#   macOS Apple Silicon: PyTorch con Metal (MPS). PyTorch con CUDA/ROCm (2-4 GB, indice propio) queda fuera.
ACCEL = {
    "windows-x64": ["onnxruntime-qnn", "torch-directml", "transformers", "scipy"],
    "macos-arm64": ["torch", "transformers", "scipy"],
}


def extras_for(target: str) -> list[str]:
    return EXTRAS + ACCEL.get(target, [])
PBS_FALLBACK = "https://github.com/astral-sh/python-build-standalone/releases/download/20261009/cpython-3.11.17+20261009-{triple}-install_only_stripped.tar.gz"


GLIBC = (39, 28)   # Ubuntu 24.04 y el minimo soportado (glibc 2.28: Debian 10, RHEL 8, Ubuntu 20.04 tiene 2.31)


def _manylinux(arch: str, glibc: int = GLIBC[0]) -> list[str]:
    # pip download solo acepta las etiquetas que se le dan: todas las de glibc 2.5 a la indicada
    return ([f"manylinux_2_{n}_{arch}" for n in range(glibc, 4, -1)]
            + [f"manylinux2014_{arch}", f"manylinux2010_{arch}", f"manylinux1_{arch}", f"linux_{arch}"])


TARGETS = {
    "linux-x86_64": {"platforms": _manylinux("x86_64"), "pbs": "x86_64-unknown-linux-gnu", "arch": "x86_64"},
    "linux-aarch64": {"platforms": _manylinux("aarch64"), "pbs": "aarch64-unknown-linux-gnu", "arch": "aarch64"},
    "windows-x64": {"platforms": ["win_amd64"], "pbs": "x86_64-pc-windows-msvc"},
    "windows-arm64": {"platforms": ["win_arm64"], "pbs": "aarch64-pc-windows-msvc"},
    "macos-arm64": {"platforms": [f"macosx_{v}_0_arm64" for v in range(14, 10, -1)] + ["macosx_10_9_universal2"],
                    "pbs": "aarch64-apple-darwin"},
    "macos-x86_64": {"platforms": ["macosx_10_13_x86_64", "macosx_10_12_x86_64", "macosx_10_9_x86_64", "macosx_10_9_universal2"],
                     "pbs": "x86_64-apple-darwin"},
}


def is_android() -> bool:
    return bool(os.environ.get("TERMUX_VERSION")) or sys.platform == "android" or hasattr(sys, "getandroidapilevel")


def current_target() -> str | None:
    m = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(m)
    if not arch or is_android():
        return None
    osname = {"linux": "linux", "win32": "windows", "darwin": "macos"}.get(sys.platform)
    if osname in ("windows", "macos"):
        arch = {"x86_64": "x64" if osname == "windows" else "x86_64", "aarch64": "arm64"}[arch]
    return f"{osname}-{arch}" if osname else None


def wheels(dest: Path = WHEELHOUSE) -> list[Path]:
    return sorted(dest.glob("*.whl")) if dest.is_dir() else []


def offline_only() -> bool:
    return os.environ.get("NEXUS_OFFLINE", "") not in ("", "0")


def binary_args() -> list[str]:
    """Nunca compilar: solo wheels. En Termux no hay wheels de PyPI para casi nada, alli si se permite."""
    if is_android() or os.environ.get("NEXUS_ALLOW_BUILD", "") not in ("", "0"):
        return ["--prefer-binary"]
    return ["--only-binary=:all:"]


def _vendor() -> list[str]:
    return ["--find-links", str(VENDOR)] if VENDOR.is_dir() else []


def has_local(packages: list[str], dest: Path = WHEELHOUSE) -> bool:
    """Todas las wheels de packages (solo los nombres pedidos) estan en el wheelhouse: instalarlas no descarga nada."""
    have = {re.sub(r"[-_.]+", "-", f.name.split("-")[0]).lower() for f in wheels(dest)}
    return bool(have) and all(re.sub(r"[-_.]+", "-", re.split(r"[<>=~!\[ ]", p)[0]).lower() in have for p in packages)


def pip_install(packages: list[str], index: str | None = None, dest: Path = WHEELHOUSE) -> None:
    """pip install usando primero el wheelhouse sin red; si le falta algo y hay permiso, completa desde internet."""
    base = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-warn-script-location",
            *binary_args(), *_vendor()]
    if wheels(dest):
        r = subprocess.run(base + ["--no-index", "--find-links", str(dest)] + packages)
        if r.returncode == 0:
            return
        if offline_only():
            raise RuntimeError(f"faltan wheels en {dest} para {' '.join(packages)} (modo offline)")
        base += ["--find-links", str(dest)]
    elif offline_only():
        raise RuntimeError(f"modo offline sin wheelhouse: {dest} no tiene wheels")
    if index:
        base += ["--extra-index-url", index]
    if subprocess.run(base + packages).returncode != 0:
        raise RuntimeError(f"no hay wheel de {' '.join(packages)} para esta plataforma y no se compila "
                           "(NEXUS_ALLOW_BUILD=1 lo permitiria)")


def requirements(path: Path = ROOT / "requirements.txt") -> list[str]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def _variants(target: str) -> list[tuple[str, list[str]]]:
    """Juegos de etiquetas a cubrir. En Linux, uno por glibc: algunas wheels (p. ej. cryptography) piden
    glibc 2.34, y con solo esa una distro mas antigua no podria instalar sin internet."""
    t = TARGETS[target]
    if "arch" in t:
        return [(f"glibc 2.{g}", _manylinux(t["arch"], g)) for g in GLIBC]
    return [("", t["platforms"])]


def _pip_target(platforms: list[str]) -> list[str]:
    args = ["--only-binary=:all:", "--python-version", PYTHON_VERSION, "--implementation", "cp"]
    for p in platforms:
        args += ["--platform", p]
    return args


def _pkg():
    try:
        from packaging import markers, requirements, specifiers, tags, utils  # noqa: F401
        import packaging
    except ImportError:   # siempre viene con pip
        from pip._vendor import packaging  # type: ignore[no-redef]
        from pip._vendor.packaging import markers, requirements, specifiers, tags, utils  # noqa: F401
    return packaging


def _marker_env(target: str) -> dict:
    """Entorno de la plataforma destino para evaluar marcadores (pip download los evalua con el de este equipo)."""
    osname, arch = target.split("-", 1)
    machine = {"x86_64": "x86_64", "aarch64": "aarch64", "x64": "AMD64", "arm64": "ARM64"}[arch]
    if osname == "macos":
        machine = {"arm64": "arm64", "x86_64": "x86_64"}[arch]
    return {"sys_platform": {"linux": "linux", "windows": "win32", "macos": "darwin"}[osname],
            "platform_system": {"linux": "Linux", "windows": "Windows", "macos": "Darwin"}[osname],
            "os_name": "nt" if osname == "windows" else "posix", "platform_machine": machine,
            "python_version": PYTHON_VERSION, "python_full_version": PYTHON_VERSION + ".0", "implementation_name": "cpython",
            "platform_python_implementation": "CPython", "platform_release": "", "platform_version": "",
            "implementation_version": PYTHON_VERSION + ".0", "extra": ""}


def _index(dest: Path, plats: list[str]) -> dict:
    """{nombre normalizado: [(version, wheel)]} de las wheels de dest validas para estas etiquetas y CPython 3.11."""
    P = _pkg()
    ok_plats = set(plats) | {"any"}
    mac_max: dict = {}     # pip acepta en macOS cualquier version igual o anterior a la indicada
    for p in plats:
        m = re.fullmatch(r"macosx_(\d+)_(\d+)_(\w+)", p)
        if m:
            mac_max[m[3]] = max(mac_max.get(m[3], (0, 0)), (int(m[1]), int(m[2])))

    def plat_ok(tag: str) -> bool:
        m = re.fullmatch(r"macosx_(\d+)_(\d+)_(\w+)", tag)
        if m and m[3] in mac_max:
            return (int(m[1]), int(m[2])) <= mac_max[m[3]]
        return tag in ok_plats
    pyv = "".join(PYTHON_VERSION.split("."))
    out: dict = {}
    for f in wheels(dest):
        try:
            name, ver, _, wtags = P.utils.parse_wheel_filename(f.name)
        except Exception:  # noqa: BLE001
            continue
        if any(plat_ok(t.platform) and (t.interpreter in (f"cp{pyv}", "py3", "py2.py3") or t.interpreter.startswith("py3")
                                           or (t.abi == "abi3" and t.interpreter.startswith("cp3")))
               for t in wtags):
            out.setdefault(name, []).append((ver, f))
    for v in out.values():
        v.sort(key=lambda x: x[0], reverse=True)
    return out


def _requires(whl: Path) -> list[str]:
    import zipfile
    with zipfile.ZipFile(whl) as z:
        meta = next(n for n in z.namelist() if n.endswith(".dist-info/METADATA"))
        text = z.read(meta).decode("utf-8", "replace")
    head = text.split("\n\n", 1)[0]
    return [ln.split(":", 1)[1].strip() for ln in head.splitlines() if ln.startswith("Requires-Dist:")]


def closure_missing(target: str, roots: list[str], dest: Path, plats: list[str], fetch=None) -> list[str]:
    """Recorre las dependencias de roots con los marcadores de la plataforma destino y devuelve las que no tienen
    wheel en dest. Con fetch(req) intenta descargarlas (sin dependencias) y sigue recorriendo."""
    P = _pkg()
    env = _marker_env(target)
    idx = _index(dest, plats)
    seen, missing, todo = set(), [], [(r, "") for r in roots]
    while todo:
        spec, extra = todo.pop()
        try:
            req = P.requirements.Requirement(spec)
        except Exception:  # noqa: BLE001
            continue
        if req.marker is not None and not req.marker.evaluate({**env, "extra": extra}):
            continue
        name = P.utils.canonicalize_name(req.name)
        key = (name, str(req.specifier), tuple(sorted(req.extras)))
        if key in seen:
            continue
        seen.add(key)
        cands = [w for v, w in idx.get(name, []) if req.specifier.contains(v, prereleases=True)]
        if not cands and fetch is not None and fetch(f"{req.name}{req.specifier}"):
            idx = _index(dest, plats)
            cands = [w for v, w in idx.get(name, []) if req.specifier.contains(v, prereleases=True)]
        if not cands:
            missing.append(f"{req.name}{req.specifier}")
            continue
        for dep in _requires(cands[0]):
            todo.append((dep, ""))
            for ex in req.extras:
                todo.append((dep, ex))
    return sorted(set(missing))


def download(target: str, dest: Path = WHEELHOUSE, extras: bool = True, log=print) -> list[str]:
    """Descarga a dest las wheels de requirements.txt (+ extras) para target. Devuelve los extras que no tienen wheel."""
    dest.mkdir(parents=True, exist_ok=True)
    missing = []
    for label, plats in _variants(target):
        name = f"{target} {label}".strip()
        pip = [sys.executable, "-m", "pip", "download", "--disable-pip-version-check", "-q", "-d", str(dest),
               *_pip_target(plats), *_vendor()]
        log(f"[wheels] {name}: dependencias base")
        subprocess.run(pip + ["-r", str(ROOT / "requirements.txt")], check=True)
        for pkg in extras_for(target) if extras else []:
            log(f"[wheels] {name}: {pkg}")
            # sin dependencias si pip las resuelve con los marcadores de este equipo (torch-directml pide las de CUDA
            # de Linux): closure_missing completa despues las de la plataforma destino
            if subprocess.run(pip + [pkg], capture_output=True).returncode != 0 and \
                    subprocess.run(pip + ["--no-deps", pkg], capture_output=True).returncode != 0:
                missing.append(f"{pkg} ({label})" if label else pkg)
        # dependencias que solo existen en la plataforma destino (p. ej. comtypes en Windows, pyobjc en macOS):
        # pip download evalua los marcadores con este equipo y no las baja
        def fetch(spec: str) -> bool:
            log(f"[wheels] {name}: {spec} (dependencia de {target})")
            return subprocess.run(pip + ["--no-deps", spec], capture_output=True).returncode == 0
        roots = requirements() + ([p for p in extras_for(target) if p not in " ".join(missing)] if extras else [])
        closure_missing(target, roots, dest, plats, fetch)
    return missing


def _dry_run(pkgs: list[str], plats: list[str], dest: Path) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--dry-run", "--ignore-installed",
               "--no-index", "--find-links", str(dest), "--target", tmp, *_pip_target(plats), *pkgs]
        r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip().splitlines()
    return r.returncode == 0, (out[-1] if out else "")


def check(target: str, dest: Path = WHEELHOUSE, extras: bool = True) -> tuple[bool, str]:
    """Simula la instalacion solo con el wheelhouse (sin internet ni compilacion), para cada glibc.
    La base debe instalar siempre; de los extras se informa de los que no instalarian (no todos existen en todas partes)."""
    notes = []
    for label, plats in _variants(target):
        ok, msg = _dry_run(requirements(), plats, dest)
        lack = closure_missing(target, requirements(), dest, plats)   # con los marcadores de la plataforma destino
        if not ok or lack:
            return False, f"{label}: {msg if not ok else 'faltan ' + ', '.join(lack)}".lstrip(": ")
        for pkg in extras_for(target) if extras else []:
            ok, msg = _dry_run([pkg], plats, dest)
            # pip evalua los marcadores con este equipo: un fallo por una dependencia con marcador (p. ej. las de CUDA
            # de Linux que pide torch) no cuenta si con los marcadores de la plataforma destino no falta nada
            ok = ok or ";" in msg
            if not ok or closure_missing(target, [pkg], dest, plats):
                notes.append(f"{pkg} ({label})" if label else pkg)
    return True, ("extras sin wheel: " + ", ".join(notes)) if notes else ""


def write_manifest(dest: Path = WHEELHOUSE, targets: list[str] | None = None) -> None:
    files = wheels(dest)
    sums = [f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.name}" for f in files]
    (dest / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    info = {"python": PYTHON_VERSION, "targets": sorted(set(targets or [])), "wheels": len(files),
            "mb": round(sum(f.stat().st_size for f in files) / 2**20, 1),
            "extras": sorted(set(EXTRAS).union(*(ACCEL.get(t, []) for t in targets or [])))}
    old = dest / "wheelhouse.json"
    if old.exists():
        try:
            info["targets"] = sorted(set(info["targets"]) | set(json.loads(old.read_text(encoding="utf-8")).get("targets", [])))
        except ValueError:
            pass
    old.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")


def verify(dest: Path = WHEELHOUSE) -> list[str]:
    """Wheels cuya huella no coincide con SHA256SUMS (descarga corrupta o incompleta)."""
    sums = dest / "SHA256SUMS"
    if not sums.exists():
        return []
    bad = []
    for line in sums.read_text(encoding="utf-8").splitlines():
        h, _, name = line.partition("  ")
        f = dest / name
        if name and (not f.exists() or hashlib.sha256(f.read_bytes()).hexdigest() != h):
            bad.append(name)
    return bad


def download_runtime(target: str, dest: Path = RUNTIME_DIR, log=print) -> Path:
    """CPython portable (python-build-standalone) para que install.sh / install.ps1 no tengan que descargarlo."""
    triple = TARGETS[target]["pbs"]
    dest.mkdir(parents=True, exist_ok=True)
    have = sorted(dest.glob(f"cpython-*{triple}*.tar.gz"))
    if have:
        return have[-1]
    url = PBS_FALLBACK.format(triple=triple)
    try:
        rel = json.load(urllib.request.urlopen(urllib.request.Request(
            "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest", headers={"User-Agent": "nexus-office"}), timeout=30))
        pat = re.compile(rf"^cpython-{re.escape(PYTHON_VERSION)}\.\d+\+\d+-{re.escape(triple)}-install_only_stripped\.tar\.gz$")
        url = next((a["browser_download_url"] for a in rel["assets"] if pat.match(a["name"])), url)
    except Exception:  # noqa: BLE001 - sin API (limite de GitHub): la version probada
        pass
    out = dest / url.rsplit("/", 1)[-1].replace("%2B", "+")
    log(f"[wheels] runtime {out.name}")
    tmp = out.with_suffix(".part")
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "nexus-office"}), timeout=60) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    tmp.replace(out)
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="nexus wheels", description="prepara wheelhouse/ para instalar Nexus-Office sin internet ni compilacion")
    p.add_argument("--target", action="append", choices=sorted(TARGETS) + ["linux", "all"],
                   help="plataforma (por defecto, la de este equipo); se puede repetir")
    p.add_argument("--dest", type=Path, default=WHEELHOUSE)
    p.add_argument("--sin-extras", dest="extras", action="store_false", help="solo requirements.txt, sin voz ni NPU")
    p.add_argument("--runtime", action="store_true", help="descarga tambien el CPython portable a runtime/")
    p.add_argument("--check", action="store_true", help="solo comprueba el wheelhouse existente")
    a = p.parse_args(argv)
    targets = []
    for t in a.target or [current_target() or ""]:
        targets += {"linux": ["linux-x86_64", "linux-aarch64"], "all": sorted(TARGETS)}.get(t, [t])
    if not all(targets):
        print("[wheels] plataforma no soportada; indica --target", file=sys.stderr)
        return 2
    ok = True
    for t in targets:
        if not a.check:
            missing = download(t, a.dest, a.extras)
            if missing:
                print(f"[wheels] {t}: sin wheel para {', '.join(missing)} (se omiten)")
            if a.runtime:
                download_runtime(t)
        good, msg = check(t, a.dest, a.extras)
        ok &= good
        print(f"[wheels] {t}: {'OK, instala sin internet' if good else 'INCOMPLETO'}{'; ' + msg if msg else ''}")
    sdists = [f.name for f in a.dest.glob("*") if f.suffix in (".gz", ".zip", ".bz2")] if a.dest.is_dir() else []
    if sdists:
        print(f"[wheels] aviso: paquetes fuente (necesitarian compilar): {', '.join(sdists)}")
    if not a.check:
        write_manifest(a.dest, targets)
    bad = verify(a.dest)
    if bad:
        print(f"[wheels] huella incorrecta: {', '.join(bad)}")
    files = wheels(a.dest)
    print(f"[wheels] {len(files)} wheels, {sum(f.stat().st_size for f in files) / 2**20:.0f} MB en {a.dest}")
    return 0 if ok and not bad else 1
