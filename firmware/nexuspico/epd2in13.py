# Pantalla de tinta Waveshare 2,13" V3 / V4 (controlador SSD1680, 122x250) para el Pico W.
# Es la pantalla del pwnagotchi: el modulo "Pico-ePaper-2.13" se pincha directamente en el Pico W.
# Buffer: 122x250 en vertical, MONO_HLSB, 16 bytes por linea (4000 bytes), 1 = blanco.
from machine import Pin, SPI
import time

ANCHO, ALTO = 122, 250
BYTES = ((ANCHO + 7) // 8) * ALTO   # 4000

# pines del Pico-ePaper-2.13 (se pueden cambiar en config.json -> "pines")
PINES = {"rst": 12, "dc": 8, "cs": 9, "clk": 10, "din": 11, "busy": 13}


class EPD:
    def __init__(self, pines=None):
        p = dict(PINES)
        p.update(pines or {})
        self.rst = Pin(p["rst"], Pin.OUT)
        self.dc = Pin(p["dc"], Pin.OUT)
        self.cs = Pin(p["cs"], Pin.OUT, value=1)
        self.busy = Pin(p["busy"], Pin.IN, Pin.PULL_UP)
        self.spi = SPI(1, baudrate=4_000_000, polarity=0, phase=0, sck=Pin(p["clk"]), mosi=Pin(p["din"]))
        self.parciales = 0
        self.init()

    # ------------------------------------------------------------ bajo nivel
    def _cmd(self, c, datos=None):
        self.dc(0)
        self.cs(0)
        self.spi.write(bytes([c]))
        self.cs(1)
        if datos is not None:
            self._data(datos)

    def _data(self, datos):
        self.dc(1)
        self.cs(0)
        self.spi.write(datos if isinstance(datos, (bytes, bytearray, memoryview)) else bytes(datos))
        self.cs(1)

    def _espera(self, maximo_ms=6000):
        t0 = time.ticks_ms()
        while self.busy.value() == 1:
            if time.ticks_diff(time.ticks_ms(), t0) > maximo_ms:
                break
            time.sleep_ms(10)

    def _reset(self):
        self.rst(1)
        time.sleep_ms(20)
        self.rst(0)
        time.sleep_ms(2)
        self.rst(1)
        time.sleep_ms(20)

    def _ventana(self):
        self._cmd(0x44, [0, (ANCHO - 1) >> 3])
        self._cmd(0x45, [0, 0, (ALTO - 1) & 0xFF, (ALTO - 1) >> 8])
        self._cmd(0x4E, [0])
        self._cmd(0x4F, [0, 0])

    # ------------------------------------------------------------ alto nivel
    def init(self):
        self._reset()
        self._espera()
        self._cmd(0x12)                      # reinicio por software
        self._espera()
        self._cmd(0x01, [0xF9, 0x00, 0x00])  # 250 lineas
        self._cmd(0x11, [0x03])              # x+, y+
        self._ventana()
        self._cmd(0x3C, [0x05])              # borde
        self._cmd(0x21, [0x00, 0x80])
        self._cmd(0x18, [0x80])              # sensor de temperatura interno
        self._espera()
        self.parciales = 0

    def completa(self, buf):
        """Refresco completo (parpadea, limpia los restos). Tambien deja la imagen como base del parcial."""
        self.init()
        self._cmd(0x24, buf)
        self._cmd(0x26, buf)
        self._cmd(0x22, [0xF7])
        self._cmd(0x20)
        self._espera()

    def parcial(self, buf):
        """Refresco rapido sin parpadeo (~0,3 s). Cada cierto numero conviene uno completo."""
        self.rst(0)
        time.sleep_ms(1)
        self.rst(1)
        self._cmd(0x3C, [0x80])
        self._cmd(0x01, [0xF9, 0x00, 0x00])
        self._cmd(0x11, [0x03])
        self._ventana()
        self._cmd(0x24, buf)
        self._cmd(0x22, [0xFF])
        self._cmd(0x20)
        self._espera()
        self.parciales += 1

    def mostrar(self, buf, completo_cada=20):
        if self.parciales == 0 or self.parciales >= completo_cada:
            self.completa(buf)
            self.parciales = 1
        else:
            self.parcial(buf)

    def dormir(self):
        self._cmd(0x10, [0x01])
        time.sleep_ms(100)
