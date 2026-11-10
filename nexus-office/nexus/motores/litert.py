"""LiteRT-LM (Google) por su API C (``c/engine.h`` y ``c/conversation.h``, version 1.x), cargada con ctypes.

Es el motor que mejor va en los Pixel (TPU de Tensor) y en los moviles con NPU de Qualcomm o MediaTek, y tambien en
la NPU de Intel. La biblioteca (``liblitert-lm.so``, ``litert-lm.dll``) y las de «dispatch» de cada NPU las
compila ``motores-npu.yml`` y van en ``bundled/runtimes/litert/<variante>/``.

Una conversacion guarda su cache KV: si la peticion nueva es la anterior mas la respuesta y un mensaje nuevo (lo
normal en un chat) se reutiliza y solo se procesa el mensaje nuevo; si no, se crea otra con el historial.
"""
from __future__ import annotations

import ctypes as C
import json
import queue
from pathlib import Path
from typing import Callable, Iterator

from .. import log, paths
from . import Motor, Params, lib_path, mensajes_simples

L = log.get("litert")
P = C.c_void_p
CB = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p)

FIRMAS = {
    "litert_lm_get_api_version": (C.c_int, [C.POINTER(C.c_int)] * 3),
    "litert_lm_set_min_log_level": (C.c_int, [C.c_int]),
    "litert_lm_engine_settings_create": (C.c_int, [C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(P)]),
    "litert_lm_engine_settings_delete": (None, [P]),
    "litert_lm_engine_settings_set_max_num_tokens": (C.c_int, [P, C.c_int]),
    "litert_lm_engine_settings_set_num_threads": (C.c_int, [P, C.c_int]),
    "litert_lm_engine_settings_set_cache_dir": (C.c_int, [P, C.c_char_p]),
    "litert_lm_engine_settings_set_litert_dispatch_lib_dir": (C.c_int, [P, C.c_char_p]),
    "litert_lm_engine_create": (C.c_int, [P, C.POINTER(P)]),
    "litert_lm_engine_delete": (None, [P]),
    "litert_lm_sampler_params_create": (C.c_int, [C.c_int, C.POINTER(P)]),
    "litert_lm_sampler_params_delete": (None, [P]),
    "litert_lm_sampler_params_set_top_k": (C.c_int, [P, C.c_int32]),
    "litert_lm_sampler_params_set_top_p": (C.c_int, [P, C.c_float]),
    "litert_lm_sampler_params_set_temperature": (C.c_int, [P, C.c_float]),
    "litert_lm_sampler_params_set_seed": (C.c_int, [P, C.c_int32]),
    "litert_lm_session_config_create": (C.c_int, [C.POINTER(P)]),
    "litert_lm_session_config_delete": (None, [P]),
    "litert_lm_session_config_set_max_output_tokens": (C.c_int, [P, C.c_int]),
    "litert_lm_session_config_set_sampler_params": (C.c_int, [P, P]),
    "litert_lm_conversation_config_create": (C.c_int, [C.POINTER(P)]),
    "litert_lm_conversation_config_delete": (None, [P]),
    "litert_lm_conversation_config_set_session_config": (C.c_int, [P, P]),
    "litert_lm_conversation_config_set_system_message": (C.c_int, [P, C.c_char_p]),
    "litert_lm_conversation_config_set_messages": (C.c_int, [P, C.c_char_p]),
    "litert_lm_conversation_create": (C.c_int, [P, P, C.POINTER(P)]),
    "litert_lm_conversation_delete": (None, [P]),
    "litert_lm_conversation_send_message_stream": (C.c_int, [P, C.c_char_p, C.c_char_p, C.c_void_p, CB, C.c_void_p]),
    "litert_lm_conversation_cancel_process": (C.c_int, [P]),
    "litert_lm_stream_chunk_get_text": (C.c_int, [P, C.POINTER(C.c_char_p)]),
    "litert_lm_stream_chunk_is_final": (C.c_int, [P, C.POINTER(C.c_bool)]),
    "litert_lm_stream_chunk_get_error": (C.c_int, [P, C.POINTER(C.c_char_p)]),
}


