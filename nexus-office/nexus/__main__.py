"""CLI de Nexus-Office.

    python -m nexus run [--host H] [--port P] [--no-browser]
    python -m nexus detect
    python -m nexus setup --auto [--all]
    python -m nexus pull <id-recomendado|auto> (p.ej. qwen3-vl-2b-thinking, sdxs-512, piper-es-davefx)
    python -m nexus modelos [--descargar] [--todos]   (evalua el hardware y elige el modelo de IA de cada tipo)
    python -m nexus servicio instalar|quitar|estado [--red] [--puerto P]   (arranque automatico con el equipo)
    python -m nexus chat
    python -m nexus oficina [<perfil>|--quitar] [--reset-agentes]   (la instalacion como oficina de una pyme)
    python -m nexus wheels [--target T] [--runtime] [--check]   (wheelhouse/ para instalar sin internet ni compilar)
    python -m nexus cleanup [--yes] [--keep|--no-keep] [--dry-run]   (quita lo que este equipo no usa del paquete)
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import webbrowser


def cmd_run(args) -> None:
    import uvicorn

    from .config import get_config
    from .server import create_app
    cfg = get_config()
    host = args.host or cfg.get("server.host", "127.0.0.1")
    port = args.port or cfg.get("server.port", 8775)
    url = f"http://{'127.0.0.1' if host in ('0.0.0.0', '::') else host}:{port}"
    from . import paths, service
    if sys.stdout is None or sys.stderr is None:   # pythonw.exe (servicio sin ventana en Windows): salida al log
        log_file = open(paths.logs_dir() / "servicio.log", "a", encoding="utf-8", buffering=1)  # noqa: SIM115
        sys.stdout = sys.stdout or log_file
        sys.stderr = sys.stderr or log_file
    if service.running(port):                      # ya lo arranco el servicio: el acceso directo solo abre el navegador
        print(f"\n  Nexus-Office ya esta en marcha (servicio)  ->  {url}\n")
        if not args.no_browser:
            webbrowser.open(url)
        return
    print(f"\n  Nexus-Office  ->  {url}\n")
    if cfg.get("server.open_browser", True) and not args.no_browser:
        threading.Timer(4.0, lambda: webbrowser.open(url)).start()
    app = create_app()
    if not cfg.get("remote.enabled", False):
        uvicorn.run(app, host=host, port=port, log_level="warning")
        return
    # acceso remoto: segundo puerto HTTPS (TLS 1.3) sobre la misma aplicacion
    import asyncio
    import signal

    from .remote import server_config

    class Quiet(uvicorn.Server):            # las senales las gestiona run_all para parar ambos puertos a la vez
        def capture_signals(self):
            import contextlib
            return contextlib.nullcontext()

    local = Quiet(uvicorn.Config(app, host=host, port=port, log_level="warning", timeout_graceful_shutdown=3))
    try:
        rconf = server_config(app, app.state.nexus_state, cfg)
    except Exception as e:  # noqa: BLE001
        print(f"  Acceso remoto desactivado: {e}")
        uvicorn.run(app, host=host, port=port, log_level="warning")
        return
    remote = Quiet(rconf)
    app.state.remote_server = remote
    print(f"  Acceso remoto ->  https://<tu-ip>:{rconf.port}  (TLS 1.3, solo dispositivos emparejados)\n")

    async def run_all() -> None:
        def stop(*_):
            local.should_exit = remote.should_exit = True
        signal.signal(signal.SIGINT, stop)
        signal.signal(signal.SIGTERM, stop)

        async def guarded(srv, label):
            try:
                await srv.serve()
            except SystemExit:
                print(f"  No se pudo abrir el puerto {label}; Nexus-Office sigue sin el")
        rtask = asyncio.create_task(guarded(remote, f"remoto {rconf.port}"))
        await guarded(local, f"local {port}")
        remote.should_exit = True
        await rtask
    asyncio.run(run_all())


def cmd_detect(args) -> None:
    from . import hardware, log
    log.setup()
    hw = hardware.detect()
    hardware.save(hw)
    prof = hardware.build_profile(hw)
    print(json.dumps({"hardware": hw.to_dict(), "profile": prof}, indent=2, ensure_ascii=False))


def cmd_setup(args) -> None:
    from .setup_extras import run
    run(auto=args.auto, include_optional=args.all)


def cmd_pull(args) -> None:
    from . import hardware, log
    from .config import get_config
    from .models import ModelManager
    log.setup()
    prof = hardware.build_profile(hardware.load_cached() or hardware.detect(probe_runtimes=False))
    mm = ModelManager(get_config(), lambda: prof)
    ids = [args.model]
    if args.model == "auto":  # el mejor modelo de cada tipo para este hardware
        ids = [r["id"] for r in mm.recommended() if r.get("best") and not r["installed"]]
        print("Modelos recomendados para este equipo:", ", ".join(ids) or "ya instalados")
    for mid in ids:
        jid = mm.download_recommended(mid)
        while mm.jobs[jid]["status"] not in ("completado", "error", "cancelado"):
            j = mm.jobs[jid]
            pct = 100 * j["done"] / j["total"] if j["total"] else 0
            print(f"\r{mid}: {j['status'][:44]:44s} {pct:5.1f}%", end="", flush=True)
            time.sleep(1)
        print(f"\r{mid}: {mm.jobs[jid]['status']} {mm.jobs[jid].get('error', '')}".ljust(70))


KIND_NAME = {"llm": "Chat y vision", "embed": "Documentos (RAG)", "tts": "Voz", "image": "Imagen"}
ESSENTIAL = ("llm", "embed", "tts")           # lo que se descarga al instalar; la imagen con --todos


def _online(timeout: float = 6) -> bool:
    import urllib.request
    try:
        urllib.request.urlopen("https://huggingface.co/api/whoami-v2", timeout=timeout)
    except Exception as e:  # noqa: BLE001
        return getattr(e, "code", 0) in (401, 403)   # responde (sin sesion): hay internet
    return True


def cmd_models(args) -> None:
    """Evalua el hardware y elige el mejor modelo de cada tipo; con --descargar lo baja si hay internet."""
    from . import hardware, log
    from .config import get_config
    from .models import ModelManager
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    log.setup()
    hw = hardware.load_cached() or hardware.detect(probe_runtimes=False)
    prof = hardware.build_profile(hw)
    mm = ModelManager(get_config(), lambda: prof)
    gpu = f"{prof.get('vram_gb', 0):.0f} GB de VRAM" if prof.get("vram_gb") else "sin GPU dedicada"
    print(f"Equipo: {prof.get('device_name', '?')} | {prof.get('ram_gb', 0):.0f} GB de RAM | {gpu} | "
          f"acelerador {prof.get('accelerator', 'cpu')} | gama {prof.get('tier', '?')}")
    recs = mm.recommended()
    chosen = []
    for kind in ("llm", "embed", "tts", "image"):
        best = next((r for r in recs if r["kind"] == kind and r.get("best")), None)
        if not best:
            continue
        state = "ya instalado" if best["installed"] else f"{best['size_mb'] / 1024:.1f} GB"
        print(f"  {KIND_NAME[kind]:18s} -> {best['name']} ({state})")
        if kind in ESSENTIAL or args.todos:
            chosen.append(best)
    sector = [r for r in recs if r.get("sector") and not r.get("best")]
    if sector:
        print("  Sugeridos para tu oficina (en la app: Modelos > Recomendados):")
        for r in sector:
            how = {"ideal": "", "ok": "", "pesado": " · lento en este equipo", "no_cabe": " · no cabe en este equipo"}[r["fit"]]
            print(f"    - {r['name']}: {r['sector']}{how}{' (ya instalado)' if r['installed'] else ''}")
    llm = next((r for r in recs if r["kind"] == "llm" and r.get("best")), None)
    if llm:
        alt = [r["name"] for r in recs if r["kind"] == "llm" and r["fit"] == "pesado"]
        print(f"  Motivo: el modelo de chat mas capaz que cabe en este equipo con fluidez"
              + (f" (mas grande pero lento aqui: {', '.join(alt)})" if alt else ""))
    todo = [r for r in chosen if not r["installed"]]
    if not args.descargar:
        if todo:
            print("Para descargarlos: python -m nexus modelos --descargar   (o en la app: Modelos > Recomendados)")
        return
    if not todo:
        print("Los modelos elegidos ya estan instalados.")
        return
    if not _online():
        print("Sin conexion a internet: los modelos elegidos se descargaran en cuanto haya red "
              "(python -m nexus modelos --descargar, o Modelos > Recomendados en la app).")
        sys.exit(3)                                  # pendiente: los lanzadores lo reintentan en el siguiente arranque
    for r in todo:
        cmd_pull(argparse.Namespace(model=r["id"]))


def cmd_service(args) -> None:
    from .service import main as service_main
    sys.exit(service_main(args.rest))


def cmd_chat(args) -> None:
    from .core import Services
    s = Services()
    s.start()
    sid = s.memory.new_session("Terminal")
    print(f"Nexus-Office ({s.llm.status()['backend']}) - escribe 'salir' para terminar")
    try:
        while True:
            text = input("\n> ").strip()
            if text.lower() in ("salir", "exit", "quit"):
                break

            def emit(e):
                if e["type"] == "token":
                    print(e["text"], end="", flush=True)
                elif e["type"] == "tool_call":
                    print(f"\n[herramienta {e['name']} {e['args']}]")
                elif e["type"] == "approval_required":
                    a = e["approval"]
                    ok = input(f"\n¿Aprobar {a['tool']} {a['args']}? (s/n) ").lower().startswith("s")
                    s.supervisor.resolve(a["id"], ok, a["level"] == "admin")

            s.agents.run(args.agent, text, sid, emit=emit)
            print()
    finally:
        s.shutdown()


def cmd_office(args) -> None:
    from . import office
    from .config import get_config
    if hasattr(sys.stdout, "reconfigure"):   # consolas de Windows con codificacion antigua: nunca falla por un acento
        sys.stdout.reconfigure(errors="replace")
    o = office.Office(get_config())
    if args.quitar:
        o.clear()
        print("Perfil de oficina quitado: vuelven los agentes por defecto de Nexus-Office.")
        return
    if not args.perfil:
        active = o.active()
        print("Perfiles de oficina (python -m nexus oficina <perfil>):\n")
        for p in o.list():
            trades = ", ".join(a["name"] for a in p["agents"] if a["gremio"])
            desk = ", ".join(a["name"] for a in p["agents"] if not a["gremio"])
            print(f"  {'*' if p['id'] == active else ' '} {p['id']:26s} {p['name']}")
            print(f"      gremio: {trades}\n      oficina: {desk}")
        print("\n  * = perfil activo" if active else "\n  Ningun perfil activo (agentes por defecto de Nexus-Office)")
        return
    try:
        p = o.apply(args.perfil, reset_agents=args.reset_agentes)
    except ValueError as e:
        raise SystemExit(str(e)) from e
    print(f"Oficina «{p['assistant_name']}» lista ({p['name']}). Agentes: " + ", ".join(a["name"] for a in p["agents"]))
    print("Cada agente tiene su propio grafo de conocimiento; se siembra al arrancar (python -m nexus run).")
    sugg = office.sector_models(args.perfil)
    if sugg:
        from .models.recommended import RECOMMENDED
        names = {r["id"]: r["name"] for r in RECOMMENDED}
        print("Modelos de IA sugeridos para esta oficina (Modelos > Recomendados, o python -m nexus modelos):")
        for mid, why in sugg.items():
            print(f"  - {names.get(mid, mid)}: {why}")


def cmd_bolsa(args) -> None:
    """Mesa de bolsa simulada en la terminal (ver nexus/office/bolsa.py)."""
    from pathlib import Path

    from .office import bolsa
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    b = bolsa.get()
    a, rest, cid = args.accion or "cartera", args.args, args.cartera or None

    def show(x):
        print(json.dumps(x, ensure_ascii=False, indent=2, default=str))

    def need(n, uso):
        if len(rest) < n:
            raise SystemExit(f"Uso: python -m nexus bolsa {uso}")

    try:
        if a == "cartera":
            r = b.resumen(cid)
            print(f"{r['nombre']} ({r['modo']}, tarifa {r['tarifa']}) · total {r['total_eur']:.2f} € · efectivo "
                  f"{r['efectivo_eur']:.2f} € · rentabilidad {r['rentabilidad_pct']:+.2f} % · gastos {r['gastos_eur']:.2f} €")
            for p in r["posiciones"]:
                print(f"  {p['simbolo']:10s} {p['cantidad']:>12g}  coste {p['coste_medio_eur']:>10.4f} €  precio "
                      f"{p['precio']:>10} {p['divisa']}  valor {p['valor_eur']:>10.2f} €  {p['plusvalia_pct']:+.2f} %")
            print("\n" + bolsa.AVISO)
        elif a == "mercados":
            for m in b.mercados():
                nxt = f"cierra {m['cierra']}" if m["abierto"] else f"abre {m['abre']}"
                print(f"  {m['nombre']:32s} {'ABIERTA' if m['abierto'] else 'cerrada':8s} {m['hora_local']}  {nxt}")
        elif a == "valores":
            for i in b.instrumentos(" ".join(rest)):
                print(f"  {i['simbolo']:10s} {i['mercado']:9s} {i['divisa']}  {i['nombre']}")
        elif a == "cotizar":
            need(1, "cotizar SIMBOLO")
            show(b.precio(rest[0]))
        elif a in ("comprar", "vender"):
            need(2, f"{a} SIMBOLO CANTIDAD [--tipo limitada --limite P] [--stop P] [--validez hasta_cancelar]")
            show(b.ordenar(rest[0], a, rest[1], args.tipo, args.limite, args.stop, args.validez, cid, origen="terminal"))
        elif a == "ordenes":
            for o in b.ordenes(cid):
                print(f"  #{o['id']:<5} {o['estado']:13s} {o['lado']:6s} {o['cantidad']:>10g} {o['simbolo']:10s} "
                      f"{o['tipo']} {o.get('motivo') or ''}")
        elif a == "operaciones":
            for o in b.operaciones(cid):
                g = "" if o["ganancia_eur"] is None else f" resultado {o['ganancia_eur']:+.2f} €"
                print(f"  {o['fecha']} {o['lado']:6s} {o['cantidad']:>10g} {o['simbolo']:10s} a {o['precio']:.4f}  total "
                      f"{o['total_eur']:.2f} € (comisión {o['comision_eur']:.2f}, impuestos {o['impuestos_eur']:.2f}, "
                      f"cambio {o['cambio_eur']:.2f}){g}")
        elif a == "cancelar":
            need(1, "cancelar ID")
            show(b.cancelar(int(rest[0])))
        elif a == "confirmar":   # solo una persona, en la terminal, confirma una orden de cartera real
            need(1, "confirmar ID")
            o = b.orden(int(rest[0]))
            print(f"Orden REAL #{o['id']}: {o['lado']} {o['cantidad']:g} {o['simbolo']} ({o['tipo']}). Nexus-Office no la "
                  "envía a ningún bróker: después la ejecutas tú.")
            show(b.confirmar(o["id"], input(f"Escribe {bolsa.CONFIRMACION} para confirmarla: ")))
        elif a == "ejecucion":
            need(2, "ejecucion ID PRECIO [--comision EUROS]")
            show(b.registrar_ejecucion(int(rest[0]), rest[1], args.comision or 0))
        elif a == "nueva":
            show(b.crear_cartera(" ".join(rest) or "Cartera simulada", args.capital, args.tarifa, args.modo))
        elif a == "reiniciar":
            show(b.reiniciar(cid, args.capital if args.capital != 10000 else None))
        elif a == "importar":
            need(2, "importar SIMBOLO fichero.csv")
            show(b.importar_csv(rest[0], Path(rest[1]).read_text(encoding="utf-8", errors="replace")))
        elif a == "fiscal":
            show(b.informe_fiscal(cid, int(rest[0]) if rest else None))
        else:
            raise SystemExit(f"acción desconocida: {a}")
    except (ValueError, PermissionError) as e:
        raise SystemExit(str(e)) from e


def main(argv=None) -> None:
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["wheels"]:                # CLI propia (nexus/wheelhouse.py): no importa nada pesado
        from .wheelhouse import main as wheels_main
        return wheels_main(argv[1:])
    if argv[:1] == ["servicio"]:
        from .service import main as service_main
        return service_main(argv[1:])
    if argv[:1] == ["cleanup"]:
        from .cleanup import main as cleanup_main
        return cleanup_main(argv[1:])
    p = argparse.ArgumentParser(prog="nexus", description="Nexus-Office - agente multimodal portable")
    sub = p.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="arranca el servidor y el webui")
    r.add_argument("--host")
    r.add_argument("--port", type=int)
    r.add_argument("--no-browser", action="store_true")
    sub.add_parser("detect", help="detecta el hardware y muestra el perfil")
    st = sub.add_parser("setup", help="instala extras segun el hardware")
    st.add_argument("--auto", action="store_true")
    st.add_argument("--all", action="store_true", help="incluye extras opcionales pesados (PyTorch)")
    pl = sub.add_parser("pull", help="descarga un modelo recomendado")
    pl.add_argument("model")
    ch = sub.add_parser("chat", help="chat en la terminal")
    ch.add_argument("--agent", default="auto")
    of = sub.add_parser("oficina", help="perfiles de oficina para pymes (agentes por gremio)")
    of.add_argument("perfil", nargs="?", help="id del perfil (sin nada: lista los perfiles)")
    of.add_argument("--quitar", action="store_true", help="vuelve a los agentes por defecto de Nexus-Office")
    of.add_argument("--reset-agentes", action="store_true", help="reescribe tambien los agentes ya personalizados")
    bo = sub.add_parser("bolsa", help="mesa de bolsa simulada: cartera | mercados | valores | cotizar | comprar | vender | "
                        "ordenes | operaciones | cancelar | confirmar | ejecucion | nueva | reiniciar | importar | fiscal")
    bo.add_argument("accion", nargs="?")
    bo.add_argument("args", nargs="*")
    bo.add_argument("--cartera", type=int)
    bo.add_argument("--tipo", default="mercado", choices=["mercado", "limitada", "stop", "stop_limitada"])
    bo.add_argument("--limite", type=float)
    bo.add_argument("--stop", type=float)
    bo.add_argument("--validez", default="dia", choices=["dia", "hasta_cancelar"])
    bo.add_argument("--capital", type=float, default=10000)
    bo.add_argument("--tarifa", default="broker-online", choices=["broker-online", "banco", "neobroker"])
    bo.add_argument("--modo", default="simulado", choices=["simulado", "real"])
    bo.add_argument("--comision", type=float)
    md = sub.add_parser("modelos", help="evalua el hardware y elige (y descarga) el modelo de IA adecuado")
    md.add_argument("--descargar", action="store_true", help="descarga los elegidos si hay internet")
    md.add_argument("--todos", action="store_true", help="incluye tambien el modelo de imagen")
    sv = sub.add_parser("servicio", help="instala Nexus-Office como servicio: instalar | quitar | estado [--red] [--puerto P]")
    sv.add_argument("rest", nargs=argparse.REMAINDER)
    sub.add_parser("wheels", help="prepara wheelhouse/ para instalar sin internet (ver --help)")
    sub.add_parser("cleanup", help="quita del paquete offline lo que este equipo no usa (ver --help)")
    args = p.parse_args(argv)
    {"run": cmd_run, "detect": cmd_detect, "setup": cmd_setup, "pull": cmd_pull, "chat": cmd_chat,
     "oficina": cmd_office, "bolsa": cmd_bolsa, "modelos": cmd_models, "servicio": cmd_service}.get(
        args.cmd or "run", cmd_run)(args if args.cmd else p.parse_args(["run"]))


if __name__ == "__main__":
    sys.exit(main())
