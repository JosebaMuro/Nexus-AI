"""Centro de dispositivos: reparte el estado de Nexus a las luces, al reloj y al avatar de bolsillo, y aplica los
modos preconfigurados (que se pueden cambiar desde el escritorio, el movil o el reloj indistintamente).

* El navegador (escritorio o app Android) informa del estado (escuchando, pensando, hablando...) y del nivel de voz
  con ``POST /api/dispositivos/estado``; el hub lo manda en el acto a las tiras NexusLED por UDP.
* El reloj (ESP32-2424S012C) y el Pico W consultan ``GET /api/dispositivos/estado`` (ligero) cada segundo o dos.
* El Pico W descarga una vez las imagenes de cada estado (``/api/dispositivos/pico/...``), que Nexus genera a partir
  del avatar, y despues solo cambia de imagen cuando cambia el estado.
"""
from __future__ import annotations

import hashlib
import io
import threading
import time
from pathlib import Path

from .. import log
from . import leds as leds_mod

L = log.get("dispositivos")
WEB = Path(__file__).resolve().parent.parent / "web"

COLORES = {"none": "#000000", "idle": "#00f0ff", "listening": "#7cff6b", "thinking": "#a66bff", "tool": "#ffb020",
           "speaking": "#ff3df2", "success": "#28ff5a", "error": "#ff1414", "alert": "#ffb020"}
# caras de pwnagotchi para la pantalla de tinta (ademas del avatar)
CARAS = {"idle": "(◕‿‿◕)", "listening": "(⊙‿‿⊙)", "thinking": "(◔_◔ )", "tool": "(•̀ᴗ•́ )و", "speaking": "(ᵔ◡◡ᵔ)",
         "success": "(★‿‿★)", "error": "(╥☁╥ )", "alert": "(⚆_⚆)", "none": "(-__-)"}

# modos preconfigurados: cada uno cambia unas claves de configuracion (y, si se indica, el perfil de hardware)
MODOS_DEFECTO: list[dict] = [
    {"id": "normal", "nombre": "Normal", "icono": "◆", "descripcion": "Voz y luces como siempre",
     "config": {"user.voice_enabled": True, "user.auto_speak": False, "lounge.auto_meet": True}, "leds": {"bri": 160}},
    {"id": "voz", "nombre": "Manos libres", "icono": "🎙", "descripcion": "Responde siempre en voz alta",
     "config": {"user.voice_enabled": True, "user.auto_speak": True}, "leds": {"bri": 200}},
    {"id": "silencio", "nombre": "Silencio", "icono": "🔇", "descripcion": "Sin voz y luces tenues",
     "config": {"user.voice_enabled": False, "user.auto_speak": False}, "leds": {"bri": 25}},
    {"id": "concentracion", "nombre": "Concentración", "icono": "🎯",
     "descripcion": "Sin reuniones automáticas ni voz; respuestas breves",
     "config": {"user.auto_speak": False, "lounge.auto_meet": False, "user.response_style": "breve"}, "leds": {"bri": 60}},
    {"id": "rendimiento", "nombre": "Rendimiento", "icono": "⚡", "descripcion": "Perfil de hardware más potente",
     "config": {}, "perfil_hw": "rendimiento", "leds": {"bri": 200}},
    {"id": "ahorro", "nombre": "Ahorro", "icono": "🔋", "descripcion": "Perfil ligero: menos RAM y batería",
     "config": {}, "perfil_hw": "ahorro", "leds": {"bri": 40}},
    {"id": "noche", "nombre": "Noche", "icono": "🌙", "descripcion": "Luces apagadas y sin voz",
     "config": {"user.voice_enabled": False, "user.auto_speak": False}, "leds": {"on": False}},
]


