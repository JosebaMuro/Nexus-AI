/* Codificador de vídeo HTML (Estudio): una serie de imágenes a un vídeo WebM, todo en el navegador y sin conexión.
 *  - WebCodecs (VideoEncoder) codifica en VP9 o VP8 con la aceleración del dispositivo si la hay; el contenedor WebM
 *    lo escribe un muxer propio (EBML) de este fichero: sin bibliotecas externas.
 *  - Sin WebCodecs (navegadores antiguos) se graba el lienzo con MediaRecorder (más lento: va en tiempo real).
 *  - Transiciones: corte, fundido o Ken Burns (zoom lento). Formatos: 16:9, 9:16 y cuadrado.
 *  - El resultado se ve aquí, se descarga y se puede guardar en Agente IA (outputs/videos).
 */
(() => {
  "use strict";
  const $ = (s) => document.querySelector(s);
  let fuentes = [];   // {nombre, bmp}
  let ultimo = null;  // {blob, url}

  // ---------------------------------------------------------------- muxer WebM (EBML) minimo
  const enc = new TextEncoder();
  function vint(n) {   // tamano EBML con el minimo de bytes
    for (let len = 1; len <= 8; len++) {
      if (n < 2 ** (7 * len) - 1) {
        const b = new Uint8Array(len);
        let v = n;
        for (let i = len - 1; i >= 0; i--) { b[i] = v & 0xff; v = Math.floor(v / 256); }
        b[0] |= 1 << (8 - len);
        return b;
      }
    }
    throw new Error("tamaño EBML demasiado grande");
  }
  function idBytes(id) { const b = []; while (id > 0) { b.unshift(id & 0xff); id = Math.floor(id / 256); } return Uint8Array.from(b); }
  function cat(partes) {
    const n = partes.reduce((a, p) => a + p.length, 0), out = new Uint8Array(n);
    let o = 0; for (const p of partes) { out.set(p, o); o += p.length; }
    return out;
  }
  function el(id, datos) {
    const d = datos instanceof Uint8Array ? datos : Array.isArray(datos) ? cat(datos) : datos;
    return cat([idBytes(id), vint(d.length), d]);
  }
  function uint(id, v) {
    const b = []; let x = v; do { b.unshift(x & 0xff); x = Math.floor(x / 256); } while (x > 0);
    return el(id, Uint8Array.from(b));
  }
  function flt(id, v) { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v); return el(id, b); }
  function str(id, s) { return el(id, enc.encode(s)); }

  // chunks: [{datos: Uint8Array, ms, clave}] en orden de tiempo
  function webm(chunks, ancho, alto, codec, duracionMs) {
    const cabecera = el(0x1A45DFA3, [uint(0x4286, 1), uint(0x42F7, 1), uint(0x42F2, 4), uint(0x42F3, 8),
      str(0x4282, "webm"), uint(0x4287, 4), uint(0x4285, 2)]);
    const info = el(0x1549A966, [uint(0x2AD7B1, 1000000), flt(0x4489, duracionMs),
      str(0x4D80, "Agente IA"), str(0x5741, "Agente IA · codificador de vídeo HTML")]);
    const pista = el(0xAE, [uint(0xD7, 1), uint(0x73C5, 1), uint(0x83, 1), uint(0x9C, 0),
      str(0x86, codec === "vp09" ? "V_VP9" : "V_VP8"), el(0xE0, [uint(0xB0, ancho), uint(0xBA, alto)])]);
    const pistas = el(0x1654AE6B, [pista]);
    // un cluster en cada fotograma clave (los tiempos de los bloques son relativos y caben en 16 bits)
    const clusters = [], cues = [];
    let actual = null, base = 0, offset = 0;
    const cierra = () => { if (actual) { const c = el(0x1F43B675, actual); clusters.push(c); offset += c.length; } };
    for (const ch of chunks) {
      if (!actual || ch.clave || ch.ms - base > 30000) {
        cierra();
        base = ch.ms;
        actual = [uint(0xE7, base)];
        cues.push({ ms: base, pos: offset });
      }
      const cab = new Uint8Array(4);
      cab[0] = 0x81;   // pista 1
      new DataView(cab.buffer).setInt16(1, ch.ms - base);
      cab[3] = ch.clave ? 0x80 : 0;
      actual.push(el(0xA3, cat([cab, ch.datos])));
    }
    cierra();
    // indice de busqueda: posicion de cada cluster desde el inicio de los datos del segmento
    const antes = info.length + pistas.length;
    const indice = el(0x1C53BB6B, cues.map((c) => el(0xBB, [uint(0xB3, c.ms),
      el(0xB7, [uint(0xF7, 1), uint(0xF1, antes + c.pos)])])));
    const segmento = el(0x18538067, [info, pistas, ...clusters, indice]);
    return new Blob([cabecera, segmento], { type: "video/webm" });
  }

  // ---------------------------------------------------------------- dibujo de cada fotograma
  function dibujar(ctx, W, H, bmp, t, efecto) {
    // t: 0..1 dentro de la imagen; «contener» con fondo negro, o «cubrir» con zoom en Ken Burns
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);
    if (!bmp) return;
    const ken = efecto === "kenburns";
    const esc = ken ? Math.max(W / bmp.width, H / bmp.height) * (1.0 + 0.12 * t) : Math.min(W / bmp.width, H / bmp.height);
    const w = bmp.width * esc, h = bmp.height * esc;
    const dx = ken ? (W - w) / 2 + (t - 0.5) * W * 0.04 : (W - w) / 2, dy = (H - h) / 2;
    ctx.drawImage(bmp, dx, dy, w, h);
  }

  function fotograma(ctx, W, H, i, t, opc) {
    const a = fuentes[i].bmp, b = fuentes[i + 1] ? fuentes[i + 1].bmp : null;
    const fundido = opc.efecto !== "corte" ? Math.min(0.5, opc.trans / opc.seg) : 0;
    dibujar(ctx, W, H, a, t, opc.efecto);
    if (b && fundido > 0 && t > 1 - fundido) {
      ctx.globalAlpha = (t - (1 - fundido)) / fundido;
      dibujar(ctx, W, H, b, 0, opc.efecto);
      ctx.globalAlpha = 1;
    }
  }

  function tam(formato, alto) {
    const h = Number(alto) || 720;
    if (formato === "vertical") return [Math.round(h * 9 / 16 / 2) * 2, h];
    if (formato === "cuadrado") return [h, h];
    return [Math.round(h * 16 / 9 / 2) * 2, h];
  }

  async function codecDisponible(W, H, fps) {
    if (!("VideoEncoder" in window)) return null;
    for (const codec of ["vp09.00.10.08", "vp8"]) {
      const cfg = { codec, width: W, height: H, bitrate: Math.round(W * H * fps * 0.12), framerate: fps,
        hardwareAcceleration: "prefer-hardware", latencyMode: "quality" };
      try {
        const r = await VideoEncoder.isConfigSupported(cfg);
        if (r.supported) return r.config;
        const r2 = await VideoEncoder.isConfigSupported({ ...cfg, hardwareAcceleration: "no-preference" });
        if (r2.supported) return r2.config;
      } catch (e) { /* siguiente */ }
    }
    return null;
  }

  function estado(t) { const e = $("#venc-estado"); if (e) e.textContent = t; }
  function progreso(f) { const p = $("#venc-prog"); if (p) { p.hidden = f < 0; p.value = Math.max(0, f); } }

  async function codificarWebCodecs(cfg, W, H, opc) {
    const canvas = new OffscreenCanvas(W, H), ctx = canvas.getContext("2d");
    const chunks = [];
    let error = null;
    const encoder = new VideoEncoder({
      output: (chunk) => {
        const d = new Uint8Array(chunk.byteLength);
        chunk.copyTo(d);
        chunks.push({ datos: d, ms: Math.round(chunk.timestamp / 1000), clave: chunk.type === "key" });
      },
      error: (e) => { error = e; },
    });
    encoder.configure(cfg);
    const porImagen = Math.max(1, Math.round(opc.seg * opc.fps)), total = porImagen * fuentes.length;
    const dur = 1e6 / opc.fps;
    let n = 0;
    for (let i = 0; i < fuentes.length; i++) {
      for (let k = 0; k < porImagen; k++, n++) {
        if (error) throw error;
        fotograma(ctx, W, H, i, k / porImagen, opc);
        const vf = new VideoFrame(canvas, { timestamp: Math.round(n * dur), duration: Math.round(dur) });
        encoder.encode(vf, { keyFrame: n % (opc.fps * 2) === 0 });
        vf.close();
        // la cola del codificador no crece sin limite (memoria del movil)
        while (encoder.encodeQueueSize > 8) await new Promise((r) => setTimeout(r, 1));
        if (n % 10 === 0) { progreso(n / total); estado(`Codificando ${n}/${total} fotogramas (${cfg.codec.startsWith("vp09") ? "VP9" : "VP8"})`); await new Promise((r) => setTimeout(r, 0)); }
      }
    }
    await encoder.flush();
    encoder.close();
    if (error) throw error;
    return webm(chunks, W, H, cfg.codec.startsWith("vp09") ? "vp09" : "vp8", total * 1000 / opc.fps);
  }

  async function codificarGrabando(W, H, opc) {
    // respaldo: MediaRecorder graba el lienzo en tiempo real
    const canvas = document.createElement("canvas");
    canvas.width = W; canvas.height = H;
    const ctx = canvas.getContext("2d");
    const stream = canvas.captureStream(opc.fps);
    const tipo = ["video/webm;codecs=vp9", "video/webm;codecs=vp8", "video/webm"].find((t) => MediaRecorder.isTypeSupported(t));
    if (!tipo) throw new Error("Este navegador no puede codificar vídeo");
    const rec = new MediaRecorder(stream, { mimeType: tipo, videoBitsPerSecond: Math.round(W * H * opc.fps * 0.12) });
    const partes = [];
    rec.ondataavailable = (e) => e.data.size && partes.push(e.data);
    const fin = new Promise((r) => (rec.onstop = r));
    rec.start(1000);
    const porImagen = Math.max(1, Math.round(opc.seg * opc.fps)), total = porImagen * fuentes.length;
    let n = 0;
    for (let i = 0; i < fuentes.length; i++)
      for (let k = 0; k < porImagen; k++, n++) {
        fotograma(ctx, W, H, i, k / porImagen, opc);
        progreso(n / total);
        estado(`Grabando ${n}/${total} (tiempo real: este navegador no tiene WebCodecs)`);
        await new Promise((r) => setTimeout(r, 1000 / opc.fps));
      }
    rec.stop();
    await fin;
    return new Blob(partes, { type: "video/webm" });
  }

  async function crear() {
    if (fuentes.length < 1) { estado("Añade al menos una imagen"); return; }
    const opc = {
      seg: Math.max(0.2, Number($("#venc-seg").value) || 2),
      fps: Math.min(60, Math.max(1, Number($("#venc-fps").value) || 30)),
      efecto: $("#venc-efecto").value,
      trans: 0.6,
    };
    const [W, H] = tam($("#venc-formato").value, $("#venc-alto").value);
    const b = $("#venc-go"); b.disabled = true;
    const t0 = performance.now();
    try {
      const cfg = await codecDisponible(W, H, opc.fps);
      const blob = cfg ? await codificarWebCodecs(cfg, W, H, opc) : await codificarGrabando(W, H, opc);
      if (ultimo) URL.revokeObjectURL(ultimo.url);
      ultimo = { blob, url: URL.createObjectURL(blob) };
      const v = $("#venc-video"); v.src = ultimo.url; v.hidden = false;
      const a = $("#venc-bajar"); a.href = ultimo.url; a.download = `video-${Date.now()}.webm`; a.hidden = false;
      $("#venc-guardar").hidden = false;
      const s = ((performance.now() - t0) / 1000).toFixed(1);
      estado(`Listo: ${W}×${H}, ${(fuentes.length * opc.seg).toFixed(1)} s de vídeo, ${(blob.size / 1048576).toFixed(1)} MB en ${s} s${cfg ? " (" + (cfg.codec.startsWith("vp09") ? "VP9" : "VP8") + ")" : ""}`);
    } catch (e) {
      estado("No se pudo crear el vídeo: " + (e.message || e));
    } finally {
      b.disabled = false; progreso(-1);
    }
  }

  async function guardar() {
    if (!ultimo) return;
    const r = await fetch("/api/media/video/subir?nombre=imagenes", { method: "POST", headers: { "Content-Type": "video/webm" }, body: ultimo.blob });
    const j = await r.json().catch(() => ({}));
    estado(r.ok ? `Guardado en Agente IA: ${j.url}` : (j.detail || "No se pudo guardar"));
  }

  // ---------------------------------------------------------------- imagenes de entrada
  async function añadir(files) {
    for (const f of files) {
      if (!f.type.startsWith("image/")) continue;
      try { fuentes.push({ nombre: f.name, bmp: await createImageBitmap(f) }); } catch (e) { /* imagen no valida */ }
    }
    pintarLista();
  }
  async function deGaleria() {
    try {
      const o = await (await fetch("/api/outputs?limit=24")).json();
      const urls = (o.images || []).slice().reverse();   // de la mas antigua a la mas nueva
      for (const u of urls) {
        const r = await fetch(u); if (!r.ok) continue;
        fuentes.push({ nombre: u.split("/").pop(), bmp: await createImageBitmap(await r.blob()) });
      }
      pintarLista();
    } catch (e) { estado("No se pudo leer la galería"); }
  }
  function pintarLista() {
    const ul = $("#venc-lista"); if (!ul) return;
    ul.innerHTML = "";
    fuentes.forEach((f, i) => {
      const li = document.createElement("li");
      const c = document.createElement("canvas"); c.width = 64; c.height = 40;
      const cx = c.getContext("2d"); dibujar(cx, 64, 40, f.bmp, 0, "contener");
      li.appendChild(c);
      const s = document.createElement("span"); s.textContent = `${i + 1}. ${f.nombre}`; li.appendChild(s);
      const mk = (txt, title, fn) => { const b = document.createElement("button"); b.className = "btn sm"; b.textContent = txt; b.title = title; b.onclick = fn; li.appendChild(b); };
      mk("↑", "Subir", () => { if (i > 0) { [fuentes[i - 1], fuentes[i]] = [fuentes[i], fuentes[i - 1]]; pintarLista(); } });
      mk("✕", "Quitar", () => { fuentes.splice(i, 1)[0].bmp.close?.(); pintarLista(); });
      ul.appendChild(li);
    });
    estado(fuentes.length ? `${fuentes.length} imágenes · ${"VideoEncoder" in window ? "WebCodecs" : "MediaRecorder"}` : "Sin imágenes");
  }

  document.addEventListener("DOMContentLoaded", () => {
    const fi = $("#venc-file"), drop = $("#venc-drop");
    if (!fi || !drop) return;
    drop.onclick = () => fi.click();
    fi.onchange = () => { añadir([...fi.files]); fi.value = ""; };
    drop.ondragover = (e) => { e.preventDefault(); drop.classList.add("over"); };
    drop.ondragleave = () => drop.classList.remove("over");
    drop.ondrop = (e) => { e.preventDefault(); drop.classList.remove("over"); añadir([...e.dataTransfer.files]); };
    $("#venc-galeria").onclick = deGaleria;
    $("#venc-vaciar").onclick = () => { fuentes.forEach((f) => f.bmp.close?.()); fuentes = []; pintarLista(); };
    $("#venc-go").onclick = crear;
    $("#venc-guardar").onclick = guardar;
    pintarLista();
  });
  window.NexusVideoEncoder = { webm, tam };   // para las pruebas
})();
