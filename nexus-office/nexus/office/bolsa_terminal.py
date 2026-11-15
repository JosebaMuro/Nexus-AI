"""Terminal de bolsa: de una pregunta en lenguaje corriente a las recetas de datos que la responden.

La ruta es determinista (palabras clave y simbolos), sin modelo: asi funciona igual con un modelo pequeno, sin
modelo y desde la interfaz. Cada marco lleva su fuente y la hora en que se recibio, y las cifras de una respuesta
se pueden comprobar contra los marcos de la sesion (verificar). Idea de Open Terminal de spovalova (MIT): recetas
con nombre, escritorio de paneles y numeros siempre trazables. No se ha copiado codigo.
"""
from __future__ import annotations

import json
import re
import threading
import time
import unicodedata

from . import mercados

# nombre corriente -> simbolo
CONOCIDOS = {"ibex": "^IBEX", "ibex 35": "^IBEX", "s&p": "^GSPC", "s&p 500": "^GSPC", "sp500": "^GSPC",
             "nasdaq": "^NDX", "dow": "^DJI", "dow jones": "^DJI", "dax": "^GDAXI", "cac": "^FCHI", "ftse": "^FTSE",
             "nikkei": "^N225", "eurostoxx": "^STOXX50E", "euro stoxx": "^STOXX50E", "vix": "^VIX",
             "bitcoin": "BTC-USD", "ether": "ETH-USD", "ethereum": "ETH-USD", "oro": "GC=F", "petroleo": "BZ=F",
             "brent": "BZ=F", "euro": "EURUSD=X", "eurodolar": "EURUSD=X"}
# (receta, palabras) por orden de preferencia
_RUTAS = [
    ("opciones", ("opciones", "opcion ", "call", "put", "griegas", "volatilidad implicita", "strike")),
    ("historial_bpa", ("sorpresa", "beneficio por accion", "bpa", "batio", "resultados de")),
    ("resultados", ("publican resultados", "resultados de hoy", "resultados del dia", "presentan resultados")),
    ("dividendos", ("dividendo",)),
    ("internos", ("directivo", "insider", "consejero", "internos")),
    ("instituciones", ("institucion", "accionistas", "fondos que", "quien tiene")),
    ("cortos", ("cortos", "en corto", "short", "bajistas")),
    ("estimaciones", ("estimacion", "consenso", "precio objetivo", "analistas", "prevision")),
    ("financieros", ("balance", "cuenta de resultados", "flujo de caja", "flujos de caja", "estados financieros",
                     "ingresos")),
    ("sectores", ("sector",)),
    ("cripto", ("cripto", "criptomoneda", "dominancia", "altcoin")),
    ("macro", ("curva de tipos", "tipos de interes", "bono", "tesoro", "macro", "diferencial")),
    ("calendario", ("calendario", "ipc", "pib", "empleo", "banco central", "bce", "fed ", "datos de la semana")),
    ("salidas_bolsa", ("salida a bolsa", "salidas a bolsa", "opv", "ipo", "debut")),
    ("movimientos", ("mas suben", "mas bajan", "mas negociad", "ganadores", "perdedores", "movimientos")),
    ("criba", ("criba", "filtra", "busca valores", "empresas con", "valores con", "acciones con", "screener")),
    ("indicadores", ("indicador", "rsi", "macd", "media movil", "tecnico", "bollinger", "sobrecompra", "sobreventa",
                     "atr")),
    ("velas", ("grafico", "historico", "evolucion", "velas", "ultimo ano", "ultimos meses")),
    ("metricas", ("metrica", "valoracion", "per ", "ratio", "fundamental", "capitalizacion", "margen", "roe", "beta")),
    ("perfil", ("perfil", "ficha", "a que se dedica", "que es ", "que hace")),
    ("noticias", ("noticia", "titular", "que ha pasado", "que pasa con", "novedades")),
    ("cotizacion", ("cotiza", "precio", "cuanto vale", "a cuanto", "como va")),
]
_NO_SIMBOLO = {"PER", "BPA", "RSI", "MACD", "ATR", "IPC", "PIB", "BCE", "FED", "OPV", "IPO", "ROE", "USD", "EUR",
               "ETF", "DCF", "VAR", "CEO", "IA", "EE", "UU", "VIX"}