class Hub:
    def __init__(self, services=None) -> None:
        self.s = services
        self.leds = leds_mod.Leds()
        self._lock = threading.Lock()
        self.state, self.level, self.since, self.seq = "idle", 0.0, time.time(), 0
        self.texto = ""
        self.clientes: dict[str, dict] = {}   # relojes y Pico W que han preguntado (para la vista de dispositivos)
        self._pico_cache: dict[str, tuple[str, dict]] = {}

    # ------------------------------------------------------------ estado
    def set_state(self, state: str, level: float | None = None, texto: str | None = None, origen: str = "") -> dict:
        state = state if state in COLORES else "idle"
        with self._lock:
            changed = state != self.state
            self.state = state
            if changed:
                self.since, self.seq = time.time(), self.seq + 1
            if level is not None:
                self.level = max(0.0, min(1.0, float(level)))
            if texto is not None:
                self.texto = texto[:120]
        self.leds.send_state(state, level)
        return self.snapshot()

    def snapshot(self) -> dict:
        return {"estado": self.state, "nombre": leds_mod.NOMBRES.get(self.state, self.state),
                "color": COLORES[self.state], "nivel": round(self.level, 2), "desde": self.since, "seq": self.seq,
                "texto": self.texto, "modo": self.modo_actual(), "pico_version": self.pico_version()}

    def visto(self, cid: str, tipo: str, extra: dict | None = None) -> None:
        if cid:
            self.clientes[cid] = {"id": cid, "tipo": tipo, "visto": time.time(), **(extra or {})}

    # ------------------------------------------------------------ modos
    def _cfg(self):
        from ..config import get_config
        return get_config()

    def modos(self) -> list[dict]:
        extra = self._cfg().get("dispositivos.modos", []) or []
        ids = {m["id"] for m in extra if isinstance(m, dict) and m.get("id")}
        return [m for m in MODOS_DEFECTO if m["id"] not in ids] + [m for m in extra if isinstance(m, dict)]

    def modo_actual(self) -> str:
        return str(self._cfg().get("dispositivos.modo", "normal") or "normal")

    def aplicar_modo(self, mid: str, origen: str = "") -> dict:
        modo = next((m for m in self.modos() if m["id"] == mid), None)
        if not modo:
            raise ValueError(f"modo desconocido: {mid}")
        cfg = self._cfg()
        for k, v in (modo.get("config") or {}).items():
            if k.startswith(("admin.", "nube.", "server.", "remote.", "vpn.")):
                continue   # un modo nunca toca seguridad ni red
            cfg.set(k, v, save=False)
        if modo.get("perfil_hw"):
            try:
                from ..hardware import hw_profiles
                hw_profiles.activar(modo["perfil_hw"], cfg)
            except Exception as e:   # noqa: BLE001
                L.warning("Modo %s: no se pudo aplicar el perfil de hardware %s: %s", mid, modo["perfil_hw"], e)
        cfg.set("dispositivos.modo", mid)
        for d in self.leds.devices:
            if d.get("enabled", True) and modo.get("leds"):
                try:
                    self.leds.call(d["id"], "POST", "/api/state", modo["leds"])
                except Exception as e:   # noqa: BLE001
                    L.debug("modo en %s: %s", d.get("host"), e)
        L.info("Modo %s activado%s", mid, f" desde {origen}" if origen else "")
        with self._lock:
            self.seq += 1
        return {"modo": mid, "aplicado": modo}

    # ------------------------------------------------------------ reloj
    def resumen_reloj(self) -> dict:
        """Lo que muestra el reloj: estado, agentes con lo que hacen y los modos."""
        from ..agents.activity import ACTIVITY
        act = ACTIVITY.all()
        agentes = []
        if self.s is not None:
            for a in self.s.agents.list():
                st = act.get(a.id, {})
                agentes.append({"id": a.id, "nombre": a.name[:22], "color": a.color, "icono": a.icon,
                                "estado": st.get("estado", "idle"), "escena": st.get("escena", "idle"),
                                "tarea": (st.get("tarea") or "")[:60], "segundos": st.get("segundos", 0),
                                "tareas": st.get("tareas", 0)})
        orden = {"tool": 0, "thinking": 1, "writing": 1, "delegating": 2, "meeting": 3, "idle": 9}
        agentes.sort(key=lambda a: (orden.get(a["estado"], 5), a["nombre"]))
        return {**self.snapshot(), "agentes": agentes[:24],
                "modos": [{"id": m["id"], "nombre": m["nombre"], "icono": m.get("icono", "")} for m in self.modos()],
                "hora": time.strftime("%H:%M")}

    # ------------------------------------------------------------ Pico W (tinta electronica)
    def pico_version(self) -> str:
        name = self._cfg().get("user.assistant_name", "Nexus-Office") or "Nexus-Office"
        style = self._cfg().get("dispositivos.pico.estilo", "avatar")
        return hashlib.sha1(f"{name}|{style}|3".encode()).hexdigest()[:10]

    def pico_imagen(self, state: str, ancho: int = 250, alto: int = 122) -> "Image.Image":   # noqa: F821
        """Imagen en blanco y negro del estado: el avatar tramado a la izquierda y el estado a la derecha."""
        from PIL import Image, ImageDraw, ImageFont, ImageOps
        state = state if state in COLORES else "idle"
        img = Image.new("L", (ancho, alto), 255)
        d = ImageDraw.Draw(img)
        face = WEB / "assets" / "geisha" / "face_front.png"
        x0 = 0
        if face.exists() and self._cfg().get("dispositivos.pico.estilo", "avatar") == "avatar":
            f = Image.open(face).convert("RGBA")
            bg = Image.new("RGBA", f.size, (255, 255, 255, 255))
            f = Image.alpha_composite(bg, f).convert("L")
            f = ImageOps.autocontrast(f, cutoff=2)
            f.thumbnail((alto, alto))
            if state == "thinking":
                f = f.transform(f.size, Image.AFFINE, (1, 0.12, -8, 0, 1, 0), fillcolor=255)
            elif state == "error":
                f = ImageOps.invert(f)
            img.paste(f, (0, (alto - f.height) // 2))
            x0 = f.width + 4
        fonts = [Path(p) for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                   "C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/arialbd.ttf")]
        fpath = next((str(p) for p in fonts if p.exists()), None)

        def font(sz):
            try:
                return ImageFont.truetype(fpath, sz) if fpath else ImageFont.load_default(sz)
            except Exception:   # noqa: BLE001
                return ImageFont.load_default()
        name = (self._cfg().get("user.assistant_name", "Nexus-Office") or "Nexus-Office")[:16]
        d.text((x0, 4), name, font=font(13), fill=0)
        d.line((x0, 21, ancho - 4, 21), fill=0)
        d.text((x0, 28), CARAS.get(state, ""), font=font(20), fill=0)
        label = leds_mod.NOMBRES.get(state, state).upper()
        sz = 18
        while sz > 9 and d.textlength(label, font=font(sz)) > ancho - x0 - 4:
            sz -= 1
        d.text((x0, 62), label, font=font(sz), fill=0)
        # icono del estado: ondas al escuchar, puntos al pensar, barras al hablar
        cy = 98
        if state == "listening":
            for r in (6, 12, 18):
                d.arc((x0 + 20 - r, cy - r, x0 + 20 + r, cy + r), 300, 60, fill=0, width=2)
        elif state == "thinking":
            for i in range(3):
                d.ellipse((x0 + 4 + i * 14, cy - 4, x0 + 12 + i * 14, cy + 4), fill=0)
        elif state == "speaking":
            for i, h in enumerate((6, 14, 20, 12, 8, 16)):
                d.rectangle((x0 + 4 + i * 8, cy - h // 2, x0 + 9 + i * 8, cy + h // 2), fill=0)
        elif state == "tool":
            d.rectangle((x0 + 4, cy - 8, x0 + 30, cy + 8), outline=0, width=2)
            d.line((x0 + 30, cy, x0 + 44, cy), fill=0, width=3)
        return img.convert("1", dither=Image.FLOYDSTEINBERG)

    def pico_buffer(self, state: str) -> bytes:
        """Buffer para la pantalla Waveshare 2.13" (V3/V4) del Pico: 122x250 en vertical, MONO_HLSB, 1 = blanco.
        La imagen apaisada (250x122) se gira 90 grados como hace el controlador de Waveshare."""
        img = self.pico_imagen(state).rotate(90, expand=True)   # 122 x 250
        w, h = img.size
        wb = (w + 7) // 8
        px = img.load()
        out = bytearray(wb * h)
        for y in range(h):
            for x in range(w):
                if px[x, y]:
                    out[y * wb + x // 8] |= 0x80 >> (x % 8)
        return bytes(out)

    def pico_png(self, state: str) -> bytes:
        bio = io.BytesIO()
        self.pico_imagen(state).save(bio, "PNG")
        return bio.getvalue()

    def pico_manifiesto(self) -> dict:
        return {"version": self.pico_version(), "ancho": 250, "alto": 122, "formato": "MONO_HLSB 122x250 vertical",
                "estados": [s for s in leds_mod.ESTADOS if s != "none"]}


_HUB: Hub | None = None
_HLOCK = threading.Lock()


def get(services=None) -> Hub:
    global _HUB
    with _HLOCK:
        if _HUB is None:
            _HUB = Hub(services)
        elif services is not None:
            _HUB.s = services
        return _HUB


def reset() -> None:
    global _HUB
    with _HLOCK:
        _HUB = None
