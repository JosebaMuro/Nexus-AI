"""Nucleo del Terminal IA: sesiones de consola, procesos de fondo, ficheros con deshacer y contexto del agente.

Cada sesion guarda su carpeta y su entorno; cada orden corre en un proceso de la consola del sistema (bash o sh
en Linux, macOS y Android; PowerShell o cmd en Windows) y, al acabar, la sesion recoge el codigo de salida, la
carpeta en la que quedo y las variables exportadas. Asi «cd» y «export» valen para la orden siguiente sin tener
un proceso colgado, y una orden que se cuelga se corta sin perder la sesion.

No hay aislamiento: las ordenes ven todo el equipo, como en una terminal corriente.
"""
from __future__ import annotations

import base64
import codecs
import fnmatch
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from .. import log, paths

L = log.get("terminal")
WINDOWS = os.name == "nt"
_SALTAR = {".git", "node_modules", "__pycache__", ".venv", "venv", ".mypy_cache", ".pytest_cache", ".tox", "dist",
           "build", ".gradle", ".idea", "target"}
_NO_ENTORNO = {"PWD", "OLDPWD", "SHLVL", "_", "NX_MARCA", "NX_CWD", "PSMODULEPATH", "PROMPT"}
MODOS = ("construir", "plan")


def recortar(texto: str, tope: int) -> tuple[str, bool]:
    """Deja el principio y, sobre todo, el final de una salida larga."""
    if len(texto) <= tope:
        return texto, False
    cabeza, cola = tope // 3, tope - tope // 3
    return (texto[:cabeza] + f"\n[... {len(texto) - tope} caracteres omitidos ...]\n" + texto[-cola:]), True


