"""Informes oficiales de empresas cotizadas (SEC EDGAR): lista de presentaciones, texto, cifras y lectura.

La lectura es lexica y local: reparte las frases del informe en impulsores y frenos (demanda, margenes, deuda,
regulacion...) y devuelve citas LITERALES. Cada cita se comprueba contra el texto original antes de darla; la que
no aparece tal cual, se descarta. La idea de las citas verificadas y de los impulsores viene de OpenTerminalUI
(MIT); no se ha copiado codigo.

EDGAR pide que el cliente se identifique: se usa un nombre generico configurable en bolsa.sec_agente, nunca datos
del usuario.
"""
from __future__ import annotations

import html
import re
import threading
import time

AGENTE = "AgenteIA/4.2 (consulta personal de informes; contacto en la configuracion)"
_mu = threading.Lock()
_cache: dict[str, tuple[float, object]] = {}

# impulsor -> (nombre, palabras clave en ingles, que es la lengua de los informes)
IMPULSORES = {
    "demanda": ("Demanda y ventas", ("demand", "sales volume", "net sales", "revenue growth", "unit sales", "orders",
                                     "bookings", "backlog", "customer adoption")),
    "precios": ("Precios", ("pricing", "price increase", "price reduction", "average selling price", "discounting",
                            "promotional")),
    "costes": ("Costes", ("cost of sales", "input costs", "inflation", "raw material", "freight", "component costs",
                          "operating expenses", "cost reduction", "restructuring")),
    "margenes": ("Márgenes", ("gross margin", "operating margin", "margin expansion", "margin compression",
                              "profitability")),
    "competencia": ("Competencia", ("competition", "competitor", "competitive", "market share", "new entrants")),
    "regulacion": ("Regulación", ("regulation", "regulatory", "antitrust", "compliance", "legislation", "tariff",
                                  "export control", "data protection", "privacy law")),
    "litigios": ("Litigios", ("litigation", "lawsuit", "legal proceeding", "settlement", "class action", "subpoena",
                              "investigation by")),
    "divisa": ("Divisa", ("foreign currency", "exchange rate", "currency fluctuation", "stronger u.s. dollar",
                          "weaker u.s. dollar", "hedging")),
    "tipos_interes": ("Tipos de interés", ("interest rate", "interest expense", "borrowing cost", "monetary policy")),
    "deuda_liquidez": ("Deuda y liquidez", ("indebtedness", "liquidity", "credit facility", "debt maturities",
                                            "covenant", "refinanc", "cash and cash equivalents", "free cash flow")),
    "cadena_suministro": ("Cadena de suministro", ("supply chain", "supplier", "shortage", "single-source",
                                                   "manufacturing capacity", "logistics", "inventory")),
    "mano_obra": ("Personal", ("labor", "workforce", "headcount", "talent", "employees", "layoff", "union", "wage")),
    "tecnologia_ia": ("Tecnología e IA", ("artificial intelligence", "machine learning", "generative ai", "cloud",
                                          "data center", "semiconductor", "research and development", "innovation")),
    "ciberseguridad": ("Ciberseguridad", ("cybersecurity", "cyberattack", "data breach", "ransomware",
                                          "security incident")),
    "clima_energia": ("Clima y energía", ("climate", "emissions", "energy costs", "renewable", "natural disaster",
                                          "extreme weather")),
    "geopolitica": ("Geopolítica", ("geopolitical", "sanctions", "war", "conflict", "china", "taiwan", "trade tension",
                                    "political instability")),
    "adquisiciones": ("Adquisiciones", ("acquisition", "merger", "divestiture", "integration of", "goodwill impairment")),
    "accionista": ("Retribución al accionista", ("share repurchase", "buyback", "dividend", "capital return")),
    "inversion": ("Inversión", ("capital expenditure", "capex", "investment in", "capacity expansion", "new facilities")),
    "clientes": ("Concentración de clientes", ("significant customer", "customer concentration", "largest customers",
                                               "a small number of customers")),
    "perspectivas": ("Perspectivas", ("we expect", "outlook", "guidance", "we anticipate", "we believe that",
                                      "going forward", "fiscal year 20")),
}
_POSITIVAS = ("increase", "increased", "growth", "grew", "higher", "improve", "improved", "strong", "record", "expand",
              "expansion", "benefit", "favorable", "gain", "accelerat", "exceed", "robust")