def cargar(lib: Path):
    try:
        dll = C.CDLL(str(lib))
    except OSError as e:
        raise RuntimeError(f"no se pudo cargar LiteRT-LM: {e}") from e
    for nombre, (res, args) in FIRMAS.items():
        f = getattr(dll, nombre, None)
        if f is None:
            raise RuntimeError(f"LiteRT-LM incompatible (le falta {nombre}): se necesita la version 0.18 o posterior")
        f.restype, f.argtypes = res, args
    last = getattr(dll, "litert_lm_get_last_error_message", None)
    if last is not None:
        last.restype, last.argtypes = C.c_char_p, []
    v = [C.c_int(), C.c_int(), C.c_int()]
    if dll.litert_lm_get_api_version(*[C.byref(x) for x in v]) != 0 or v[0].value != 1:
        raise RuntimeError(f"API C de LiteRT-LM {v[0].value}.{v[1].value} no compatible (se usa la 1.x)")
    return dll


def texto_trozo(s: str) -> str:
    """LiteRT-LM manda el mensaje en JSON ({"role", "content": [{"type": "text", "text"}]}) o texto."""
    try:
        m = json.loads(s)
    except ValueError:
        return s
    if not isinstance(m, dict):
        return s
    c = m.get("content")
    if isinstance(c, str):
        return c
    if isinstance(c, list):
        return "".join(p.get("text", "") if isinstance(p, dict) and p.get("type", "text") == "text"
                       else p if isinstance(p, str) else "" for p in c)
    return ""


