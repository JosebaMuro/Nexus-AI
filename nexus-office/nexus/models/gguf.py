"""Lector minimo de metadatos GGUF (sin dependencias). Lee la cabecera hasta los datos del tokenizer."""
from __future__ import annotations

import struct
from pathlib import Path

_SCALARS = {0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i", 6: "<f", 7: "<?", 10: "<Q", 11: "<q", 12: "<d"}
FILE_TYPES = {0: "F32", 1: "F16", 2: "Q4_0", 3: "Q4_1", 7: "Q8_0", 8: "Q5_0", 9: "Q5_1", 10: "Q2_K", 11: "Q3_K_S",
              12: "Q3_K_M", 13: "Q3_K_L", 14: "Q4_K_S", 15: "Q4_K_M", 16: "Q5_K_S", 17: "Q5_K_M", 18: "Q6_K",
              32: "BF16"}


def _read(f, fmt):
    size = struct.calcsize(fmt)
    return struct.unpack(fmt, f.read(size))[0]


def _string(f) -> str:
    n = _read(f, "<Q")
    return f.read(n).decode("utf-8", errors="replace")


def _value(f, vtype, keep: bool):
    if vtype in _SCALARS:
        return _read(f, _SCALARS[vtype])
    if vtype == 8:
        return _string(f)
    if vtype == 9:
        itype, n = _read(f, "<I"), _read(f, "<Q")
        if itype in _SCALARS and not keep:
            f.seek(struct.calcsize(_SCALARS[itype]) * n, 1)
            return f"[array {n}]"
        if itype == 8 and not keep:  # vocabularios: saltar sin decodificar
            for _ in range(n):
                f.seek(_read(f, "<Q"), 1)
            return f"[array {n}]"
        items = [_value(f, itype, keep) for _ in range(n)]
        return items if keep else f"[array {n}]"
    raise ValueError(f"tipo GGUF desconocido {vtype}")


def read_metadata(path: str | Path, max_keys: int = 400) -> dict:
    meta: dict = {}
    with open(path, "rb") as f:
        if f.read(4) != b"GGUF":
            raise ValueError("no es un fichero GGUF")
        version = _read(f, "<I")
        n_tensors, n_kv = _read(f, "<Q"), _read(f, "<Q")
        meta["gguf.version"], meta["gguf.tensors"] = version, n_tensors
        for _ in range(min(n_kv, max_keys)):
            key = _string(f)
            vtype = _read(f, "<I")
            value = _value(f, vtype, keep=False)
            if not key.startswith("tokenizer.") or vtype not in (8, 9):   # del tokenizer solo los indicadores
                meta[key] = value
    return meta


def summarize(path: str | Path) -> dict:
    try:
        m = read_metadata(path)
    except (OSError, ValueError, struct.error) as e:
        return {"error": str(e)}
    arch = m.get("general.architecture", "")
    return {
        "architecture": arch,
        "name": m.get("general.name", ""),
        "quant": FILE_TYPES.get(m.get("general.file_type"), str(m.get("general.file_type", ""))),
        "context_length": m.get(f"{arch}.context_length") if arch else None,
        "size_label": m.get("general.size_label", ""),
        "is_projector": arch == "clip" or "clip.has_vision_encoder" in m or "clip.projector_type" in m,
        # modelos de embeddings: llevan tipo de agrupacion (pooling) o se declaran como tales
        "pooling": m.get(f"{arch}.pooling_type") if arch else None,
        "embedding_length": m.get(f"{arch}.embedding_length") if arch else None,
        "is_embedding": bool(arch and (f"{arch}.pooling_type" in m or "embed" in str(m.get("general.name", "")).lower()
                                       or "embed" in str(m.get("general.basename", "")).lower())),
        "add_eos": m.get("tokenizer.ggml.add_eos_token"),
        "tensors": m.get("gguf.tensors"),
    }
