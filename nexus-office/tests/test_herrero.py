"""Oficina Herrero: perfiles, comprobaciones del Eurocodigo 3, tipologias, despiece, planos, imagen, API y agente."""
import xml.etree.ElementTree as ET

import pytest
from fastapi.testclient import TestClient

from nexus.office import herrero
from nexus.office import herrero_calculo as C
from nexus.office import herrero_dibujo as D
from nexus.office import herrero_modelo as M


# ------------------------------------------------------------------ secciones frente a las tablas de los fabricantes
@pytest.mark.parametrize("nombre,area_cm2,iy_cm4,wply_cm3,kg_m", [
    ("IPE 200", 28.5, 1943, 221, 22.4), ("IPE 300", 53.8, 8356, 628, 42.2), ("HEB 200", 78.1, 5696, 643, 61.3),
    ("HEA 200", 53.8, 3692, 430, 42.3), ("Tubo 100x100x4", 14.95, 226, 53.3, 11.7), ("UPN 120", 17.0, 364, 72.6, 13.4),
])
def test_propiedades_de_los_perfiles(nombre, area_cm2, iy_cm4, wply_cm3, kg_m):
    p = C.perfil(nombre)
    pub = p.publico()
    assert pub["area_cm2"] == pytest.approx(area_cm2, rel=0.015)
    assert pub["Iy_cm4"] == pytest.approx(iy_cm4, rel=0.015)
    assert pub["Wply_cm3"] == pytest.approx(wply_cm3, rel=0.02)
    assert pub["peso_kg_m"] == pytest.approx(kg_m, rel=0.02)


def test_buscar_perfiles_por_nombre():
    assert C.perfil("ipe200").nombre == "IPE 200"
    assert C.perfil("80x40x3").nombre == "Tubo 80x40x3"
    assert C.perfil("tubo 80x80x3").nombre == "Tubo 80x80x3"
    assert C.perfil("Tubo Ø42,4x2").serie == "CHS"
    with pytest.raises(KeyError):
        C.perfil("IPE 999")


def test_pandeo_curvas_europeas():
    # HEB 200, S275, Lcr = 4 m en el eje debil (curva c): lambda ~ 0,91 y chi ~ 0,60 (EN 1993-1-1 tabla 6.2)
    chi, lam = C.chi(C.perfil("HEB 200"), "S275", 4000, "z")
    assert lam == pytest.approx(0.91, abs=0.01) and chi == pytest.approx(0.594, abs=0.01)
    assert C.chi(C.perfil("HEB 200"), "S275", 500, "z")[0] == 1.0     # corto: sin reduccion


def test_viga_biapoyada_a_mano():
    # IPE 200 S275, 5 m, q = 10 kN/m ELU (sin peso propio): M = 31,25 kNm; Mpl,Rd = 221 * 275 / 1,05 = 57,8 kNm
    r = C.comprobar_viga(C.perfil("IPE 200"), "S275", 5000, 10.0, 7.0, peso_propio=False)
    assert r["M_kNm"] == pytest.approx(31.25, abs=0.01)
    assert r["aprovechamiento"]["flexion"] == pytest.approx(31.25 / 57.78, rel=0.01)
    assert r["flecha_mm"] == pytest.approx(5 * 7 * 5000 ** 4 / (384 * 210000 * 1943e4), rel=0.01)


def test_cargas_y_combinaciones():
    c = C.Cargas(1.0, 2.0, 0.5, 0.0)
    assert c.elu() == pytest.approx(1.35 + 1.5 * 2 + 1.5 * 0.5 * 0.5)
    assert C.Cargas(0.2, 0.4, 0.9).elu() == pytest.approx(1.35 * 0.2 + 1.5 * 0.9)   # G1 no se suma a la nieve
    assert C.nieve(100) < C.nieve(700) < C.nieve(1500)


def test_soldadura_en_angulo():
    s = C.soldadura_angulo(8, 10, "S275", fuerza_kn=50)
    assert 3 <= s["garganta_mm"] <= 0.7 * 8 and s["simbolo_iso2553"].startswith("a")
    assert s["longitud_necesaria_mm"] > 0
    assert "aviso" in C.soldadura_angulo(3, 30)                     # chapa fina con chapa gruesa


