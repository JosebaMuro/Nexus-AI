# NexusPico 1.0.0 - avatar de bolsillo de Nexus-Office en un Pico W con la pantalla de tinta del pwnagotchi
# (Waveshare 2,13" V3/V4).
#
# Nexus genera una imagen por estado (en linea, escuchando, pensando, usando herramienta, hablando, hecho, error,
# aviso) a partir del avatar. El Pico las descarga una vez, las guarda en su memoria y despues solo pregunta el
# estado cada 2 s: cuando cambia, pinta la imagen guardada. Si Nexus cambia de nombre o de estilo, la version cambia
# y el Pico vuelve a descargarlas.
#
# Configuracion (/config.json): la graba el script de Nexus (firmware/flash.py) o la pagina del propio Pico: si no
# hay Wi-Fi, crea la red "NexusPico-XXXX" (clave "nexuspico") con una pagina en http://192.168.4.1.
#   {"wifi": {"ssid": "...", "pass": "..."}, "nexus": {"host": "192.168.1.10:8775", "key": "..."}}
import json
import os
import time

VERSION = "1.0.0"
CONFIG = "/config.json"
IMG = "/img"
CADA_S = 2


def leer_config(ruta=CONFIG):
    try:
        with open(ruta) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def guardar_config(cfg, ruta=CONFIG):
    with open(ruta, "w") as f:
        json.dump(cfg, f)


def _existe(ruta):
    try:
        os.stat(ruta)
        return True
    except OSError:
        return False


def girar(buf_apaisado, ancho=250, alto=122):
    """Gira 90 grados (como hace Nexus) una imagen apaisada MONO_HLSB de ancho x alto al formato de la pantalla
    (alto x ancho en vertical). Sirve para los textos que pinta el propio Pico."""
    wb_in = (ancho + 7) // 8
    wb = (alto + 7) // 8
    out = bytearray(b"\xff" * (wb * ancho))
    for yv in range(ancho):          # fila en la pantalla vertical
        xo = ancho - 1 - yv          # columna de la imagen apaisada
        bo, mo = xo >> 3, 0x80 >> (xo & 7)
        base = yv * wb
        for xv in range(alto):
            if not buf_apaisado[xv * wb_in + bo] & mo:
                out[base + (xv >> 3)] &= ~(0x80 >> (xv & 7)) & 0xFF
    return out


class Nucleo:
    """Logica del Pico sin hardware (se prueba en el PC): estado, cache de imagenes y que pintar."""

    def __init__(self, http_get, pantalla, cliente="pico", carpeta=IMG):
        self.get = http_get            # get(ruta) -> (codigo, bytes)
        self.pantalla = pantalla       # pantalla(buf, motivo)
        self.cliente = cliente
        self.carpeta = carpeta
        self.estado = None
        self.version = None
        self.fallos = 0
        try:
            with open(carpeta + "/version.txt") as f:
                self.version = f.read().strip()
        except OSError:
            pass

    def _ruta(self, estado):
        return "%s/%s.bin" % (self.carpeta, estado)

    def sincronizar_imagenes(self):
        cod, cuerpo = self.get("/api/dispositivos/pico/manifiesto")
        if cod != 200:
            raise OSError("manifiesto: HTTP %s" % cod)
        man = json.loads(cuerpo)
        if not _existe(self.carpeta):
            os.mkdir(self.carpeta)
        for est in man["estados"]:
            cod, datos = self.get("/api/dispositivos/pico/%s.bin" % est)
            if cod != 200 or len(datos) != 4000:
                raise OSError("imagen %s: HTTP %s, %d bytes" % (est, cod, len(datos or b"")))
            with open(self._ruta(est), "wb") as f:
                f.write(datos)
        with open(self.carpeta + "/version.txt", "w") as f:
            f.write(man["version"])
        self.version = man["version"]
        self.estado = None             # repintar con las imagenes nuevas

    def imagen(self, estado):
        for e in (estado, "idle"):
            try:
                with open(self._ruta(e), "rb") as f:
                    return f.read()
            except OSError:
                pass
        return None

    def paso(self):
        """Una consulta: devuelve el estado mostrado (o None si no hay conexion)."""
        try:
            cod, cuerpo = self.get("/api/dispositivos/estado?cliente=%s&tipo=pico" % self.cliente)
            if cod != 200:
                raise OSError("HTTP %s" % cod)
            snap = json.loads(cuerpo)
        except Exception as e:   # noqa: BLE001 - sin red o Nexus apagado
            self.fallos += 1
            if self.fallos == 3:
                self.pantalla(None, "sin conexion: %s" % e)
            return None
        self.fallos = 0
        if snap.get("pico_version") and snap["pico_version"] != self.version:
            self.sincronizar_imagenes()
        est = snap.get("estado") or "idle"
        if est != self.estado:
            buf = self.imagen(est)
            if buf:
                self.pantalla(buf, est)
                self.estado = est
        return self.estado


