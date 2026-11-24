"""Turnos de modelos: si la memoria no da para tener todos los modelos cargados a la vez (texto, vision, imagen, voz,
embeddings...), se cargan y descargan segun la etapa del flujo de trabajo en curso.

Cada parte que tiene un modelo residente lo registra con una estimacion de lo que ocupa (RAM y VRAM), una forma de
saber si esta cargado y una forma de liberarlo. Cada paso del flujo (responder con el LLM, generar una imagen,
transcribir, indexar...) entra en una *etapa* que dice que tipos de modelo necesita:

* si todo cabe, no se toca nada (es lo normal en un equipo con memoria de sobra);
* si no cabe, antes de empezar se liberan los modelos que esa etapa no usa, del menos usado recientemente al mas,
  hasta que haya sitio; asi cada paso tiene para si toda la memoria y puede usar el mejor modelo posible;
* cada etapa recibe su *presupuesto* (MB libres para ella) y la parte que la usa lo aprovecha: el LLM ajusta el
  contexto, la imagen la resolucion y el troceado del VAE y la voz el tamano de whisper, en vez de quedarse sin
  memoria o tirar de swap;
* las tareas de fondo (indexacion semantica) nunca echan a otro modelo: esperan a que haya sitio.

Convive con el Autoajuste adaptativo (nexus/hardware/tuner.py): las recargas de cada sesion se apuntan en el monitor
y, si son demasiadas, el autoajuste recomienda un modelo mas pequeno al siguiente arranque.
"""
from __future__ import annotations

import collections
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Callable

from .. import log

L = log.get("turnos")

MODOS = ("auto", "siempre", "nunca")
MARGEN_MB = 384          # holgura que se deja libre al decidir si algo cabe
WHISPER_MB = {"tiny": 200, "base": 350, "small": 1000, "medium": 2600, "large-v3": 5000, "large": 5000}
WHISPER_ORDEN = ["large-v3", "medium", "small", "base", "tiny"]


@dataclass
class Residente:
    """Un modelo que se queda cargado entre usos."""
    tipo: str                                   # llm | embed | stt | tts | imagen | ...
    nombre: str
    estimar: Callable[[], tuple[float, float]]  # (MB de RAM, MB de VRAM) que ocupa o va a ocupar
    cargado: Callable[[], bool] = lambda: False
    liberar: Callable[[], None] = lambda: None
    ultimo: float = 0.0
    recargas: int = 0
    liberado: bool = False


@dataclass
class Turno:
    """Lo que recibe un paso del flujo al entrar en su etapa."""
    etapa: str
    tipos: tuple
    alternando: bool
    ram_mb: float                 # memoria libre para esta etapa (incluye lo que ya tiene cargado)
    vram_mb: float
    liberados: list = field(default_factory=list)

    def cabe(self, ram_mb: float, vram_mb: float = 0.0) -> bool:
        return ram_mb <= self.ram_mb and (vram_mb <= 0 or vram_mb <= self.vram_mb)


def kv_mb(peso_mb: float, n_ctx: int) -> float:
    """Cache KV aproximada de un LLM cuantizado (GQA, f16): ~2e-5 MB por token y MB de modelo."""
    return n_ctx * peso_mb * 2e-5


def llm_mb(peso_mb: float, n_ctx: int, vision: bool = False) -> float:
    return peso_mb * 1.05 + kv_mb(peso_mb, n_ctx) + 250 + (450 if vision else 0)


def imagen_mb(peso_mb: float, lado: int, troceado: bool = False) -> float:
    calculo = 700 * (max(256, lado) / 512) ** 2
    return peso_mb * 1.1 + (calculo * 0.4 if troceado else calculo)


def whisper_para(presupuesto_mb: float, preferido: str) -> str:
    """El whisper mas grande que cabe, sin pasar del que pide el perfil."""
    orden = WHISPER_ORDEN[WHISPER_ORDEN.index(preferido):] if preferido in WHISPER_ORDEN else WHISPER_ORDEN
    for m in orden:
        if WHISPER_MB[m] + MARGEN_MB <= presupuesto_mb:
            return m
    return orden[-1]