class LiteRtLm(Motor):
    id = "litert"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, acelerador or "cpu", params)
        lib = lib_path("litert")
        if not lib:
            raise RuntimeError("LiteRT-LM no esta instalado (bundled/runtimes/litert)")
        self.dll = d = cargar(lib)
        d.litert_lm_set_min_log_level(3)
        s = P()
        if d.litert_lm_engine_settings_create(str(self.modelo).encode(), self.acelerador.encode(), None, None, C.byref(s)) or not s:
            raise RuntimeError(self._error(f"LiteRT-LM no acepta el modelo con {self.acelerador}"))
        d.litert_lm_engine_settings_set_max_num_tokens(s, max(512, params.n_ctx))
        if self.acelerador == "cpu" and params.hilos > 0:
            d.litert_lm_engine_settings_set_num_threads(s, params.hilos)
        # cache de programas de GPU y NPU compilados: la segunda carga es mucho mas rapida
        d.litert_lm_engine_settings_set_cache_dir(s, str(paths.sub("motores", "cache-litert")).encode())
        if self.acelerador == "npu":
            d.litert_lm_engine_settings_set_litert_dispatch_lib_dir(s, str(lib.parent).encode())
        self.engine = P()
        st = d.litert_lm_engine_create(s, C.byref(self.engine))
        d.litert_lm_engine_settings_delete(s)
        if st or not self.engine:
            raise RuntimeError(self._error(f"LiteRT-LM no pudo cargar el modelo en {self.acelerador}"))
        self.conv = P()
        self.hist: list[dict] = []
        self.clave = ""

    def _error(self, que: str) -> str:
        f = getattr(self.dll, "litert_lm_get_last_error_message", None)
        e = f() if f else None
        return que + (f": {e.decode(errors='replace')}" if e else "")

    def _reset(self) -> None:
        if self.conv:
            self.dll.litert_lm_conversation_delete(self.conv)
        self.conv = P()
        self.hist = []

    def _nueva(self, system: str, turnos: list[dict], p: Params, clave: str) -> None:
        d = self.dll
        self._reset()
        sc, sp, cc = P(), P(), P()
        if d.litert_lm_session_config_create(C.byref(sc)):
            raise RuntimeError(self._error("LiteRT-LM: configuracion de sesion"))
        d.litert_lm_session_config_set_max_output_tokens(sc, max(16, p.max_tokens))
        greedy = p.temperature <= 0.01
        if d.litert_lm_sampler_params_create(3 if greedy else 2, C.byref(sp)) == 0 and sp:
            if not greedy:
                d.litert_lm_sampler_params_set_top_k(sp, max(1, p.top_k))
                d.litert_lm_sampler_params_set_top_p(sp, p.top_p)
                d.litert_lm_sampler_params_set_temperature(sp, p.temperature)
            if p.seed >= 0:
                d.litert_lm_sampler_params_set_seed(sp, p.seed & 0x7FFFFFFF)
            d.litert_lm_session_config_set_sampler_params(sc, sp)
        st = d.litert_lm_conversation_config_create(C.byref(cc))
        if st == 0:
            d.litert_lm_conversation_config_set_session_config(cc, sc)
            if system:   # como cadena JSON: un texto que pareciera JSON no se interpreta
                d.litert_lm_conversation_config_set_system_message(cc, json.dumps(system).encode())
            if turnos:
                d.litert_lm_conversation_config_set_messages(cc, json.dumps(turnos, ensure_ascii=False).encode())
            st = d.litert_lm_conversation_create(self.engine, cc, C.byref(self.conv))
        err = self._error("LiteRT-LM no pudo crear la conversacion") if st or not self.conv else ""
        if cc:
            d.litert_lm_conversation_config_delete(cc)
        if sp:
            d.litert_lm_sampler_params_delete(sp)
        d.litert_lm_session_config_delete(sc)
        if err:
            self.conv = P()
            raise RuntimeError(err)
        self.hist, self.clave = list(turnos), clave

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        d = self.dll
        msgs = mensajes_simples(messages)
        if not msgs or msgs[-1]["role"] != "user":
            raise ValueError("falta el mensaje del usuario")
        system = "\n\n".join(m["content"] for m in msgs if m["role"] == "system")
        turnos = [m for m in msgs if m["role"] != "system"]
        ultimo = turnos.pop()
        clave = f"{p.max_tokens}|{p.temperature}|{p.top_p}|{p.top_k}|{system}"
        if not (self.conv and clave == self.clave and turnos == self.hist):
            self._nueva(system, turnos, p, clave)
        q: queue.Queue = queue.Queue()

        @CB
        def cb(_data, chunk):
            err, text, final = C.c_char_p(), C.c_char_p(), C.c_bool(False)
            d.litert_lm_stream_chunk_get_error(chunk, C.byref(err))
            if not err.value:
                d.litert_lm_stream_chunk_get_text(chunk, C.byref(text))
            d.litert_lm_stream_chunk_is_final(chunk, C.byref(final))
            if err.value:
                q.put(("error", err.value.decode(errors="replace")))
            elif text.value:
                q.put(("texto", texto_trozo(text.value.decode(errors="replace"))))
            if final.value or err.value:
                q.put(("fin", ""))
        self._cb = cb   # que no lo libere el recolector mientras la biblioteca lo llama
        msg = json.dumps({"role": "user", "content": ultimo["content"]}, ensure_ascii=False).encode()
        if d.litert_lm_conversation_send_message_stream(self.conv, msg, None, None, cb, None):
            self._reset()
            raise RuntimeError(self._error("LiteRT-LM no pudo procesar el mensaje"))
        respuesta, error, cancelado = "", "", False
        while True:
            tipo, valor = q.get()
            if tipo == "fin":
                break
            if tipo == "error":
                error = valor
                continue
            if cancelado:
                continue
            respuesta += valor
            yield valor
            if parar():
                cancelado = True
                d.litert_lm_conversation_cancel_process(self.conv)
        limite = "Max number of tokens" in error or "CANCELLED" in error
        if cancelado or error:
            self._reset()   # tras cancelar, la conversacion no se puede reutilizar
            if error and not limite and not respuesta:
                raise RuntimeError(f"LiteRT-LM: {error}")
            return
        self.hist = turnos + [ultimo, {"role": "assistant", "content": respuesta}]

    def cerrar(self) -> None:
        self._reset()
        if self.engine:
            self.dll.litert_lm_engine_delete(self.engine)
            self.engine = P()
