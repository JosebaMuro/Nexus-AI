/* Agente IA — Turnos de modelos: estado de la carga por etapas cuando la RAM no da para todos los modelos. Usa window.NexusUI. */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);
  const gb = (mb) => (mb / 1024).toLocaleString("es-ES", { maximumFractionDigits: 1 }) + " GB";
  const MODOS = { auto: "Automático (solo si no caben)", siempre: "Siempre por turnos", nunca: "Nunca (todo cargado)" };

  async function cargar() {
    const box = $("#turnos-box");
    if (!box) return;
    let d;
    try { d = await api("/api/turnos"); } catch (e) { box.innerHTML = `<span class="muted">No disponible: ${esc(e.message || e)}</span>`; return; }
    const cap = d.capacidad, nec = d.necesidad;
    const pct = Math.min(100, Math.round((100 * nec.ram_mb) / Math.max(1, cap.ram_mb)));
    const estado = d.alternando ? '<span class="tag warn">ALTERNANDO POR ETAPAS</span>' : '<span class="tag ok">TODO CABE A LA VEZ</span>';
    box.innerHTML = `<div class="row" style="align-items:center;gap:10px;flex-wrap:wrap">${estado}
        <span class="mono muted">modelos ${gb(nec.ram_mb)} de ${gb(cap.ram_mb)} útiles (RAM total ${gb(d.ram_total_mb)}, libre ${gb(d.ram_disponible_mb)})${cap.vram_mb ? ` · VRAM ${gb(nec.vram_mb)} de ${gb(cap.vram_mb)}` : ""}</span>
        <span style="flex:1"></span>
        <label class="admin-only">Modo <select id="turnos-modo">${Object.entries(MODOS).map(([k, v]) => `<option value="${k}" ${k === d.modo ? "selected" : ""}>${v}</option>`).join("")}</select></label></div>
      <div style="height:6px;background:var(--line,#333);border-radius:3px;margin:8px 0"><div style="height:6px;width:${pct}%;background:${d.alternando ? "var(--warn,#e0a030)" : "var(--ok,#3c3)"};border-radius:3px"></div></div>
      <div class="list">${d.modelos.filter((m) => m.ram_mb || m.vram_mb).map((m) => `<div class="item"><div class="row"><b style="flex:1">${esc(m.nombre)}</b>
        <span class="tag ${m.cargado ? "ok" : ""}">${m.en_uso ? "EN USO" : m.cargado ? "CARGADO" : "DESCARGADO"}</span></div>
        <div class="d mono">${m.ram_mb ? gb(m.ram_mb) + " RAM" : ""}${m.vram_mb ? " · " + gb(m.vram_mb) + " VRAM" : ""}${m.recargas ? ` · ${m.recargas} recargas` : ""}</div></div>`).join("") || '<div class="item muted">Aún no hay modelos locales que repartir.</div>'}</div>
      ${d.historial.length ? `<div class="item mono muted" style="margin-top:6px">Últimas etapas: ${d.historial.slice(0, 8).map((h) => `${esc(h.etapa)} ${h.segundos}s${h.liberados.length ? " (libera " + h.liberados.map(esc).join(", ") + ")" : ""}`).join(" · ")}</div>` : ""}
      <div class="muted" style="margin-top:6px">Cada paso (responder, ver una imagen, generar imagen, voz, búsqueda semántica) recibe toda la memoria que necesita: se descargan los modelos que no usa y se recargan al volver a necesitarlos. Con poca memoria, el contexto, la resolución de imagen y el tamaño de whisper se ajustan a lo que cabe.</div>`;
    const sel = $("#turnos-modo");
    if (sel) sel.onchange = guard(async () => { await api("/api/turnos", { body: { modo: sel.value } }); toast("Turnos de modelos: " + MODOS[sel.value]); cargar(); });
  }
  window.NexusTurnos = { cargar };
})();
