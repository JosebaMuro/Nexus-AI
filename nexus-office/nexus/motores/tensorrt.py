"""TensorRT-LLM (NVIDIA): kernels de Tensor Cores, KV en paginas y FP8/INT4 AWQ; el mas rapido en GPU NVIDIA.

El modelo es un checkpoint de Hugging Face (carpeta con config.json y safetensors, marcada con ``trtllm.json``) o un
motor ya compilado (``rank0.engine``). La primera carga de un checkpoint compila el motor para la GPU de este equipo
(minutos); despues se reutiliza. Solo Linux x86_64/aarch64 y Windows con CUDA.
"""
from __future__ import annotations

import asyncio
import queue
import threading
from pathlib import Path
from typing import Callable, Iterator

from . import Motor, Params, mensajes_simples, plantillas


class TensorRtLlm(Motor):
    id = "tensorrt"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, "cuda", params)
        try:
            from tensorrt_llm import LLM, SamplingParams   # noqa: PLC0415
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"TensorRT-LLM no se pudo importar: {e}") from e
        self.SamplingParams = SamplingParams
        self.carpeta = self.modelo if self.modelo.is_dir() else self.modelo.parent
        try:
            self.llm = LLM(model=str(self.carpeta), max_seq_len=params.n_ctx)
        except TypeError:
            self.llm = LLM(model=str(self.carpeta))
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"TensorRT-LLM no pudo cargar el modelo: {e}") from e

    def _prompt(self, msgs: list[dict]) -> str:
        tok = getattr(self.llm, "tokenizer", None)
        try:
            return tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True)
        except Exception:  # noqa: BLE001
            return plantillas.prompt(self.carpeta, msgs)

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        prompt = self._prompt(mensajes_simples(messages))
        kw = {"max_tokens": max(16, p.max_tokens), "temperature": max(0.0, p.temperature), "top_p": p.top_p,
              "top_k": max(1, p.top_k)}
        if p.seed >= 0:
            kw["seed"] = p.seed
        sp = self.SamplingParams(**kw)
        q: queue.Queue = queue.Queue()
        fin = object()
        error: list[BaseException] = []

        async def leer():
            enviado = 0
            res = self.llm.generate_async(prompt, sp, streaming=True)
            async for out in res:
                texto = out.outputs[0].text   # acumulado: se manda lo nuevo
                if len(texto) > enviado:
                    q.put(texto[enviado:])
                    enviado = len(texto)
                if parar():
                    abort = getattr(res, "abort", None)
                    if abort:
                        abort()
                    break

        def trabajo():
            try:
                asyncio.run(leer())
            except BaseException as e:  # noqa: BLE001
                error.append(e)
            finally:
                q.put(fin)
        threading.Thread(target=trabajo, daemon=True, name="trtllm-gen").start()
        while True:
            x = q.get()
            if x is fin:
                break
            yield x
        if error and not parar():
            raise RuntimeError(f"TensorRT-LLM: {error[0]}")

    def cerrar(self) -> None:
        try:
            self.llm.shutdown()
        except Exception:  # noqa: BLE001
            pass
