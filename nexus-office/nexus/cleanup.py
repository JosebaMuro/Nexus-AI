"""Limpieza al final de la instalacion: quita lo que este equipo no usa del paquete offline.

    python -m nexus cleanup [--yes] [--keep | --no-keep] [--dry-run]

El paquete offline trae wheels y runtimes para cualquier hardware de su plataforma. Tras instalar:
  - no usados: wheels que no se han instalado en venv/ y variantes de runtime (GPU o CPU) que este equipo no usa.
    Se borran si se confirma.
  - usados: las wheels instaladas y el CPython de runtime/. Se pueden guardar para reinstalar sin internet
    (`install --force --offline`) o borrar para ahorrar espacio; Nexus-Office sigue funcionando igual.
El runtime de llama.cpp / sd.cpp que usa el equipo y su variante CPU de respaldo nunca se borran.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from importlib import metadata
from pathlib import Path

from .paths import ROOT

WHEELHOUSE = ROOT / "wheelhouse"
RUNTIME = ROOT / "runtime"
BUNDLED = ROOT / "bundled" / "runtimes"


def _norm(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def _installed() -> set[tuple[str, str]]:
    return {(_norm(d.metadata["Name"] or ""), d.version) for d in metadata.distributions()}


def _wheel_id(path: Path) -> tuple[str, str]:
    name, version = path.name.split("-")[:2]
    return _norm(name), version


def _size(paths: list[Path]) -> int:
    total = 0
    for p in paths:
        if p.is_file():
            total += p.stat().st_size
        elif p.is_dir():
            total += sum(f.stat().st_size for f in p.rglob("*") if f.is_file())
    return total


def _runtimes_in_use() -> set[Path]:
    """Carpetas de bundled/runtimes que usa este equipo: la elegida para su hardware y una CPU de respaldo."""
    keep: set[Path] = set()
    if not BUNDLED.is_dir():
        return keep
    try:
        from . import hardware
        from .runtimes import RUNTIMES, RuntimeManager, bundled_variants
        from .hardware.accel_choice import plan as accel_plan
        hw = hardware.load_cached() or hardware.detect(probe_runtimes=False)
        profile = hardware.build_profile(hw)
        rm = RuntimeManager()
        for rt in [p.name for p in BUNDLED.iterdir() if p.is_dir() and p.name in RUNTIMES]:   # no GIMP ni llama-python
            # la del perfil y las que el selector de inicio puede pedir en este equipo (GPU y NPU)
            prefs = [""]
            if rt == "llama" and hw is not None:
                prefs += [accel_plan(hw, a)["variant"] for a in ("gpu", "npu")]
            for pref in dict.fromkeys(prefs):
                found = rm.exe(rt, profile, pref)
                if found and BUNDLED in found[0].parents:
                    keep.add(next(p for p in found[0].parents if p.parent == BUNDLED / rt))
            cpu = [v for v in bundled_variants(rt) if v.startswith("cpu")]
            if cpu:
                keep.add(BUNDLED / rt / cpu[0])
    except Exception:  # noqa: BLE001  ante la duda, no se borra ningun runtime
        return {p for rt in BUNDLED.iterdir() if rt.is_dir() for p in rt.iterdir()}
    return keep


def plan() -> dict:
    installed = _installed()
    wheels = sorted(WHEELHOUSE.glob("*.whl")) if WHEELHOUSE.is_dir() else []
    used_wheels = [w for w in wheels if _wheel_id(w) in installed]
    unused = [w for w in wheels if w not in used_wheels]
    keep_rt = _runtimes_in_use()
    from .runtimes import RUNTIMES
    variants = [v for rt in BUNDLED.iterdir() if rt.is_dir() and rt.name in RUNTIMES for v in rt.iterdir() if v.is_dir()] \
        if BUNDLED.is_dir() else []
    unused += [v for v in variants if v not in keep_rt]
    used = used_wheels + (sorted(RUNTIME.glob("*")) if RUNTIME.is_dir() else [])
    temp = [p for p in (ROOT / ".cache" / "extract", ROOT / ".cache" / "python-standalone.tar.gz") if p.exists()]
    return {"unused": unused, "used": used, "temp": temp, "runtimes_kept": sorted(keep_rt)}


def _ask(question: str, default: bool) -> bool:
    if not sys.stdin or not sys.stdin.isatty():
        return default
    hint = "S/n" if default else "s/N"
    try:
        answer = input(f"[Nexus-Office] {question} [{hint}] ").strip().lower()
    except EOFError:
        return default
    return default if not answer else answer[0] in "sy"


def _remove(paths: list[Path]) -> None:
    for p in paths:
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        else:
            p.unlink(missing_ok=True)


def run(yes: bool = False, keep: bool | None = None, dry_run: bool = False) -> dict:
    pl = plan()
    mb = lambda paths: _size(paths) / 2**20  # noqa: E731
    print(f"[Nexus-Office] Limpieza de la instalacion: {len(pl['unused'])} elementos sin usar ({mb(pl['unused']):.0f} MB); "
          f"{len(pl['used'])} usados para reinstalar ({mb(pl['used']):.0f} MB).")
    for p in pl["runtimes_kept"]:
        print(f"[Nexus-Office]   se conserva el runtime {p.relative_to(ROOT)}")
    report = {"removed": [], "kept_for_reinstall": [], "dry_run": dry_run}
    if dry_run:
        report["removed"] = [str(p.relative_to(ROOT)) for p in pl["unused"] + pl["temp"]]
        return report
    freed = mb(pl["unused"] + pl["temp"])
    if pl["unused"] and (yes or _ask(f"¿Eliminar lo que este equipo no usa ({freed:.0f} MB)?", False)):
        _remove(pl["unused"] + pl["temp"])
        report["removed"] = [str(p.relative_to(ROOT)) for p in pl["unused"] + pl["temp"]]
        if keep is None:
            keep = yes or _ask(f"¿Guardar los usados para reinstalar sin internet ({mb(pl['used']):.0f} MB)?", True)
        if keep:
            report["kept_for_reinstall"] = [str(p.relative_to(ROOT)) for p in pl["used"]]
            if WHEELHOUSE.is_dir():
                from . import wheelhouse
                wheelhouse.write_manifest(WHEELHOUSE)
        else:
            _remove(pl["used"])
            for d in (WHEELHOUSE, RUNTIME):
                if d.is_dir() and not any(d.glob("*.whl")) and not any(d.glob("*.tar.gz")):
                    shutil.rmtree(d, ignore_errors=True)
        print(f"[Nexus-Office] Eliminado: {freed:.0f} MB."
              + (" Guardado para reinstalar: wheelhouse/ y runtime/ (install --force --offline)." if keep else ""))
    else:
        print("[Nexus-Office] Sin limpieza: el paquete queda completo (reinstalable en cualquier equipo de esta plataforma).")
    (ROOT / ".cache").mkdir(exist_ok=True)
    (ROOT / ".cache" / "limpieza.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="nexus cleanup", description="Quita del paquete offline lo que este equipo no usa")
    p.add_argument("--yes", action="store_true", help="sin preguntar: borra lo no usado y guarda lo usado")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--keep", dest="keep", action="store_true", default=None, help="guardar lo usado para reinstalar")
    g.add_argument("--no-keep", dest="keep", action="store_false", help="borrar tambien lo usado (wheelhouse/ y runtime/)")
    p.add_argument("--dry-run", action="store_true", help="solo muestra lo que se borraria")
    a = p.parse_args(argv)
    report = run(a.yes, a.keep, a.dry_run)
    if a.dry_run:
        print("\n".join(report["removed"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
