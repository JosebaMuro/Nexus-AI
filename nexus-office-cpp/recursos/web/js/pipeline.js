/* Pipelines del dossier de arquitectura en la interfaz (Estudio):
 *  - Pantalla de difusion: cada paso que publica el motor de difusion se pinta en un canvas. El dibujo va en un Web
 *    Worker con OffscreenCanvas (descargar, decodificar y pintar fuera del hilo principal); sin OffscreenCanvas se
 *    usa createImageBitmap en el hilo principal.
 *  - Percepcion: la camara del dispositivo manda un frame JPEG cada pocos segundos a /api/pipeline/frame; el motor
 *    de percepcion lo describe (red de vision en GPU/NPU si la hay) y lo guarda en la base vectorial local.
 */
(() => {
  "use strict";
  const $ = (s) => document.querySelector(s);
  let worker = null, ctx2d = null, ultima = 0, stream = null, timer = null, enviando = false;

  const WORKER = `
    let canvas = null, c = null, ultima = 0;
    onmessage = async (ev) => {
      const m = ev.data;
      if (m.canvas) { canvas = m.canvas; c = canvas.getContext("2d"); return; }
      if (!c || !m.url || m.version <= ultima) return;
      try {
        const r = await fetch(m.url, { cache: "no-store", credentials: "same-origin" });
        if (r.status !== 200) return;
        const bmp = await createImageBitmap(await r.blob());
        if (m.version < ultima) return;
        ultima = m.version;
        canvas.width = bmp.width; canvas.height = bmp.height;
        c.drawImage(bmp, 0, 0);
        bmp.close();
        postMessage({ pintado: m.version });
      } catch (e) { postMessage({ error: String(e) }); }
    };`;

  function preparar() {
    const cv = $("#pipe-canvas");
    if (!cv || worker || ctx2d) return;
    if (cv.transferControlToOffscreen && window.Worker) {
      try {
        worker = new Worker(URL.createObjectURL(new Blob([WORKER], { type: "text/javascript" })));
        const off = cv.transferControlToOffscreen();
        worker.postMessage({ canvas: off }, [off]);
        worker.onmessage = (ev) => { if (ev.data.pintado) $("#pipe-canvas").classList.add("on"); };
        return;
      } catch (e) { worker = null; }
    }
    ctx2d = cv.getContext("2d");
  }

  async function pintar(url, version) {
    preparar();
    if (version <= ultima) return;
    // el worker nace de un blob: las rutas relativas no valen alli, va la URL completa
    if (worker) { worker.postMessage({ url: new URL(url, location.href).href, version }); ultima = version; return; }
    if (!ctx2d) return;
    try {
      const r = await fetch(url, { cache: "no-store" });
      if (r.status !== 200) return;
      const bmp = await createImageBitmap(await r.blob());
      if (version < ultima) return;
      ultima = version;
      const cv = $("#pipe-canvas");
      cv.width = bmp.width; cv.height = bmp.height;
      ctx2d.drawImage(bmp, 0, 0);
      cv.classList.add("on");
    } catch (e) { /* frame perdido: llegara el siguiente */ }
  }

  function estado(texto) { const el = $("#pipe-estado"); if (el) el.textContent = texto; }

  async function cargar() {
    preparar();
    try {
      const r = await fetch("/api/pipeline");
      if (!r.ok) return;
      const e = await r.json();
      const d = e.difusion || {}, p = e.percepcion || {}, h = e.hal || {};
      $("#pipe-info").textContent = `cola ${d.en_cola || 0} · generando ${d.generando || 0} · hechas ${d.hechos || 0}` +
        ` · percepción ${p.base_vectorial || 0} frames${p.vision ? " (visión)" : ""}` +
        ` · ${h.artefacto || ""}${(h.sdk_compilados || []).length ? " · SDK " + h.sdk_compilados.join(", ") : ""}`;
      if (e.pantalla && e.pantalla.version) pintar("/api/pipeline/pantalla?v=" + e.pantalla.version, e.pantalla.version);
    } catch (e) { /* sin servidor */ }
    buscar();
  }

  async function encargar() {
    const prompt = ($("#img-prompt") && $("#img-prompt").value.trim()) || "";
    if (!prompt) { estado("Escribe primero el prompt en «Generar imagen»"); return; }
    const r = await fetch("/api/pipeline/difusion", { method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt, negative: ($("#img-neg") && $("#img-neg").value) || "" }) });
    const j = await r.json().catch(() => ({}));
    estado(r.ok ? `Trabajo ${j.id} en cola: se pinta aquí paso a paso` : (j.detail || "No se pudo encargar"));
  }

  // ---------------------------------------------------------------- percepcion con la camara
  async function camara() {
    if (stream) { parar(); return; }
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) { estado("Este navegador no da acceso a la cámara"); return; }
    try {
      stream = await navigator.mediaDevices.getUserMedia({ video: { width: { ideal: 640 }, facingMode: "environment" }, audio: false });
    } catch (e) { estado("Sin permiso para la cámara"); return; }
    const v = $("#pipe-video");
    v.srcObject = stream; v.hidden = false; await v.play().catch(() => {});
    $("#pipe-cam").textContent = "⏹ PARAR CÁMARA";
    const cada = Math.max(1, Number($("#pipe-cada").value) || 3) * 1000;
    timer = setInterval(enviar, cada);
  }
  function parar() {
    clearInterval(timer); timer = null;
    if (stream) stream.getTracks().forEach((t) => t.stop());
    stream = null;
    const v = $("#pipe-video"); if (v) { v.srcObject = null; v.hidden = true; }
    const b = $("#pipe-cam"); if (b) b.textContent = "📷 CÁMARA";
  }
  async function enviar() {
    const v = $("#pipe-video");
    if (enviando || !v || !v.videoWidth) return;
    enviando = true;
    try {
      const w = Math.min(640, v.videoWidth), h = Math.round(v.videoHeight * w / v.videoWidth);
      const c = new OffscreenCanvas(w, h);
      c.getContext("2d").drawImage(v, 0, 0, w, h);
      const blob = await c.convertToBlob({ type: "image/jpeg", quality: 0.8 });
      const r = await fetch("/api/pipeline/frame?fuente=camara", { method: "POST", headers: { "Content-Type": "image/jpeg" }, body: blob });
      if (r.status === 429) estado("Percepción ocupada: frame descartado");
    } catch (e) { /* siguiente frame */ }
    enviando = false;
  }

  async function buscar() {
    const q = ($("#pipe-q") && $("#pipe-q").value.trim()) || "";
    try {
      const r = await fetch("/api/pipeline/percepcion?k=8&q=" + encodeURIComponent(q));
      if (!r.ok) return;
      const j = await r.json();
      const ul = $("#pipe-lista"); if (!ul) return;
      ul.innerHTML = "";
      for (const it of j.resultados || []) {
        const li = document.createElement("li");
        const t = new Date((it.ts || 0) * 1000).toLocaleTimeString();
        li.textContent = `${t} · ${it.descripcion || ""}${it.score != null ? ` (${it.score})` : ""}`;
        if (it.url) { li.style.cursor = "zoom-in"; li.onclick = () => window.open(it.url, "_blank"); }
        ul.appendChild(li);
      }
    } catch (e) { /* sin servidor */ }
  }

  function onEvent(e) {
    if (e.type === "pantalla") {
      pintar(e.url, e.version);
      estado(`Trabajo ${e.trabajo}: ${e.final ? "terminado" : "paso " + e.paso}`);
    } else if (e.type === "difusion") {
      if (e.estado === "error") estado(`Trabajo ${e.id}: ${e.error}`);
      else if (e.estado !== "hecho") estado(`Trabajo ${e.id}: ${e.estado}`);
      cargar();
    } else if (e.type === "percepcion") {
      cargar();
    }
  }

  document.addEventListener("DOMContentLoaded", () => {
    const b = (id, f) => { const el = $(id); if (el) el.onclick = f; };
    b("#pipe-go", encargar);
    b("#pipe-cam", camara);
    const q = $("#pipe-q"); if (q) q.onkeydown = (ev) => { if (ev.key === "Enter") buscar(); };
  });
  window.addEventListener("pagehide", parar);
  window.NexusPipeline = { onEvent, cargar, parar };
})();
