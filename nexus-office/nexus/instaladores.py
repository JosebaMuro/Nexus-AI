"""Instaladores sin conexion: el «maletin» de Agente IA.

Junta en un sitio los instaladores de todas las plataformas para pasarlos a otros equipos sin internet:

* **Dentro de la app** (sin descargar nada): la propia APK autonoma (Android da su ruta), la APK cliente, el codigo
  fuente, el script de la Ubuntu minima y los manuales. Los mete ``packaging/build_apk_autonomo.py`` en
  ``app/maletin/`` del runtime.
* **Guardados en el equipo**: los instaladores grandes (Windows, Linux, macOS; de 0,4 a 1,9 GB cada uno, unos 14 GB
  en total) no caben en una APK (Android no instala APK de mas de 4 GB). Se bajan UNA vez de la Release de GitHub
  de esta misma version, se comprueban con su SHA256 y desde entonces estan disponibles offline. Tambien se pueden
  importar de una carpeta o un USB sin conexion.
* **Repartir**: un servidor de solo lectura en la red local (wifi o punto de acceso del movil) sirve los ficheros con
  una pagina de descarga, reanudable (Range), sin tocar la API de Agente IA.
"""
from __future__ import annotations

import hashlib
import html
import json
import os
import re
import shutil
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote

from . import __version__
from .paths import ROOT, data_dir

REPO = "JosebaMuro/Nexus-AI"
PUERTO = 8776
MARGEN = 300 * 1024 * 1024        # espacio que se deja libre al descargar

# id -> (plataforma, detalle, patron del nombre). {v} es la version.
CATALOGO: list[tuple[str, str, str, str]] = [
    ("android-autonomo", "Android", "APK autónoma (todo dentro, ARM64)", "Nexus-Office-{v}-android-autonomo.apk"),
    ("android-cliente", "Android", "APK cliente (se conecta a un ordenador)", "Nexus-Office-{v}-android-vpn.apk"),
    ("windows-x64", "Windows", "x64 (Intel/AMD)", "Nexus-Office-Setup-{v}-windows-x64.exe"),
    ("windows-arm64", "Windows", "ARM64 (Snapdragon X)", "Nexus-Office-Setup-{v}-windows-arm64.exe"),
    ("linux-x86_64-run", "Linux x86_64", "instalador .run", "Nexus-Office-{v}-linux-x86_64.run"),
    ("linux-x86_64-deb", "Linux x86_64", "paquete .deb (Debian, Ubuntu)", "nexus-office_{v}_amd64.deb"),
    ("linux-x86_64-appimage", "Linux x86_64", "AppImage", "Nexus-Office-{v}-linux-x86_64.AppImage"),
    ("linux-x86_64-flatpak", "Linux x86_64", "Flatpak", "Nexus-Office-{v}-linux-x86_64.flatpak"),
    ("linux-aarch64-run", "Linux ARM64 / Raspberry Pi", "instalador .run", "Nexus-Office-{v}-linux-aarch64.run"),
    ("linux-aarch64-deb", "Linux ARM64 / Raspberry Pi", "paquete .deb", "nexus-office_{v}_arm64.deb"),
    ("linux-aarch64-appimage", "Linux ARM64 / Raspberry Pi", "AppImage", "Nexus-Office-{v}-linux-aarch64.AppImage"),
    ("linux-aarch64-flatpak", "Linux ARM64 / Raspberry Pi", "Flatpak", "Nexus-Office-{v}-linux-aarch64.flatpak"),
    ("macos-arm64", "macOS", "Apple Silicon (M1 y posteriores)", "Nexus-Office-{v}-macos-arm64.tar.gz"),
    ("macos-x86_64", "macOS", "Intel", "Nexus-Office-{v}-macos-x86_64.tar.gz"),
    ("ubuntu-minima", "Ubuntu mínima", "script que rehace la Ubuntu mínima con el demonio", "rehacer-ubuntu.sh"),
    ("codigo-fuente", "Código fuente", "zip del proyecto limpio", "Nexus-Office-{v}-codigo-fuente.zip"),
    ("manual-usuario", "Documentación", "manual de usuario (PDF)", "Manual-de-usuario.pdf"),
    ("doc-tecnica", "Documentación", "documentación técnica (PDF)", "Documentacion-tecnica.pdf"),
    ("leeme", "Documentación", "qué fichero bajar para cada equipo", "LEEME.txt"),
]
COMO = {   # como se instala en el otro equipo (se ve en la pagina de descarga)
    "Android": "Abre la APK en el otro móvil y permite instalar apps de origen desconocido.",
    "Windows": "Ejecuta el .exe; puede instalarse como servicio de Windows.",
    "Linux x86_64": "sh fichero.run, o sudo apt install ./fichero.deb, o la AppImage con chmod +x.",
    "Linux ARM64 / Raspberry Pi": "sh fichero.run, o sudo apt install ./fichero.deb, o la AppImage con chmod +x.",
    "macOS": "tar xzf fichero.tar.gz y abre Agente IA.",
    "Ubuntu mínima": "sudo bash rehacer-ubuntu.sh en una Ubuntu Server mínima.",
}
_NOMBRE_SEGURO = re.compile(r"^[A-Za-z0-9._-]{1,160}$")