def matar_arbol(proc: subprocess.Popen) -> None:
    """Corta un proceso y a sus hijos."""
    if proc.poll() is not None:
        return
    try:
        if WINDOWS:
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True, timeout=15,
                           creationflags=0x08000000)
        else:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                proc.terminate()
            try:
                proc.wait(2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    proc.kill()
    except Exception as e:  # noqa: BLE001
        L.debug("No se pudo cortar el proceso %s: %s", proc.pid, e)
        try:
            proc.kill()
        except Exception:  # noqa: BLE001
            pass


def _popen(cmd: list[str], cwd: str, env: dict, stdin=subprocess.DEVNULL) -> subprocess.Popen:
    kw: dict = {}
    if WINDOWS:
        kw["creationflags"] = 0x08000000 | 0x00000200   # sin ventana y con grupo propio
    else:
        kw["start_new_session"] = True
    return subprocess.Popen(cmd, cwd=cwd, env=env, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0, **kw)


_PS_RUTA_ENTRE_COMILLAS = re.compile(r"""^(\s*)(["'])([^"']+)\2(?=\s|$)""")


def _ps_invocar(orden: str) -> str:
    """En PowerShell una linea que empieza por una ruta entre comillas es una cadena, no una orden: le antepone
    el operador & ("C:\\Program Files\\x.exe" -v -> & "C:\\Program Files\\x.exe" -v), como haria quien la escribe."""
    m = _PS_RUTA_ENTRE_COMILLAS.match(orden)
    if not m:
        return orden
    ruta = m.group(3)
    if not ("\\" in ruta or "/" in ruta or re.search(r"\.(exe|cmd|bat|ps1|com)$", ruta, re.I)):
        return orden
    return m.group(1) + "& " + orden[len(m.group(1)):]


class Sesion:
    def __init__(self, sid: str, carpeta: str, modo: str = "construir") -> None:
        self.id = sid
        self.carpeta = carpeta
        self.entorno: dict[str, str] = {}
        self.quitadas: set[str] = set()
        self.modo = modo if modo in MODOS else "construir"
        self.historial: list[dict] = []
        self.tareas: list[dict] = []
        self.creada = self.usada = time.time()
        self.proceso: subprocess.Popen | None = None
        self.interprete = None
        self.candado = threading.Lock()

    def publico(self) -> dict:
        return {"id": self.id, "carpeta": self.carpeta, "modo": self.modo, "ocupada": self.proceso is not None,
                "ordenes": len(self.historial), "ultima": (self.historial[-1] if self.historial else None),
                "tareas": self.tareas, "creada": self.creada, "usada": self.usada,
                "interprete": bool(self.interprete and self.interprete.vivo())}


class ProcesoFondo:
    """Proceso largo (un servidor, una compilacion) cuya salida se va leyendo y al que se puede escribir."""

    def __init__(self, pid: str, orden: str, proc: subprocess.Popen, carpeta: str, nombre: str = "") -> None:
        self.id, self.orden, self.proc, self.carpeta, self.nombre = pid, orden, proc, carpeta, nombre or orden[:40]
        self.inicio = time.time()
        self._trozos: list[str] = []
        self._total = 0          # caracteres recibidos desde el principio
        self._quitados = 0       # caracteres ya descartados del principio
        self._mu = threading.Lock()
        threading.Thread(target=self._leer, name=f"nexus-term-{pid}", daemon=True).start()

    def _leer(self) -> None:
        dec = codecs.getincrementaldecoder("utf-8")("replace")
        try:
            while True:
                bloque = self.proc.stdout.read(65536)
                if not bloque:
                    break
                t = dec.decode(bloque)
                with self._mu:
                    self._trozos.append(t)
                    self._total += len(t)
                    while self._total - self._quitados > 400_000 and len(self._trozos) > 1:
                        self._quitados += len(self._trozos.pop(0))
        except Exception:  # noqa: BLE001
            pass

    def leer(self, desde: int = 0, tope: int = 20000) -> dict:
        with self._mu:
            texto = "".join(self._trozos)
            ini = max(0, desde - self._quitados)
            trozo = texto[ini:]
            total = self._total
        cortado = len(trozo) > tope
        return {"id": self.id, "salida": trozo[-tope:] if cortado else trozo, "desde": total, "cortada": cortado,
                "vivo": self.proc.poll() is None, "codigo": self.proc.poll(), "segundos": round(time.time() - self.inicio, 1)}

    def publico(self) -> dict:
        return {"id": self.id, "nombre": self.nombre, "orden": self.orden, "carpeta": self.carpeta,
                "vivo": self.proc.poll() is None, "codigo": self.proc.poll(), "pid": self.proc.pid,
                "segundos": round(time.time() - self.inicio, 1), "caracteres": self._total}


class Terminal:
    def __init__(self, servicios=None, config=None) -> None:
        self.servicios = servicios
        self.config = config or getattr(servicios, "config", None)
        self.sesiones: dict[str, Sesion] = {}
        self.fondo: dict[str, ProcesoFondo] = {}
        self._mu = threading.Lock()
        self.oyentes: list = []      # reciben {"type": "terminal", ...} para la interfaz

    # ------------------------------------------------------------ configuracion
    def cfg(self, clave: str, defecto=None):
        return self.config.get(f"terminal.{clave}", defecto) if self.config is not None else defecto

    def carpeta_base(self) -> str:
        c = str(self.cfg("carpeta", "") or "")
        if c and Path(c).expanduser().is_dir():
            return str(Path(c).expanduser().resolve())
        return str(paths.sandbox_dir())

    def consola(self) -> tuple[str, str]:
        """(clase, ejecutable) de la consola del sistema: bash | sh | powershell | cmd."""
        pedida = str(self.cfg("shell", "") or "").strip()
        if pedida:
            exe = shutil.which(pedida) or (pedida if Path(pedida).exists() else "")
            if exe:
                base = Path(exe).name.lower()
                clase = ("powershell" if base.startswith(("powershell", "pwsh")) else "cmd" if base.startswith("cmd")
                         else "bash" if "bash" in base or "zsh" in base else "sh")
                return clase, exe
        if WINDOWS:
            for nombre in ("pwsh", "powershell"):
                exe = shutil.which(nombre)
                if exe:
                    return "powershell", exe
            return "cmd", os.environ.get("COMSPEC") or "cmd.exe"
        exe = shutil.which("bash")
        if exe:
            return "bash", exe
        return "sh", shutil.which("sh") or "/system/bin/sh"

    def _avisar(self, evento: dict) -> None:
        for cb in list(self.oyentes):
            try:
                cb({"type": "terminal", **evento})
            except Exception:  # noqa: BLE001
                pass

    # ------------------------------------------------------------ sesiones
    def sesion(self, sid: str | None = None, crear: bool = True) -> Sesion:
        sid = (sid or "principal").strip()[:64] or "principal"
        with self._mu:
            s = self.sesiones.get(sid)
            if s is None:
                if not crear:
                    raise KeyError(f"no existe la sesión {sid}")
                tope = int(self.cfg("sesiones_max", 8) or 8)
                if len(self.sesiones) >= tope:   # se retira la que lleva mas tiempo sin usarse y no esta ocupada
                    libres = sorted((x for x in self.sesiones.values() if x.proceso is None and x.id != "principal"),
                                    key=lambda x: x.usada)
                    if libres:
                        self._cerrar_sesion(libres[0])
                s = self.sesiones[sid] = Sesion(sid, self.carpeta_base(), str(self.cfg("modo", "construir")))
            s.usada = time.time()
            return s

    def _cerrar_sesion(self, s: Sesion) -> None:
        if s.proceso is not None:
            matar_arbol(s.proceso)
        if s.interprete is not None:
            s.interprete.cerrar()
        self.sesiones.pop(s.id, None)

    def cerrar_sesion(self, sid: str) -> bool:
        with self._mu:
            s = self.sesiones.get(sid)
            if s is None:
                return False
            self._cerrar_sesion(s)
        return True

    def modo(self, sid: str | None = None, nuevo: str | None = None) -> str:
        s = self.sesion(sid)
        if nuevo:
            if nuevo not in MODOS:
                raise ValueError(f"modo debe ser uno de {MODOS}")
            s.modo = nuevo
        return s.modo

    def solo_lectura(self, sid: str | None) -> str:
        """Texto de aviso si la sesion esta en modo plan (no se cambia nada), o vacio."""
        return ("La sesión está en modo plan: solo se puede leer y buscar. Explica el plan y pide pasar al modo "
                "construir para aplicarlo.") if self.sesion(sid).modo == "plan" else ""

    def cambiar_carpeta(self, sid: str | None, carpeta: str) -> str:
        s = self.sesion(sid)
        p = self.ruta(sid, carpeta)
        if not p.is_dir():
            raise FileNotFoundError(f"no existe la carpeta {p}")
        s.carpeta = str(p)
        return s.carpeta

    # ------------------------------------------------------------ ordenes
    def _entorno(self, s: Sesion, marca: str) -> dict:
        env = {k: v for k, v in os.environ.items() if k not in s.quitadas}
        env.update(s.entorno)
        env.update(NX_MARCA=marca, NX_CWD=s.carpeta, PYTHONIOENCODING="utf-8", PYTHONUTF8="1", TERM="dumb",
                   NO_COLOR="1", PAGER="cat", GIT_PAGER="cat", GIT_TERMINAL_PROMPT="0")
        return env

    def _orden_consola(self, clase: str, exe: str, orden: str) -> list[str]:
        if clase in ("bash", "sh"):
            cola = ('\n__nx_c=$?\nprintf \'\\n%s %s %s\\n\' "$NX_MARCA" "$__nx_c" "$PWD"\n'
                    'env -0 2>/dev/null || true\n')
            return [exe, "-c", 'cd -- "$NX_CWD" 2>/dev/null\n' + orden + cola]
        if clase == "powershell":
            guion = ("$ProgressPreference='SilentlyContinue'\n"
                     "try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}\n"
                     "try { Set-Location -LiteralPath $env:NX_CWD } catch {}\n"
                     "$global:LASTEXITCODE = 0\n" + _ps_invocar(orden) + "\n"
                     "$nx_ok = $?; $nx_c = $LASTEXITCODE\n"
                     "if (-not $nx_ok -and -not $nx_c) { $nx_c = 1 }\n"
                     "if (-not $nx_c) { $nx_c = 0 }\n"
                     "Write-Output ''\n"
                     "Write-Output ('{0} {1} {2}' -f $env:NX_MARCA, $nx_c, (Get-Location).Path)\n"
                     "Write-Output ((Get-ChildItem Env: | ForEach-Object { $_.Name + '=' + $_.Value }) -join [char]0)\n")
            cod = base64.b64encode(guion.encode("utf-16-le")).decode("ascii")
            return [exe, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-OutputFormat",
                    "Text", "-EncodedCommand", cod]
        linea = (f'cd /d "%NX_CWD%" & ({orden}) & set NX_C=!errorlevel!& echo.& echo !NX_MARCA! !NX_C! !cd!')
        return [exe, "/d", "/v:on", "/s", "/c", linea]

    def ejecutar(self, orden: str, sid: str | None = None, espera_s: float | None = None, al_salir=None,
                 tope: int | None = None) -> dict:
        """Ejecuta una orden en la consola de la sesion y espera a que acabe (o la corta al agotar la espera)."""
        orden = str(orden or "").strip()
        if not orden:
            raise ValueError("Falta la orden")
        s = self.sesion(sid)
        espera = float(espera_s or self.cfg("espera_s", 120) or 120)
        tope = int(tope or self.cfg("salida_max", 30000) or 30000)
        if not s.candado.acquire(timeout=1.5):
            return {"error": "La sesión está ocupada con otra orden. Espera, interrúmpela o usa otra sesión.",
                    "codigo": None, "carpeta": s.carpeta}
        t0 = time.time()
        try:
            if not Path(s.carpeta).is_dir():
                s.carpeta = self.carpeta_base()
            clase, exe = self.consola()
            marca = "__NX_" + secrets.token_hex(8) + "__"
            try:
                proc = _popen(self._orden_consola(clase, exe, orden), s.carpeta, self._entorno(s, marca))
            except OSError as e:
                return {"error": f"No se pudo abrir la consola ({exe}): {e}", "codigo": None, "carpeta": s.carpeta}
            s.proceso = proc
            self._avisar({"evento": "inicio", "sesion": s.id, "orden": orden})
            trozos: list[str] = []
            estado = {"emitido": 0, "marca": False}
            mu = threading.Lock()

            def leer() -> None:
                dec = codecs.getincrementaldecoder("utf-8")("replace")
                try:
                    while True:
                        bloque = proc.stdout.read(65536)
                        if not bloque:
                            break
                        texto = dec.decode(bloque)
                        with mu:
                            trozos.append(texto)
                            if estado["marca"]:
                                continue
                            todo = "".join(trozos)
                            pos = todo.find("\n" + marca + " ")
                            hasta = pos if pos >= 0 else max(estado["emitido"], len(todo) - len(marca) - 3)
                            nuevo = todo[estado["emitido"]:hasta]
                            estado["emitido"] = hasta
                            estado["marca"] = pos >= 0
                        if nuevo:
                            if al_salir:
                                try:
                                    al_salir(nuevo)
                                except Exception:  # noqa: BLE001
                                    pass
                            self._avisar({"evento": "salida", "sesion": s.id, "texto": nuevo})
                except Exception:  # noqa: BLE001
                    pass

            hilo = threading.Thread(target=leer, name="nexus-term-lee", daemon=True)
            hilo.start()
            agotado = False
            try:
                proc.wait(espera)
            except subprocess.TimeoutExpired:
                agotado = True
                matar_arbol(proc)
            hilo.join(0.6 if not agotado else 2.0)   # un hijo en segundo plano puede dejar la tuberia abierta
            interrumpida = proc.returncode not in (None, 0) and getattr(proc, "_nx_cortado", False)
            with mu:
                todo = "".join(trozos)
            codigo = proc.returncode
            salida = todo
            fin = None
            for fin in re.finditer(r"\r?\n" + re.escape(marca) + r" (-?\d+) ?([^\r\n]*)\r?\n?", todo):
                pass
            if fin is not None:
                salida = todo[:fin.start()]
                codigo = int(fin.group(1))
                destino = fin.group(2).strip()
                if destino and Path(destino).is_dir():
                    s.carpeta = destino
                self._recoger_entorno(s, todo[fin.end():])
            salida = salida.replace("\r\n", "\n").rstrip("\n")
            salida = re.sub(r"#< CLIXML\s*(<Objs.*?</Objs>)?", "", salida, flags=re.S).strip("\n")
            completa = None
            corta, cortada = recortar(salida, tope)
            if cortada:
                d = paths.sub("terminal", "salidas")
                completa = d / f"{re.sub(r'[^A-Za-z0-9_-]', '_', s.id)[:24]}-{int(t0)}.txt"
                completa.write_text(salida, encoding="utf-8", errors="replace")
            ms = round((time.time() - t0) * 1000)
            out = {"salida": corta, "codigo": codigo, "carpeta": s.carpeta, "ms": ms, "consola": clase}
            if cortada:
                out.update(cortada=True, salida_completa=str(completa), caracteres=len(salida))
            if agotado:
                out.update(agotada=True, nota=f"Se cortó a los {espera:g} s. Para algo largo usa un proceso de fondo.")
            elif interrumpida:
                out["interrumpida"] = True
            s.historial.append({"ts": t0, "orden": orden[:500], "codigo": codigo, "ms": ms})
            del s.historial[:-200]
            self._avisar({"evento": "fin", "sesion": s.id, "codigo": codigo, "carpeta": s.carpeta, "ms": ms})
            return out
        finally:
            s.proceso = None
            s.usada = time.time()
            s.candado.release()

    def _recoger_entorno(self, s: Sesion, crudo: str) -> None:
        """Variables que la orden ha exportado o quitado: valen para las ordenes siguientes de la sesion."""
        if not crudo or len(crudo) > 400_000:
            return
        visto: dict[str, str] = {}
        for par in crudo.split("\0"):
            k, sep, v = par.strip("\r\n").partition("=")
            if sep and k and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_()]*", k) and k.upper() not in _NO_ENTORNO \
                    and not k.startswith(("__nx", "BASH_FUNC")) and k not in ("NX_C", "nx_c"):
                visto[k] = v
        if len(visto) < 3:    # la consola no supo volcar el entorno (sh antiguo): se deja como estaba
            return
        fijos = {"PYTHONIOENCODING", "PYTHONUTF8", "TERM", "NO_COLOR", "PAGER", "GIT_PAGER", "GIT_TERMINAL_PROMPT"}
        base = os.environ
        claves = {k.upper(): k for k in base} if WINDOWS else None
        for k, v in visto.items():
            if k in fijos:
                continue
            original = base.get(claves.get(k.upper(), k)) if claves else base.get(k)
            if original != v:
                s.entorno[k] = v
            else:
                s.entorno.pop(k, None)
            s.quitadas.discard(k)
        vistas = {k.upper() for k in visto} if WINDOWS else set(visto)
        for k in list(s.entorno):
            if (k.upper() if WINDOWS else k) not in vistas:
                s.entorno.pop(k, None)
        for k in base:
            if (k.upper() if WINDOWS else k) not in vistas and k.upper() not in _NO_ENTORNO and k not in fijos:
                s.quitadas.add(k)

    def interrumpir(self, sid: str | None = None) -> bool:
        s = self.sesion(sid)
        proc = s.proceso
        if proc is None:
            return False
        proc._nx_cortado = True   # type: ignore[attr-defined]
        matar_arbol(proc)
        return True

    # ------------------------------------------------------------ procesos de fondo
    def lanzar(self, orden: str, sid: str | None = None, nombre: str = "") -> dict:
        s = self.sesion(sid)
        clase, exe = self.consola()
        if clase in ("bash", "sh"):
            cmd = [exe, "-c", orden]
        elif clase == "powershell":
            cmd = [exe, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text",
                   "-EncodedCommand", base64.b64encode(
                       ("$ProgressPreference='SilentlyContinue'\n" + _ps_invocar(orden)).encode("utf-16-le")).decode("ascii")]
        else:
            cmd = [exe, "/d", "/s", "/c", orden]
        vivos = [p for p in self.fondo.values() if p.proc.poll() is None]
        if len(vivos) >= 12:
            raise RuntimeError("Hay demasiados procesos de fondo en marcha; para alguno antes de lanzar otro")
        proc = _popen(cmd, s.carpeta, self._entorno(s, ""), stdin=subprocess.PIPE)
        pid = secrets.token_hex(4)
        with self._mu:
            for viejo in [k for k, p in self.fondo.items() if p.proc.poll() is not None][:-8]:
                self.fondo.pop(viejo, None)
            self.fondo[pid] = ProcesoFondo(pid, orden, proc, s.carpeta, nombre)
        return self.fondo[pid].publico()

    def _fondo(self, pid: str) -> ProcesoFondo:
        p = self.fondo.get(str(pid))
        if p is None:
            raise KeyError(f"no hay ningún proceso de fondo con id {pid}")
        return p

    def leer_proceso(self, pid: str, desde: int = 0, espera_s: float = 0.0) -> dict:
        p = self._fondo(pid)
        fin = time.time() + min(float(espera_s or 0), 60)
        while time.time() < fin and p.proc.poll() is None and p._total <= desde:
            time.sleep(0.1)
        return p.leer(desde)

    def escribir_proceso(self, pid: str, texto: str) -> dict:
        p = self._fondo(pid)
        if p.proc.poll() is not None:
            raise RuntimeError("El proceso ya terminó")
        p.proc.stdin.write(texto.encode("utf-8"))
        p.proc.stdin.flush()
        return {"id": p.id, "escrito": len(texto)}

    def parar_proceso(self, pid: str) -> dict:
        p = self._fondo(pid)
        matar_arbol(p.proc)
        try:
            p.proc.wait(3)
        except subprocess.TimeoutExpired:
            pass
        return p.publico()

    def procesos(self) -> list[dict]:
        return [p.publico() for p in self.fondo.values()]

    # ------------------------------------------------------------ interprete de codigo
    def codigo(self, lenguaje: str, codigo: str, sid: str | None = None, espera_s: float | None = None) -> dict:
        """Ejecuta codigo. Python conserva variables e importaciones entre llamadas de la misma sesion."""
        from .interprete import Interprete
        s = self.sesion(sid)
        leng = str(lenguaje or "python").strip().lower()
        espera = float(espera_s or self.cfg("espera_s", 120) or 120)
        tope = int(self.cfg("salida_max", 30000) or 30000)
        if leng in ("python", "py", "python3"):
            if s.interprete is None or not s.interprete.vivo():
                s.interprete = Interprete(s.carpeta, self._entorno(s, ""))
            r = s.interprete.ejecutar(codigo, espera)
            r["salida"], cortada = recortar(r.get("salida") or "", tope)
            if cortada:
                r["cortada"] = True
            s.usada = time.time()
            return {"lenguaje": "python", **r}
        if leng in ("shell", "sh", "bash", "consola", "cmd", "powershell", "ps1"):
            return {"lenguaje": "consola", **self.ejecutar(codigo, sid, espera)}
        if leng in ("javascript", "js", "node"):
            node = shutil.which("node")
            if not node:
                return {"lenguaje": "javascript", "error": "Node.js no está instalado en este equipo"}
            d = paths.sub("terminal", "tmp")
            f = d / f"nx-{secrets.token_hex(4)}.js"
            f.write_text(codigo, encoding="utf-8")
            try:
                return {"lenguaje": "javascript", **self.ejecutar(f'"{node}" "{f}"', sid, espera)}
            finally:
                f.unlink(missing_ok=True)
        if leng in ("html",):
            f = paths.outputs_dir() / f"terminal-{int(time.time())}.html"
            f.write_text(codigo, encoding="utf-8")
            return {"lenguaje": "html", "fichero": str(f), "nota": "Guardado; ábrelo en el navegador"}
        return {"error": f"Lenguaje no admitido: {lenguaje}. Usa python, consola, javascript o html"}

    def reiniciar_interprete(self, sid: str | None = None) -> bool:
        s = self.sesion(sid)
        if s.interprete is not None:
            s.interprete.cerrar()
            s.interprete = None
            return True
        return False

    # ------------------------------------------------------------ ficheros
    def ruta(self, sid: str | None, ruta: str) -> Path:
        p = Path(os.path.expandvars(str(ruta or "."))).expanduser()
        if not p.is_absolute():
            p = Path(self.sesion(sid).carpeta) / p
        return Path(os.path.normpath(str(p)))

    @staticmethod
    def _texto(p: Path) -> str:
        datos = p.read_bytes()
        if b"\0" in datos[:8192]:
            raise ValueError(f"{p.name} parece un fichero binario")
        for cod in ("utf-8-sig", "utf-8"):
            try:
                return datos.decode(cod)
            except UnicodeDecodeError:
                continue
        return datos.decode("latin-1")

    def leer(self, ruta: str, sid: str | None = None, desde: int = 1, lineas: int = 400, numeradas: bool = True) -> dict:
        p = self.ruta(sid, ruta)
        if p.is_dir():
            return self.listar(ruta, sid)
        if not p.is_file():
            raise FileNotFoundError(f"no existe {p}")
        if p.stat().st_size > 20 * 1024 * 1024:
            raise ValueError("El fichero pasa de 20 MB; búscalo por partes con buscar o con una orden")
        todas = self._texto(p).splitlines()
        desde = max(1, int(desde or 1))
        trozo = todas[desde - 1:desde - 1 + max(1, min(int(lineas or 400), 3000))]
        cuerpo = "\n".join((f"{desde + i:>5}  {t}" if numeradas else t) for i, t in enumerate(trozo))
        cuerpo, cortada = recortar(cuerpo, 60000)
        out = {"ruta": str(p), "desde": desde, "hasta": desde + len(trozo) - 1, "total_lineas": len(todas), "texto": cuerpo}
        if desde + len(trozo) - 1 < len(todas):
            out["siguiente"] = desde + len(trozo)
        if cortada:
            out["cortada"] = True
        return out

    def _copia(self, p: Path, sid: str) -> None:
        """Guarda como estaba un fichero antes de cambiarlo, para poder deshacer."""
        d = paths.sub("terminal", "copias")
        indice = d / "indice.json"
        try:
            lista = json.loads(indice.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            lista = []
        entrada = {"ruta": str(p), "ts": time.time(), "sesion": sid, "existia": p.is_file(), "copia": ""}
        if p.is_file() and p.stat().st_size <= 30 * 1024 * 1024:
            nombre = f"{int(time.time() * 1000)}-{hashlib.sha1(str(p).encode()).hexdigest()[:10]}"
            shutil.copy2(p, d / nombre)
            entrada["copia"] = nombre
        lista.append(entrada)
        for vieja in lista[:-200]:
            if vieja.get("copia"):
                (d / vieja["copia"]).unlink(missing_ok=True)
        indice.write_text(json.dumps(lista[-200:], ensure_ascii=False), encoding="utf-8")

    def escribir(self, ruta: str, contenido: str, sid: str | None = None) -> dict:
        s = self.sesion(sid)
        p = self.ruta(sid, ruta)
        existia = p.is_file()
        self._copia(p, s.id)
        p.parent.mkdir(parents=True, exist_ok=True)
        salto = "\r\n" if existia and b"\r\n" in p.read_bytes()[:4096] else None
        with open(p, "w", encoding="utf-8", newline=salto or "") as f:
            f.write(contenido.replace("\r\n", "\n").replace("\n", salto) if salto else contenido)
        return {"ruta": str(p), "creado": not existia, "lineas": contenido.count("\n") + 1, "bytes": p.stat().st_size}

    def editar(self, ruta: str, buscar: str, reemplazar: str, sid: str | None = None, todas: bool = False) -> dict:
        """Cambia un texto exacto por otro. El texto buscado tiene que aparecer una sola vez (o usar todas)."""
        s = self.sesion(sid)
        p = self.ruta(sid, ruta)
        if not p.is_file():
            raise FileNotFoundError(f"no existe {p}")
        if buscar == reemplazar:
            raise ValueError("El texto nuevo es igual al que se busca")
        crudo = p.read_bytes()
        salto = "\r\n" if b"\r\n" in crudo[:8192] else "\n"
        texto = self._texto(p).replace("\r\n", "\n")
        buscar_n, reemplazar_n = buscar.replace("\r\n", "\n"), reemplazar.replace("\r\n", "\n")
        veces = texto.count(buscar_n) if buscar_n else 0
        if veces == 0:
            pista = ""
            limpio = buscar_n.strip()
            if limpio and limpio in texto:
                pista = " Revisa los espacios y saltos de línea del principio y del final."
            raise ValueError(f"No se encuentra el texto en {p.name}.{pista} Lee el fichero y copia el texto exacto.")
        if veces > 1 and not todas:
            raise ValueError(f"El texto aparece {veces} veces en {p.name}: añade más contexto para que sea único "
                             "o usa todas=true")
        self._copia(p, s.id)
        nuevo = texto.replace(buscar_n, reemplazar_n) if todas else texto.replace(buscar_n, reemplazar_n, 1)
        linea = texto[:texto.find(buscar_n)].count("\n") + 1
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write(nuevo.replace("\n", salto) if salto != "\n" else nuevo)
        return {"ruta": str(p), "cambios": veces if todas else 1, "linea": linea,
                "lineas_antes": texto.count("\n") + 1, "lineas_despues": nuevo.count("\n") + 1}

    def deshacer(self, sid: str | None = None, ruta: str = "") -> dict:
        """Devuelve el ultimo fichero cambiado (o el indicado) a como estaba antes del cambio."""
        d = paths.sub("terminal", "copias")
        indice = d / "indice.json"
        try:
            lista = json.loads(indice.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            lista = []
        quiero = str(self.ruta(sid, ruta)) if ruta else ""
        for i in range(len(lista) - 1, -1, -1):
            e = lista[i]
            if quiero and e["ruta"] != quiero:
                continue
            p = Path(e["ruta"])
            if e.get("copia") and (d / e["copia"]).is_file():
                shutil.copy2(d / e["copia"], p)
                (d / e["copia"]).unlink(missing_ok=True)
                hecho = "restaurado"
            elif not e.get("existia"):
                p.unlink(missing_ok=True)
                hecho = "borrado (no existía antes del cambio)"
            else:
                continue
            lista.pop(i)
            indice.write_text(json.dumps(lista, ensure_ascii=False), encoding="utf-8")
            return {"ruta": e["ruta"], "hecho": hecho, "quedan": len(lista)}
        return {"error": "No hay cambios que deshacer" + (f" en {quiero}" if quiero else "")}

    def listar(self, ruta: str = ".", sid: str | None = None, patron: str = "", limite: int = 400) -> dict:
        """Contenido de una carpeta o, con patron (src/**/*.py), los ficheros que casan, del mas reciente al mas viejo."""
        base = self.ruta(sid, ruta)
        if not base.is_dir():
            raise FileNotFoundError(f"no existe la carpeta {base}")
        if patron:
            encontrados = []
            fin = time.time() + 15
            for raiz, dirs, ficheros in os.walk(base):
                dirs[:] = [x for x in dirs if x not in _SALTAR]
                for f in ficheros:
                    rel = os.path.relpath(os.path.join(raiz, f), base).replace("\\", "/")
                    if fnmatch.fnmatch(rel, patron) or fnmatch.fnmatch(f, patron) or \
                            fnmatch.fnmatch(rel, patron.replace("**/", "")):
                        try:
                            encontrados.append((os.path.getmtime(os.path.join(raiz, f)), rel))
                        except OSError:
                            pass
                if time.time() > fin or len(encontrados) > 20000:
                    break
            encontrados.sort(reverse=True)
            return {"carpeta": str(base), "patron": patron, "ficheros": [r for _, r in encontrados[:limite]],
                    "total": len(encontrados)}
        filas = []
        for e in sorted(os.scandir(base), key=lambda e: (not e.is_dir(), e.name.lower())):
            try:
                st = e.stat()
                filas.append({"nombre": e.name, "carpeta": e.is_dir(), "bytes": 0 if e.is_dir() else st.st_size,
                              "modificado": st.st_mtime})
            except OSError:
                filas.append({"nombre": e.name, "carpeta": False, "bytes": 0, "modificado": 0})
        return {"carpeta": str(base), "padre": str(base.parent), "entradas": filas[:limite], "total": len(filas)}

    def arbol(self, ruta: str = ".", sid: str | None = None, profundidad: int = 3, tope: int = 400) -> dict:
        base = self.ruta(sid, ruta)
        if not base.is_dir():
            raise FileNotFoundError(f"no existe la carpeta {base}")
        lineas: list[str] = []

        def bajar(p: Path, nivel: int) -> None:
            if len(lineas) >= tope:
                return
            try:
                hijos = sorted(os.scandir(p), key=lambda e: (not e.is_dir(), e.name.lower()))
            except OSError:
                return
            for e in hijos:
                if len(lineas) >= tope:
                    lineas.append("  " * nivel + "…")
                    return
                if e.is_dir():
                    if e.name in _SALTAR:
                        lineas.append("  " * nivel + e.name + "/ (omitida)")
                        continue
                    lineas.append("  " * nivel + e.name + "/")
                    if nivel + 1 < profundidad:
                        bajar(Path(e.path), nivel + 1)
                else:
                    lineas.append("  " * nivel + e.name)
        bajar(base, 0)
        return {"carpeta": str(base), "arbol": "\n".join(lineas), "entradas": len(lineas)}

    def buscar(self, patron: str, ruta: str = ".", sid: str | None = None, ficheros: str = "", limite: int = 120,
               contexto: int = 0, mayusculas: bool = False) -> dict:
        """Busca una expresion regular en el contenido de los ficheros de una carpeta (como grep -rn)."""
        base = self.ruta(sid, ruta)
        try:
            rx = re.compile(patron, 0 if mayusculas else re.I)
        except re.error:
            rx = re.compile(re.escape(patron), 0 if mayusculas else re.I)
        lista = [base] if base.is_file() else None
        if lista is None and not base.is_dir():
            raise FileNotFoundError(f"no existe {base}")
        aciertos, vistos, fin, corte = [], 0, time.time() + 25, False

        def caminar():
            for raiz, dirs, nombres in os.walk(base):
                dirs[:] = [x for x in dirs if x not in _SALTAR]
                for f in nombres:
                    yield Path(raiz) / f
        for p in (lista or caminar()):
            if time.time() > fin or len(aciertos) >= limite:
                corte = True
                break
            rel = os.path.relpath(p, base).replace("\\", "/") if lista is None else p.name
            if ficheros and not (fnmatch.fnmatch(rel, ficheros) or fnmatch.fnmatch(p.name, ficheros)):
                continue
            try:
                if p.stat().st_size > 3 * 1024 * 1024:
                    continue
                lineas = self._texto(p).splitlines()
            except (OSError, ValueError):
                continue
            vistos += 1
            for i, linea in enumerate(lineas):
                if rx.search(linea):
                    a = {"fichero": rel, "linea": i + 1, "texto": linea.strip()[:300]}
                    if contexto:
                        a["contexto"] = "\n".join(lineas[max(0, i - contexto): i + contexto + 1])[:1200]
                    aciertos.append(a)
                    if len(aciertos) >= limite:
                        break
        return {"patron": patron, "carpeta": str(base), "aciertos": aciertos, "ficheros_leidos": vistos,
                **({"cortada": True} if corte else {})}

    # ------------------------------------------------------------ tareas y contexto del agente
    def tareas(self, sid: str | None = None, lista: list | None = None) -> list[dict]:
        s = self.sesion(sid)
        if lista is not None:
            limpias = []
            for t in lista[:40]:
                if isinstance(t, str):
                    t = {"texto": t}
                estado = str(t.get("estado") or "pendiente")
                limpias.append({"texto": str(t.get("texto") or t.get("tarea") or "")[:300],
                                "estado": estado if estado in ("pendiente", "en_curso", "hecha") else "pendiente"})
            s.tareas = [t for t in limpias if t["texto"]]
            self._avisar({"evento": "tareas", "sesion": s.id, "tareas": s.tareas})
        return s.tareas

    def _carpetas_proyecto(self, carpeta: str) -> list[Path]:
        p = Path(carpeta)
        return [p, *list(p.parents)[:4]]

    def instrucciones(self, carpeta: str) -> tuple[str, str]:
        """AGENTS.md del proyecto (en la carpeta o en alguna de las cuatro de encima)."""
        for d in self._carpetas_proyecto(carpeta):
            for nombre in ("AGENTS.md", "agents.md"):
                f = d / nombre
                if f.is_file():
                    try:
                        return str(f), self._texto(f)[:6000]
                    except (OSError, ValueError):
                        return "", ""
        return "", ""

    def habilidades(self, carpeta: str) -> list[dict]:
        """Habilidades disponibles: .agents/skills/<nombre>/SKILL.md del proyecto y las de data/terminal/habilidades."""
        out, vistas = [], set()
        raices = [d / ".agents" / "skills" for d in self._carpetas_proyecto(carpeta)] + [paths.sub("terminal", "habilidades")]
        for raiz in raices:
            if not raiz.is_dir():
                continue
            for f in sorted(raiz.glob("*/SKILL.md")):
                try:
                    texto = self._texto(f)[:3000]
                except (OSError, ValueError):
                    continue
                nombre = re.search(r"(?m)^name:\s*(.+)$", texto)
                desc = re.search(r"(?m)^description:\s*(.+)$", texto)
                n = (nombre.group(1) if nombre else f.parent.name).strip().strip("\"'")
                if n in vistas:
                    continue
                vistas.add(n)
                out.append({"nombre": n, "descripcion": (desc.group(1).strip().strip("\"'") if desc else "")[:300],
                            "fichero": str(f)})
        return out[:40]

    def ordenes(self, carpeta: str | None = None) -> dict[str, dict]:
        """Ordenes personalizadas: ficheros .md cuyo texto es la peticion ($ARGUMENTOS se sustituye)."""
        out: dict[str, dict] = {}
        raices = [paths.sub("terminal", "ordenes")]
        if carpeta:
            raices += [d / ".agents" / "commands" for d in reversed(self._carpetas_proyecto(carpeta))]
        for raiz in raices:
            if not raiz.is_dir():
                continue
            for f in sorted(raiz.glob("*.md")):
                try:
                    texto = self._texto(f)
                except (OSError, ValueError):
                    continue
                cuerpo = re.sub(r"(?s)\A---\n.*?\n---\n", "", texto).strip()
                desc = re.search(r"(?m)^description:\s*(.+)$", texto)
                out[f.stem.lower()] = {"nombre": f.stem.lower(), "texto": cuerpo, "fichero": str(f),
                                       "descripcion": (desc.group(1).strip() if desc else cuerpo.splitlines()[0][:120]
                                                       if cuerpo else "")}
        return out

    def expandir(self, texto: str, sid: str | None = None) -> str:
        """«/orden argumentos» -> el texto de la orden personalizada con sus argumentos; si no existe, tal cual."""
        m = re.match(r"\s*/([\w-]+)\s*(.*)\Z", texto or "", re.S)
        if not m:
            return texto
        o = self.ordenes(self.sesion(sid).carpeta).get(m.group(1).lower())
        if not o:
            return texto
        args = m.group(2).strip()
        cuerpo = o["texto"]
        if "$ARGUMENTOS" in cuerpo or "$ARGUMENTS" in cuerpo:
            return cuerpo.replace("$ARGUMENTOS", args).replace("$ARGUMENTS", args)
        return cuerpo + (("\n\n" + args) if args else "")

    def contexto(self, sid: str | None = None) -> str:
        """Lo que el agente necesita saber de la sesion: sistema, consola, carpeta, modo, instrucciones y tareas."""
        s = self.sesion(sid)
        clase, _ = self.consola()
        sistema = "Android" if os.environ.get("ANDROID_ROOT") and not WINDOWS else \
            {"win32": "Windows", "darwin": "macOS"}.get(sys.platform, "Linux")
        partes = [f"Terminal: sistema {sistema}, consola {clase}, carpeta de trabajo {s.carpeta}, modo {s.modo}."]
        if s.modo == "plan":
            partes.append("Estás en modo plan: lee y busca lo que haga falta y propón un plan por pasos; no ejecutes "
                          "órdenes ni cambies ficheros hasta que el usuario pase al modo construir.")
        fichero, texto = self.instrucciones(s.carpeta)
        if texto:
            partes.append(f"Instrucciones del proyecto ({fichero}), que debes seguir:\n{texto}")
        hab = self.habilidades(s.carpeta)
        if hab:
            partes.append("Habilidades disponibles (lee su fichero con terminal_leer cuando venga al caso):\n" +
                          "\n".join(f"- {h['nombre']}: {h['descripcion']} [{h['fichero']}]" for h in hab[:20]))
        if s.tareas:
            marca = {"pendiente": "[ ]", "en_curso": "[~]", "hecha": "[x]"}
            partes.append("Lista de tareas de esta sesión:\n" + "\n".join(f"{marca[t['estado']]} {t['texto']}"
                                                                       for t in s.tareas))
        return "\n\n".join(partes)

    # ------------------------------------------------------------ estado y cierre
    def estado(self) -> dict:
        clase, exe = self.consola()
        return {"activo": bool(self.cfg("activo", True)), "consola": clase, "ejecutable": exe,
                "carpeta_base": self.carpeta_base(), "sesiones": [s.publico() for s in self.sesiones.values()],
                "procesos": self.procesos(), "modos": list(MODOS), "node": bool(shutil.which("node")),
                "espera_s": self.cfg("espera_s", 120), "con_ficha": bool(self.cfg("ficha", ""))}

    def cerrar(self) -> None:
        with self._mu:
            for s in list(self.sesiones.values()):
                self._cerrar_sesion(s)
        for p in list(self.fondo.values()):
            matar_arbol(p.proc)
