"""MLC LLM (Apache TVM): CUDA, ROCm, Vulkan, Metal y OpenCL (Adreno, Mali) con kernels compilados para cada GPU.

El modelo es una carpeta con ``mlc-chat-config.json`` y los pesos (``params_shard_*.bin``), p. ej.
mlc-ai/Qwen3-4B-q4f16_1-MLC. Si la carpeta trae la biblioteca del modelo ya compilada (``*.so``, ``*.dll``,
``*.dylib``) se usa; si no, MLC la compila al primer uso (necesita el compilador de TVM incluido en el paquete).
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Iterator

from . import Motor, Params, mensajes_simples


def _lib_del_modelo(carpeta: Path) -> str | None:
    for patron in ("*.so", "*.dll", "*.dylib", "*.tar"):
        hits = sorted(carpeta.glob(patron))
        if hits:
            return str(hits[0])
    return None


class Mlc(Motor):
    id = "mlc"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, acelerador or "auto", params)
        try:
            from mlc_llm import MLCEngine   # noqa: PLC0415
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"MLC LLM no se pudo importar: {e}") from e
        self.carpeta = self.modelo if self.modelo.is_dir() else self.modelo.parent
        kw = {"model": str(self.carpeta), "device": self.acelerador if self.acelerador != "cpu" else "cpu", "mode": "local"}
        lib = _lib_del_modelo(self.carpeta)
        if lib:
            kw["model_lib"] = lib
        try:
            self.engine = MLCEngine(**kw)
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"MLC LLM no pudo cargar el modelo en {self.acelerador}: {e}") from e

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        kw = {"messages": mensajes_simples(messages), "stream": True, "max_tokens": max(16, p.max_tokens),
              "temperature": max(0.0, p.temperature), "top_p": p.top_p}
        if p.seed >= 0:
            kw["seed"] = p.seed
        for resp in self.engine.chat.completions.create(**kw):
            for ch in resp.choices:
                t = getattr(ch.delta, "content", None)
                if t:
                    yield t
            if parar():
                break

    def cerrar(self) -> None:
        try:
            self.engine.terminate()
        except Exception:  # noqa: BLE001
            pass
