"""Turnos de modelos: con la memoria justa, cada etapa libera los modelos que no usa y recibe su presupuesto."""
from __future__ import annotations

from nexus.hardware import turnos as T


class Cfg(dict):
    def get(self, k, d=None):
        return super().get(k, d)


class Memoria:
    """RAM simulada: total y lo que ocupan los modelos cargados."""
    def __init__(self, total_mb=8192, base_mb=2000):
        self.total, self.base, self.cargados = total_mb, base_mb, {}

    def ram(self):
        return self.total, self.total - self.base - sum(self.cargados.values())


def montar(total=8192, modo="auto", tamanos=None):
    mem = Memoria(total)
    t = T.Turnos(Cfg({"hardware.alternar_modelos": modo}), lambda: {"id": "cpu-cpu-only", "ram_gb": total / 1024})
    t.medir_ram = mem.ram
    t.medir_vram = lambda: (0.0, 0.0)
    t._movil = lambda: False
    for tipo, mb in (tamanos or {"llm": 4500, "embed": 900, "stt": 1000, "imagen": 2500}).items():
        def liberar(tp=tipo):
            mem.cargados.pop(tp, None)
        t.registrar(tipo, tipo, lambda mb=mb: (float(mb), 0.0), lambda tp=tipo: tp in mem.cargados, liberar)
    return t, mem


def test_todo_cabe_no_alterna():
    t, mem = montar(total=32768)
    assert not t.alternando()
    mem.cargados.update(llm=4500, embed=900)
    with t.etapa("imagen", ["imagen"]) as turno:
        assert not turno.alternando and turno.liberados == []
    assert "llm" in mem.cargados and "embed" in mem.cargados


def test_memoria_justa_libera_lo_que_no_usa_la_etapa():
    t, mem = montar(total=8192)
    assert t.alternando()   # 8,9 GB de modelos y ~6,5 GB utiles
    mem.cargados.update(llm=4500, embed=900)
    with t.etapa("imagen", ["imagen"]) as turno:
        mem.cargados["imagen"] = 2500
        assert turno.alternando
        assert "llm" in turno.liberados
        assert turno.cabe(2500)
    mem.cargados.pop("imagen")
    # vuelve el texto: no hace falta echar a nadie y cuenta como recarga
    with t.etapa("texto", ["llm"]) as turno:
        mem.cargados["llm"] = 4500
        assert turno.liberados == []
    assert t.residentes["llm"].recargas == 1 and t.recargas == 1


def test_libera_primero_el_menos_usado():
    t, mem = montar(total=8192, tamanos={"llm": 3000, "embed": 900, "stt": 1000, "imagen": 3000})
    mem.cargados.update(llm=3000, embed=900, stt=1000)
    t.residentes["stt"].ultimo = 1.0
    t.residentes["embed"].ultimo = 2.0
    t.residentes["llm"].ultimo = 3.0
    with t.etapa("imagen", ["imagen"]) as turno:
        assert turno.liberados[0] == "stt"
        assert "llm" not in turno.liberados or turno.liberados.index("llm") > 0


def test_no_libera_un_modelo_en_uso():
    t, mem = montar(total=8192)
    mem.cargados.update(llm=4500, embed=900)
    with t.etapa("texto", ["llm"]):
        with t.etapa("imagen", ["imagen"]) as turno:
            assert "llm" not in turno.liberados
    assert "llm" in mem.cargados


def test_fondo_espera_y_no_echa_a_nadie():
    t, mem = montar(total=8192)
    mem.cargados.update(llm=4500)
    with t.etapa("texto", ["llm"]):
        with t.fondo("indexacion", ["embed"]) as toca:
            assert toca is False
    with t.fondo("indexacion", ["embed"]) as toca:
        assert toca is True   # sin etapas en primer plano y con sitio para el modelo de embeddings
    assert "llm" in mem.cargados


def test_modos_siempre_y_nunca():
    t, mem = montar(total=65536, modo="siempre")
    mem.cargados.update(llm=4500, embed=900)
    with t.etapa("texto", ["llm"]) as turno:
        assert turno.liberados == ["embed"]
    t2, mem2 = montar(total=4096, modo="nunca")
    mem2.cargados.update(llm=4500, embed=900)
    with t2.etapa("imagen", ["imagen"]) as turno:
        assert turno.liberados == [] and not turno.alternando


def test_estado_y_motivo():
    t, _ = montar(total=8192)
    e = t.estado()
    assert e["alternando"] and e["necesidad"]["ram_mb"] == 8900 and len(e["modelos"]) == 4
    assert "por turnos" in t.motivo_arranque()


def test_ayudas_de_tamano():
    assert T.whisper_para(10000, "small") == "small"     # nunca pasa del que pide el perfil
    assert T.whisper_para(800, "small") == "base"
    assert T.whisper_para(100, "small") == "tiny"
    assert T.imagen_mb(2000, 1024, True) < T.imagen_mb(2000, 1024)
    assert T.llm_mb(4000, 8192) > T.llm_mb(4000, 2048)


def test_llm_ajusta_contexto_al_presupuesto():
    from nexus.llm.manager import LLMManager
    m = LLMManager.__new__(LLMManager)
    m.profile = {"id": "cpu-cpu-only", "llm": {"gpu_layers": 0, "n_ctx": 16384}}
    m._entrada_turno = {"id": "x", "size_mb": 4000}
    turno = T.Turno("texto", ("llm",), True, ram_mb=T.llm_mb(4000, 4096) + 10, vram_mb=0)
    full = {"n_ctx": 16384}
    m._ajustar_a_turno(turno, full)
    assert full["n_ctx"] == 4096


def test_imagen_baja_resolucion_si_no_cabe():
    from nexus.media.image import ImageEngine
    eng = ImageEngine.__new__(ImageEngine)
    eng.turnos = None
    model = {"id": "sd", "size_mb": 2000}
    turno = T.Turno("imagen", ("imagen",), True, ram_mb=T.imagen_mb(2000, 512, True) + 5, vram_mb=0)
    w, h, tiling, ajuste = eng._ajustar_a_turno(turno, model, "cpu", 1024, 1024)
    assert tiling and w <= 576 and h <= 576 and "resolucion" in ajuste
    holgado = T.Turno("imagen", ("imagen",), True, ram_mb=99999, vram_mb=0)
    assert eng._ajustar_a_turno(holgado, model, "cpu", 1024, 1024) == (1024, 1024, False, "")


def test_autoajuste_con_muchas_recargas():
    from nexus.hardware.tuner import tune
    profile = {"llm": {"n_ctx": 8192, "max_ctx": 16384, "n_batch": 256, "gpu_layers": 0, "n_threads": 4}}
    hist = [{"llm_calls": 10, "avg_tps": 9, "recargas_llm": 9, "peak_ram_pct": 80, "clean_exit": True,
             "settings": {"n_threads": 4}}]
    tuned, _, reasons = tune(profile, hist, False, 8, state={"overrides": {}, "thread_trials": {}, "log": []})
    assert tuned["llm"]["suggest_smaller_model"] and tuned["llm"]["n_ctx"] < 8192
    assert any("recargo" in r for r in reasons)


def test_servicios_registran_turnos_y_ruta(client=None):
    from nexus.core import Services
    s = Services(detect_hardware=False)
    try:
        assert set(s.turnos.residentes) >= {"llm", "embed", "stt", "tts", "imagen"}
        assert s.llm.turnos is s.turnos and s.images.turnos is s.turnos
        e = s.turnos.estado()
        assert e["modo"] == "auto" and "capacidad" in e
    finally:
        s.shutdown()
