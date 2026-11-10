"""Backend del gestor de LLM para cada motor de ``nexus.motores``.

Carga el modelo dentro del proceso (los tokens llegan por llamada directa, sin HTTP) y lo deja cargado entre turnos.
Si un acelerador no arranca, prueba el siguiente del orden (NPU -> GPU -> CPU) y lo recuerda para ese modelo. Mientras
carga o genera deja una marca en disco: si Agente IA se cae (driver de la NPU o la GPU), al volver a arrancar esa
combinacion queda apuntada como caida y no se usa hasta olvidarla en Sistema.
"""
from __future__ import annotations

import threading
import time
from typing import Iterator

from .. import log
from ..llm.backends import Backend, ThinkSplitter, split_stream
from . import MOTORES, Marca, Params, abrir, caidas, disponible, ruta_modelo

L = log.get("motores")


class MotorBackend(Backend):
    def __init__(self, motor: str, resolve, orden_aceleradores) -> None:
        super().__init__()
        self.motor = motor
        self.name = motor
        self.resolve = resolve                       # model_id -> (ruta, entrada del catalogo) o None
        self.orden_aceleradores = orden_aceleradores  # (motor, entrada) -> ["npu", "gpu", "cpu"]
        self._m = None
        self._clave: tuple = ()
        self._fallidos: dict[str, set] = {}
        self._lock = threading.Lock()
        self.efectivo: dict = {}

    def available(self) -> bool:
        return disponible(self.motor)[0] and self.resolve("") is not None

    def models(self) -> list[str]:
        r = self.resolve("")
        return [r[1]["id"]] if r else []

    def stop(self) -> None:
        with self._lock:
            self._cerrar()

    unload = stop

    def _cerrar(self) -> None:
        if self._m is not None:
            try:
                self._m.cerrar()
            except Exception:  # noqa: BLE001
                pass
        self._m, self._clave = None, ()

    def _cargar(self, entry: dict, opts: dict):
        ruta = ruta_modelo(entry)
        params = Params(n_ctx=int(opts.get("n_ctx") or 4096), hilos=int(opts.get("n_threads") or 0))
        caidos = caidas()
        errores = []
        for acel in self.orden_aceleradores(self.motor, entry):
            if acel in self._fallidos.get(entry["id"], set()) or f"{self.motor}-{acel}" in caidos:
                continue
            clave = (str(ruta), acel, params.n_ctx)
            if self._m is not None and self._clave == clave:
                return self._m
            self._cerrar()
            t0 = time.time()
            try:
                with Marca(self.motor, acel):
                    self._m = abrir(self.motor, ruta, acel, params)
            except Exception as e:  # noqa: BLE001
                L.warning("%s no arranco %s en %s: %s", MOTORES[self.motor].nombre, entry["id"], acel, e)
                self._fallidos.setdefault(entry["id"], set()).add(acel)
                errores.append(f"{acel}: {e}")
                continue
            self._clave = clave
            self.efectivo = {"motor": self.motor, "modelo": entry["id"], "acelerador": self._m.acelerador,
                             "carga_s": round(time.time() - t0, 2)}
            L.info("%s: %s en %s (%.1f s)", MOTORES[self.motor].nombre, entry["id"], self._m.acelerador,
                   time.time() - t0)
            return self._m
        raise RuntimeError(f"{MOTORES[self.motor].nombre} no pudo cargar {entry['id']}: " + ("; ".join(errores)
                           or "todos los aceleradores fallaron antes o se cayeron (Sistema > Motores > olvidar caidas)"))

    def stream_chat(self, messages, model="", **opts) -> Iterator[str]:
        r = self.resolve(model)
        if not r:
            raise RuntimeError(f"No hay modelo para {MOTORES[self.motor].nombre} en el catalogo")
        entry = r[1]
        p = Params(max_tokens=int(opts.get("max_tokens") or 1024), temperature=float(opts.get("temperature", 0.7)),
                   top_p=float(opts.get("top_p", 0.95)), top_k=int(opts.get("top_k", 40)),
                   seed=int(opts.get("seed", -1)), n_ctx=int(opts.get("n_ctx") or 4096),
                   hilos=int(opts.get("n_threads") or 0))
        parar = opts.get("should_stop") or (lambda: False)

        def gen():
            with self._lock:
                m = self._cargar(entry, opts)
                with Marca(self.motor, m.acelerador):
                    yield from m.generar(messages, p, parar)

        sp = ThinkSplitter(opts.get("think_mode", "tags"))
        yield from self._finish(self._timed(split_stream(gen(), "", opts.get("on_reasoning"), sp), lambda: sp.raw), sp)