# ---------------------------------------------------------------------- solo en el Pico W
def _texto(lineas):
    import framebuf
    w, h = 250, 122
    b = bytearray(b"\xff" * (((w + 7) // 8) * h))
    fb = framebuf.FrameBuffer(b, w, h, framebuf.MONO_HLSB)
    fb.rect(0, 0, w, h, 0)
    for i, ln in enumerate(lineas):
        fb.text(ln[:30], 6, 8 + i * 14, 0)
    return girar(b, w, h)


def _portal(cfg, nombre):
    """Punto de acceso con una pagina para poner la Wi-Fi y la direccion de Nexus."""
    import network
    import socket
    ap = network.WLAN(network.AP_IF)
    ap.config(essid=nombre, password="nexuspico")
    ap.active(True)
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 80))
    s.listen(1)
    pagina = ("<html><head><meta name=viewport content='width=device-width'><title>NexusPico</title></head>"
              "<body style='font-family:sans-serif;max-width:420px;margin:auto'><h2>NexusPico</h2><form>"
              "Wi-Fi<br><input name=ssid><br>Clave<br><input name=pass type=password><br>"
              "Nexus (ip:puerto)<br><input name=host placeholder='192.168.1.10:8775'><br>"
              "Clave de Nexus (Red COOP &gt; Conectar dispositivos)<br><input name=key><br><br>"
              "<button>Guardar</button></form></body></html>")
    while True:
        c, _ = s.accept()
        try:
            req = c.recv(1024).decode()
            linea = req.split("\r\n", 1)[0]
            if "?" in linea and "ssid=" in linea:
                q = linea.split(" ")[1].split("?", 1)[1]
                v = {}
                for par in q.split("&"):
                    if "=" in par:
                        k, x = par.split("=", 1)
                        v[k] = _desescapar(x)
                cfg["wifi"] = {"ssid": v.get("ssid", ""), "pass": v.get("pass", "")}
                cfg["nexus"] = {"host": v.get("host", ""), "key": v.get("key", "")}
                guardar_config(cfg)
                c.send("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<h2>Guardado. Reiniciando...</h2>")
                c.close()
                time.sleep(1)
                import machine
                machine.reset()
            c.send("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n" + pagina)
        except OSError:
            pass
        c.close()


def _desescapar(s):
    s = s.replace("+", " ")
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "%" and i + 2 < len(s) + 1:
            try:
                out.append(int(s[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out += s[i].encode()
        i += 1
    return out.decode()


def run():
    import machine
    import network
    import ubinascii
    from epd2in13 import EPD

    cfg = leer_config()
    epd = EPD(cfg.get("pines"))
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    mac = ubinascii.hexlify(wlan.config("mac")).decode()
    cliente = "pico-" + mac[-6:]
    led = machine.Pin("LED", machine.Pin.OUT)

    wifi = cfg.get("wifi") or {}
    host = (cfg.get("nexus") or {}).get("host", "")
    if not wifi.get("ssid") or not host:
        nombre = "NexusPico-" + mac[-4:].upper()
        epd.mostrar(_texto(["NexusPico " + VERSION, "", "Configurame:", "Wi-Fi " + nombre, "clave: nexuspico",
                            "http://192.168.4.1"]))
        _portal(cfg, nombre)

    epd.mostrar(_texto(["NexusPico " + VERSION, "", "Conectando a", wifi["ssid"]]))
    wlan.connect(wifi["ssid"], wifi.get("pass", ""))
    for _ in range(40):
        if wlan.isconnected():
            break
        led.toggle()
        time.sleep_ms(500)
    if not wlan.isconnected():
        epd.mostrar(_texto(["NexusPico " + VERSION, "", "No conecta a la Wi-Fi", wifi["ssid"], "",
                            "Configurame en la red", "NexusPico-" + mac[-4:].upper()]))
        _portal(cfg, "NexusPico-" + mac[-4:].upper())

    try:
        import requests
    except ImportError:
        import urequests as requests
    key = (cfg.get("nexus") or {}).get("key", "")
    base = "http://" + host if not host.startswith("http") else host

    def get(ruta):
        r = requests.get(base + ruta, headers={"X-Nexus-Key": key}, timeout=8)
        try:
            return r.status_code, r.content
        finally:
            r.close()

    def pantalla(buf, motivo):
        if buf is None:
            epd.mostrar(_texto(["NexusPico " + VERSION, "", "Sin conexion con Nexus", host, "", motivo[-30:]]))
        else:
            epd.mostrar(buf, completo_cada=cfg.get("completo_cada", 20))

    nucleo = Nucleo(get, pantalla, cliente)
    cada = cfg.get("cada_s", CADA_S)
    while True:
        led(1)
        try:
            nucleo.paso()
        except Exception as e:   # noqa: BLE001
            print("NexusPico:", e)
        led(0)
        if not wlan.isconnected():
            wlan.connect(wifi["ssid"], wifi.get("pass", ""))
        time.sleep(cada)
