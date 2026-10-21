"""Extrae las tablas de datos de la version Python (catalogos, prompts, patrones) a src/gen/data.cpp.

La edicion C++ usa exactamente los mismos datos que la version Python: si cambian alli, se vuelve a ejecutar
este script (python3 tools/gen_data.py) y se recompila. No hace falta para compilar: data.cpp va en el repositorio.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PY = ROOT.parent / "nexus-office"
sys.path.insert(0, str(PY))


def collect() -> dict:
    out = {}
    from nexus.models import recommended as rec
    out["recommended"] = rec.RECOMMENDED
    out["legacy_llm"] = rec.LEGACY_LLM
    out["stt_models"] = rec.STT_MODELS
    for name, fn in EXTRA.items():
        out[name] = fn()
    return out


EXTRA = {}


def extra(name):
    def deco(fn):
        EXTRA[name] = fn
        return fn
    return deco


def main() -> None:
    data = collect()
    lines = ["// GENERADO por tools/gen_data.py a partir de la version Python: no editar a mano.",
             '#include "gen/data.h"', "", "#include <map>", "", "namespace nx::data {", ""]
    names = []
    for name, value in data.items():
        raw = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        ident = "D_" + name.upper()
        names.append((name, ident, len(raw)))
        lines.append(f"static const unsigned char {ident}[] = {{")
        for i in range(0, len(raw), 24):
            lines.append("  " + ",".join(str(b) for b in raw[i:i + 24]) + ",")
        lines.append("  0};")
        lines.append("")
    lines.append("std::string_view raw(const std::string& name) {")
    lines.append("  static const std::map<std::string, std::string_view> M = {")
    for name, ident, n in names:
        lines.append(f'      {{"{name}", std::string_view((const char*){ident}, {n})}},')
    lines.append("  };")
    lines.append("  auto it = M.find(name);")
    lines.append('  return it == M.end() ? std::string_view("null") : it->second;')
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace nx::data")
    (ROOT / "src" / "gen" / "data.cpp").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("datos:", ", ".join(f"{n} ({s} B)" for n, _, s in names))


if __name__ == "__main__":
    main()
