"""Compara el lector de grafos de la edicion C++ (nexus-office grafo leer) con kg_io.parse_many de Python, fichero a
fichero, con las muestras de tests/kg_samples (un ejemplo de cada formato, comprimidos incluidos).

    NEXUS_DATA_DIR=/tmp/x python tests/compare_kg.py RUTA/nexus-office [carpeta] [-v]
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "nexus-office"))
from nexus.memory import kg_io  # noqa: E402

exe = sys.argv[1]
folder = next((a for a in sys.argv[2:] if not a.startswith("-")), os.path.join(HERE, "kg_samples"))
ok = bad = 0
for f in sorted(os.listdir(folder)):
    p = os.path.join(folder, f)
    try:
        py = json.loads(json.dumps(kg_io.parse_many([(f, open(p, "rb").read())]), ensure_ascii=False, default=str))
    except Exception as e:  # noqa: BLE001
        py = {"error": str(e)}
    r = subprocess.run([exe, "grafo", "leer", p], capture_output=True, text=True)
    cp = json.loads(r.stdout) if r.returncode == 0 else {"error": r.stderr.strip().removeprefix("No se pudo leer el grafo: ")}
    if cp == py:
        ok += 1
        print("OK ", f)
    else:
        bad += 1
        print("DIF", f)
        if "-v" in sys.argv:
            print("   py: ", json.dumps(py, ensure_ascii=False)[:800])
            print("   c++:", json.dumps(cp, ensure_ascii=False)[:800])
print(f"\n{ok}/{ok + bad} ficheros iguales")
sys.exit(1 if bad else 0)
