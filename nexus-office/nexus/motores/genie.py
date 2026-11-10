"""Qualcomm Genie (QAIRT) por su API C (``GenieDialog.h``), cargada con ctypes: el LLM entero en la NPU Hexagon.

El modelo es una carpeta de Qualcomm AI Hub: ``genie_config.json``, ``tokenizer.json``, los binarios de contexto QNN
(``*.bin``) compilados para el Snapdragon concreto y ``htp_backend_ext_config.json``. Las rutas del
genie_config.json son relativas a la carpeta del modelo. Genie no aplica la plantilla de chat: va en
``plantillas.py``. Android, Windows ARM (Snapdragon X) y Linux ARM con Snapdragon.
"""
from __future__ import annotations

import ctypes as C
import os
import queue
import threading
from pathlib import Path
from typing import Callable, Iterator

from . import Motor, Params, lib_path, mensajes_simples, plantillas

H = C.c_void_p
QUERY_CB = C.CFUNCTYPE(None, C.c_char_p, C.c_int, C.c_void_p)
SENTENCE_COMPLETE = 0
ABORT = 1
_cwd_lock = threading.Lock()


def cargar(lib: Path):
    try:
        d = C.CDLL(str(lib))
    except OSError as e:
        raise RuntimeError(f"no se pudo cargar Genie: {e}") from e
    need = {
        "GenieDialogConfig_createFromJson": (C.c_int32, [C.c_char_p, C.POINTER(H)]),
        "GenieDialogConfig_free": (C.c_int32, [H]),
        "GenieDialog_create": (C.c_int32, [H, C.POINTER(H)]),
        "GenieDialog_query": (C.c_int32, [H, C.c_char_p, C.c_int, QUERY_CB, C.c_void_p]),
        "GenieDialog_reset": (C.c_int32, [H]),
        "GenieDialog_free": (C.c_int32, [H]),
    }
    opt = {
        "GenieDialog_signal": (C.c_int32, [H, C.c_int]),
        "GenieDialog_getSampler": (C.c_int32, [H, C.POINTER(H)]),
        "GenieSamplerConfig_createFromJson": (C.c_int32, [C.c_char_p, C.POINTER(H)]),
        "GenieSamplerConfig_setParam": (C.c_int32, [H, C.c_char_p, C.c_char_p]),
        "GenieSampler_applyConfig": (C.c_int32, [H, H]),
        "GenieSamplerConfig_free": (C.c_int32, [H]),
    }
    for nombre, (res, args) in need.items():
        f = getattr(d, nombre, None)
        if f is None:
            raise RuntimeError(f"Genie incompatible (le falta {nombre})")
        f.restype, f.argtypes = res, args
    for nombre, (res, args) in opt.items():
        f = getattr(d, nombre, None)
        if f is not None:
            f.restype, f.argtypes = res, args
    return d


class Genie(Motor):
    id = "genie"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, "npu", params)
        lib = lib_path("genie")
        if not lib:
            raise RuntimeError("Genie (QAIRT) no esta instalado (bundled/runtimes/genie)")
        self.d = d = cargar(lib)
        self.carpeta = self.modelo if self.modelo.is_dir() else self.modelo.parent
        try:
            texto = (self.carpeta / "genie_config.json").read_text(encoding="utf-8")
        except OSError as e:
            raise RuntimeError(f"falta genie_config.json en {self.carpeta}") from e
        self.tipo = plantillas.detectar(self.carpeta)
        cfg, self.dialog = H(), H()
        with _cwd_lock:   # las rutas del genie_config.json son relativas a la carpeta del modelo
            antes = os.getcwd()
            os.chdir(self.carpeta)
            try:
                if d.GenieDialogConfig_createFromJson(texto.encode(), C.byref(cfg)) or not cfg:
                    raise RuntimeError(f"Genie no acepta genie_config.json de {self.carpeta}")
                st = d.GenieDialog_create(cfg, C.byref(self.dialog))
                d.GenieDialogConfig_free(cfg)
            finally:
                os.chdir(antes)
        if st or not self.dialog:
            raise RuntimeError(f"Genie no pudo cargar el modelo en la NPU (estado {st}): revisa que los .bin sean "
                               "para este Snapdragon y que esten las bibliotecas QNN HTP")
        self.muestreo = ""

    def _muestreo(self, p: Params) -> None:
        d = self.d
        clave = f"{p.temperature}|{p.top_k}|{p.top_p}|{p.seed}"
        if clave == self.muestreo or not all(hasattr(d, n) for n in (
                "GenieDialog_getSampler", "GenieSamplerConfig_createFromJson", "GenieSamplerConfig_setParam",
                "GenieSampler_applyConfig")):
            return
        self.muestreo = clave
        sampler, sc = H(), H()
        if d.GenieDialog_getSampler(self.dialog, C.byref(sampler)) or not sampler:
            return
        if d.GenieSamplerConfig_createFromJson(b'{"sampler":{"version":1}}', C.byref(sc)) or not sc:
            return
        d.GenieSamplerConfig_setParam(sc, b"temp", f"{max(0.0, p.temperature):.2f}".encode())
        d.GenieSamplerConfig_setParam(sc, b"top-k", str(max(1, p.top_k)).encode())
        d.GenieSamplerConfig_setParam(sc, b"top-p", f"{p.top_p:.2f}".encode())
        if p.seed >= 0:
            d.GenieSamplerConfig_setParam(sc, b"seed", str(p.seed & 0x7FFFFFFF).encode())
        d.GenieSampler_applyConfig(sampler, sc)
        if hasattr(d, "GenieSamplerConfig_free"):
            d.GenieSamplerConfig_free(sc)

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        d = self.d
        msgs = mensajes_simples(messages)
        if not msgs or msgs[-1]["role"] != "user":
            raise ValueError("falta el mensaje del usuario")
        self._muestreo(p)
        d.GenieDialog_reset(self.dialog)   # cada peticion lleva toda la conversacion
        prompt = plantillas.prompt(self.carpeta, msgs)
        q: queue.Queue = queue.Queue()
        fin = object()
        tope = max(16, p.max_tokens) * 8
        estado = {"enviado": 0, "stop": False}

        @QUERY_CB
        def cb(resp, _code, _data):
            if estado["stop"] or not resp:
                return
            s = resp.decode(errors="replace")
            estado["enviado"] += len(s)
            q.put(s)
            if parar() or estado["enviado"] > tope:
                estado["stop"] = True
                if hasattr(d, "GenieDialog_signal"):
                    d.GenieDialog_signal(self.dialog, ABORT)
        res: list[int] = []

        def trabajo():
            try:
                res.append(d.GenieDialog_query(self.dialog, prompt.encode(), SENTENCE_COMPLETE, cb, None))
            finally:
                q.put(fin)
        threading.Thread(target=trabajo, daemon=True, name="genie-query").start()
        while True:
            x = q.get()
            if x is fin:
                break
            yield plantillas.limpiar(x)
        if res and res[0] and not estado["stop"] and not estado["enviado"]:
            raise RuntimeError(f"Genie no pudo generar la respuesta (estado {res[0]})")

    def cerrar(self) -> None:
        if self.dialog:
            self.d.GenieDialog_free(self.dialog)
            self.dialog = H()
