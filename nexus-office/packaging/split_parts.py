"""Prepara los paquetes de dist/ para enviarlos al movil (limite de ~30 MB por fichero).

    python packaging/split_parts.py                 # dist/ -> dist/envio/
    python packaging/split_parts.py --size 25 --out dist/envio fichero1 fichero2

Los ficheros pequenos se copian tal cual; los grandes se dividen en partes .001, .002... con
UNIR-<nombre>.bat (Windows) y unir-<nombre>.sh (Linux/macOS/Android-Termux). Cada union se comprueba
aqui mismo contra su SHA-256 antes de dar el envio por bueno; el indice LEEME-DESCARGAS.txt lo resume.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
SKIP_SUFFIX = {".idsig"}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def split(src: Path, out: Path, size: int) -> list[Path]:
    parts = []
    with src.open("rb") as f:
        i = 1
        while chunk := f.read(size):
            p = out / f"{src.name}.{i:03d}"
            p.write_bytes(chunk)
            parts.append(p)
            i += 1
    return parts


def join_scripts(src: Path, parts: list[Path], digest: str, out: Path) -> list[Path]:
    name = src.name
    bat = out / f"UNIR-{name}.bat"
    bat.write_text(
        "@echo off\r\n"
        f"rem Une las {len(parts)} partes de {name} y comprueba su huella SHA-256.\r\n"
        "cd /d \"%~dp0\"\r\n"
        f"copy /b {'+'.join(p.name for p in parts)} \"{name}\" >nul || (echo Faltan partes & pause & exit /b 1)\r\n"
        f"powershell -NoProfile -Command \"if ((Get-FileHash '{name}' -Algorithm SHA256).Hash -eq '{digest.upper()}') "
        f"{{ Write-Host 'OK: {name} completo y verificado' -ForegroundColor Green }} "
        f"else {{ Write-Host 'ERROR: el fichero unido no coincide (parte corrupta o incompleta)' -ForegroundColor Red }}\"\r\n"
        "pause\r\n", encoding="utf-8")
    sh = out / f"unir-{name}.sh"
    sh.write_text(
        "#!/bin/sh\n"
        f"# Une las {len(parts)} partes de {name} y comprueba su huella SHA-256.\n"
        "cd \"$(dirname \"$0\")\" || exit 1\n"
        f"cat {' '.join(p.name for p in parts)} > '{name}' || exit 1\n"
        f"echo '{digest}  {name}' | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) && echo 'OK: {name} completo y verificado'\n",
        encoding="utf-8", newline="\n")
    return [bat, sh]


def verify_join(parts: list[Path], digest: str) -> bool:
    h = hashlib.sha256()
    for p in parts:
        h.update(p.read_bytes())
    return h.hexdigest() == digest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", type=Path)
    ap.add_argument("--size", type=float, default=25, help="tamano maximo por fichero en MB (por defecto 25)")
    ap.add_argument("--out", type=Path, default=DIST / "envio")
    ap.add_argument("--skip", nargs="*", default=[], help="nombres de fichero a omitir")
    a = ap.parse_args()
    limit = int(a.size * 1024 * 1024)
    files = a.files or sorted(p for p in DIST.iterdir() if p.is_file() and p.suffix not in SKIP_SUFFIX and p.name not in a.skip)
    shutil.rmtree(a.out, ignore_errors=True)
    a.out.mkdir(parents=True)
    index = ["Nexus-Office - ficheros para descargar", "Partes: une con UNIR-<nombre>.bat (Windows) o sh unir-<nombre>.sh (Linux/macOS/Termux).", ""]
    ok = True
    for src in files:
        digest, mb = sha256(src), src.stat().st_size / 2**20
        if src.stat().st_size <= limit:
            shutil.copy2(src, a.out / src.name)
            index.append(f"{src.name}  ({mb:.1f} MB)  sha256 {digest}")
            print(f"[envio] {src.name}: {mb:.1f} MB, entero")
            continue
        parts = split(src, a.out, limit)
        join_scripts(src, parts, digest, a.out)
        good = verify_join(parts, digest)
        ok &= good
        index.append(f"{src.name}  ({mb:.1f} MB en {len(parts)} partes)  sha256 {digest}  union {'verificada' if good else 'FALLIDA'}")
        print(f"[envio] {src.name}: {mb:.1f} MB -> {len(parts)} partes, union {'verificada' if good else 'FALLIDA'}")
    (a.out / "LEEME-DESCARGAS.txt").write_text("\n".join(index) + "\n", encoding="utf-8")
    biggest = max(p.stat().st_size for p in a.out.iterdir()) / 2**20
    print(f"[envio] {len(list(a.out.iterdir()))} ficheros en {a.out} (el mayor: {biggest:.1f} MB)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
