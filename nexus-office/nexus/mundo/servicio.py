"""Servicio «Mundo»: actualiza cada fuente a su intervalo, guarda lo recibido ordenado por categoria y pais y lo
pone a disposicion de los agentes (herramientas ``mundo_*``), de la interfaz y de la API.

Configuracion (``mundo`` en data/config.yaml, todo se cambia desde Ajustes > Mundo):

    activo            interruptor general
    factor            multiplica todos los intervalos (0,25 = cuatro veces mas a menudo; 4 = cuatro veces menos)
    fuentes.<id>      {activa, intervalo_s} de cada fuente; lo que falte usa el valor de fabrica
    paises            paises de interes (noticias, tiempo y radio se piden para ellos)
    claves.<nombre>   claves gratuitas de las fuentes que la piden (firms, aisstream)
    retencion_h       horas que se conservan las noticias
    contexto          anadir al agente un resumen cuando la pregunta nombra un pais o un tema del mundo
"""
from __future__ import annotations

import concurrent.futures
import json
import sqlite3
import threading
import time
import unicodedata

from .. import log, paths
from . import paises
from .fuentes import CATEGORIAS, FUENTES, Fuente

L = log.get("mundo")
INTERVALO_MIN = 30

_ESQUEMA = """
CREATE TABLE IF NOT EXISTS registros (
    fuente TEXT NOT NULL, uid TEXT NOT NULL, categoria TEXT NOT NULL, pais TEXT NOT NULL DEFAULT '',
    titulo TEXT NOT NULL, detalle TEXT DEFAULT '', lat REAL, lon REAL, ts REAL, url TEXT DEFAULT '', valor REAL,
    datos TEXT DEFAULT '{}', recibido REAL NOT NULL, PRIMARY KEY (fuente, uid));
CREATE INDEX IF NOT EXISTS ix_reg_cat_pais ON registros(categoria, pais);
CREATE INDEX IF NOT EXISTS ix_reg_pais ON registros(pais);
CREATE TABLE IF NOT EXISTS estado (
    fuente TEXT PRIMARY KEY, ultimo REAL, ultimo_ok REAL, registros INTEGER DEFAULT 0, error TEXT DEFAULT '',
    ms REAL DEFAULT 0, fallos INTEGER DEFAULT 0, avisos TEXT DEFAULT '');
"""

# palabras que indican que una pregunta va de algo que las fuentes conocen
_TEMAS = {
    "sucesos": ("terremoto", "sismo", "seismo", "temblor", "incendio", "volcan", "erupcion", "inundacion", "desastre",
                "tsunami", "catastrofe"),
    "movimiento": ("vuelo", "avion", "aviones", "aeronave", "barco", "buque", "trafico aereo", "trafico maritimo",
                   "bicicleta compartida"),
    "clima": ("tiempo hace", "temperatura", "lluvia", "huracan", "ciclon", "tormenta", "calidad del aire", "clima"),
    "espacio": ("satelite", "lanzamiento", "cohete", "estacion espacial", "orbita"),
    "noticias": ("noticia", "titular", "conflicto", "guerra", "protesta", "que pasa en", "que esta pasando",
                 "actualidad"),
    "mercados": ("bolsa", "indice", "bitcoin", "cripto", "tipos de interes", "curva de tipos", "calendario economico",
                 "resultados empresariales", "sector"),
    "audio": ("radio", "emisora"),
    "camaras": ("camara de trafico", "camaras de trafico"),
    "naturaleza": ("avistamiento", "fauna", "especie"),
}


def _plano(s: str) -> str:
    t = unicodedata.normalize("NFKD", str(s or "").lower())
    return "".join(c for c in t if not unicodedata.combining(c))


class SinClave(RuntimeError):
    pass