def carpeta() -> Path:
    d = data_dir() / "instaladores"
    d.mkdir(parents=True, exist_ok=True)
    return d


def carpeta_app() -> Path:
    """Ficheros que vienen dentro de la app (APK autonoma: runtime/app/maletin)."""
    return Path(os.environ.get("NEXUS_MALETIN_APP", ROOT / "maletin"))


def _nombre(patron: str, version: str) -> str:
    return patron.format(v=version)


def _sha256(path: Path, bloque: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bloque)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _humano(n: int) -> str:
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.0f} {u}" if u in ("B", "KB") else f"{n:.1f} {u}".replace(".", ",")
        n /= 1024
    return str(n)


class Maletin:
    """Estado de los instaladores: que hay, que falta, descargas en curso y el servidor de la red local."""

    def __init__(self, version: str = __version__):
        self.version = version
        self._lock = threading.RLock()
        self._cola: list[str] = []
        self._hilo: threading.Thread | None = None
        self._cancelar = False
        self.descarga: dict = {}          # {"id", "nombre", "hecho", "total", "error"}
        self.errores: dict[str, str] = {}
        self._srv: ThreadingHTTPServer | None = None
        self._srv_hilo: threading.Thread | None = None

    # ------------------------------------------------------------------ catalogo de la Release (cache local)
    def _release_json(self) -> Path:
        return carpeta() / "release.json"

    def release(self) -> dict:
        """Lo que se sabe de la Release (nombres, tamanos, URL y SHA256), guardado para usarlo sin conexion."""
        try:
            return json.loads(self._release_json().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def actualizar_release(self) -> dict:
        """Consulta la Release de esta version en GitHub (con conexion) y guarda su lista y sus SHA256."""
        from . import net
        api = f"https://api.github.com/repos/{REPO}/releases"
        aviso = ""
        try:
            rel = net.get_json(f"{api}/tags/v{self.version}")
        except Exception:  # noqa: BLE001 - version sin Release publicada: la ultima
            rel = net.get_json(f"{api}/latest")
            aviso = f"La versión {self.version} no tiene Release; se usa {rel.get('tag_name')}."
        assets = {a["name"]: {"tam": a.get("size", 0), "url": a.get("browser_download_url", "")}
                  for a in rel.get("assets", [])}
        sumas: dict[str, str] = {}
        if "SHA256SUMS.txt" in assets:
            import httpx
            r = httpx.get(assets["SHA256SUMS.txt"]["url"], headers=net.UA, follow_redirects=True, timeout=30)
            r.raise_for_status()
            for linea in r.text.splitlines():
                p = linea.split()
                if len(p) == 2:
                    sumas[p[1].lstrip("*")] = p[0].lower()
        tag = rel.get("tag_name", f"v{self.version}")
        info = {"tag": tag, "version": tag.lstrip("v"), "url": rel.get("html_url", ""), "assets": assets,
                "sha256": sumas, "consultado": time.strftime("%Y-%m-%d %H:%M"), "aviso": aviso}
        tmp = self._release_json().with_suffix(".tmp")
        tmp.write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
        os.replace(tmp, self._release_json())
        return info

    def version_release(self) -> str:
        return self.release().get("version") or self.version

    # ------------------------------------------------------------------ que hay
    def _verificados(self) -> dict:
        try:
            return json.loads((carpeta() / "verificados.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _marcar(self, nombre: str, estado: str) -> None:
        v = self._verificados()
        v[nombre] = estado
        (carpeta() / "verificados.json").write_text(json.dumps(v, indent=1), encoding="utf-8")

    def _apk_propia(self) -> Path | None:
        p = os.environ.get("NEXUS_APK_PROPIA", "")
        return Path(p) if p and Path(p).is_file() else None

    def localizar(self, ident: str) -> tuple[Path | None, str, str]:
        """(ruta, nombre, origen) de un elemento del catalogo; origen: app, equipo o ''."""
        _, _, _, patron = next(c for c in CATALOGO if c[0] == ident)
        for ver in dict.fromkeys((self.version, self.version_release())):
            nombre = _nombre(patron, ver)
            if ident == "android-autonomo" and ver == self.version and self._apk_propia():
                return self._apk_propia(), nombre, "app"
            for d, origen in ((carpeta_app(), "app"), (carpeta(), "equipo")):
                if (d / nombre).is_file():
                    return d / nombre, nombre, origen
        return None, _nombre(patron, self.version_release()), ""

    def estado(self) -> dict:
        rel = self.release()
        assets, sumas, ver = rel.get("assets", {}), rel.get("sha256", {}), self._verificados()
        items = []
        with self._lock:
            cola = list(self._cola)
            desc = dict(self.descarga)
        for ident, plataforma, detalle, _ in CATALOGO:
            ruta, nombre, origen = self.localizar(ident)
            tam = ruta.stat().st_size if ruta else int(assets.get(nombre, {}).get("tam", 0))
            if desc.get("id") == ident:
                est = "descargando"
            elif ident in cola:
                est = "en cola"
            elif ruta:
                est = "listo"
            elif nombre in assets:
                est = "falta"
            else:
                est = "no publicado" if assets else "falta"
            items.append({"id": ident, "plataforma": plataforma, "detalle": detalle, "nombre": nombre, "tam": tam,
                          "estado": est, "origen": origen, "ruta": str(ruta) if ruta else "",
                          "sha256": sumas.get(nombre, ""), "verificado": ver.get(nombre, "") if origen == "equipo" else
                          ("app" if origen == "app" else ""), "error": self.errores.get(ident, "")})
        try:
            libre = shutil.disk_usage(carpeta()).free
        except OSError:
            libre = 0
        listos = [i for i in items if i["estado"] == "listo"]
        return {"version": self.version, "release": {k: rel.get(k) for k in ("tag", "url", "consultado", "aviso")},
                "carpeta": str(carpeta()), "libre": libre, "items": items, "descarga": desc, "cola": cola,
                "listos": len(listos), "bytes_listos": sum(i["tam"] for i in listos),
                "faltan_bytes": sum(i["tam"] for i in items if i["estado"] == "falta"),
                "compartir": self.compartir_estado()}

    # ------------------------------------------------------------------ descargas (una vez, luego offline)
    def descargar(self, ids: list[str]) -> dict:
        rel = self.release() or self.actualizar_release()
        conocidos = {c[0] for c in CATALOGO}
        nuevos = []
        with self._lock:
            for i in ids:
                if i not in conocidos:
                    raise ValueError(f"instalador desconocido: {i}")
                ruta, nombre, _ = self.localizar(i)
                if ruta or i in self._cola or self.descarga.get("id") == i:
                    continue
                if nombre not in rel.get("assets", {}):
                    raise ValueError(f"{nombre} no está en la Release {rel.get('tag', '')}")
                nuevos.append(i)
            necesita = sum(int(rel["assets"][self.localizar(i)[1]]["tam"]) for i in nuevos + self._cola)
            libre = shutil.disk_usage(carpeta()).free
            if necesita + MARGEN > libre:
                raise ValueError(f"No hay espacio: hacen falta {_humano(necesita)} y quedan {_humano(libre)} libres")
            self._cola += nuevos
            self._cancelar = False
            if nuevos and not (self._hilo and self._hilo.is_alive()):
                self._hilo = threading.Thread(target=self._bucle, name="maletin-descargas", daemon=True)
                self._hilo.start()
        return self.estado()

    def cancelar(self) -> None:
        with self._lock:
            self._cola.clear()
            self._cancelar = True

    def _bucle(self) -> None:
        from . import net
        while True:
            with self._lock:
                if not self._cola:
                    self.descarga = {}
                    return
                ident = self._cola.pop(0)
                _, nombre, _ = self.localizar(ident)
                rel = self.release()
                url = rel["assets"][nombre]["url"]
                self.descarga = {"id": ident, "nombre": nombre, "hecho": 0,
                                 "total": int(rel["assets"][nombre]["tam"]), "t0": time.time()}
                self.errores.pop(ident, None)

            def progreso(hecho: int, total: int) -> None:
                self.descarga.update(hecho=hecho, total=total or self.descarga.get("total", 0))
            destino = carpeta() / nombre
            try:
                net.download(url, destino, progress=progreso, cancel=lambda: self._cancelar, retries=5)
                self._comprobar(destino, rel.get("sha256", {}).get(nombre, ""))
            except InterruptedError:
                with self._lock:
                    self.descarga = {}
                return   # el .part se queda: la siguiente vez se reanuda
            except Exception as e:  # noqa: BLE001
                self.errores[ident] = str(e)[:300]

    def _comprobar(self, f: Path, esperado: str) -> str:
        if not esperado:
            self._marcar(f.name, "sin suma")
            return "sin suma"
        if _sha256(f) != esperado:
            f.unlink(missing_ok=True)
            self._marcar(f.name, "")
            raise ValueError(f"{f.name}: el SHA256 no coincide; se ha borrado (vuelve a descargarlo)")
        self._marcar(f.name, "sha256")
        return "sha256"

    def revisar(self) -> dict:
        """Tras importar ficheros a la carpeta (USB, carpeta compartida, la app de Android): los comprueba."""
        sumas = self.release().get("sha256", {})
        nombres = {_nombre(c[3], v) for c in CATALOGO for v in {self.version, self.version_release()}}
        hechos, malos = [], []
        for f in sorted(carpeta().iterdir()):
            if not f.is_file() or f.name not in nombres or self._verificados().get(f.name):
                continue
            try:
                hechos.append((f.name, self._comprobar(f, sumas.get(f.name, ""))))
            except ValueError as e:
                malos.append(str(e))
        return {"comprobados": hechos, "errores": malos, **self.estado()}

    def importar(self, origen: str) -> dict:
        """Copia de una carpeta (USB, disco, carpeta de red) los instaladores que reconozca y los comprueba."""
        src = Path(origen).expanduser()
        if not src.is_dir():
            raise ValueError(f"No existe la carpeta {origen}")
        nombres = {_nombre(c[3], v) for c in CATALOGO for v in {self.version, self.version_release()}}
        nombres.add("SHA256SUMS.txt")
        copiados = []
        for f in sorted(src.iterdir()):
            if f.is_file() and f.name in nombres and not (carpeta() / f.name).is_file():
                if f.stat().st_size + MARGEN > shutil.disk_usage(carpeta()).free:
                    raise ValueError(f"No hay espacio para {f.name}")
                tmp = carpeta() / (f.name + ".part")
                shutil.copyfile(f, tmp)
                os.replace(tmp, carpeta() / f.name)
                copiados.append(f.name)
        sums = carpeta() / "SHA256SUMS.txt"
        if sums.is_file():   # sumas de la Release copiadas con los ficheros: sirven sin conexion
            rel = self.release() or {"tag": f"v{self.version}", "version": self.version, "assets": {}}
            rel.setdefault("sha256", {})
            for linea in sums.read_text(encoding="utf-8", errors="replace").splitlines():
                p = linea.split()
                if len(p) == 2:
                    rel["sha256"].setdefault(p[1].lstrip("*"), p[0].lower())
            self._release_json().write_text(json.dumps(rel, ensure_ascii=False, indent=1), encoding="utf-8")
        r = self.revisar()
        r["copiados"] = copiados
        return r

    def borrar(self, ident: str) -> None:
        ruta, nombre, origen = self.localizar(ident)
        if origen != "equipo" or not ruta:
            raise ValueError("Solo se pueden borrar los instaladores guardados en el equipo")
        ruta.unlink(missing_ok=True)
        (carpeta() / (nombre + ".part")).unlink(missing_ok=True)
        v = self._verificados()
        v.pop(nombre, None)
        (carpeta() / "verificados.json").write_text(json.dumps(v, indent=1), encoding="utf-8")

    # ------------------------------------------------------------------ repartir por la red local
    def disponibles(self) -> dict[str, tuple[Path, dict]]:
        out = {}
        for it in self.estado()["items"]:
            if it["estado"] == "listo" and it["ruta"]:
                out[it["nombre"]] = (Path(it["ruta"]), it)
        return out

    def compartir(self, activo: bool, puerto: int = PUERTO) -> dict:
        with self._lock:
            if not activo:
                if self._srv:
                    self._srv.shutdown()
                    self._srv.server_close()
                self._srv = None
                return self.compartir_estado()
            if self._srv:
                return self.compartir_estado()
            maletin = self

            class Manejador(_Manejador):
                m = maletin
            srv = ThreadingHTTPServer(("0.0.0.0", int(puerto)), Manejador)
            srv.daemon_threads = True
            self._srv = srv
            self._srv_hilo = threading.Thread(target=srv.serve_forever, name="maletin-red", daemon=True)
            self._srv_hilo.start()
        return self.compartir_estado()

    def compartir_estado(self) -> dict:
        if not self._srv:
            return {"activo": False, "urls": []}
        p = self._srv.server_address[1]
        return {"activo": True, "puerto": p, "urls": [f"http://{ip}:{p}/" for ip in ips_locales()]}

    def parar(self) -> None:
        self.cancelar()
        self.compartir(False)


def ips_locales() -> list[str]:
    """IPv4 de la red local (wifi, punto de acceso, cable), sin la de bucle."""
    ips: list[str] = []
    try:
        import psutil
        for nombre, dirs in psutil.net_if_addrs().items():
            for d in dirs:
                if d.family == socket.AF_INET and not d.address.startswith("127.") and not nombre.startswith(("docker", "veth", "br-")):
                    ips.append(d.address)
    except Exception:  # noqa: BLE001 - Android puede no dejar listar interfaces
        pass
    if not ips:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("10.255.255.255", 1))
                ips.append(s.getsockname()[0])
        except OSError:
            pass
    # las de red domestica y punto de acceso primero
    return sorted(dict.fromkeys(ips), key=lambda ip: (not ip.startswith(("192.168.", "10.", "172.")), ip))


class _Manejador(BaseHTTPRequestHandler):
    """Solo lectura: la pagina y los ficheros listos del maletin. Nada de la API de Agente IA."""
    m: Maletin
    server_version = "AgenteIA-instaladores"

    def log_message(self, *a) -> None:  # noqa: D102
        pass

    def do_HEAD(self) -> None:  # noqa: N802
        self.do_GET(cuerpo=False)

    def do_GET(self, cuerpo: bool = True) -> None:  # noqa: N802
        ruta = unquote(self.path.split("?", 1)[0])
        if ruta in ("/", "/index.html"):
            return self._enviar(200, self._pagina().encode("utf-8"), "text/html; charset=utf-8", cuerpo)
        if ruta == "/SHA256SUMS.txt":
            lineas = [f"{it['sha256']}  {n}" for n, (_, it) in self.m.disponibles().items() if it["sha256"]]
            return self._enviar(200, ("\n".join(lineas) + "\n").encode(), "text/plain; charset=utf-8", cuerpo)
        nombre = ruta[3:] if ruta.startswith("/f/") else ""
        disp = self.m.disponibles()
        if not _NOMBRE_SEGURO.match(nombre) or nombre not in disp:
            return self._enviar(404, b"No encontrado", "text/plain", cuerpo)
        self._fichero(disp[nombre][0], nombre, cuerpo)

    def _enviar(self, codigo: int, datos: bytes, tipo: str, cuerpo: bool) -> None:
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(datos)))
        self.end_headers()
        if cuerpo:
            self.wfile.write(datos)

    def _fichero(self, path: Path, nombre: str, cuerpo: bool) -> None:
        total = path.stat().st_size
        ini, fin = 0, total - 1
        rango = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", ""))
        if rango and (rango.group(1) or rango.group(2)):
            if rango.group(1):
                ini = int(rango.group(1))
                fin = min(int(rango.group(2)), total - 1) if rango.group(2) else total - 1
            else:
                ini = max(0, total - int(rango.group(2)))
            if ini > fin or ini >= total:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{total}")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {ini}-{fin}/{total}")
        else:
            self.send_response(200)
        tipo = "application/vnd.android.package-archive" if nombre.endswith(".apk") else "application/octet-stream"
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(fin - ini + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Disposition", f"attachment; filename=\"{nombre}\"")
        self.end_headers()
        if not cuerpo:
            return
        quedan = fin - ini + 1
        try:
            with open(path, "rb") as f:
                f.seek(ini)
                while quedan > 0:
                    b = f.read(min(1 << 20, quedan))
                    if not b:
                        break
                    self.wfile.write(b)
                    quedan -= len(b)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _pagina(self) -> str:
        disp = self.m.disponibles()
        grupos: dict[str, list] = {}
        for nombre, (_, it) in disp.items():
            grupos.setdefault(it["plataforma"], []).append(it)
        e = html.escape
        filas = []
        for plat, its in grupos.items():
            como = COMO.get(plat, "")
            filas.append(f"<h2>{e(plat)}</h2>" + (f"<p class=c>{e(como)}</p>" if como else ""))
            for it in its:
                sha = f"<div class=s>SHA256 {e(it['sha256'])}</div>" if it["sha256"] else ""
                filas.append(f"<a class=f href=\"/f/{quote(it['nombre'])}\"><b>{e(it['detalle'])}</b>"
                             f"<span>{e(it['nombre'])} · {_humano(it['tam'])}</span>{sha}</a>")
        cuerpo = "".join(filas) or "<p>Todavía no hay instaladores guardados en este equipo.</p>"
        return f"""<!doctype html><html lang=es><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Agente IA · instaladores</title>
<style>body{{font-family:system-ui,sans-serif;max-width:760px;margin:0 auto;padding:16px;background:#0f1216;color:#e8edf2}}
h1{{font-size:22px}}h2{{font-size:16px;margin:22px 0 4px;color:#7fd1ff}}.c{{margin:0 0 8px;color:#9aa7b4;font-size:14px}}
a.f{{display:block;padding:12px 14px;margin:8px 0;border-radius:10px;background:#1a2027;color:inherit;text-decoration:none;border:1px solid #2a333d}}
a.f span{{display:block;color:#9aa7b4;font-size:13px;margin-top:2px}}.s{{font:11px monospace;color:#6b7785;word-break:break-all;margin-top:4px}}
</style></head><body><h1>Agente IA {e(self.m.version)} · instaladores</h1>
<p class=c>Descarga sin internet desde el equipo que comparte. Las descargas se pueden reanudar. <a href="/SHA256SUMS.txt" style="color:#7fd1ff">SHA256SUMS.txt</a></p>
{cuerpo}</body></html>"""


_MALETIN: Maletin | None = None


def maletin() -> Maletin:
    global _MALETIN
    if _MALETIN is None:
        _MALETIN = Maletin()
    return _MALETIN