class Turnos:
    def __init__(self, config=None, profile_getter: Callable[[], dict] | None = None, monitor=None) -> None:
        self.config = config
        self.profile_getter = profile_getter or (lambda: {})
        self.monitor = monitor
        self.residentes: dict[str, Residente] = {}
        self.uso: collections.Counter = collections.Counter()   # etapas en curso por tipo de modelo
        self.primer_plano = 0
        self.actual: list[str] = []
        self.historial: collections.deque = collections.deque(maxlen=40)
        self.recargas = 0
        self._lock = threading.RLock()
        self._cache: tuple[float, bool, dict] = (0.0, False, {})
        # para pruebas: medidas de memoria inyectables
        self.medir_ram: Callable[[], tuple[float, float]] = self._ram_sistema
        self.medir_vram: Callable[[], tuple[float, float]] = self._vram_sistema

    # ------------------------------------------------------------ registro
    def registrar(self, tipo: str, nombre: str, estimar, cargado=None, liberar=None) -> Residente:
        r = Residente(tipo, nombre, estimar, cargado or (lambda: False), liberar or (lambda: None))
        self.residentes[tipo] = r
        self._cache = (0.0, False, {})
        return r

    # ------------------------------------------------------------ memoria
    def modo(self) -> str:
        m = self.config.get("hardware.alternar_modelos", "auto") if self.config is not None else "auto"
        return m if m in MODOS else "auto"

    def _movil(self) -> bool:
        from .profiles import android
        return android()

    def _ram_sistema(self) -> tuple[float, float]:
        """(MB totales, MB disponibles ahora)."""
        try:
            import psutil
            vm = psutil.virtual_memory()
            return vm.total / 2**20, vm.available / 2**20
        except Exception:  # noqa: BLE001
            gb = float(self.profile_getter().get("ram_gb") or 8)
            return gb * 1024, gb * 512

    def _vram_sistema(self) -> tuple[float, float]:
        """(MB totales, MB libres) de la GPU dedicada; (0, 0) si no hay o no se usa."""
        if not self.gpu_dedicada():
            return 0.0, 0.0
        total = float(self.profile_getter().get("vram_gb") or 0) * 1024
        libre = total
        try:
            cur = (self.monitor.samples[-1] if self.monitor and self.monitor.samples else {})
            if cur.get("vram_used_gb") is not None:
                libre = total - float(cur["vram_used_gb"]) * 1024
        except Exception:  # noqa: BLE001
            pass
        return total, max(0.0, libre)

    def gpu_dedicada(self) -> bool:
        p = self.profile_getter() or {}
        return str(p.get("id", "")).endswith("dgpu") and float(p.get("vram_gb") or 0) > 0 and not self._movil()

    def reserva_mb(self, total_mb: float) -> float:
        """Lo que se deja al sistema y a las demas apps."""
        fija = float(self.config.get("hardware.reserva_ram_mb", 0) or 0) if self.config is not None else 0.0
        if fija > 0:
            return fija
        if self._movil():   # Android cierra apps en segundo plano mucho antes de llenar la RAM
            return total_mb * 0.45
        return max(1536.0, total_mb * 0.2)

    def capacidad(self) -> dict:
        total, _ = self.medir_ram()
        vtotal, _ = self.medir_vram()
        return {"ram_mb": round(max(256.0, total - self.reserva_mb(total))), "vram_mb": round(vtotal * 0.9)}

    def necesidad(self) -> dict:
        """Lo que ocuparian todos los modelos registrados cargados a la vez."""
        ram = vram = 0.0
        por_tipo = {}
        for t, r in self.residentes.items():
            try:
                a, b = r.estimar()
            except Exception:  # noqa: BLE001
                a, b = 0.0, 0.0
            por_tipo[t] = {"ram_mb": round(a), "vram_mb": round(b)}
            ram, vram = ram + a, vram + b
        return {"ram_mb": round(ram), "vram_mb": round(vram), "por_tipo": por_tipo}

    def alternando(self) -> bool:
        """Si hay que alternar: modo «siempre», o en «auto» cuando los modelos no caben todos juntos."""
        modo = self.modo()
        if modo != "auto":
            return modo == "siempre"
        t, val, _ = self._cache
        if time.time() - t < 20:
            return val
        cap, nec = self.capacidad(), self.necesidad()
        val = nec["ram_mb"] > cap["ram_mb"] or (cap["vram_mb"] > 0 and nec["vram_mb"] > cap["vram_mb"])
        self._cache = (time.time(), val, nec)
        return val

    def _libre(self) -> tuple[float, float]:
        _, ram = self.medir_ram()
        _, vram = self.medir_vram()
        # en el movil no se cuenta la reserva como libre aunque Android la vea disponible
        if self._movil():
            ram = min(ram, self.capacidad()["ram_mb"] - sum(
                r.estimar()[0] for r in self.residentes.values() if self._cargado(r)))
        return ram - MARGEN_MB, vram - MARGEN_MB if vram else 0.0

    @staticmethod
    def _cargado(r: Residente) -> bool:
        try:
            return bool(r.cargado())
        except Exception:  # noqa: BLE001
            return False

    # ------------------------------------------------------------ etapas
    def _falta(self, tipos, extra: dict) -> tuple[float, float]:
        ram = vram = 0.0
        for t in tipos:
            if t in extra:
                a, b = extra[t]
            else:
                r = self.residentes.get(t)
                if r is None or self._cargado(r):
                    continue
                a, b = r.estimar()
            ram, vram = ram + a, vram + b
        return ram, vram

    def _liberar(self, r: Residente) -> bool:
        try:
            r.liberar()
            r.liberado = True
            L.info("Turnos: se libera %s (%s) para la etapa en curso", r.nombre, r.tipo)
            return True
        except Exception as e:  # noqa: BLE001
            L.warning("Turnos: no se pudo liberar %s: %s", r.nombre, e)
            return False

    def _hacer_sitio(self, tipos, extra: dict) -> list[str]:
        modo = self.modo()
        candidatos = sorted((r for t, r in self.residentes.items()
                             if t not in tipos and not self.uso[t] and self._cargado(r)), key=lambda r: r.ultimo)
        liberados = []
        if modo == "siempre":
            for r in candidatos:
                if self._liberar(r):
                    liberados.append(r.tipo)
            return liberados
        falta_ram, falta_vram = self._falta(tipos, extra)
        libre_ram, libre_vram = self._libre()
        for r in candidatos:
            sobra_vram = not self.gpu_dedicada() or falta_vram <= libre_vram
            if falta_ram <= libre_ram and sobra_vram:
                break
            a, b = r.estimar()
            if falta_ram <= libre_ram and b <= 0:   # solo falta VRAM y este no la ocupa
                continue
            if self._liberar(r):
                liberados.append(r.tipo)
                time.sleep(0.05)
                nr, nv = self._libre()
                # si el sistema aun no ha devuelto la memoria, se cuenta lo que ocupaba
                libre_ram, libre_vram = max(nr, libre_ram + a), max(nv, libre_vram + b)
        return liberados

    def _presupuesto(self, tipos) -> tuple[float, float]:
        libre_ram, libre_vram = self._libre()
        propio_ram = propio_vram = 0.0
        for t in tipos:
            r = self.residentes.get(t)
            if r is not None and self._cargado(r):
                a, b = r.estimar()
                propio_ram, propio_vram = propio_ram + a, propio_vram + b
        cap = self.capacidad()
        ram = min(cap["ram_mb"], max(0.0, libre_ram) + propio_ram)
        vram = min(cap["vram_mb"], max(0.0, libre_vram) + propio_vram) if cap["vram_mb"] else 0.0
        return ram, vram

    @contextmanager
    def etapa(self, nombre: str, tipos, extra: dict | None = None):
        """Paso del flujo en primer plano: hace sitio si hace falta y da su presupuesto de memoria.

        extra: {tipo: (ram_mb, vram_mb)} cuando el modelo de la etapa no es el registrado (otro LLM, otra resolucion).
        """
        tipos = tuple(tipos)
        extra = extra or {}
        t0 = time.time()
        with self._lock:
            alt = self.alternando()
            for t in tipos:
                self.uso[t] += 1
            self.primer_plano += 1
            self.actual.append(nombre)
            liberados = self._hacer_sitio(tipos, extra) if alt else []
            ram, vram = self._presupuesto(tipos) if alt else (float(self.capacidad()["ram_mb"]),
                                                              float(self.capacidad()["vram_mb"]))
            for t in tipos:
                r = self.residentes.get(t)
                if r is not None:
                    if r.liberado and not self._cargado(r):   # se libero antes y ahora vuelve a cargarse
                        r.recargas += 1
                        self.recargas += 1
                        if self.monitor is not None and hasattr(self.monitor, "record_turno"):
                            self.monitor.record_turno(nombre, t)
                    r.liberado = False
                    r.ultimo = time.time()
        turno = Turno(nombre, tipos, alt, ram, vram, liberados)
        try:
            yield turno
        finally:
            with self._lock:
                for t in tipos:
                    self.uso[t] -= 1
                    r = self.residentes.get(t)
                    if r is not None:
                        r.ultimo = time.time()
                self.primer_plano -= 1
                if nombre in self.actual:
                    self.actual.remove(nombre)
                self.historial.append({"etapa": nombre, "tipos": list(tipos), "t": round(t0, 1),
                                       "segundos": round(time.time() - t0, 2), "liberados": liberados,
                                       "alternando": alt, "presupuesto_mb": round(ram)})

    @contextmanager
    def fondo(self, nombre: str, tipos):
        """Tarea de fondo: no echa a nadie. Devuelve False si ahora no le toca (que espere y lo reintente)."""
        tipos = tuple(tipos)
        with self._lock:
            ok = True
            if self.alternando():
                if self.primer_plano > 0:
                    ok = False
                else:
                    falta_ram, falta_vram = self._falta(tipos, {})
                    libre_ram, libre_vram = self._libre()
                    ok = falta_ram <= libre_ram and (not self.gpu_dedicada() or falta_vram <= libre_vram)
            if ok:
                for t in tipos:
                    self.uso[t] += 1
        try:
            yield ok
        finally:
            if ok:
                with self._lock:
                    for t in tipos:
                        self.uso[t] -= 1
                        r = self.residentes.get(t)
                        if r is not None:
                            r.ultimo = time.time()

    # ------------------------------------------------------------ informes
    def estado(self) -> dict:
        alt = self.alternando()
        cap, nec = self.capacidad(), self.necesidad()
        total, disp = self.medir_ram()
        return {
            "modo": self.modo(), "alternando": alt, "capacidad": cap, "necesidad": nec,
            "ram_total_mb": round(total), "ram_disponible_mb": round(disp), "gpu_dedicada": self.gpu_dedicada(),
            "etapas_en_curso": list(self.actual), "recargas": self.recargas,
            "modelos": [{"tipo": r.tipo, "nombre": r.nombre, "cargado": self._cargado(r),
                         "ram_mb": nec["por_tipo"].get(t, {}).get("ram_mb", 0),
                         "vram_mb": nec["por_tipo"].get(t, {}).get("vram_mb", 0),
                         "recargas": r.recargas, "en_uso": bool(self.uso[t]),
                         "ultimo": round(r.ultimo, 1)} for t, r in self.residentes.items()],
            "historial": list(self.historial)[-15:][::-1],
        }

    def motivo_arranque(self) -> str:
        """Linea para el Autoajuste de este arranque (vacia si todo cabe)."""
        if self.modo() == "nunca":
            return ""
        cap, nec = self.capacidad(), self.necesidad()
        if self.modo() == "siempre":
            return "Turnos de modelos: modo «siempre», cada etapa descarga los modelos que no usa"
        if not self.alternando():
            return ""
        return (f"Memoria justa para todos los modelos a la vez ({nec['ram_mb'] / 1024:.1f} GB de "
                f"{cap['ram_mb'] / 1024:.1f} GB utiles): se cargan por turnos segun la etapa del flujo")
