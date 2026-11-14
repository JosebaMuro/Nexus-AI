"""Ubicacion en tiempo real (herramienta ubicacion_actual y /api/ubicacion). Todo dentro del equipo: la posicion no
se envia a ningun servicio externo ni se comparte por la red COOP. Fuentes, de la mas directa a la de reserva:

  - app:       la app Android escribe la ultima posicion del GPS del movil en data/ubicacion.json
  - gpsd:      receptor GPS servido por gpsd (Linux, Raspberry Pi, macOS) en 127.0.0.1:2947
  - nmea:      receptor GPS por puerto serie o USB que habla NMEA 0183 (ubicacion.serie: /dev/ttyACM0, COM3...)
  - windows:   servicio de ubicacion de Windows (System.Device.Location, por PowerShell)
  - geoclue:   servicio de ubicacion de Linux (GeoClue2, con la utilidad where-am-i si esta instalada)
  - navegador: la interfaz web manda la posicion del navegador si se activa (ubicacion.navegador)
  - manual:    la posicion fija que pone el usuario en Ajustes (ubicacion.manual)

Ademas da la ciudad de referencia mas cercana sin internet (tabla propia) y distancias en linea recta.
"""
from __future__ import annotations

import json
import math
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import unicodedata
from functools import lru_cache
from pathlib import Path

from .. import log, paths
from ..config import get_config

L = log.get("ubicacion")
FUENTES = ("app", "gpsd", "nmea", "windows", "geoclue", "navegador", "manual")
_mu = threading.Lock()
_recibidas: dict[str, dict] = {}
_ultima: dict | None = None


# ------------------------------------------------------------------ piezas puras
def coordenadas_validas(lat, lon) -> bool:
    try:
        lat, lon = float(lat), float(lon)
    except (TypeError, ValueError):
        return False
    return math.isfinite(lat) and math.isfinite(lon) and -90 <= lat <= 90 and -180 <= lon <= 180 and \
        not (lat == 0 and lon == 0)


def parse_coordenadas(s: str) -> tuple[float, float] | None:
    m = re.fullmatch(r"\s*(-?\d+(?:[.,]\d+)?)\s*[,; ]\s*(-?\d+(?:[.,]\d+)?)\s*", s or "")
    if not m:
        return None
    lat, lon = float(m[1].replace(",", ".")), float(m[2].replace(",", "."))
    return (lat, lon) if coordenadas_validas(lat, lon) else None