_NEGATIVAS = ("decrease", "decreased", "decline", "declined", "lower", "adverse", "adversely", "weak", "loss",
              "impairment", "risk", "uncertain", "unfavorable", "harm", "shortage", "delay", "disrupt", "pressure",
              "could reduce", "negatively")

# conceptos XBRL habituales -> nombre en espanol (se prueba cada alternativa en orden)
CONCEPTOS = {
    "ingresos": ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax", "SalesRevenueNet"),
    "beneficio_bruto": ("GrossProfit",),
    "resultado_explotacion": ("OperatingIncomeLoss",),
    "beneficio_neto": ("NetIncomeLoss",),
    "bpa_diluido": ("EarningsPerShareDiluted",),
    "flujo_operativo": ("NetCashProvidedByUsedInOperatingActivities",),
    "inversion_capital": ("PaymentsToAcquirePropertyPlantAndEquipment",),
    "efectivo": ("CashAndCashEquivalentsAtCarryingValue",),
    "deuda_largo_plazo": ("LongTermDebtNoncurrent", "LongTermDebt"),
    "activos": ("Assets",),
    "pasivos": ("Liabilities",),
    "patrimonio": ("StockholdersEquity",),
    "acciones": ("CommonStockSharesOutstanding", "EntityCommonStockSharesOutstanding"),
    "investigacion_desarrollo": ("ResearchAndDevelopmentExpense",),
}


def _cliente(cliente=None):
    if cliente is not None:
        return cliente
    import httpx
    from ..config import get_config
    agente = str(get_config().get("bolsa.sec_agente", "") or AGENTE)
    return httpx.Client(timeout=25, follow_redirects=True, headers={"User-Agent": agente, "Accept-Encoding": "gzip"})


def _pedir(url: str, ttl: float, cliente=None, como_json: bool = True):
    ahora = time.time()
    with _mu:
        previo = _cache.get(url)
    if previo and ahora - previo[0] < ttl:
        return previo[1]
    propio = cliente is None
    c = _cliente(cliente)
    try:
        r = c.get(url)
        if r.status_code >= 400:
            raise RuntimeError(f"EDGAR respondió {r.status_code}")
        valor = r.json() if como_json else r.text
    finally:
        if propio:
            c.close()
    with _mu:
        _cache[url] = (ahora, valor)
        if len(_cache) > 60:
            for k in sorted(_cache, key=lambda k: _cache[k][0])[:20]:
                _cache.pop(k, None)
    return valor


def cik(simbolo: str, cliente=None) -> dict:
    """Numero CIK y nombre de una empresa a partir de su simbolo en Estados Unidos."""
    sym = simbolo.strip().upper().replace(".", "-")
    datos = _pedir("https://www.sec.gov/files/company_tickers.json", 86400, cliente)
    for v in (datos.values() if isinstance(datos, dict) else datos):
        if str(v.get("ticker", "")).upper() == sym:
            return {"cik": int(v["cik_str"]), "simbolo": sym, "nombre": v.get("title", sym)}
    raise ValueError(f"EDGAR no conoce el símbolo {sym} (solo hay empresas que presentan informes en Estados Unidos)")


