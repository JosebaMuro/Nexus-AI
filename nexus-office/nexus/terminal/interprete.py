"""Interprete de Python con memoria: un proceso aparte que conserva variables e importaciones entre llamadas.

El proceso recibe cada bloque de codigo por su entrada y devuelve una linea marcada con el resultado; lo que el
codigo escriba por su cuenta en la salida real (un subproceso, os.write) se recoge igual como salida.
"""
from __future__ import annotations

import json
import queue
import secrets
import subprocess
import threading
import time

from .. import proc as nproc

_BUCLE = r'''
import ast, contextlib, io, json, sys, traceback
MARCA = sys.argv[1]
real = sys.stdout
ns = {"__name__": "__main__", "__builtins__": __builtins__}
def _sin_entrada(*a, **k):
    raise RuntimeError("El interprete no tiene entrada interactiva: pasa los datos en el codigo")
ns["input"] = _sin_entrada
for linea in sys.stdin:
    try:
        m = json.loads(linea)
    except ValueError:
        continue
    buf, valor, error = io.StringIO(), None, None
    try:
        arbol = ast.parse(m.get("code", ""), "<terminal>", "exec")
        ultimo = None
        if arbol.body and isinstance(arbol.body[-1], ast.Expr):
            ultimo = ast.Expression(arbol.body.pop().value)
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            exec(compile(arbol, "<terminal>", "exec"), ns)
            if ultimo is not None:
                v = eval(compile(ultimo, "<terminal>", "eval"), ns)
                if v is not None:
                    ns["_"] = v
                    valor = repr(v)[:20000]
    except SystemExit as e:
        error = "SystemExit: " + str(e.code)
    except BaseException:
        tipo, exc, tb = sys.exc_info()
        while tb is not None and tb.tb_frame.f_code.co_filename != "<terminal>":
            tb = tb.tb_next
        error = "".join(traceback.format_exception(tipo, exc, tb))[-6000:]
    real.write("\n" + MARCA + json.dumps({"id": m.get("id"), "salida": buf.getvalue(), "valor": valor,
                                          "error": error}) + "\n")
    real.flush()
'''


class Interprete:
    def __init__(self, carpeta: str, entorno: dict | None = None) -> None:
        self.marca = "\x01NX" + secrets.token_hex(6) + "\x01"
        self.carpeta = carpeta
        self._cola: queue.Queue = queue.Queue()
        self._mu = threading.Lock()
        self.proc = nproc.popen([*nproc.python_cmd(), "-X", "utf8", "-u", "-c", _BUCLE, self.marca], cwd=carpeta,
                                env=entorno, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        threading.Thread(target=self._leer, name="nexus-interprete", daemon=True).start()

    def _leer(self) -> None:
        try:
            for linea in self.proc.stdout:
                self._cola.put(linea)
        except Exception:  # noqa: BLE001
            pass
        self._cola.put(None)

    def vivo(self) -> bool:
        return self.proc.poll() is None

    def ejecutar(self, codigo: str, espera_s: float = 120) -> dict:
        with self._mu:
            if not self.vivo():
                return {"error": "El intérprete se había cerrado; vuelve a ejecutar (las variables se han perdido)",
                        "salida": "", "valor": None}
            ident = secrets.token_hex(4)
            t0 = time.time()
            try:
                self.proc.stdin.write(json.dumps({"id": ident, "code": codigo}) + "\n")
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError) as e:
                return {"error": f"El intérprete no responde: {e}", "salida": "", "valor": None}
            suelto: list[str] = []
            fin = t0 + espera_s
            while True:
                try:
                    linea = self._cola.get(timeout=max(0.05, fin - time.time()))
                except queue.Empty:
                    self.cerrar()
                    return {"error": f"Tiempo agotado ({espera_s:g} s): se ha reiniciado el intérprete y sus variables "
                            "se han perdido", "salida": "".join(suelto), "valor": None, "agotada": True}
                if linea is None:
                    return {"error": "El intérprete terminó de forma inesperada", "salida": "".join(suelto),
                            "valor": None, "codigo": self.proc.poll()}
                if linea.startswith(self.marca):
                    try:
                        r = json.loads(linea[len(self.marca):])
                    except ValueError:
                        continue
                    if r.get("id") != ident:
                        continue
                    extra = "".join(suelto).strip("\n")
                    salida = (r.get("salida") or "") + (("\n" + extra) if extra else "")
                    return {"salida": salida, "valor": r.get("valor"), "error": r.get("error"),
                            "ms": round((time.time() - t0) * 1000)}
                suelto.append(linea)

    def cerrar(self) -> None:
        try:
            self.proc.kill()
            self.proc.wait(3)
        except Exception:  # noqa: BLE001
            pass