# ------------------------------------------------------------------ encargos en lenguaje corriente
@pytest.mark.parametrize("texto,tipo,esperado", [
    ("Porche de 6x4 m y 2,7 m de altura galvanizado", "porche", {"largo": 6, "ancho": 4, "alto": 2.7, "acabado": "galvanizado"}),
    ("nave de 24 x 15 m, 7 m de altura a 900 m de altitud", "nave", {"largo": 24, "ancho": 15, "alto": 7, "altitud": 900}),
    ("altillo de 600x400 cm a 3 m de altura para almacén con tramex", "entreplanta", {"largo": 6, "ancho": 4, "forjado": "tramex", "uso": "almacen"}),
    ("escalera de 3 m de altura y 1 m de ancho en oxirón", "escalera", {"alto": 3, "ancho": 1, "acabado": "oxiron"}),
    ("barandilla de 8 m con 7 m de desnivel", "barandilla", {"largo": 8, "desnivel": 7}),
    ("pasarela de 10 m de largo y 1,5 m de ancho en S355", "pasarela", {"largo": 10, "ancho": 1.5, "acero": "S355"}),
    ("pérgola de 4x3 con lamas", "pergola", {"largo": 4, "ancho": 3, "cubierta": "lamas"}),
])
def test_interpretar_encargos(texto, tipo, esperado):
    t, P, _ = herrero.interpretar(texto)
    assert t == tipo
    for k, v in esperado.items():
        assert P[k] == v, (k, P)


def test_encargo_sin_tipo_avisa():
    t, _, sup = herrero.interpretar("algo de 3x3")
    assert t == "porche" and any("No he reconocido" in s for s in sup)


# ------------------------------------------------------------------ tipologias completas
@pytest.mark.parametrize("tipo", list(M.TIPOS))
def test_todas_las_tipologias_cumplen_con_sus_valores_por_defecto(tipo):
    e = M.construir(tipo, {})
    assert e.barras and e.comprobaciones
    assert all(c["cumple"] for c in e.comprobaciones), e.comprobaciones
    assert all(b.marca for b in e.barras)
    assert any("EN 1090" in n for n in e.normas)


def test_barandilla_cumple_el_cte():
    e = M.construir("barandilla", {"largo": 5, "desnivel": 7})
    assert e.cotas["alto"] == 1100                                  # mas de 6 m de desnivel
    barrotes = sorted(b.p1[0] for b in e.barras if b.rol == "barrote")
    huecos = [b - a - 14 for a, b in zip(barrotes, barrotes[1:]) if b - a < 200]
    assert huecos and max(huecos) < 100                             # no pasa una esfera de 10 cm


def test_escalera_cumple_la_regla_del_paso():
    e = M.construir("escalera", {"alto": 2.8, "ancho": 1.0})
    c, h = e.cotas["contrahuella"], e.cotas["huella"]
    assert 130 <= c <= 175 and h >= 280 and 540 <= 2 * c + h <= 700


def test_los_perfiles_crecen_con_la_luz():
    p1 = M.construir("entreplanta", {"largo": 4, "ancho": 3})
    p2 = M.construir("entreplanta", {"largo": 4, "ancho": 5, "separacion_mm": 5000})
    v1 = next(c for c in p1.comprobaciones if c["elemento"] == "Vigueta")["perfil"]
    v2 = next(c for c in p2.comprobaciones if c["elemento"] == "Vigueta")["perfil"]
    assert C.perfil(v2).kg_m > C.perfil(v1).kg_m


def test_medidas_fuera_de_rango():
    with pytest.raises(ValueError):
        M.construir("porche", {"largo": 500})


# ------------------------------------------------------------------ despiece, corte, planos e imagen
def test_proyecto_completo_y_ficheros(data_dir):
    r = herrero.proyecto("Porche de 6 x 4 m y 2,6 m de altura galvanizado", imagen=True, imagen_ia=False)
    assert r["cumple"] and r["tipo"] == "porche"
    assert r["totales"]["kg_total"] == pytest.approx(sum(f["kg_total"] for f in r["despiece"]) +
                                                      sum(f["kg_total"] for f in r["placas"]), abs=0.5)
    assert sum(f["cantidad"] for f in r["despiece"]) == len(r["geometria"]["barras"])
    for c in r["lista_corte"]:                                      # cada barra comercial cabe y no se pasa
        for barra in c["plan"]:
            assert sum(p["mm"] for p in barra["piezas"]) <= c["largo_barra_mm"]
    carpeta = data_dir / "outputs" / "herrero" / r["id"]
    for f in ("plano-montaje.svg", "isometrica.svg", "proyecto.pdf", "despiece.csv", "render.jpg", "proyecto.json"):
        assert (carpeta / f).stat().st_size > 200, f
    ET.fromstring((carpeta / "plano-montaje.svg").read_text(encoding="utf-8"))   # SVG bien formado
    pdf = (carpeta / "proyecto.pdf").read_bytes()
    assert pdf.startswith(b"%PDF-1.4") and pdf.rstrip().endswith(b"%%EOF") and b"/Count 3" in pdf
    from PIL import Image
    assert Image.open(carpeta / "render.jpg").size == (1280, 800)
    assert herrero.proyectos()[0]["id"] == r["id"] and herrero.cargar(r["id"])["titulo"] == r["titulo"]
    assert "Peso" in herrero.resumen(r)
    with pytest.raises(KeyError):
        herrero.cargar("../../etc")