class Contexto:
    """Lo que una fuente necesita para pedir sus datos: red con limites, configuracion y claves."""

    def __init__(self, mundo: "Mundo", fuente: Fuente) -> None:
        self.mundo, self.fuente = mundo, fuente
        self.avisos: list[str] = []

    def cfg(self, clave: str, defecto=None):
        return self.mundo.cfg(clave, defecto)

    def clave(self, nombre: str) -> str:
        v = str((self.mundo.cfg("claves", {}) or {}).get(nombre) or "").strip()
        if not v:
            raise SinClave(f"falta la clave «{nombre}»")
        return v

    def paises_interes(self) -> list[str]:
        return self.mundo.paises_interes()

    def aviso(self, texto: str) -> None:
        self.avisos.append(str(texto)[:200])

    def pedir(self, url: str, params: dict | None = None, tope_mb: float = 12, espera: float = 25,
              cabeceras: dict | None = None, metodo: str = "GET", cuerpo=None):
        return self.mundo.pedir(url, params=params, tope_mb=tope_mb, espera=espera, cabeceras=cabeceras,
                                metodo=metodo, cuerpo=cuerpo)

    def texto(self, url: str, **kw) -> str:
        return self.pedir(url, **kw).decode("utf-8", errors="replace")

    def json(self, url: str, **kw):
        return json.loads(self.pedir(url, **kw))