_mu = threading.Lock()
_sesiones: dict[str, list[dict]] = {}


def _plano(s: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFD", s.lower()) if unicodedata.category(c) != "Mn")


def simbolos(pregunta: str) -> list[str]:
    """Simbolos nombrados en la pregunta: en mayusculas (AAPL, SAN.MC, ^IBEX, BTC-USD) o por su nombre corriente."""
    out: list[str] = []
    plano = _plano(pregunta)
    for nombre, sym in sorted(CONOCIDOS.items(), key=lambda kv: -len(kv[0])):
        if re.search(rf"(?<![a-z0-9]){re.escape(nombre)}(?![a-z0-9])", plano) and sym not in out:
            out.append(sym)
    for m in re.finditer(r"(?<![A-Za-z0-9^])(\^?[A-Z][A-Z0-9]{0,5}(?:[.\-=][A-Z]{1,3})?)(?![A-Za-z0-9])", pregunta):
        s = m.group(1)
        if s in _NO_SIMBOLO or (len(s) == 1 and s not in ("F", "T", "V", "C")):
            continue
        if s not in out:
            out.append(s)
    return out[:8]


def ruta(pregunta: str) -> list[dict]:
    """Recetas que responden a la pregunta: [{receta, params, motivo}]."""
    plano = " " + _plano(pregunta) + " "
    syms = simbolos(pregunta)
    elegidas: list[dict] = []
    for receta, palabras in _RUTAS:
        hit = next((w for w in palabras if re.search(r"(?<![a-z0-9])" + re.escape(w), plano)), None)
        if not hit:
            continue
        params: dict = {}
        info = mercados.RECETAS[receta][1]
        if "simbolo" in info or "simbolos" in info:
            if not syms and receta not in ("noticias",):
                continue
            params["simbolos" if "simbolos" in info else "simbolo"] = syms if "simbolos" in info else (syms[0] if syms else "")
        if receta == "criba":
            params["filtros"] = mercados.leer_filtros(pregunta)
            pais = _pais(pregunta)
            if pais:
                params["mercado"] = pais
        if receta in ("sectores",) and _pais(pregunta):
            params["mercado"] = _pais(pregunta)
        if receta == "calendario" and _pais(pregunta):
            params["pais"] = _pais(pregunta)
        if receta == "velas":
            params["rango"] = "5y" if "5 anos" in plano else "1y" if "ano" in plano else "6mo"
        elegidas.append({"receta": receta, "params": params, "motivo": f"la pregunta habla de «{hit.strip()}»"})
        if len(elegidas) >= 4:
            break
    if not elegidas:
        if syms:
            elegidas = [{"receta": "cotizacion", "params": {"simbolos": syms}, "motivo": "símbolos sin más detalle"},
                        {"receta": "noticias", "params": {"simbolo": syms[0]}, "motivo": "contexto reciente"}]
        else:
            elegidas = [{"receta": "buscar", "params": {"texto": pregunta.strip()[:60]},
                         "motivo": "no hay símbolo: se busca por nombre"}]
    return elegidas


def _pais(pregunta: str) -> str:
    from ..mundo import paises
    for cod in paises.en_texto(pregunta):
        if cod in mercados.MERCADOS_TV:
            return cod
    return ""


def panel(pregunta: str, sesion: str = "", m=None) -> dict:
    """Ejecuta la ruta de una pregunta y devuelve sus marcos (los que fallan quedan anotados, no rompen el panel)."""
    marcos, errores = [], []
    for paso in ruta(pregunta):
        try:
            marco = mercados.receta(paso["receta"], paso["params"], m)
            marco["motivo"] = paso["motivo"]
            marco["params"] = paso["params"]
            marcos.append(marco)
        except Exception as e:  # noqa: BLE001
            errores.append({"receta": paso["receta"], "error": f"{type(e).__name__}: {e}"[:200]})
    recordar(sesion, marcos)
    return {"pregunta": pregunta, "marcos": marcos, "errores": errores,
            "aviso": "Datos públicos con retraso posible; cada marco indica su fuente y su hora."}


def recordar(sesion: str, marcos: list[dict]) -> None:
    """Guarda los marcos consultados en la sesion para poder comprobar despues las cifras de la respuesta."""
    if not sesion or not marcos:
        return
    with _mu:
        lista = _sesiones.setdefault(sesion, [])
        lista.extend(marcos)
        del lista[:-12]
        if len(_sesiones) > 200:
            for k in list(_sesiones)[:50]:
                _sesiones.pop(k, None)


def verificar(texto: str, sesion: str = "", datos=None) -> dict:
    """Cifras de un texto que no salen de los datos consultados en la sesion (o de los datos que se pasen)."""
    with _mu:
        base = list(_sesiones.get(sesion, [])) if sesion else []
    if datos is not None:
        base.append(datos)
    if not base:
        return {"cifras": 0, "respaldadas": 0, "sin_respaldo": [], "nota": "No hay datos consultados en esta sesión"}
    r = mercados.verificar_cifras(texto, base)
    r["veredicto"] = ("todas las cifras salen de los datos consultados" if not r["sin_respaldo"] else
                      "hay cifras que no aparecen en los datos consultados: revísalas o consúltalas antes de darlas")
    return r


# ------------------------------------------------------------------ escritorio de paneles
def _ruta_escritorio():
    from .. import paths
    return paths.sub("bolsa") / "escritorio.json"


FABRICA = [{"id": 1, "titulo": "Índices", "receta": "cotizacion",
            "params": {"simbolos": ["^GSPC", "^NDX", "^IBEX", "^STOXX50E", "^VIX"]}},
           {"id": 2, "titulo": "Sectores", "receta": "sectores", "params": {"mercado": "america"}},
           {"id": 3, "titulo": "Calendario económico", "receta": "calendario", "params": {"importancia": "alta"}},
           {"id": 4, "titulo": "Noticias del mercado", "receta": "noticias", "params": {}}]


def escritorio() -> list[dict]:
    try:
        return json.loads(_ruta_escritorio().read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return [dict(w) for w in FABRICA]


def escritorio_guardar(paneles: list[dict]) -> list[dict]:
    limpios = []
    for i, w in enumerate(paneles[:24]):
        if w.get("receta") not in mercados.RECETAS:
            raise ValueError(f"receta desconocida: {w.get('receta')}")
        limpios.append({"id": i + 1, "titulo": str(w.get("titulo") or mercados.RECETAS[w["receta"]][0])[:80],
                        "receta": w["receta"], "params": dict(w.get("params") or {})})
    with _mu:
        p = _ruta_escritorio()
        p.write_text(json.dumps(limpios, ensure_ascii=False, indent=1), encoding="utf-8")
    return limpios


def escritorio_anadir(receta: str, params: dict | None = None, titulo: str = "") -> list[dict]:
    return escritorio_guardar(escritorio() + [{"receta": receta, "params": params or {}, "titulo": titulo}])


def escritorio_datos(m=None, sesion: str = "") -> list[dict]:
    """Cada panel del escritorio con sus datos al dia (en paralelo; el que falla lleva su error)."""
    from concurrent.futures import ThreadPoolExecutor
    paneles = escritorio()

    def uno(w):
        t0 = time.time()
        try:
            return {**w, "marco": mercados.receta(w["receta"], w["params"], m), "ms": round((time.time() - t0) * 1000)}
        except Exception as e:  # noqa: BLE001
            return {**w, "error": f"{type(e).__name__}: {e}"[:200]}
    with ThreadPoolExecutor(max_workers=4) as ex:
        out = list(ex.map(uno, paneles))
    recordar(sesion, [w["marco"] for w in out if w.get("marco")])
    return out
