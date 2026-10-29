"""Subprocesos ligados a la vida de Nexus-Office: si el proceso principal muere (incluso de forma abrupta),
los runtimes hijos (llama-server, sd-cli...) mueren con el. Windows: Job Object con
KILL_ON_JOB_CLOSE. Linux: PR_SET_PDEATHSIG. macOS: limpieza al salir (atexit)."""
from __future__ import annotations

import atexit
import ctypes
import os
import subprocess
import sys

_job = None
_children: list[subprocess.Popen] = []


def _windows_job():
    global _job
    if _job is not None:
        return _job
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE

    class BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class IO(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ("r", "w", "o", "rb", "wb", "ob")]

    class EXT(ctypes.Structure):
        _fields_ = [("Basic", BASIC), ("Io", IO), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    job = k32.CreateJobObjectW(None, None)
    info = EXT()
    info.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))  # ExtendedLimitInformation
    _job = (k32, job)
    return _job


def _linux_pdeathsig():
    for name in ("libc.so.6", None):   # None: la libc ya cargada (musl en la app Android autonoma)
        try:
            ctypes.CDLL(name, use_errno=True).prctl(1, 15)  # PR_SET_PDEATHSIG, SIGTERM
            return
        except (OSError, AttributeError):
            pass


def python_cmd() -> list[str]:
    """Orden para lanzar otro Python como el actual. En la app Android autonoma el interprete vive en los datos
    de la app, donde Android no deja ejecutar ficheros: se arranca a traves del cargador de musl (NEXUS_PY_LOADER),
    que si esta en la carpeta de bibliotecas nativas de la APK."""
    loader = os.environ.get("NEXUS_PY_LOADER", "")
    if not loader:
        return [sys.executable]
    pre = os.environ.get("NEXUS_PY_PRELOAD", "")   # manejador de SIGSYS de la app (llamadas que Android prohibe)
    return [loader, "--library-path", os.environ.get("NEXUS_PY_LIBRARY_PATH", ""), *(["--preload", pre] if pre else []),
            sys.executable]


def popen(cmd: list[str], **kw) -> subprocess.Popen:
    if os.name == "nt":
        kw.setdefault("creationflags", 0x08000000)  # CREATE_NO_WINDOW
    elif sys.platform.startswith("linux"):
        kw.setdefault("preexec_fn", _linux_pdeathsig)
    p = subprocess.Popen(cmd, **kw)
    if os.name == "nt":
        try:
            k32, job = _windows_job()
            k32.AssignProcessToJobObject(job, ctypes.c_void_p(int(p._handle)))
        except Exception:  # noqa: BLE001
            pass
    _children.append(p)
    return p


@atexit.register
def _cleanup() -> None:
    for p in _children:
        if p.poll() is None:
            try:
                p.terminate()
            except OSError:
                pass
