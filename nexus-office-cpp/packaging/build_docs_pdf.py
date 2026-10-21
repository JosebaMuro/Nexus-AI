"""Manual-de-usuario.pdf y Documentacion-tecnica.pdf de la edicion C++, con el mismo formato que los de la edicion
Python (nexus-office/packaging/build_docs_pdf.py): las guias propias de la edicion C++ (docs/USUARIO.md y
docs/CPP.md) y las compartidas que valen igual para las dos.

    python packaging/build_docs_pdf.py [carpeta_salida]        # necesita markdown y playwright (Chromium)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = ROOT.parent / "nexus-office"
sys.path.insert(0, str(PY / "packaging"))
import build_docs_pdf as base  # noqa: E402

base.VERSION = re.search(r'set\(NEXUS_VERSION "([^"]+)"\)', (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
C, P = str(ROOT / "docs"), str(PY / "docs")
base.BOOKS = {
    "Manual-de-usuario": ("Manual de usuario", "Edición C++ (beta) · oficina de IA local para pymes",
                          [f"{C}/USUARIO.md", f"{C}/CPP.md", f"{P}/OFICINA.md", f"{P}/SERVICIO.md", f"{P}/AVATAR.md",
                           f"{P}/SALON.md", f"{P}/ACELERADOR.md", f"{P}/RAG.md"]),
    "Documentacion-tecnica": ("Documentación técnica", "Edición C++ (beta): cómo está hecha, administración y API",
                              [f"{C}/CPP.md", f"{P}/ACELERADOR.md", f"{P}/ADMINISTRADOR.md", f"{P}/SERVICIO.md",
                               f"{P}/API.md", f"{P}/HERRAMIENTAS.md", f"{P}/MEMORIA.md", f"{P}/RAG.md"]),
}

if __name__ == "__main__":
    base.main(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist")