def distancia_km(lat1, lon1, lat2, lon2) -> float:
    r = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def rumbo_grados(lat1, lon1, lat2, lon2) -> float:
    p1, p2, dl = math.radians(lat1), math.radians(lat2), math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def rumbo_texto(grados: float) -> str:
    return ("norte", "noreste", "este", "sureste", "sur", "suroeste", "oeste", "noroeste")[int((grados + 22.5) // 45) % 8]


def _nmea_grados(valor: str, hemisferio: str) -> float | None:
    if not valor or "." not in valor:
        return None
    punto = valor.index(".")
    try:
        g = float(valor[:punto - 2] or 0) + float(valor[punto - 2:]) / 60
    except ValueError:
        return None
    return -g if hemisferio in ("S", "W") else g


def _nmea_ok(linea: str) -> bool:
    if not linea.startswith("$") or "*" not in linea:
        return False
    cuerpo, suma = linea[1:].rsplit("*", 1)
    x = 0
    for ch in cuerpo:
        x ^= ord(ch)
    try:
        return x == int(suma[:2], 16)
    except ValueError:
        return False


def parse_nmea(texto: str) -> dict | None:
    """$GxGGA / $GxRMC con suma de control: devuelve la primera fijacion valida (las dos frases se combinan)."""
    fix: dict = {}
    for linea in (texto or "").replace("\r", "\n").split("\n"):
        linea = linea.strip()
        if not _nmea_ok(linea):
            continue
        c = linea[1:].rsplit("*", 1)[0].split(",")
        tipo = c[0][2:]
        if tipo == "GGA" and len(c) > 9 and c[6] not in ("", "0"):
            lat, lon = _nmea_grados(c[2], c[3]), _nmea_grados(c[4], c[5])
            if lat is None or lon is None:
                continue
            fix.update(lat=lat, lon=lon)
            if c[7]:
                fix["satelites"] = int(float(c[7]))
            if c[8]:
                fix["precision"] = round(float(c[8]) * 5.0, 1)   # HDOP x 5 m: estimacion habitual
            if c[9]:
                fix["altitud"] = float(c[9])
        elif tipo == "RMC" and len(c) > 8 and c[2] == "A":
            lat, lon = _nmea_grados(c[3], c[4]), _nmea_grados(c[5], c[6])
            if lat is None or lon is None:
                continue
            fix.setdefault("lat", lat)
            fix.setdefault("lon", lon)
            if c[7]:
                fix["velocidad"] = round(float(c[7]) * 0.514444, 2)   # nudos -> m/s
            if c[8]:
                fix["rumbo"] = float(c[8])
    if "lat" in fix and coordenadas_validas(fix["lat"], fix["lon"]):
        return {**fix, "fuente": "nmea", "t": time.time()}
    return None


def parse_gpsd(lineas: str) -> dict | None:
    """Primera TPV con fijacion (mode >= 2) de la salida JSON de gpsd."""
    for linea in (lineas or "").split("\n"):
        try:
            j = json.loads(linea)
        except ValueError:
            continue
        if j.get("class") != "TPV" or int(j.get("mode", 0)) < 2 or not coordenadas_validas(j.get("lat"), j.get("lon")):
            continue
        fix = {"lat": float(j["lat"]), "lon": float(j["lon"]), "fuente": "gpsd", "t": time.time()}
        if "eph" in j:
            fix["precision"] = float(j["eph"])
        elif "epx" in j and "epy" in j:
            fix["precision"] = max(float(j["epx"]), float(j["epy"]))
        for src, dst in (("altMSL", "altitud"), ("alt", "altitud"), ("speed", "velocidad"), ("track", "rumbo")):
            if src in j and dst not in fix:
                fix[dst] = float(j[src])
        return fix
    return None


def parse_fix_json(j: dict, fuente: str) -> dict | None:
    if not isinstance(j, dict) or not coordenadas_validas(j.get("lat"), j.get("lon")):
        return None
    fix = {"lat": float(j["lat"]), "lon": float(j["lon"]), "fuente": fuente, "t": float(j.get("t") or time.time())}
    if fix["t"] > 1e11:   # milisegundos (navegador, Android)
        fix["t"] /= 1000
    for k in ("precision", "altitud", "velocidad", "rumbo"):
        try:
            if j.get(k) is not None and math.isfinite(float(j[k])):
                fix[k] = float(j[k])
        except (TypeError, ValueError):
            pass
    return fix


@lru_cache(maxsize=1)
def _ciudades() -> list:
    try:
        return json.loads(Path(__file__).with_name("ciudades.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []


def _plano(s: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFD", (s or "").lower()) if unicodedata.category(c) != "Mn").strip()


def ciudad_cercana(lat: float, lon: float) -> dict | None:
    mejor, dmin = None, 1e9
    for c in _ciudades():
        d = distancia_km(lat, lon, c[3], c[4])
        if d < dmin:
            mejor, dmin = c, d
    if mejor is None:
        return None
    # el rumbo es desde la ciudad hacia la posicion: «a 3 km al noreste de Bilbao»
    return {"nombre": mejor[0], "region": mejor[1], "pais": mejor[2], "distancia_km": round(dmin, 1),
            "direccion": rumbo_texto(rumbo_grados(mejor[3], mejor[4], lat, lon))}


def buscar_lugar(nombre: str, lugares: list | None = None) -> dict | None:
    """Lugares guardados primero, luego la tabla de ciudades, o directamente "lat, lon"."""
    par = parse_coordenadas(nombre)
    if par:
        return {"nombre": nombre.strip(), "lat": par[0], "lon": par[1]}
    q = _plano(nombre)
    if not q:
        return None
    for lu in lugares or []:
        if isinstance(lu, dict) and _plano(lu.get("nombre", "")) == q and coordenadas_validas(lu.get("lat"), lu.get("lon")):
            return {"nombre": lu["nombre"], "lat": float(lu["lat"]), "lon": float(lu["lon"])}
    for c in _ciudades():
        if _plano(c[0]) == q:
            return {"nombre": c[0], "lat": c[3], "lon": c[4]}
    return None


def fix_json(f: dict, ahora: float | None = None) -> dict:
    ahora = ahora or time.time()
    o = {"lat": round(f["lat"], 6), "lon": round(f["lon"], 6), "fuente": f.get("fuente", ""),
         "edad_s": max(0, int(ahora - f.get("t", ahora))),
         "mapa": f"geo:{f['lat']:.6f},{f['lon']:.6f}"}
    for k in ("precision", "altitud", "velocidad", "rumbo", "satelites"):
        if f.get(k) is not None:
            o[k] = round(f[k], 1) if isinstance(f[k], float) else f[k]
    if "velocidad" in o:
        o["velocidad_kmh"] = round(o["velocidad"] * 3.6, 1)
    cerca = ciudad_cercana(f["lat"], f["lon"])
    if cerca:
        o["cerca_de"] = cerca
        o["descripcion"] = (f"en {cerca['nombre']}" if cerca["distancia_km"] < 3 else
                            f"a {cerca['distancia_km']} km al {cerca['direccion']} de {cerca['nombre']}") + \
            (f" ({cerca['pais']})" if cerca["pais"] else "")
    return o


# ------------------------------------------------------------------ fuentes
def _cfg(clave: str, defecto=None):
    return get_config().get(f"ubicacion.{clave}", defecto)


def fichero_app() -> Path:
    return paths.data_dir() / "ubicacion.json"


def _leer_app() -> dict | None:
    try:
        return parse_fix_json(json.loads(fichero_app().read_text(encoding="utf-8")), "app")
    except (OSError, ValueError):
        return None


def _leer_gpsd(timeout: float) -> dict | None:
    host, _, puerto = str(_cfg("gpsd", "127.0.0.1:2947")).partition(":")
    try:
        with socket.create_connection((host, int(puerto or 2947)), timeout=min(timeout, 2.0)) as s:
            s.sendall(b'?WATCH={"enable":true,"json":true}\n')
            s.settimeout(0.5)
            fin, buf = time.time() + timeout, b""
            while time.time() < fin:
                try:
                    trozo = s.recv(4096)
                except socket.timeout:
                    continue
                if not trozo:
                    break
                buf += trozo
                fix = parse_gpsd(buf.decode("utf-8", "replace"))
                if fix:
                    return fix
    except OSError:
        return None
    return None


def _leer_nmea(timeout: float) -> dict | None:
    puerto = str(_cfg("serie", "") or "")
    if not puerto:
        return None
    try:
        import serial   # pyserial (va con el grabador de firmware)
        with serial.Serial(puerto, int(_cfg("baudios", 9600)), timeout=0.5) as s:
            fin, buf = time.time() + timeout, ""
            while time.time() < fin:
                buf += s.read(512).decode("ascii", "replace")
                fix = parse_nmea(buf)
                if fix:
                    return fix
    except Exception as e:  # noqa: BLE001
        L.info("Receptor NMEA en %s: %s", puerto, e)
    return None


_PS = ("Add-Type -AssemblyName System.Device; $w = New-Object System.Device.Location.GeoCoordinateWatcher('High'); "
       "$w.Start(); $n = 0; while (($w.Status -ne 'Ready') -and ($n -lt %d)) { Start-Sleep -Milliseconds 200; $n++ }; "
       "$c = $w.Position.Location; if ($c.IsUnknown) { exit 3 }; "
       "'{0}|{1}|{2}|{3}' -f $c.Latitude.ToString([cultureinfo]::InvariantCulture), "
       "$c.Longitude.ToString([cultureinfo]::InvariantCulture), "
       "$c.HorizontalAccuracy.ToString([cultureinfo]::InvariantCulture), "
       "$c.Altitude.ToString([cultureinfo]::InvariantCulture)")


def _leer_windows(timeout: float) -> dict | None:
    if sys.platform != "win32":
        return None
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", _PS % int(timeout * 5)],
                           capture_output=True, text=True, timeout=timeout + 6,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        c = r.stdout.strip().split("|")
        if r.returncode == 0 and len(c) >= 2:
            return parse_fix_json({"lat": c[0], "lon": c[1], "precision": c[2] if len(c) > 2 else None,
                                   "altitud": c[3] if len(c) > 3 else None}, "windows")
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    return None


def _where_am_i() -> str | None:
    for ruta in (shutil.which("where-am-i"), "/usr/libexec/geoclue-2.0/demos/where-am-i",
                 "/usr/lib/geoclue-2.0/demos/where-am-i"):
        if ruta and os.path.exists(ruta):
            return ruta
    return None


def _leer_geoclue(timeout: float) -> dict | None:
    exe = _where_am_i() if sys.platform.startswith("linux") else None
    if not exe:
        return None
    try:
        r = subprocess.run([exe, "-t", str(max(2, int(timeout)))], capture_output=True, text=True, timeout=timeout + 4)
    except (OSError, subprocess.SubprocessError):
        return None
    lat = re.search(r"Latitude:\s*(-?[\d.,]+)", r.stdout)
    lon = re.search(r"Longitude:\s*(-?[\d.,]+)", r.stdout)
    if not lat or not lon:
        return None
    acc = re.search(r"Accuracy:\s*([\d.,]+)", r.stdout)
    return parse_fix_json({"lat": lat[1].replace(",", "."), "lon": lon[1].replace(",", "."),
                           "precision": acc[1].replace(",", ".") if acc else None}, "geoclue")


def _leer_manual() -> dict | None:
    par = parse_coordenadas(str(_cfg("manual", "") or ""))
    return {"lat": par[0], "lon": par[1], "fuente": "manual", "t": time.time()} if par else None


def recibir(body: dict, fuente: str) -> dict:
    """Posicion mandada por la interfaz web o por la app (POST /api/ubicacion)."""
    fuente = fuente if fuente in ("navegador", "app") else "navegador"
    fix = parse_fix_json(body, fuente)
    if not fix:
        return {"error": "Coordenadas no válidas"}
    with _mu:
        _recibidas[fuente] = fix
    return {"ok": True}


def _leer_recibida(fuente: str) -> dict | None:
    with _mu:
        return dict(_recibidas[fuente]) if fuente in _recibidas else None


def _pista() -> str:
    if sys.platform == "win32":
        return "Activa la ubicación de Windows (Configuración > Privacidad > Ubicación) o pon una posición fija."
    if sys.platform.startswith("linux"):
        return "Conecta un receptor GPS (gpsd o puerto serie), instala GeoClue o pon una posición fija en Ajustes."
    return "Conecta un receptor GPS con gpsd, activa la del navegador o pon una posición fija en Ajustes."


def consultar(opciones: dict | None = None) -> dict:
    """opciones: {fuente: "auto"|..., timeout_s, hasta: "lugar" o "lat,lon"}. Devuelve la posicion o {"error"}."""
    global _ultima
    op = opciones or {}
    if not _cfg("activa", True):
        return {"error": "La ubicación está desactivada en Ajustes > Ubicación."}
    pedida = str(op.get("fuente") or _cfg("fuente", "auto") or "auto")
    timeout = max(1.0, min(float(op.get("timeout_s") or _cfg("timeout_s", 6)), 30.0))
    fresca = float(_cfg("fresca_s", 120))
    ahora = time.time()

    def quiere(f: str) -> bool:
        return pedida in ("auto", f)

    elegida, antigua = None, None
    lectores = (("app", _leer_app), ("navegador", lambda: _leer_recibida("navegador")),
                ("gpsd", lambda: _leer_gpsd(timeout)), ("nmea", lambda: _leer_nmea(timeout)),
                ("windows", lambda: _leer_windows(timeout)), ("geoclue", lambda: _leer_geoclue(timeout)))
    for nombre, leer in lectores:
        if not quiere(nombre):
            continue
        try:
            fix = leer()
        except Exception as e:  # noqa: BLE001
            L.info("Fuente de ubicacion %s: %s", nombre, e)
            fix = None
        if not fix:
            continue
        if ahora - fix["t"] <= fresca:
            elegida = fix
            break
        if antigua is None or fix["t"] > antigua["t"]:
            antigua = fix
    with _mu:
        if not elegida and _ultima and pedida == "auto" and (antigua is None or _ultima["t"] > antigua["t"]) \
                and _ultima.get("fuente") != "manual":
            antigua = dict(_ultima)
    vieja = False
    if not elegida and antigua and pedida != "manual":
        elegida, vieja = antigua, True
    if not elegida and quiere("manual"):
        elegida = _leer_manual()
    if not elegida:
        return {"error": "No hay ninguna fuente de ubicación disponible ahora mismo. " + _pista()}
    with _mu:
        _ultima = dict(elegida)
    o = fix_json(elegida, ahora)
    if vieja:
        o["aviso"] = (f"Es la última posición conocida (de hace {int(ahora - elegida['t']) // 60} min): activa una "
                      "fuente en tiempo real para actualizarla.")
    if elegida["fuente"] == "manual":
        o["aviso"] = "Es la posición fija puesta en Ajustes, no una medida en tiempo real."
    hasta = str(op.get("hasta") or "").strip()
    if hasta:
        dest = buscar_lugar(hasta, _cfg("lugares", []))
        if dest is None:
            o["hasta"] = {"error": f"No conozco «{hasta}» sin internet: guárdalo en Ajustes > Ubicación o da sus "
                                   "coordenadas"}
        else:
            r = rumbo_grados(elegida["lat"], elegida["lon"], dest["lat"], dest["lon"])
            o["hasta"] = {"nombre": dest["nombre"],
                          "distancia_km": round(distancia_km(elegida["lat"], elegida["lon"], dest["lat"], dest["lon"]), 1),
                          "rumbo_grados": round(r), "direccion": rumbo_texto(r),
                          "nota": "distancia en línea recta, no por carretera"}
    o["privacidad"] = "La posición se ha leído en este equipo y no se ha enviado a ningún servicio externo."
    return o


def estado() -> dict:
    ahora = time.time()
    fuentes = []

    def add(fid: str, nombre: str, disponible: bool, nota: str = "") -> None:
        fuentes.append({"id": fid, "nombre": nombre, "disponible": bool(disponible), "nota": nota})

    app = _leer_app()
    from ..hardware import profiles
    if profiles.android() or app:
        add("app", "GPS del móvil (app Android)", bool(app),
            f"última medida hace {int(ahora - app['t'])} s" if app else "la app aún no ha escrito ninguna posición")
    nav = _leer_recibida("navegador")
    add("navegador", "Ubicación del navegador", bool(nav),
        f"última medida hace {int(ahora - nav['t'])} s" if nav else
        ("actívala abajo y concede el permiso" if not _cfg("navegador", False) else "esperando al navegador"))
    if sys.platform != "win32":
        host, _, puerto = str(_cfg("gpsd", "127.0.0.1:2947")).partition(":")
        try:
            socket.create_connection((host, int(puerto or 2947)), timeout=0.3).close()
            hay = True
        except OSError:
            hay = False
        add("gpsd", "Receptor GPS (gpsd)", hay, "" if hay else "gpsd no está en marcha")
    add("nmea", "Receptor GPS por puerto serie/USB", bool(_cfg("serie", "")),
        str(_cfg("serie", "")) or "indica el puerto (p. ej. /dev/ttyACM0 o COM3)")
    if sys.platform == "win32":
        add("windows", "Servicio de ubicación de Windows", True, "requiere la ubicación activada en Windows")
    if sys.platform.startswith("linux") and not profiles.android():
        add("geoclue", "GeoClue (ubicación de Linux)", bool(_where_am_i()),
            "" if _where_am_i() else "instala geoclue-2-demo (where-am-i)")
    add("manual", "Posición fija", bool(_leer_manual()), str(_cfg("manual", "") or "sin poner"))
    with _mu:
        ultima = fix_json(_ultima, ahora) if _ultima else None
    return {"activa": bool(_cfg("activa", True)), "fuente": _cfg("fuente", "auto"), "fuentes": fuentes,
            "navegador": bool(_cfg("navegador", False)), "manual": _cfg("manual", ""), "serie": _cfg("serie", ""),
            "gpsd": _cfg("gpsd", "127.0.0.1:2947"), "lugares": _cfg("lugares", []), "ultima": ultima,
            "privacidad": "La posición no sale del equipo: no se envía a servicios externos ni a la red COOP."}