def presentaciones(simbolo: str, tipos: list[str] | str | None = None, limite: int = 12, cliente=None) -> dict:
    """Ultimas presentaciones de una empresa (10-K anual, 10-Q trimestral, 8-K hecho relevante, 20-F, 6-K...)."""
    e = cik(simbolo, cliente)
    d = _pedir(f"https://data.sec.gov/submissions/CIK{e['cik']:010d}.json", 1800, cliente)
    rec = (d.get("filings") or {}).get("recent") or {}
    if isinstance(tipos, str):
        tipos = [t.strip() for t in re.split(r"[,; ]+", tipos) if t.strip()]
    quiero = {t.upper() for t in (tipos or [])}
    filas = []
    for i, forma in enumerate(rec.get("form") or []):
        if quiero and forma.upper() not in quiero:
            continue
        acc = rec["accessionNumber"][i]
        doc = (rec.get("primaryDocument") or [""] * (i + 1))[i]
        filas.append({"tipo": forma, "fecha": rec["filingDate"][i], "periodo": (rec.get("reportDate") or [""] * (i + 1))[i],
                      "descripcion": (rec.get("primaryDocDescription") or [""] * (i + 1))[i], "acceso": acc,
                      "url": f"https://www.sec.gov/Archives/edgar/data/{e['cik']}/{acc.replace('-', '')}/{doc}"})
        if len(filas) >= limite:
            break
    return {**e, "sector": d.get("sicDescription") or "", "ejercicio_acaba": d.get("fiscalYearEnd") or "",
            "presentaciones": filas, "fuente": "SEC EDGAR"}


def limpiar(doc: str) -> str:
    """Texto plano de un informe en HTML (sin tablas de estilo, guiones ni etiquetas XBRL)."""
    doc = re.sub(r"(?is)<(script|style|ix:header|head)\b.*?</\1>", " ", doc)
    doc = re.sub(r"(?i)<(br|/p|/div|/tr|/li|/h[1-6])\s*/?>", "\n", doc)
    doc = html.unescape(re.sub(r"<[^>]+>", " ", doc)).replace("\xa0", " ")
    doc = re.sub(r"[ \t\r\f]+", " ", doc)
    return re.sub(r"\n\s*\n+", "\n", doc).strip()


_APARTADOS = (("negocio", r"item\s*1\.?\s*business"), ("riesgos", r"item\s*1a\.?\s*risk\s*factors"),
              ("analisis_direccion", r"item\s*[27]\.?\s*management.s\s*discussion"),
              ("riesgo_mercado", r"item\s*(?:7a|3)\.?\s*quantitative\s*and\s*qualitative"),
              ("litigios", r"item\s*(?:3|1)\.?\s*legal\s*proceedings"))


def apartados(texto: str) -> dict[str, str]:
    """Trozos del informe por apartado. Se queda con la aparicion mas larga de cada uno (el indice las repite)."""
    marcas = []
    for nombre, patron in _APARTADOS:
        for m in re.finditer(patron, texto, re.I):
            marcas.append((m.start(), nombre))
    cortes = sorted({m.start() for m in re.finditer(r"(?im)^\s*item\s*\d+[ab]?\.", texto)} | {p for p, _ in marcas})
    out: dict[str, str] = {}
    for pos, nombre in marcas:
        fin = next((c for c in cortes if c > pos + 40), len(texto))
        trozo = texto[pos:fin].strip()
        if len(trozo) > len(out.get(nombre, "")):
            out[nombre] = trozo
    return out


def frases(texto: str) -> list[str]:
    plano = re.sub(r"\s+", " ", texto)
    return [f.strip() for f in re.split(r"(?<=[.!?])\s+(?=[A-Z(\"“])", plano) if 60 <= len(f.strip()) <= 600]