class Mundo:
    def __init__(self, config, ruta: str | None = None, cliente=None) -> None:
        self.config = config
        self.ruta = str(ruta or (paths.sub("mundo") / "mundo.db"))
        self._mu = threading.RLock()
        self._db = sqlite3.connect(self.ruta, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        with self._mu:
            try:
                self._db.execute("PRAGMA journal_mode=WAL")
                self._db.execute("PRAGMA synchronous=NORMAL")
            except sqlite3.DatabaseError:
                pass
            self._db.executescript(_ESQUEMA)
            self._db.commit()
        self._cliente = cliente
        self.fuentes: dict[str, Fuente] = {f.id: f for f in FUENTES}
        self._en_curso: set[str] = set()
        self._hilo: threading.Thread | None = None
        self._parar = threading.Event()
        self._grupo = concurrent.futures.ThreadPoolExecutor(max_workers=3, thread_name_prefix="mundo")
        self._sin_red_hasta = 0.0
        self._fallos_red = 0
        self.oyentes: list = []   # callbacks(evento) para avisar a la interfaz

    # ------------------------------------------------------------ configuracion
    def cfg(self, clave: str, defecto=None):
        v = self.config.get(f"mundo.{clave}", None) if self.config is not None else None
        return defecto if v is None else v

    def activo(self) -> bool:
        return bool(self.cfg("activo", True))

    def paises_interes(self) -> list[str]:
        out = []
        for x in self.cfg("paises", []) or []:
            c = paises.codigo(x)
            if c and c not in out:
                out.append(c)
        return out or ["ES"]

    def _es_movil(self) -> bool:
        import os
        return bool(os.environ.get("ANDROID_ROOT") or os.environ.get("NEXUS_ANDROID"))

    def ajustes(self, f: Fuente) -> dict:
        """Activa e intervalo efectivos de una fuente (configuracion del usuario sobre los valores de fabrica)."""
        propio = (self.cfg("fuentes", {}) or {}).get(f.id) or {}
        activa = propio.get("activa")
        if activa is None:
            activa = f.activa and not (f.peso == "pesada" and self._es_movil())
        try:
            base = float(propio.get("intervalo_s") or f.intervalo_s)
        except (TypeError, ValueError):
            base = float(f.intervalo_s)
        try:
            factor = min(20.0, max(0.1, float(self.cfg("factor", 1.0) or 1.0)))
        except (TypeError, ValueError):
            factor = 1.0
        tiene_clave = not f.clave or bool(str((self.cfg("claves", {}) or {}).get(f.clave) or "").strip())
        return {"activa": bool(activa), "intervalo_s": int(max(f.minimo_s, INTERVALO_MIN, base * factor)),
                "intervalo_base_s": int(base), "con_clave": tiene_clave}

    def configurar(self, cambios: dict) -> dict:
        """Aplica cambios de la interfaz: {activo, factor, paises, retencion_h, contexto, claves{}, fuentes{id:{}}}."""
        if self.config is None:
            raise RuntimeError("sin configuración")
        if "activo" in cambios:
            self.config.set("mundo.activo", bool(cambios["activo"]), save=False)
        if "contexto" in cambios:
            self.config.set("mundo.contexto", bool(cambios["contexto"]), save=False)
        if "factor" in cambios:
            self.config.set("mundo.factor", min(20.0, max(0.1, float(cambios["factor"]))), save=False)
        if "retencion_h" in cambios:
            self.config.set("mundo.retencion_h", min(720, max(1, int(cambios["retencion_h"]))), save=False)
        if "paises" in cambios:
            crudo = cambios["paises"]
            if isinstance(crudo, str):
                crudo = [x for x in crudo.replace(";", ",").split(",")]
            lista = []
            for x in crudo or []:
                c = paises.codigo(str(x).strip())
                if c and c not in lista:
                    lista.append(c)
            self.config.set("mundo.paises", lista[:25], save=False)
        if isinstance(cambios.get("claves"), dict):
            claves = dict(self.cfg("claves", {}) or {})
            for k, v in cambios["claves"].items():
                if k in {f.clave for f in FUENTES if f.clave} and "•" not in str(v):
                    claves[k] = str(v).strip()[:200]
            self.config.set("mundo.claves", claves, save=False)
        if isinstance(cambios.get("fuentes"), dict):
            actual = {k: dict(v) for k, v in (self.cfg("fuentes", {}) or {}).items() if isinstance(v, dict)}
            for fid, c in cambios["fuentes"].items():
                f = self.fuentes.get(fid)
                if f is None or not isinstance(c, dict):
                    continue
                propio = actual.setdefault(fid, {})
                if "activa" in c:
                    propio["activa"] = bool(c["activa"])
                if c.get("intervalo_s") not in (None, ""):
                    propio["intervalo_s"] = int(max(f.minimo_s, INTERVALO_MIN, min(7 * 86400, float(c["intervalo_s"]))))
            self.config.set("mundo.fuentes", actual, save=False)
        if hasattr(self.config, "save"):
            self.config.save()
        return self.estado()

    # ------------------------------------------------------------ red
    def _http(self):
        if self._cliente is None:
            import httpx
            self._cliente = httpx.Client(follow_redirects=True, headers={
                "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                              "Chrome/126.0 Safari/537.36 AgenteIA",
                "Accept": "application/json, text/plain, */*", "Accept-Language": "es-ES,es;q=0.9,en;q=0.8"})
        return self._cliente

    def pedir(self, url: str, params: dict | None = None, tope_mb: float = 12, espera: float = 25,
              cabeceras: dict | None = None, metodo: str = "GET", cuerpo=None) -> bytes:
        tope = int(tope_mb * 1024 * 1024)
        kw = {"params": params, "headers": cabeceras, "timeout": espera}
        if cuerpo is not None:
            kw["json"] = cuerpo
        with self._http().stream(metodo, url, **kw) as r:
            if r.status_code >= 400:
                raise RuntimeError(f"HTTP {r.status_code} de {r.url.host}")
            trozos, n = [], 0
            for trozo in r.iter_bytes():
                n += len(trozo)
                if n > tope:
                    raise RuntimeError(f"respuesta de más de {tope_mb:g} MB de {r.url.host}")
                trozos.append(trozo)
        return b"".join(trozos)

    # ------------------------------------------------------------ actualizar
    def _q(self, sql: str, args=()) -> list[dict]:
        with self._mu:
            return [dict(r) for r in self._db.execute(sql, args).fetchall()]

    def _fila_estado(self, fid: str) -> dict:
        r = self._q("SELECT * FROM estado WHERE fuente=?", (fid,))
        return r[0] if r else {"fuente": fid, "ultimo": None, "ultimo_ok": None, "registros": 0, "error": "",
                               "ms": 0, "fallos": 0, "avisos": ""}

    def actualizar(self, fid: str) -> dict:
        """Pide una fuente ahora y guarda lo recibido. Devuelve su estado."""
        f = self.fuentes.get(fid)
        if f is None:
            raise KeyError(f"fuente desconocida: {fid}")
        with self._mu:
            if fid in self._en_curso:
                return {**self._fila_estado(fid), "en_curso": True}
            self._en_curso.add(fid)
        t0 = time.time()
        ctx = Contexto(self, f)
        error, registros = "", None
        try:
            registros = f.obtener(ctx) or []
        except SinClave as e:
            error = str(e)
        except Exception as e:  # noqa: BLE001  (una fuente caida no afecta a las demas)
            error = f"{type(e).__name__}: {e}"[:300]
        finally:
            with self._mu:
                self._en_curso.discard(fid)
        ms = round((time.time() - t0) * 1000, 1)
        previo = self._fila_estado(fid)
        if registros is not None:
            n = self._guardar(f, registros)
            with self._mu:
                self._db.execute(
                    "INSERT OR REPLACE INTO estado(fuente, ultimo, ultimo_ok, registros, error, ms, fallos, avisos) "
                    "VALUES(?,?,?,?,?,?,0,?)", (fid, t0, t0, n, "", ms, "; ".join(ctx.avisos)[:500]))
                self._db.commit()
            self._fallos_red = 0
        else:
            with self._mu:
                self._db.execute(
                    "INSERT OR REPLACE INTO estado(fuente, ultimo, ultimo_ok, registros, error, ms, fallos, avisos) "
                    "VALUES(?,?,?,?,?,?,?,?)", (fid, t0, previo.get("ultimo_ok"), previo.get("registros") or 0, error,
                                                ms, int(previo.get("fallos") or 0) + 1, "; ".join(ctx.avisos)[:500]))
                self._db.commit()
            if any(x in error for x in ("ConnectError", "ConnectTimeout", "NameResolution", "getaddrinfo",
                                        "Network is unreachable", "ProxyError")):
                self._fallos_red += 1
                if self._fallos_red >= 3:   # sin conexion: no insistir con todas las fuentes cada pocos segundos
                    self._sin_red_hasta = time.time() + 300
            L.info("Fuente %s: %s", fid, error)
        estado = self._fila_estado(fid)
        for cb in list(self.oyentes):
            try:
                cb({"type": "mundo", "fuente": fid, "registros": estado["registros"], "error": estado["error"]})
            except Exception:  # noqa: BLE001
                pass
        return estado

    def _guardar(self, f: Fuente, registros: list[dict]) -> int:
        ahora = time.time()
        tope = int(self.cfg("max_por_fuente", 1500))
        filas = []
        vistos: set[str] = set()
        for r in registros[:tope]:
            uid = str(r.get("uid") or r.get("titulo") or "")[:300]
            titulo = str(r.get("titulo") or "").strip()
            if not uid or not titulo or uid in vistos:
                continue
            vistos.add(uid)
            filas.append((f.id, uid, f.categoria, str(r.get("pais") or "").upper()[:2], titulo[:300],
                          str(r.get("detalle") or "")[:600], r.get("lat"), r.get("lon"), r.get("ts") or ahora,
                          str(r.get("url") or "")[:500], r.get("valor"),
                          json.dumps(r.get("datos") or {}, ensure_ascii=False, default=str)[:4000], ahora))
        with self._mu:
            if f.acumula:
                limite = ahora - float(self.cfg("retencion_h", 48)) * 3600
                self._db.execute("DELETE FROM registros WHERE fuente=? AND recibido<?", (f.id, limite))
                self._db.executemany(
                    "INSERT INTO registros VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(fuente, uid) DO UPDATE SET "
                    "titulo=excluded.titulo, detalle=excluded.detalle, pais=excluded.pais, valor=excluded.valor, "
                    "datos=excluded.datos", filas)
            else:
                self._db.execute("DELETE FROM registros WHERE fuente=?", (f.id,))
                self._db.executemany("INSERT OR REPLACE INTO registros VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", filas)
            self._db.commit()
            return self._db.execute("SELECT COUNT(*) FROM registros WHERE fuente=?", (f.id,)).fetchone()[0]

    def pendientes(self, ahora: float | None = None) -> list[str]:
        """Fuentes a las que les toca actualizarse. Tras un fallo, el intervalo se alarga hasta una hora."""
        ahora = ahora or time.time()
        if not self.activo() or ahora < self._sin_red_hasta:
            return []
        out = []
        for f in self.fuentes.values():
            a = self.ajustes(f)
            if not a["activa"] or not a["con_clave"] or f.id in self._en_curso:
                continue
            e = self._fila_estado(f.id)
            espera = a["intervalo_s"]
            if e.get("fallos"):
                espera = min(3600, max(espera, 60 * 2 ** min(6, int(e["fallos"]))))
            if not e.get("ultimo") or ahora - float(e["ultimo"]) >= espera:
                out.append(f.id)
        return out

    def ciclo(self) -> list[str]:
        """Lanza las fuentes pendientes (de fondo, tres a la vez). Devuelve las que ha puesto en marcha."""
        ids = self.pendientes()
        for fid in ids:
            self._grupo.submit(self.actualizar, fid)
        return ids

    def iniciar(self, retraso: float = 25.0) -> None:
        """Planificador de fondo. Espera un poco al arrancar para no competir con la carga del modelo."""
        if self._hilo is not None:
            return

        def bucle() -> None:
            if self._parar.wait(retraso):
                return
            while not self._parar.is_set():
                try:
                    self.ciclo()
                except Exception as e:  # noqa: BLE001
                    L.warning("Planificador de Mundo: %s", e)
                self._parar.wait(10)

        self._hilo = threading.Thread(target=bucle, name="mundo-planificador", daemon=True)
        self._hilo.start()

    def cerrar(self) -> None:
        self._parar.set()
        self._grupo.shutdown(wait=False, cancel_futures=True)
        try:
            if self._cliente is not None:
                self._cliente.close()
        except Exception:  # noqa: BLE001
            pass
        with self._mu:
            self._db.close()

    # ------------------------------------------------------------ consulta
    @staticmethod
    def _publico(r: dict, corto: bool = False) -> dict:
        d = {"categoria": r["categoria"], "fuente": r["fuente"], "pais": r["pais"],
             "pais_nombre": paises.nombre(r["pais"]) if r["pais"] else "", "titulo": r["titulo"],
             "detalle": r["detalle"], "fecha": time.strftime("%Y-%m-%d %H:%M", time.gmtime(r["ts"])) + " UTC"
             if r.get("ts") else "", "url": r["url"]}
        if not corto:
            d.update(lat=r["lat"], lon=r["lon"], valor=r["valor"], ts=r["ts"])
            try:
                d["datos"] = json.loads(r["datos"] or "{}")
            except ValueError:
                d["datos"] = {}
        return {k: v for k, v in d.items() if v not in (None, "", {})}

    def consultar(self, categoria: str = "", pais: str = "", texto: str = "", fuente: str = "", limite: int = 20,
                  horas: float | None = None, orden: str = "reciente", corto: bool = False) -> list[dict]:
        cond, args = [], []
        if categoria:
            cond.append("categoria=?")
            args.append(self.categoria(categoria))
        if fuente:
            cond.append("fuente=?")
            args.append(fuente)
        if pais:
            c = paises.codigo(pais)
            if not c:
                return []
            cond.append("pais=?")
            args.append(c)
        if horas:
            cond.append("ts>=?")
            args.append(time.time() - float(horas) * 3600)
        for palabra in _plano(texto).split()[:6]:
            cond.append("(lower(titulo) LIKE ? OR lower(detalle) LIKE ?)")
            args += [f"%{palabra}%", f"%{palabra}%"]
        sql = "SELECT * FROM registros" + (" WHERE " + " AND ".join(cond) if cond else "")
        sql += " ORDER BY " + ("valor DESC, ts DESC" if orden == "valor" else "ts DESC") + " LIMIT ?"
        filas = self._q(sql, (*args, max(1, min(int(limite or 20), 500))))
        if texto and not filas and any(ord(c) > 127 for c in texto):   # titulos con tildes: comparar sin ellas
            todas = self._q("SELECT * FROM registros ORDER BY ts DESC LIMIT 4000")
            claves = _plano(texto).split()[:6]
            filas = [r for r in todas if all(k in _plano(r["titulo"] + " " + r["detalle"]) for k in claves)][:limite]
        return [self._publico(r, corto) for r in filas]

    @staticmethod
    def categoria(texto: str) -> str:
        t = _plano(texto).strip()
        if t in CATEGORIAS:
            return t
        for cid, nombre in CATEGORIAS.items():
            if t and (t in _plano(nombre) or _plano(nombre).startswith(t)):
                return cid
        return t

    def resumen(self, pais: str = "", categoria: str = "") -> dict:
        """Cuanto hay de cada categoria y de cada pais, y lo mas destacado: la vista «por categorias y paises»."""
        cond, args = [], []
        if pais:
            c = paises.codigo(pais)
            cond.append("pais=?")
            args.append(c or "??")
        if categoria:
            cond.append("categoria=?")
            args.append(self.categoria(categoria))
        donde = (" WHERE " + " AND ".join(cond)) if cond else ""
        por_cat = self._q(f"SELECT categoria, COUNT(*) n, MAX(recibido) t FROM registros{donde} GROUP BY categoria "
                          "ORDER BY n DESC", args)
        por_pais = self._q(f"SELECT pais, COUNT(*) n FROM registros{donde} GROUP BY pais ORDER BY n DESC LIMIT 60", args)
        cruce = self._q(f"SELECT categoria, pais, COUNT(*) n FROM registros{donde} GROUP BY categoria, pais", args)
        tabla: dict[str, dict[str, int]] = {}
        for r in cruce:
            tabla.setdefault(r["categoria"], {})[r["pais"]] = r["n"]
        destacados = {}
        for r in por_cat:
            filas = self._q(f"SELECT * FROM registros{donde}{' AND' if donde else ' WHERE'} categoria=? "
                            "ORDER BY (valor IS NULL), ts DESC LIMIT 60", (*args, r["categoria"]))
            filas.sort(key=lambda x: (-(x["valor"] or 0) if r["categoria"] in ("sucesos", "movimiento") else 0,
                                      -(x["ts"] or 0)))
            destacados[r["categoria"]] = [self._publico(x, corto=True) for x in filas[:4]]
        return {"categorias": [{"id": r["categoria"], "nombre": CATEGORIAS.get(r["categoria"], r["categoria"]),
                                "registros": r["n"],
                                "actualizado": time.strftime("%Y-%m-%d %H:%M", time.gmtime(r["t"])) + " UTC"}
                               for r in por_cat],
                "paises": [{"pais": r["pais"], "nombre": paises.nombre(r["pais"]), "registros": r["n"]}
                           for r in por_pais],
                "por_categoria_y_pais": tabla, "destacados": destacados,
                "total": sum(r["n"] for r in por_cat)}

    def estado(self) -> dict:
        filas = []
        ahora = time.time()
        for f in self.fuentes.values():
            a, e = self.ajustes(f), self._fila_estado(f.id)
            filas.append({**f.publico(), **a, "ultimo": e.get("ultimo"), "ultimo_ok": e.get("ultimo_ok"),
                          "registros": e.get("registros") or 0, "error": e.get("error") or "",
                          "avisos": e.get("avisos") or "", "ms": e.get("ms") or 0,
                          "hace_s": round(ahora - e["ultimo_ok"]) if e.get("ultimo_ok") else None,
                          "en_curso": f.id in self._en_curso,
                          "categoria_nombre": CATEGORIAS.get(f.categoria, f.categoria)})
        claves = {k: ("••••" + str(v)[-3:] if v else "") for k, v in (self.cfg("claves", {}) or {}).items()}
        for f in FUENTES:
            if f.clave:
                claves.setdefault(f.clave, "")
        return {"activo": self.activo(), "factor": float(self.cfg("factor", 1.0) or 1.0),
                "paises": self.paises_interes(), "retencion_h": int(self.cfg("retencion_h", 48)),
                "contexto": bool(self.cfg("contexto", True)), "claves": claves, "fuentes": filas,
                "categorias": CATEGORIAS, "sin_red": ahora < self._sin_red_hasta,
                "total": self._q("SELECT COUNT(*) n FROM registros")[0]["n"]}

    # ------------------------------------------------------------ para los agentes
    def contexto_para(self, texto: str, tope: int = 900) -> str:
        """Resumen breve para el prompt cuando la pregunta nombra un pais o un tema que las fuentes cubren."""
        if not self.activo() or not self.cfg("contexto", True):
            return ""
        plano = _plano(texto)
        temas = [c for c, palabras in _TEMAS.items() if any(p in plano for p in palabras)]
        if not temas:
            return ""
        nombrados = paises.en_texto(texto)[:2]
        lineas = []
        for cat in temas[:2]:
            for pais in (nombrados or [""]):
                filas = self.consultar(categoria=cat, pais=pais, limite=4,
                                       orden="valor" if cat in ("sucesos", "movimiento") else "reciente", corto=True)
                for r in filas:
                    lineas.append(f"- [{CATEGORIAS.get(cat, cat).split(' (')[0]}"
                                  + (f" · {r['pais_nombre']}" if r.get("pais_nombre") else "") + f"] {r['titulo']}"
                                  + (f" ({r['fecha']})" if r.get("fecha") else ""))
        if not lineas:
            return ""
        out = ("Información del mundo recibida de fuentes públicas (para más, usa mundo_consultar o mundo_resumen):\n"
               + "\n".join(lineas))
        return out[:tope]
