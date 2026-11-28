/* Agente IA — FreeLLMAPI como endpoint LLM (Ajustes): URL, clave, modelo, arrancar la incluida y usarla. Usa window.NexusUI. */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);
  let espera = null;

  function etiqueta(d) {
    if (d.responde && d.clave_puesta) return '<span class="tag ok">RESPONDE</span>';
    if (d.responde) return '<span class="tag warn">RESPONDE · FALTA LA CLAVE</span>';
    if (d.en_marcha) return '<span class="tag warn">ARRANCANDO…</span>';
    return '<span class="tag">PARADA</span>';
  }

  async function cargar() {
    const box = $("#fl-box");
    if (!box || !UI().admin()) return;
    let d;
    try { d = await api("/api/freellmapi"); } catch (e) { box.innerHTML = `<span class="muted">No disponible: ${esc(e.message || e)}</span>`; return; }
    const v = d.version || {};
    box.innerHTML = `<div class="row" style="align-items:center;gap:10px;flex-wrap:wrap">${etiqueta(d)}
        ${d.es_principal ? '<span class="tag ok">MODELO PRINCIPAL</span>' : ""}
        <span class="mono muted">${esc(d.url)}</span><span style="flex:1"></span>
        <span class="muted">${d.instalada ? `incluida: FreeLLMAPI ${esc(v.freellmapi || "?")} · Node ${esc(v.node || "?")}` : "no incluida en este paquete: usa una que ya tengas en marcha"}</span></div>
      <div class="row" style="flex-wrap:wrap;margin-top:8px">
        <input class="input" id="fl-url" placeholder="http://127.0.0.1:3001/v1" value="${esc(d.url)}" style="flex:2 1 240px;min-width:0" title="Endpoint /v1 (la app de escritorio de FreeLLMAPI usa el puerto 31415)">
        <input class="input" id="fl-clave" type="password" placeholder="${d.clave_puesta ? "clave guardada (déjala vacía para no cambiarla)" : "clave única (panel web › Keys)"}" style="flex:2 1 220px;min-width:0" autocomplete="off">
        <input class="input" id="fl-modelo" list="fl-modelos" placeholder="auto" value="${esc(d.modelo)}" style="flex:1 1 140px;min-width:0" title="auto = el mejor modelo disponible">
        <datalist id="fl-modelos"></datalist>
        <button class="btn" id="fl-guardar">GUARDAR</button></div>
      <div class="row" style="flex-wrap:wrap;margin-top:8px">
        ${d.instalada ? (d.en_marcha ? '<button class="btn" id="fl-parar">PARAR</button>' : '<button class="btn primary" id="fl-arrancar">ARRANCAR LA INCLUIDA</button>') : ""}
        <a class="btn" href="${esc(d.panel)}" target="_blank" rel="noopener">PANEL WEB (claves de proveedores)</a>
        <button class="btn" id="fl-listar">VER MODELOS</button>
        <button class="btn" id="fl-probar">PROBAR</button>
        ${d.es_principal ? '<button class="btn" id="fl-local">VOLVER A LOS MODELOS LOCALES</button>' : '<button class="btn primary" id="fl-usar">USAR COMO MODELO PRINCIPAL</button>'}</div>
      <div class="row" style="flex-wrap:wrap;margin-top:8px;gap:16px">
        ${d.instalada ? `<label class="row muted"><span class="switch"><input type="checkbox" id="fl-auto" ${d.arrancar ? "checked" : ""}><span></span></span>arrancarla al abrir Agente IA</label>
        <label class="row muted" title="Otros equipos y la app Android pueden usarla con la IP de este equipo"><span class="switch"><input type="checkbox" id="fl-red" ${d.red ? "checked" : ""}><span></span></span>accesible en la red local</label>` : ""}</div>
      <div id="fl-salida" class="mono muted" style="margin-top:6px;white-space:pre-wrap"></div>
      ${d.error ? `<div class="item mono" style="color:var(--warn,#e0a030)">${esc(d.error)}</div>` : ""}
      <div class="muted" style="margin-top:6px">FreeLLMAPI junta los planes gratis de muchos proveedores (Groq, Gemini, Mistral, OpenRouter, Cerebras…) detrás de un solo endpoint compatible con OpenAI y salta al siguiente cuando uno llega a su límite. Pasos: arráncala, abre el panel web, añade en <b>Keys</b> las claves gratis de los proveedores y ordena la cadena en <b>Fallback Chain</b>. La clave única se copia sola desde la incluida. Para que un solo agente la use, pon en su modelo <span class="mono">freellmapi:auto</span>. Necesita internet para responder; los modelos locales siguen funcionando igual y FreeLLMAPI nunca los sustituye sin que lo elijas.</div>`;
    const salida = (t) => { $("#fl-salida").textContent = t; };
    const on = (id, fn) => { const el = $("#" + id); if (el) el.onclick = guard(fn); };
    on("fl-guardar", async () => {
      const body = { url: $("#fl-url").value.trim(), modelo: $("#fl-modelo").value.trim() || "auto" };
      if ($("#fl-clave").value.trim()) body.clave = $("#fl-clave").value.trim();
      await api("/api/freellmapi/ajustes", { body }); toast("FreeLLMAPI guardada"); cargar();
    });
    on("fl-arrancar", async () => {
      await api("/api/freellmapi/arrancar", { body: {} }); toast("Arrancando FreeLLMAPI…");
      clearTimeout(espera); let n = 0;
      const mirar = async () => { const e = await api("/api/freellmapi"); if ((e.responde && e.clave_puesta) || ++n > 20 || !e.en_marcha) cargar(); else espera = setTimeout(mirar, 1500); };
      espera = setTimeout(mirar, 1500);
    });
    on("fl-parar", async () => { await api("/api/freellmapi/parar", { body: {} }); toast("FreeLLMAPI parada"); cargar(); });
    on("fl-listar", async () => {
      const r = await api("/api/freellmapi/modelos");
      $("#fl-modelos").innerHTML = ["auto", ...r.modelos].map((m) => `<option value="${esc(m)}">`).join("");
      salida(r.modelos.length ? `${r.modelos.length} modelos: ${r.modelos.slice(0, 40).join(", ")}${r.modelos.length > 40 ? "…" : ""}` : "Sin modelos: añade claves de proveedores en el panel web (Keys).");
    });
    on("fl-probar", async () => {
      salida("Probando…");
      try { const r = await api("/api/freellmapi/probar", { body: {} }); salida(`${r.modelo || "?"} en ${r.segundos} s: ${r.respuesta}`); }
      catch (e) { salida(String(e.message || e)); throw e; }
    });
    on("fl-usar", async () => { await api("/api/freellmapi/usar", { body: { principal: true } }); toast("FreeLLMAPI es ahora el modelo principal"); cargar(); if (UI().loadStatus) UI().loadStatus(); });
    on("fl-local", async () => { await api("/api/freellmapi/usar", { body: { principal: false } }); toast("De vuelta a los modelos locales"); cargar(); if (UI().loadStatus) UI().loadStatus(); });
    const sw = (id, k) => { const el = $("#" + id); if (el) el.onchange = guard(async () => { await api("/api/freellmapi/ajustes", { body: { [k]: el.checked } }); toast("Guardado"); }); };
    sw("fl-auto", "arrancar"); sw("fl-red", "red");
  }
  window.NexusFreeLLMAPI = { cargar };
})();
