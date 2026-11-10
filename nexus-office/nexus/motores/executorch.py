"""ExecuTorch (PyTorch en el dispositivo): XNNPACK en CPU ARM/x86 y los delegados QNN, Vulkan, CoreML o MPS con
los que se exporto el ``.pte``.

El modelo es un ``.pte`` con su tokenizer (``tokenizer.json`` o ``tokenizer.model`` en la misma carpeta), p. ej.
pytorch/Qwen3-4B-INT8-INT4 (8da4w). El delegado va dentro del .pte: el acelerador solo informa. Se usa el runner de
LLM de ExecuTorch (``executorch.extension.llm.runner``) con la plantilla de chat del modelo.
"""
from __future__ import annotations

import queue
import threading
from pathlib import Path
from typing import Callable, Iterator

from . import Motor, Params, mensajes_simples, plantillas


def _tokenizer(carpeta: Path) -> Path | None:
    for n in ("tokenizer.json", "tokenizer.model", "tokenizer.bin"):
        if (carpeta / n).is_file():
            return carpeta / n
    return None


class ExecuTorch(Motor):
    id = "executorch"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, acelerador or "xnnpack", params)
        pte = self.modelo if self.modelo.is_file() else next(iter(sorted(self.modelo.glob("*.pte"))), None)
        if not pte:
            raise RuntimeError(f"no hay ningun .pte en {self.modelo}")
        self.carpeta = pte.parent
        tok = _tokenizer(self.carpeta)
        if not tok:
            raise RuntimeError(f"falta el tokenizer (tokenizer.json o tokenizer.model) junto a {pte.name}")
        try:
            # registran los operadores personalizados y cuantizados que usan los .pte de LLM
            from executorch.extension.llm.custom_ops import custom_ops  # noqa: F401, PLC0415
            from executorch.kernels import quantized  # noqa: F401, PLC0415
        except Exception:  # noqa: BLE001 - algunas compilaciones los llevan dentro
            pass
        try:
            from executorch.extension.llm import runner as r   # noqa: PLC0415
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"esta instalacion de ExecuTorch no trae el runner de LLM: {e}") from e
        self.r = r
        try:
            self.runner = r.MultimodalRunner(str(pte), str(tok))
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"ExecuTorch no pudo cargar {pte.name}: {e}") from e

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        r = self.r
        prompt = plantillas.prompt(self.carpeta, mensajes_simples(messages))
        cfg = r.GenerationConfig(max_new_tokens=max(16, p.max_tokens), temperature=max(0.0, p.temperature),
                                 echo=False, seq_len=p.n_ctx)
        q: queue.Queue = queue.Queue()
        fin = object()
        error: list[BaseException] = []

        def token(t: str):
            q.put(t)
            if parar() and hasattr(self.runner, "stop"):
                self.runner.stop()

        def trabajo():
            try:
                self.runner.generate([r.make_text_input(prompt)], cfg, token)
            except BaseException as e:  # noqa: BLE001
                error.append(e)
            finally:
                q.put(fin)
        threading.Thread(target=trabajo, daemon=True, name="executorch-gen").start()
        while True:
            x = q.get()
            if x is fin:
                break
            yield plantillas.limpiar(x)
        if error and not parar():
            raise RuntimeError(f"ExecuTorch: {error[0]}")
        if hasattr(self.runner, "reset"):
            self.runner.reset()

    def cerrar(self) -> None:
        self.runner = None