def _normal(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip().lower()


def verificar_citas(texto: str, citas: list[str]) -> list[dict]:
    """Comprueba que cada cita esta literal en el texto (salvo espacios y mayusculas)."""
    base = _normal(texto)
    return [{"cita": c, "verificada": bool(c.strip()) and _normal(c) in base} for c in citas]


def impulsores(texto: str, por_impulsor: int = 2, tope: int = 16) -> list[dict]:
    """Impulsores y frenos del informe con sus citas literales verificadas."""
    grupos: dict[str, list[tuple[float, str, str]]] = {}
    for f in frases(texto):
        bajo = f.lower()
        pos = sum(bajo.count(w) for w in _POSITIVAS)
        neg = sum(bajo.count(w) for w in _NEGATIVAS)
        for clave, (_, palabras) in IMPULSORES.items():
            aciertos = sum(1 for w in palabras if w in bajo)
            if not aciertos:
                continue
            cifras = len(re.findall(r"\d+(?:\.\d+)?\s?%|\$\s?\d", f))
            peso = aciertos * 2 + cifras * 1.5 + min(pos + neg, 4) * 0.5 - (0.6 if bajo.startswith(("we may", "if we")) else 0)
            sentido = "impulso" if pos > neg else "freno" if neg > pos else "neutro"
            grupos.setdefault(clave, []).append((peso, sentido, f))
    base = _normal(texto)
    out = []
    for clave, lista in grupos.items():
        lista.sort(key=lambda x: -x[0])
        citas, vistos = [], set()
        for peso, sentido, f in lista:
            if _normal(f) not in base or f[:60] in vistos:    # la cita tiene que estar literal en el informe
                continue
            vistos.add(f[:60])
            citas.append({"cita": f, "sentido": sentido, "peso": round(peso, 1), "verificada": True})
            if len(citas) >= por_impulsor:
                break
        if citas:
            a_favor = sum(1 for _, s, _ in lista if s == "impulso")
            en_contra = sum(1 for _, s, _ in lista if s == "freno")
            out.append({"impulsor": clave, "nombre": IMPULSORES[clave][0], "menciones": len(lista),
                        "a_favor": a_favor, "en_contra": en_contra,
                        "balance": "impulso" if a_favor > en_contra * 1.3 else "freno" if en_contra > a_favor * 1.3 else "mixto",
                        "citas": citas})
    out.sort(key=lambda x: -x["menciones"])
    return out[:tope]


def novedades(texto_nuevo: str, texto_anterior: str, tope: int = 12) -> list[str]:
    """Frases del informe nuevo que no estaban en el anterior (riesgos o comentarios recien anadidos)."""
    antes = {_normal(f)[:160] for f in frases(texto_anterior)}
    nuevas = [f for f in frases(texto_nuevo) if _normal(f)[:160] not in antes]
    nuevas.sort(key=lambda f: -sum(1 for _, (_, pal) in IMPULSORES.items() for w in pal if w in f.lower()))
    return nuevas[:tope]


def leer_informe(simbolo: str, tipo: str = "10-K", indice: int = 0, cliente=None, comparar: bool = False) -> dict:
    """Descarga un informe y devuelve sus apartados resumidos por impulsores con citas verificadas."""
    p = presentaciones(simbolo, [tipo], max(2, indice + 2), cliente)
    if len(p["presentaciones"]) <= indice:
        raise ValueError(f"{p['nombre']} no tiene un informe {tipo} en esa posición")
    pres = p["presentaciones"][indice]
    texto = limpiar(_pedir(pres["url"], 86400, cliente, como_json=False))
    ap = apartados(texto)
    cuerpo = "\n".join(ap.get(k, "") for k in ("analisis_direccion", "riesgos", "negocio")) or texto
    out = {"simbolo": p["simbolo"], "nombre": p["nombre"], "informe": pres, "caracteres": len(texto),
           "apartados": {k: len(v) for k, v in ap.items()}, "impulsores": impulsores(cuerpo),
           "fuente": "SEC EDGAR", "nota": "Las citas son literales del informe (en inglés) y se han comprobado contra "
           "el texto original. La clasificación en impulsores es léxica, no una opinión."}
    if comparar and len(p["presentaciones"]) > indice + 1:
        anterior = limpiar(_pedir(p["presentaciones"][indice + 1]["url"], 86400, cliente, como_json=False))
        ap2 = apartados(anterior)
        out["riesgos_nuevos"] = novedades(ap.get("riesgos", ""), ap2.get("riesgos", "")) if ap.get("riesgos") else []
        out["comparado_con"] = p["presentaciones"][indice + 1]
    return out


def texto_informe(simbolo: str, tipo: str = "10-K", apartado: str = "analisis_direccion", indice: int = 0,
                  desde: int = 0, largo: int = 6000, cliente=None) -> dict:
    """Trozo literal de un apartado del informe, para leerlo o citarlo."""
    p = presentaciones(simbolo, [tipo], indice + 1, cliente)
    if len(p["presentaciones"]) <= indice:
        raise ValueError(f"{p['nombre']} no tiene un informe {tipo} en esa posición")
    pres = p["presentaciones"][indice]
    texto = limpiar(_pedir(pres["url"], 86400, cliente, como_json=False))
    ap = apartados(texto)
    cuerpo = ap.get(apartado) or texto
    return {"simbolo": p["simbolo"], "informe": pres, "apartado": apartado if apartado in ap else "completo",
            "apartados_disponibles": list(ap), "total": len(cuerpo), "desde": desde,
            "texto": cuerpo[desde:desde + max(500, min(int(largo), 20000))], "fuente": "SEC EDGAR"}


def cifras(simbolo: str, anuales: bool = True, limite: int = 6, cliente=None) -> dict:
    """Cifras contables publicadas (XBRL) por ejercicio o trimestre, con el informe del que sale cada una."""
    e = cik(simbolo, cliente)
    d = _pedir(f"https://data.sec.gov/api/xbrl/companyfacts/CIK{e['cik']:010d}.json", 21600, cliente)
    hechos = (d.get("facts") or {})
    gaap = {**(hechos.get("dei") or {}), **(hechos.get("ifrs-full") or {}), **(hechos.get("us-gaap") or {})}
    series: dict[str, dict[str, dict]] = {}
    for nombre, alternativas in CONCEPTOS.items():
        for concepto in alternativas:
            unidades = (gaap.get(concepto) or {}).get("units") or {}
            datos = next((v for k, v in unidades.items() if k in ("USD", "USD/shares", "shares", "EUR")), None)
            if not datos:
                continue
            for x in datos:
                forma = str(x.get("form", ""))
                es_anual = forma.startswith(("10-K", "20-F", "40-F"))
                if es_anual != anuales or x.get("val") is None:
                    continue
                if anuales and x.get("start") and x.get("end"):   # un ejercicio entero, no un trimestre suelto
                    dias = (time.mktime(time.strptime(x["end"], "%Y-%m-%d")) -
                            time.mktime(time.strptime(x["start"], "%Y-%m-%d"))) / 86400
                    if dias < 300:
                        continue
                if not anuales and x.get("start") and x.get("end"):
                    dias = (time.mktime(time.strptime(x["end"], "%Y-%m-%d")) -
                            time.mktime(time.strptime(x["start"], "%Y-%m-%d"))) / 86400
                    if dias > 100:
                        continue
                previo = series.setdefault(nombre, {}).get(x["end"])
                if previo is None or str(x.get("filed", "")) > previo["presentado"]:
                    series[nombre][x["end"]] = {"valor": x["val"], "presentado": str(x.get("filed", "")),
                                                "informe": forma, "acceso": x.get("accn", "")}
            if series.get(nombre):
                break
    fechas = sorted({f for s in series.values() for f in s}, reverse=True)[:limite]
    filas = []
    for f in fechas:
        fila = {"periodo": f}
        for nombre in CONCEPTOS:
            if f in series.get(nombre, {}):
                fila[nombre] = series[nombre][f]["valor"]
        if fila.get("ingresos") and fila.get("beneficio_neto") is not None:
            fila["margen_neto_pct"] = round(fila["beneficio_neto"] / fila["ingresos"] * 100, 2)
        if fila.get("flujo_operativo") is not None and fila.get("inversion_capital") is not None:
            fila["flujo_caja_libre"] = fila["flujo_operativo"] - fila["inversion_capital"]
        filas.append(fila)
    for nueva, vieja in zip(filas, filas[1:]):
        if nueva.get("ingresos") and vieja.get("ingresos"):
            nueva["crecimiento_ingresos_pct"] = round((nueva["ingresos"] / vieja["ingresos"] - 1) * 100, 2)
    if not filas:
        raise ValueError(f"EDGAR no tiene cifras XBRL {'anuales' if anuales else 'trimestrales'} de {e['simbolo']}")
    return {**e, "periodicidad": "anual" if anuales else "trimestral", "filas": filas, "fuente": "SEC EDGAR (XBRL)",
            "nota": "Cifras tal como las presentó la empresa, en su divisa de presentación."}
