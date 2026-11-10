"""ONNX Runtime GenAI (Microsoft): CUDA, DirectML, QNN (NPU Snapdragon X), OpenVINO, VitisAI (NPU Ryzen AI) y CPU.

El modelo es una carpeta con ``genai_config.json``. Cada exportacion esta pensada para un proveedor (la de
``cpu_and_mobile`` para CPU y QNN, la de ``gpu`` para CUDA/DirectML): con acelerador «modelo» se usa el que trae el
propio genai_config.json; con otro, se cambia el proveedor y, si no carga, quien llama prueba el siguiente.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, Iterator

from . import Motor, Params, mensajes_simples, plantillas

PROVEEDORES = {"cuda": "cuda", "dml": "dml", "qnn": "qnn", "openvino": "OpenVINO", "vitisai": "VitisAI",
               "webgpu": "WebGPU", "rocm": "rocm", "migraphx": "MIGraphX", "coreml": "coreml"}


class OnnxGenAI(Motor):
    id = "onnx"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, acelerador, params)
        import onnxruntime_genai as og   # noqa: PLC0415
        self.og = og
        carpeta = self.modelo if self.modelo.is_dir() else self.modelo.parent
        self.carpeta = carpeta
        try:
            cfg = og.Config(str(carpeta))
            if acelerador not in ("", "modelo"):
                cfg.clear_providers()
                if acelerador != "cpu":
                    cfg.append_provider(PROVEEDORES.get(acelerador, acelerador))
            self.model = og.Model(cfg)
            self.tok = og.Tokenizer(self.model)
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"ONNX Runtime GenAI no pudo cargar el modelo con {acelerador or 'su proveedor'}: {e}") from e
        if acelerador in ("", "modelo"):
            self.acelerador = self._proveedor_del_modelo() or "cpu"

    def _proveedor_del_modelo(self) -> str:
        try:
            cfg = json.loads((self.carpeta / "genai_config.json").read_text(encoding="utf-8"))
            prov = cfg["model"]["decoder"]["session_options"].get("provider_options") or []
            for p in prov:
                for k in p:
                    return k.lower()
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            pass
        return ""

    def _prompt(self, msgs: list[dict]) -> str:
        # las versiones recientes aplican la plantilla del propio modelo
        apply = getattr(self.tok, "apply_chat_template", None)
        if apply:
            try:
                return apply(json.dumps(msgs, ensure_ascii=False), add_generation_prompt=True)
            except Exception:  # noqa: BLE001
                pass
        return plantillas.prompt(self.carpeta, msgs)

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        og = self.og
        msgs = mensajes_simples(messages)
        ids = self.tok.encode(self._prompt(msgs))
        params = og.GeneratorParams(self.model)
        greedy = p.temperature <= 0.01
        opts = {"max_length": min(p.n_ctx, len(ids) + max(16, p.max_tokens)), "do_sample": not greedy}
        if not greedy:
            opts.update(temperature=p.temperature, top_p=p.top_p, top_k=max(1, p.top_k))
        params.set_search_options(**opts)
        gen = og.Generator(self.model, params)
        if hasattr(gen, "append_tokens"):
            gen.append_tokens(ids)
        else:   # versiones 0.4 y anteriores
            params.input_ids = ids
            gen = og.Generator(self.model, params)
        stream = self.tok.create_stream()
        try:
            while not gen.is_done():
                if parar():
                    break
                gen.generate_next_token()
                t = gen.get_next_tokens()[0]
                piece = stream.decode(t)
                if piece:
                    yield plantillas.limpiar(piece)
        finally:
            del gen

    def cerrar(self) -> None:
        self.model = self.tok = None
