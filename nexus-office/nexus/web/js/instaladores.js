/* Agente IA — Instaladores sin conexión: ver, bajar una vez, importar, guardar y repartir por la red local. Usa window.NexusUI. */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);
  const on = (sel, fn) => { const el = $(sel); if (el) el.onclick = guard(fn); };
  const app = () => window.NexusApp;
  const S = { d: null, t: 0 };
  const ARM = ["android-autonomo", "android-cliente", "windows-arm64", "linux-aarch64-run", "linux-aarch64-deb", "linux-aarch64-appimage", "linux-aarch64-flatpak", "macos-arm64"];

  function tam(n) {
    if (!n) return "—";
    const u = ["B", "KB", "MB", "GB"]; let i = 0;
    while (n >= 1024 && i < 3) { n /= 1024; i++; }
    return n.toLocaleString("es-ES", { maximumFractionDigits: i >= 2 ? 1 : 0 }) + " " + u[i];
  }
  function qr(url) {
    if (typeof window.qrcode !== "function") return "";
    try { const q = window.qrcode(0, "M"); q.addData(url); q.make(); return q.createSvgTag({ cellSize: 4, margin: 2, scalable: true }); } catch (e) { return ""; }
  }
  function etiqueta(it) {
    if (it.estado === "listo") return it.origen === "app" ? '<span class="tag ok">DENTRO DE LA APP</span>' : `<span class="tag ok">GUARDADO${it.verificado === "sha256" ? " · SHA256 OK" : ""}</span>`;
    if (it.estado === "descargando") return '<span class="tag warn">DESCARGANDO</span>';
    if (it.estado === "en cola") return '<span class="tag">EN COLA</span>';
    if (it.estado === "no publicado") return '<span class="tag">NO PUBLICADO</span>';
    return '<span class="tag">FALTA</span>';
  }

  function pintar(d) {
    S.d = d;
    const r = d.release || {};
    $("#in-release").textContent = r.tag ? `Release ${r.tag} · consultada ${r.consultado || ""}${r.aviso ? " · " + r.aviso : ""}` : "aún no se ha consultado la Release (hace falta conexión una vez)";
    const desc = d.descarga && d.descarga.id ? d.descarga : null;
    $("#in-resumen").innerHTML = `<span class="chip done">${d.listos} disponibles sin conexión · ${tam(d.bytes_listos)}</span>
      <span class="chip">faltan ${tam(d.faltan_bytes)}</span><span class="chip">libre en el equipo ${tam(d.libre)}</span>`;
    const filas = d.items.map((it) => {
      const pct = desc && desc.id === it.id && desc.total ? Math.floor((desc.hecho / desc.total) * 100) : null;
      const barra = pct !== null ? `<div class="muted mono">${pct} % · ${tam(desc.hecho)} de ${tam(desc.total)}</div>` : "";
      const acciones = it.estado === "listo"
        ? `<button class="btn sm" data-guardar="${esc(it.id)}">GUARDAR</button>${it.origen === "equipo" ? ` <button class="btn sm danger admin-only" data-borrar="${esc(it.id)}">✕</button>` : ""}`
        : it.estado === "falta" ? `<button class="btn sm admin-only" data-bajar="${esc(it.id)}">BAJAR</button>` : "";
      return `<tr><td><b>${esc(it.plataforma)}</b><div class="muted">${esc(it.detalle)}</div></td>
        <td class="mono" style="word-break:break-all">${esc(it.nombre)}${it.error ? `<div class="tag warn" style="white-space:normal">${esc(it.error)}</div>` : ""}${barra}</td>
        <td class="mono">${tam(it.tam)}</td><td>${etiqueta(it)}</td><td style="white-space:nowrap">${acciones}</td></tr>`;
    }).join("");
    $("#in-lista").innerHTML = `<table class="tbl"><tr><th>Plataforma</th><th>Fichero</th><th>Tamaño</th><th>Estado</th><th></th></tr>${filas}</table>`;
    $$("[data-bajar]").forEach((b) => (b.onclick = guard(() => bajar([b.dataset.bajar]))));
    $$("[data-borrar]").forEach((b) => (b.onclick = guard(async () => { if (!confirm("¿Borrar este instalador del equipo?")) return; pintar(await api(`/api/instaladores/${b.dataset.borrar}`, { method: "DELETE" })); })));
    $$("[data-guardar]").forEach((b) => (b.onclick = guard(() => guardar(d.items.find((x) => x.id === b.dataset.guardar)))));
    const c = d.compartir || {};
    $("#in-compartir").textContent = c.activo ? "DEJAR DE COMPARTIR" : "COMPARTIR POR WIFI";
    $("#in-compartir-info").textContent = c.activo ? `Compartiendo ${d.listos} ficheros en el puerto ${c.puerto}. Los demás equipos deben estar en la misma wifi o en el punto de acceso de este móvil.` : "Abre un servidor de solo lectura con los instaladores disponibles.";
    $("#in-urls").innerHTML = c.activo ? (c.urls.length ? c.urls.map((u) => `<div class="col" style="align-items:center;gap:4px"><div style="width:150px;background:#fff;padding:4px;border-radius:8px">${qr(u)}</div><a class="mono" href="${esc(u)}" target="_blank">${esc(u)}</a></div>`).join("")
      : '<p class="muted">No se ha encontrado la dirección de red local: conecta la wifi o activa el punto de acceso.</p>') : "";
    clearTimeout(S.t);
    if ((desc || (d.cola || []).length) && UI().view && UI().view() === "instaladores") S.t = setTimeout(() => cargar().catch(() => {}), 1500);
  }

  async function cargar() { pintar(await api("/api/instaladores")); }

  async function bajar(ids) {
    if (!ids.length) return toast("No falta ninguno");
    const d = S.d;
    const bytes = d.items.filter((x) => ids.includes(x.id)).reduce((a, x) => a + (x.tam || 0), 0);
    if (bytes > 1e9 && !confirm(`Se van a bajar ${tam(bytes)} de la Release (una sola vez). ¿Seguir? Mejor con wifi.`)) return;
    pintar(await api("/api/instaladores/descargar", { method: "POST", body: { ids } }));
    toast("Descarga en marcha: se puede seguir usando la app");
  }

  function guardar(it) {
    if (!it) return;
    if (app() && app().guardarInstalador) {   // APK: copia por flujo a la carpeta que elijas (Descargas, USB...)
      app().guardarInstalador(it.ruta, it.nombre);
      return;
    }
    const a = document.createElement("a");
    a.href = `/api/instaladores/fichero/${encodeURIComponent(it.id)}`; a.download = it.nombre;
    document.body.appendChild(a); a.click(); a.remove();
  }

  window.__nexusImportados = (n, err) => {
    if (err) toast(`No se pudo importar: ${err}`, true);
    api("/api/instaladores/revisar", { method: "POST" }).then((r) => { pintar(r); toast(`${n} ficheros importados${r.errores.length ? " · " + r.errores.join(" ") : ""}`, !!r.errores.length); }).catch(() => {});
  };

  on("#in-todo", () => bajar(S.d.items.filter((x) => x.estado === "falta").map((x) => x.id)));
  on("#in-movil", () => bajar(S.d.items.filter((x) => x.estado === "falta" && ARM.includes(x.id)).map((x) => x.id)));
  on("#in-cancelar", async () => { pintar(await api("/api/instaladores/cancelar", { method: "POST" })); toast("Descarga parada: se reanuda desde donde iba"); });
  on("#in-actualizar", async () => { pintar(await api("/api/instaladores/release", { method: "POST" })); toast("Release consultada"); });
  on("#in-compartir", async () => {
    const activo = !(S.d.compartir || {}).activo;
    await api("/api/instaladores/compartir", { method: "POST", body: { activo } });
    await cargar();
  });
  on("#in-importar", async () => {
    if (app() && app().importarInstaladores) { app().importarInstaladores(); return; }
    const carpeta = prompt("Carpeta (o USB) con los instaladores y su SHA256SUMS.txt:");
    if (!carpeta) return;
    const r = await api("/api/instaladores/importar", { method: "POST", body: { carpeta } });
    pintar(r);
    toast(`${r.copiados.length} ficheros importados${r.errores.length ? " · " + r.errores.join(" ") : ""}`, !!r.errores.length);
  });

  function registrar() {
    const L = UI() && UI().loaders;
    if (!L) return setTimeout(registrar, 200);
    L.instaladores = cargar;
    if (UI().view && UI().view() === "instaladores") cargar();
  }
  registrar();
})();