def test_lista_de_corte_con_empalme():
    e = M.construir("nave", {"largo": 15, "ancho": 10, "alto": 5})
    corte = herrero.lista_corte(e)
    assert all(c["aprovechamiento_pct"] <= 100 for c in corte)
    assert any(c["empalmes"] for c in corte)                         # correas de 15 m en barras de 12 m


def test_lienzo_pdf_con_tildes():
    lz = D.Lienzo(100, 50)
    lz.texto(10, 10, "Unión soldada · a5 ◺ 135 · Ø16", 3)
    lz.circulo(20, 20, 3)
    out = D.pdf([lz], "prueba")
    assert b"Uni\xf3n" in __import__("zlib").decompress(out.split(b"stream\n")[1].split(b"\nendstream")[0])


def test_imagen_ia_usa_el_render_como_partida(data_dir, monkeypatch):
    llamadas = {}

    class Imagenes:
        def refinar(self, base, prompt, fuerza, negativo, ancho, alto):
            llamadas.update(base=str(base), prompt=prompt, fuerza=fuerza)
            out = data_dir / "falsa.png"
            __import__("PIL.Image", fromlist=["Image"]).new("RGB", (ancho, alto)).save(out)
            return {"path": str(out), "engine": "sd.cpp (cpu)", "model": "sdxs"}

    class Servicios:
        images = Imagenes()

    r = herrero.proyecto("pérgola de 4x3 en blanco", services=Servicios())
    assert llamadas["base"].endswith("render.jpg") and "pergola" in llamadas["prompt"] and 0.2 < llamadas["fuerza"] < 0.7
    assert r["ficheros"]["foto_ia"].endswith("foto-ia.png")


# ------------------------------------------------------------------ API, herramientas y agente
def test_api_y_herramientas(services):
    from nexus.server.app import create_app
    from nexus.tools.base import ToolContext
    with TestClient(create_app(services)) as c:
        info = c.get("/api/herrero").json()
        assert set(info["tipos"]) == set(M.TIPOS) and "S275" in info["aceros"] and "IPE" in info["series"]
        assert c.post("/api/herrero/proyecto", json={}).status_code == 400
        r = c.post("/api/herrero/proyecto", json={"encargo": "", "parametros": {"tipo": "barandilla", "largo": 3},
                                                  "imagen_ia": False}).json()
        assert r["tipo"] == "barandilla" and c.get(r["ficheros"]["pdf"]).status_code == 200
        assert c.get(f"/api/herrero/proyecto/{r['id']}").json()["id"] == r["id"]
        assert c.get("/api/herrero/proyecto/no-existe").status_code == 404
        assert c.delete(f"/api/herrero/proyecto/{r['id']}").status_code == 403          # solo administradores
        s = c.post("/api/herrero/herramienta/herrero_soldadura", json={"espesor1_mm": 10, "espesor2_mm": 12}).json()
        assert s["garganta_mm"] >= 3
        assert c.post("/api/herrero/herramienta/python_exec", json={}).status_code == 404
        out = services.supervisor.execute("herrero_proyecto", {"encargo": "escalera de 2,5 m", "imagen_ia": False},
                                          ToolContext(services=services, agent="herrero"))
        assert "Escalera" in out["resumen"] and out["ficheros"]["pdf"].endswith("proyecto.pdf")
        p = services.supervisor.execute("herrero_perfil", {"perfil": "IPE 160", "luz_m": 4, "carga_kn_m": 5},
                                        ToolContext(services=services, agent="herrero"))
        assert p["perfil"]["nombre"] == "IPE 160" and "viga" in p
        herr = services.agents.get("herrero")
        assert herr and "herrero_proyecto" in herr.tools


def test_agentes_de_la_oficina_de_estructuras():
    from nexus import office
    cat = office.catalog()
    for a in ("herrero", "calculista-estructuras", "delineante-metal"):
        assert "herrero_proyecto" in cat[a]["tools"], a
    assert "herrero_soldadura" in cat["soldador"]["tools"]
    perfiles = office.profiles()
    assert {"calculista-estructuras", "delineante-metal"} <= set(perfiles["estructuras-metalicas"]["agents"])
