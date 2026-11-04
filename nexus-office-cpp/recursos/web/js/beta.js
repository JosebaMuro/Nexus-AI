/* Nexus-Office — Laboratorio BETA: medir el rendimiento y evaluar el aspecto.
   - Se carga antes que app.js para cronometrar cada llamada a /api (tiempo total en el navegador y tiempo del núcleo,
     que llega en la cabecera Server-Timing).
   - Métricas del navegador: FCP, LCP, CLS, tareas largas, INP aproximado, memoria JS y FPS.
   - HUD de rendimiento flotante (Alt+Mayús+P), banco de pruebas del núcleo, prueba de fluidez con efectos completos
     y ligeros, vista previa de temas sin guardarlos, galería de componentes, contraste WCAG y opiniones.
   Usa window.NexusUI (app.js), window.NexusThemes (themes.js) y window.NexusDevice (device.js). */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const UI = () => window.NexusUI;
  const esc = (s) => (UI() ? UI().esc(s) : String(s ?? ""));
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => (UI() ? UI().guard(fn) : fn);
  const local = { get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } }, set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} } };
  const r1 = (x) => (x == null || isNaN(x) ? "—" : Math.round(x * 10) / 10);
  const pct = (arr, p) => { if (!arr.length) return 0; const v = [...arr].sort((a, b) => a - b); const i = p * (v.length - 1), lo = Math.floor(i), hi = Math.ceil(i); return v[lo] + (v[hi] - v[lo]) * (i - lo); };

  /* ================================================================ llamadas a la API */
  const CALLS = new Map();   // "GET /api/x/{id}" -> { ms: [], srv: [], n, err }
  const norm = (u) => { try { u = new URL(u, location.href).pathname; } catch {} return u.replace(/\/(\d+|[0-9a-f]{8,}|[0-9a-f-]{36})(?=\/|$)/gi, "/{id}"); };
  const _fetch = window.fetch.bind(window);
  window.fetch = async (input, init = {}) => {
    const url = typeof input === "string" ? input : input.url || "";
    const path = norm(url);
    if (!path.startsWith("/api/")) return _fetch(input, init);
    const key = `${(init.method || (input.method) || "GET").toUpperCase()} ${path}`, t0 = performance.now();
    try {
      const res = await _fetch(input, init);
      const ms = performance.now() - t0, m = /dur=([\d.]+)/.exec(res.headers.get("Server-Timing") || "");
      push(key, ms, m ? +m[1] : null, res.status >= 500);
      return res;
    } catch (e) { push(key, performance.now() - t0, null, true); throw e; }
  };
  function push(key, ms, srv, err) {
    const c = CALLS.get(key) || { ms: [], srv: [], n: 0, err: 0 }; CALLS.set(key, c);
    c.n++; if (err) c.err++; c.ms.push(ms); if (srv != null) c.srv.push(srv);
    if (c.ms.length > 200) c.ms.shift(); if (c.srv.length > 200) c.srv.shift();
    LAST_API.push(ms); if (LAST_API.length > 60) LAST_API.shift();
  }
  const LAST_API = [];

  /* ================================================================ métricas del navegador */
  const V = { fcp: null, lcp: null, cls: 0, longtasks: 0, longtask_ms: 0, inp: 0, dcl: null, load: null };
  const observe = (type, fn, opts = {}) => { try { new PerformanceObserver((l) => l.getEntries().forEach(fn)).observe({ type, buffered: true, ...opts }); } catch {} };
  observe("paint", (e) => { if (e.name === "first-contentful-paint") V.fcp = e.startTime; });
  observe("largest-contentful-paint", (e) => { V.lcp = e.startTime; });
  observe("layout-shift", (e) => { if (!e.hadRecentInput) V.cls += e.value; });
  observe("longtask", (e) => { V.longtasks++; V.longtask_ms += e.duration; });
  observe("event", (e) => { if (e.interactionId) V.inp = Math.max(V.inp, e.duration); }, { durationThreshold: 16 });
  addEventListener("load", () => setTimeout(() => {
    const n = performance.getEntriesByType("navigation")[0];
    if (n) { V.dcl = n.domContentLoadedEventEnd; V.load = n.loadEventEnd; }
  }, 0));
  const heapMB = () => (performance.memory ? performance.memory.usedJSHeapSize / 1048576 : null);
  function vitals() {
    return { fcp_ms: r1(V.fcp), lcp_ms: r1(V.lcp), cls: Math.round(V.cls * 1000) / 1000, tareas_largas: V.longtasks, tareas_largas_ms: r1(V.longtask_ms),
             inp_ms: r1(V.inp), dcl_ms: r1(V.dcl), load_ms: r1(V.load), heap_mb: r1(heapMB()), fps: r1(FPS.fps), frame_p95_ms: r1(FPS.p95),
             nodos_dom: document.getElementsByTagName("*").length };
  }
  function pantalla() {
    const d = window.NexusDevice ? NexusDevice.info() : {};
    return { ancho: innerWidth, alto: innerHeight, dpr: devicePixelRatio, disposicion: d.layout, cliente: d.client, entrada: d.input,
             efectos: efectos(), navegador: navigator.userAgent.slice(0, 160), nucleos: navigator.hardwareConcurrency || null, ram_gb: navigator.deviceMemory || null };
  }

  /* ================================================================ FPS */
  const FPS = { on: 0, fps: 0, p95: 0, jank: 0, frames: [], last: 0, raf: 0,
    start() { if (this.on++ === 0) { this.frames = []; this.last = performance.now(); this.raf = requestAnimationFrame((t) => this.tick(t)); } },
    stop() { if (this.on > 0 && --this.on === 0) cancelAnimationFrame(this.raf); },
    tick(t) {
      const d = t - this.last; this.last = t;
      if (d > 0 && d < 1000) { this.frames.push({ t, d }); }
      while (this.frames.length && t - this.frames[0].t > 2000) this.frames.shift();
      const ds = this.frames.map((f) => f.d);
      if (ds.length > 5) { this.fps = 1000 / (ds.reduce((a, b) => a + b, 0) / ds.length); this.p95 = pct(ds, 0.95); this.jank = ds.filter((x) => x > 50).length; }
      this.raf = requestAnimationFrame((x) => this.tick(x));
    },
    // mide durante ms milisegundos y devuelve fps medio, p95 del fotograma y tirones (> 50 ms)
    async sample(ms) {
      const ds = []; let last = performance.now();
      await new Promise((res) => { const end = last + ms; const f = (t) => { ds.push(t - last); last = t; t < end ? requestAnimationFrame(f) : res(); }; requestAnimationFrame(f); });
      ds.shift();
      const avg = ds.reduce((a, b) => a + b, 0) / Math.max(1, ds.length);
      return { fps: r1(1000 / avg), frame_p95_ms: r1(pct(ds, 0.95)), tirones: ds.filter((x) => x > 50).length };
    },
  };

  /* ================================================================ efectos visuales (completos / ligeros) */
  const efectos = () => (window.NexusDevice && NexusDevice.effects ? NexusDevice.effects() : document.documentElement.hasAttribute("data-lowpower") ? "ligero" : "completo");
  const setEfectos = (v) => { if (window.NexusDevice && NexusDevice.setEffects) NexusDevice.setEffects(v); else document.documentElement.toggleAttribute("data-lowpower", v === "ligero"); };

  /* ================================================================ HUD flotante */
  const HUD = {
    el: null, timer: 0,
    toggle(on = !this.el) {
      local.set("nexus_beta_hud", on);
      if (!on) { this.el?.remove(); this.el = null; clearInterval(this.timer); FPS.stop(); return; }
      this.el = document.createElement("div"); this.el.id = "beta-hud"; this.el.setAttribute("role", "status");
      this.el.title = "HUD de rendimiento (Alt+Mayús+P para ocultarlo)";
      document.body.appendChild(this.el); FPS.start();
      this.el.onclick = () => (location.hash = "#beta");
      this.timer = setInterval(() => this.draw(), 500); this.draw();
    },
    draw() {
      if (!this.el) return;
      const h = heapMB(), cls = FPS.fps >= 50 ? "ok" : FPS.fps >= 30 ? "warn" : "bad";
      this.el.innerHTML = `<b class="${cls}">${r1(FPS.fps)}<small> FPS</small></b>` +
        `<span>fotograma p95 ${r1(FPS.p95)} ms</span><span>tirones ${FPS.jank}</span>` +
        `<span>API p50 ${r1(pct(LAST_API, 0.5))} · p95 ${r1(pct(LAST_API, 0.95))} ms</span>` +
        `<span>tareas largas ${V.longtasks}</span>${h != null ? `<span>memoria JS ${r1(h)} MB</span>` : ""}` +
        `<span>DOM ${document.getElementsByTagName("*").length} nodos</span><span>efectos ${efectos()}</span>`;
    },
  };
  addEventListener("keydown", (e) => { if (e.altKey && e.shiftKey && (e.key === "P" || e.key === "p")) { e.preventDefault(); HUD.toggle(); syncHudBtn(); } });
  const syncHudBtn = () => { const b = $("#beta-hud-btn"); if (b) b.textContent = HUD.el ? "OCULTAR HUD" : "MOSTRAR HUD"; };

  /* ================================================================ temas: vista previa sin guardar */
  let preview = null;   // tema que se está viendo (null: el guardado)
  const savedTheme = () => { const c = UI() && UI().cfg ? UI().cfg() : {}; return c.theme && Object.keys(c.theme).length ? c.theme : { preset: "neon" }; };
  const currentTheme = () => preview || savedTheme();
  function previewTheme(t) { preview = t; NexusThemes.apply(t || savedTheme()); renderAspect(); }

  // contraste WCAG 2.x
  const lum = (hex) => { const n = parseInt(hex.slice(1), 16); return [16, 8, 0].map((s) => { const c = ((n >> s) & 255) / 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }).reduce((a, c, i) => a + c * [0.2126, 0.7152, 0.0722][i], 0); };
  const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  function contrastRows(t) {
    const pairs = [["Texto sobre fondo", t.text, t.bg, 4.5], ["Texto sobre panel", t.text, t.panel, 4.5], ["Texto secundario sobre panel", t.muted, t.panel, 4.5],
                   ["Acento sobre fondo", t.accent, t.bg, 3], ["Botón principal (texto)", t.on_accent, t.accent, 4.5], ["Error sobre panel", t.bad, t.panel, 3],
                   ["Correcto sobre panel", t.ok, t.panel, 3], ["Aviso sobre panel", t.warn, t.panel, 3]];
    return pairs.map(([k, fg, bg, min]) => { const r = ratio(fg, bg); return { k, fg, bg, r, min, ok: r >= min, aaa: r >= (min === 3 ? 4.5 : 7) }; });
  }

  /* ================================================================ vista */
  const B = { tab: local.get("nexus_beta_tab", "rend"), hist: [], last: null, fluidez: null, info: null };
  const TABS = [["rend", "Rendimiento"], ["aspecto", "Aspecto"], ["opinion", "Opiniones"]];

  async function load() {
    $("#beta-tabs").innerHTML = TABS.map(([id, n]) => `<button class="btn sm ${B.tab === id ? "primary" : "ghost"}" data-btab="${id}">${n.toUpperCase()}</button>`).join("");
    $$("[data-btab]").forEach((b) => (b.onclick = () => { B.tab = b.dataset.btab; local.set("nexus_beta_tab", B.tab); load(); }));
    $$(".beta-pane").forEach((p) => p.classList.toggle("hidden", p.dataset.pane !== B.tab));
    syncHudBtn();
    B.info = await api("/api/beta");
    $("#beta-info").textContent = `v${B.info.version} · compilado ${B.info.compilado} · ${B.info.compilador}${B.info.optimizado ? "" : " · SIN OPTIMIZAR"} · IA ${B.info.llm.backend || "—"}${B.info.llm.modelo ? " · " + B.info.llm.modelo : ""}`;
    if (B.tab === "rend") await renderRend();
    if (B.tab === "aspecto") renderAspect();
    if (B.tab === "opinion") await renderOpinions();
  }

  /* ---------------------------------------------------------------- rendimiento */
  const METRICS = [   // [sección, clave, etiqueta, unidad, mayor es mejor]
    ["cpu", "mops_1_hilo", "CPU · 1 hilo", "Mops/s", true], ["cpu", "mops_todos", "CPU · todos los hilos", "Mops/s", true],
    ["memoria", "copia_gbs", "Memoria", "GB/s", true], ["disco", "escritura_mbs", "Disco · escritura", "MB/s", true],
    ["llm", "ttft_ms_p50", "IA · primer token (p50)", "ms", false], ["llm", "tok_s_global", "IA · velocidad", "tok/s", true],
    ["api", "p95_ms", "API · p95 del núcleo", "ms", false], ["cliente", "fps", "Interfaz · FPS", "FPS", true],
  ];
  const val = (r, s, k) => (r && r[s] && typeof r[s][k] === "number" ? r[s][k] : null);
  function deltaTag(now, prev, higher) {
    if (now == null || prev == null || !prev) return "";
    const d = ((now - prev) / Math.abs(prev)) * 100; if (Math.abs(d) < 1) return `<span class="tag">= igual</span>`;
    const good = higher ? d > 0 : d < 0;
    return `<span class="tag ${good ? "low" : "high"}">${d > 0 ? "▲" : "▼"} ${Math.abs(d).toFixed(1)} %</span>`;
  }
  async function renderRend() {
    B.hist = (await api("/api/beta/bench?n=30")).mediciones;
    const last = B.hist[B.hist.length - 1], prev = B.hist[B.hist.length - 2];
    $("#beta-cards").innerHTML = METRICS.map(([s, k, label, unit, hi]) => {
      const v = val(last, s, k);
      return `<div class="panel stat beta-stat"><span class="k">${label}</span><span class="v">${v == null ? "—" : r1(v)}<small> ${unit}</small></span>${deltaTag(v, val(prev, s, k), hi)}</div>`;
    }).join("");
    const llm = last && last.llm;
    $("#beta-llm").innerHTML = !last ? `<p class="muted">Aún no hay mediciones. Pulsa <b>MEDIR AHORA</b>.</p>`
      : !llm ? `<p class="muted">La última medición no incluyó la IA.</p>` : llm.omitido ? `<div class="notice">${esc(llm.omitido)}</div>`
      : `<table class="tbl"><tr><th>#</th><th>Tipo</th><th>Primer token</th><th>Total</th><th>Tokens</th><th>tok/s</th></tr>${llm.rondas.map((r) =>
          `<tr><td>${r.ronda}</td><td>${esc(r.tipo)}</td>${r.error ? `<td colspan="4" class="bad">${esc(r.error)}</td>` : `<td>${r1(r.ttft_ms)} ms</td><td>${r1(r.total_ms)} ms</td><td>${r.tokens}</td><td>${r1(r.tok_s)}</td>`}</tr>`).join("")}</table>
        <p class="muted mono">${esc(llm.backend)} · ${esc(llm.modelo)}</p>`;
    $("#beta-hist").innerHTML = B.hist.length ? `<table class="tbl"><tr><th>Fecha</th><th>Versión</th><th>CPU 1h</th><th>CPU todos</th><th>Mem GB/s</th><th>Disco MB/s</th><th>TTFT ms</th><th>tok/s</th><th>FPS</th></tr>${
      [...B.hist].reverse().map((h) => `<tr><td>${new Date(h.ts * 1000).toLocaleString()}</td><td class="mono">${esc(h.version)}</td><td>${r1(val(h, "cpu", "mops_1_hilo"))}</td><td>${r1(val(h, "cpu", "mops_todos"))}</td><td>${r1(val(h, "memoria", "copia_gbs"))}</td><td>${r1(val(h, "disco", "escritura_mbs"))}</td><td>${r1(val(h, "llm", "ttft_ms_p50"))}</td><td>${r1(val(h, "llm", "tok_s_global"))}</td><td>${r1(val(h, "cliente", "fps"))}</td></tr>`).join("")}</table>`
      : `<p class="muted">Sin historial.</p>`;
    renderVitals(); await renderApi(); renderFluidez();
  }
  function renderVitals() {
    const v = vitals(), rate = (x, good, poor) => (x === "—" || x == null ? "" : x <= good ? "ok" : x <= poor ? "warn" : "bad");
    const rows = [["Primer pintado (FCP)", v.fcp_ms, "ms", rate(v.fcp_ms, 1800, 3000)], ["Mayor pintado (LCP)", v.lcp_ms, "ms", rate(v.lcp_ms, 2500, 4000)],
                  ["Saltos de diseño (CLS)", v.cls, "", rate(v.cls, 0.1, 0.25)], ["Respuesta a la interacción (INP aprox.)", v.inp_ms, "ms", rate(v.inp_ms, 200, 500)],
                  ["Tareas largas", v.tareas_largas, `(${v.tareas_largas_ms} ms)`, ""], ["DOM listo", v.dcl_ms, "ms", ""], ["Carga completa", v.load_ms, "ms", ""],
                  ["Memoria JS", v.heap_mb, "MB", ""], ["Nodos del DOM", v.nodos_dom, "", rate(v.nodos_dom, 1500, 3000)]];
    $("#beta-vitals").innerHTML = `<dl class="kv">${rows.map(([k, x, u, c]) => `<dt>${k}</dt><dd class="${c}">${x ?? "—"} ${u}</dd>`).join("")}</dl>`;
  }
  async function renderApi() {
    const srv = await api("/api/beta/http");
    const cli = [...CALLS.entries()].map(([k, c]) => ({ k, n: c.n, p50: pct(c.ms, 0.5), p95: pct(c.ms, 0.95), srv: c.srv.length ? pct(c.srv, 0.5) : null, err: c.err }))
      .sort((a, b) => b.p95 - a.p95).slice(0, 12);
    $("#beta-api").innerHTML = `<p class="muted">Núcleo: ${srv.total.n} peticiones · p50 ${r1(srv.total.p50_ms)} ms · p95 ${r1(srv.total.p95_ms)} ms · ${srv.total.errores} errores</p>` +
      `<table class="tbl"><tr><th>Ruta (este navegador)</th><th>n</th><th>p50</th><th>p95</th><th>núcleo p50</th><th>red + navegador</th></tr>${cli.map((c) =>
        `<tr><td class="mono">${esc(c.k)}</td><td>${c.n}</td><td>${r1(c.p50)}</td><td>${r1(c.p95)}</td><td>${r1(c.srv)}</td><td>${c.srv == null ? "—" : r1(Math.max(0, c.p50 - c.srv))}</td></tr>`).join("")}</table>` +
      `<details><summary class="muted">Rutas más lentas en el núcleo (todos los clientes)</summary><table class="tbl"><tr><th>Ruta</th><th>n</th><th>media</th><th>p50</th><th>p95</th><th>máx.</th></tr>${
        srv.rutas.slice(0, 20).map((r) => `<tr><td class="mono">${esc(r.ruta)}</td><td>${r.n}</td><td>${r1(r.media_ms)}</td><td>${r1(r.p50_ms)}</td><td>${r1(r.p95_ms)}</td><td>${r1(r.max_ms)}</td></tr>`).join("")}</table></details>`;
  }
  function renderFluidez() {
    const f = B.fluidez;
    $("#beta-fluidez").innerHTML = f ? `<table class="tbl"><tr><th>Efectos</th><th>FPS</th><th>Fotograma p95</th><th>Tirones</th></tr>${
      Object.entries(f).map(([k, x]) => `<tr><td>${k}</td><td>${x.fps}</td><td>${x.frame_p95_ms} ms</td><td>${x.tirones}</td></tr>`).join("")}</table>`
      : `<p class="muted">Mide los FPS con los efectos completos (desenfoque, estrellas, rejilla) y con los ligeros, para ver cuánto cuesta el aspecto en este equipo.</p>`;
  }
  async function runFluidez() {
    const antes = efectos(), out = {};
    for (const m of ["completo", "ligero"]) { setEfectos(m); await new Promise((r) => setTimeout(r, 300)); out[m] = await FPS.sample(3000); }
    setEfectos(antes); B.fluidez = out; renderFluidez(); return out;
  }
  async function runBench() {
    const btn = $("#beta-run"); btn.disabled = true; btn.textContent = "MIDIENDO…";
    try {
      const fl = $("#beta-opt-fluidez").checked ? await runFluidez() : null;
      FPS.start(); await new Promise((r) => setTimeout(r, 1200)); const v = vitals(); FPS.stop();
      const body = { rondas: +$("#beta-opt-rondas").value, max_tokens: +$("#beta-opt-tokens").value, llm: $("#beta-opt-llm").checked, sistema: $("#beta-opt-sis").checked,
                     cliente: { ...v, fps: fl ? fl.completo.fps : v.fps, fluidez: fl, pantalla: pantalla(), tema: currentTheme().preset || "propio" } };
      await api("/api/beta/bench", { body }); toast("Medición guardada"); await renderRend();
    } finally { btn.disabled = false; btn.textContent = "MEDIR AHORA"; }
  }

  /* ---------------------------------------------------------------- aspecto */
  function renderAspect() {
    const t = NexusThemes.resolve(currentTheme());
    $("#beta-themes").innerHTML = Object.entries(NexusThemes.PRESETS).map(([id, p]) =>
      `<button class="beta-swatch ${(currentTheme().preset || "neon") === id && !currentTheme().bg ? "on" : ""}" data-preset="${id}" title="Ver ${esc(p.name)} sin guardarlo">
        <i style="background:${p.bg}"><b style="background:${p.accent}"></b><b style="background:${p.accent2}"></b><b style="background:${p.panel}"></b></i>${esc(p.name)}</button>`).join("") +
      `<button class="btn sm ghost" id="beta-theme-cycle" title="Pasa por todos los temas, 4 s cada uno">▶ RECORRER</button>` +
      `<button class="btn sm" id="beta-theme-back" ${preview ? "" : "disabled"}>VOLVER A MI TEMA</button>`;
    $$("[data-preset]").forEach((b) => (b.onclick = () => previewTheme({ preset: b.dataset.preset })));
    $("#beta-theme-back").onclick = () => previewTheme(null);
    $("#beta-theme-cycle").onclick = cycleThemes;
    $("#beta-fx").innerHTML = ["completo", "ligero"].map((m) => `<button class="btn sm ${efectos() === m ? "primary" : "ghost"}" data-fx="${m}">${m.toUpperCase()}</button>`).join("") +
      `<button class="btn sm ghost" data-fx="auto" title="Lo que decide el dispositivo">AUTOMÁTICO</button>`;
    $$("[data-fx]").forEach((b) => (b.onclick = () => { setEfectos(b.dataset.fx); renderAspect(); }));
    const lay = window.NexusDevice ? NexusDevice.preference : "auto";
    $("#beta-layout").innerHTML = ["auto", "desktop", "tablet", "phone"].map((m) => `<button class="btn sm ${lay === m ? "primary" : "ghost"}" data-lay="${m}">${{ auto: "AUTOMÁTICO", desktop: "ESCRITORIO", tablet: "TABLET", phone: "MÓVIL" }[m]}</button>`).join("");
    $$("[data-lay]").forEach((b) => (b.onclick = () => { NexusDevice.setPreference(b.dataset.lay); renderAspect(); }));
    const rows = contrastRows(t), bad = rows.filter((r) => !r.ok).length;
    $("#beta-contrast").innerHTML = `<p class="${bad ? "warn" : "ok"}">${bad ? `${bad} combinaciones no llegan al mínimo de WCAG AA` : "Todas las combinaciones cumplen WCAG AA"} · tema «${esc(t.name || "propio")}»</p>` +
      `<table class="tbl"><tr><th>Combinación</th><th>Muestra</th><th>Contraste</th><th>AA</th><th>AAA</th></tr>${rows.map((r) =>
        `<tr><td>${r.k}</td><td><span class="beta-sample" style="color:${r.fg};background:${r.bg}">Aa 123</span></td><td>${r.r.toFixed(2)} : 1</td><td class="${r.ok ? "ok" : "bad"}">${r.ok ? "✓" : "✗"} ${r.min}</td><td class="${r.aaa ? "ok" : "muted"}">${r.aaa ? "✓" : "—"}</td></tr>`).join("")}</table>`;
  }
  let cycling = 0;
  async function cycleThemes() {
    if (cycling) { cycling = 0; return; }
    const ids = Object.keys(NexusThemes.PRESETS); cycling = 1; toast("Recorriendo los temas; pulsa de nuevo ▶ para parar");
    for (const id of ids) { if (!cycling) break; previewTheme({ preset: id }); await new Promise((r) => setTimeout(r, 4000)); }
    cycling = 0;
  }

  /* ---------------------------------------------------------------- opiniones */
  const ZONAS = [["general", "General"], ["chat", "Chat"], ["salon", "Salón"], ["nucleo", "Núcleo"], ["estudio", "Estudio"], ["bolsa", "Bolsa"], ["agentes", "Agentes"],
                 ["modelos", "Modelos"], ["herramientas", "Herramientas"], ["sistema", "Sistema"], ["ajustes", "Ajustes"], ["movil", "Móvil"], ["rendimiento", "Rendimiento"]];
  let nota = 0;
  async function renderOpinions() {
    $("#beta-zona").innerHTML = ZONAS.map(([id, n]) => `<option value="${id}">${n}</option>`).join("");
    drawStars();
    const box = $("#beta-op-list");
    if (!UI().admin()) { box.innerHTML = `<p class="muted">Entra como ADMIN para ver todas las opiniones y su resumen.</p>`; return; }
    const ops = (await api("/api/beta/opiniones?n=300")).opiniones;
    const by = {}; ops.forEach((o) => { (by[o.zona] = by[o.zona] || []).push(o.nota); });
    box.innerHTML = (ops.length ? `<div class="beta-zonas">${Object.entries(by).map(([z, v]) => {
      const m = v.reduce((a, b) => a + b, 0) / v.length;
      return `<div class="beta-zona"><span>${esc((ZONAS.find((x) => x[0] === z) || [z, z])[1])}</span><div class="bar"><i style="width:${(m / 5) * 100}%"></i></div><b>${m.toFixed(1)}</b><small class="muted">${v.length}</small></div>`;
    }).join("")}</div>` : `<p class="muted">Todavía no hay opiniones.</p>`) +
      ops.slice().reverse().slice(0, 40).map((o) => `<div class="item"><b>${"★".repeat(o.nota)}${"☆".repeat(5 - o.nota)}</b> <span class="tag">${esc(o.zona)}</span> <span class="tag">${esc(o.tema || "—")}</span> <span class="tag">${esc(o.disposicion || "—")}</span> <span class="muted">${UI().ago(o.ts)}</span>${o.texto ? `<div>${esc(o.texto)}</div>` : ""}</div>`).join("");
  }
  function drawStars() {
    $("#beta-stars").innerHTML = [1, 2, 3, 4, 5].map((n) => `<button type="button" data-star="${n}" class="${n <= nota ? "on" : ""}" aria-label="${n} de 5">★</button>`).join("");
    $$("[data-star]").forEach((b) => (b.onclick = () => { nota = +b.dataset.star; drawStars(); }));
  }
  async function sendOpinion() {
    if (!nota) throw new Error("Elige una nota de 1 a 5 estrellas");
    const t = currentTheme();
    await api("/api/beta/opinion", { body: { zona: $("#beta-zona").value, nota, texto: $("#beta-op-text").value.trim(), tema: t.preset || "propio",
      disposicion: window.NexusDevice ? NexusDevice.layout : "", efectos: efectos(), pantalla: pantalla(), vitals: vitals() } });
    nota = 0; $("#beta-op-text").value = ""; toast("¡Gracias! Opinión guardada"); await renderOpinions();
  }

  /* ================================================================ arranque */
  function wire() {
    $("#beta-run").onclick = guard(runBench);
    $("#beta-fluidez-run").onclick = guard(async () => { await runFluidez(); toast("Prueba de fluidez terminada"); });
    $("#beta-hud-btn").onclick = () => { HUD.toggle(); syncHudBtn(); };
    $("#beta-reset-api").onclick = guard(async () => { CALLS.clear(); LAST_API.length = 0; if (UI().admin()) await api("/api/beta/http", { method: "DELETE" }); await renderApi(); toast("Tiempos de la API a cero"); });
    $("#beta-export").onclick = guard(async () => { const h = await api("/api/beta/bench?n=1000"); UI().saveFile("nexus-office-beta-mediciones.json", JSON.stringify(h, null, 2), "application/json"); });
    $("#beta-report").onclick = guard(async () => { const r = await api("/api/beta/informe", { raw: true }); UI().saveFile("nexus-office-beta-informe.md", await r.text(), "text/markdown;charset=utf-8"); });
    $("#beta-op-send").onclick = guard(sendOpinion);
    $("#beta-gallery-toast").onclick = () => toast("Así se ve un aviso");
    $("#beta-gallery-toast-err").onclick = () => toast("Así se ve un error", true);
    $$("#view-beta .beta-tag-link").forEach((b) => (b.onclick = () => (location.hash = "#beta")));
  }
  document.addEventListener("DOMContentLoaded", () => {
    if (!$("#view-beta")) return;
    wire();
    $(".beta-tag")?.addEventListener("click", () => (location.hash = "#beta"));
    if (local.get("nexus_beta_hud", false)) HUD.toggle(true);
  });
  window.NexusBeta = { load, vitals, calls: () => CALLS, hud: (on) => HUD.toggle(on), fluidez: runFluidez };
})();
