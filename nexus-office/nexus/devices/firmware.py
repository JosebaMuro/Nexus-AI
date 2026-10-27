"""Grabar el firmware de las luces, el reloj y el Pico W desde Nexus (Dispositivos > Grabar firmware).

Usa ``firmware/flash.py`` (el mismo script que se puede lanzar a mano) en un hilo, y guarda el registro de cada
trabajo para que la web lo vaya mostrando. Solo se puede usar desde el propio equipo: toca los puertos USB.
"""
from __future__ import annotations

import importlib.util
import os
import threading
import time
import uuid
from pathlib import Path

from .. import log

L = log.get("firmware")
_AQUI = Path(__file__).resolve()


def carpeta() -> Path | None:
    """firmware/ junto al codigo (repositorio) o dentro de la instalacion portable."""
    cands = [os.environ.get("NEXUS_FIRMWARE", ""), _AQUI.parents[2] / "firmware", _AQUI.parents[3] / "firmware"]
    for c in cands:
        if c and (Path(c) / "flash.py").exists():
            return Path(c)
    return None


_MOD = None


def script():
    global _MOD
    if _MOD is None:
        base = carpeta()
        if not base:
            raise RuntimeError("no encuentro la carpeta firmware/ (con flash.py)")
        spec = importlib.util.spec_from_file_location("nexus_flash", base / "flash.py")
        _MOD = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_MOD)   # type: ignore[union-attr]
    return _MOD


def disponible() -> dict:
    falta = []
    for m in ("serial", "esptool"):
        if importlib.util.find_spec(m) is None:
            falta.append("pyserial" if m == "serial" else m)
    return {"carpeta": str(carpeta() or ""), "falta": falta}


class Trabajos:
    def __init__(self) -> None:
        self.t: dict[str, dict] = {}
        self._lock = threading.Lock()
        self._ocupado = threading.Lock()   # un solo grabado a la vez

    def lanzar(self, tipo: str, puerto: str = "", modelo: str = "", config: dict | None = None,
               solo_config: bool = False) -> dict:
        if tipo not in ("luces", "reloj", "pico"):
            raise ValueError("tipo: luces, reloj o pico")
        tid = uuid.uuid4().hex[:8]
        job = {"id": tid, "tipo": tipo, "estado": "en_curso", "log": [], "inicio": time.time(), "resultado": None}
        with self._lock:
            self.t[tid] = job
            for k in sorted(self.t, key=lambda k: self.t[k]["inicio"])[:-20]:
                self.t.pop(k, None)

        def log_(linea: str) -> None:
            job["log"].append(linea[-300:])
            del job["log"][:-400]

        def run() -> None:
            if not self._ocupado.acquire(timeout=1):
                job.update(estado="error", resultado={"error": "ya hay otro grabado en curso"})
                return
            try:
                f = script()
                res: dict = {}
                if tipo == "pico":
                    p = puerto
                    if not solo_config:
                        p = f.flashear_pico(puerto if puerto and Path(puerto).is_dir() else "", log_)["puerto"]
                    if config:
                        res = f.configurar_pico(p, config, log_)
                else:
                    p = puerto
                    if not solo_config:
                        res = f.flashear_esp32(tipo, puerto, modelo, log_)
                        p = res["puerto"]
                    if config:
                        res["config"] = f.configurar_esp32(tipo, p, config, log_)
                job.update(estado="hecho", resultado=res or {"ok": True})
                log_("Listo.")
            except Exception as e:   # noqa: BLE001
                L.warning("firmware %s: %s", tipo, e)
                log_(f"Error: {e}")
                job.update(estado="error", resultado={"error": str(e)})
            finally:
                self._ocupado.release()
                job["fin"] = time.time()

        threading.Thread(target=run, daemon=True, name=f"firmware-{tid}").start()
        return job

    def get(self, tid: str) -> dict | None:
        return self.t.get(tid)


TRABAJOS = Trabajos()
