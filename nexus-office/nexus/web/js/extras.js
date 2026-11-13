/* Agente IA 4.1 — paneles traídos de la edición C++ 3.x: hilo de la conversación, lista de URLs del rastreador,
   ubicación, MCP, laboratorio beta, laboratorio de bolsa y vídeo. Usa window.NexusUI (app.js). */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);
  const num = (v, d = 2) => (v === null || v === undefined || Number.isNaN(+v) ? "—" : (+v).toLocaleString("es-ES", { maximumFractionDigits: d }));
  const kv = (pares) => `<dl class="kv">${pares.filter(Boolean).map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join("")}</dl>`;
  const on = (sel, fn) => { const el = $(sel); if (el) el.onclick = guard(fn); };

  /* ---------------------------------------------------------------- hilo de la conversación */
  const sesion = () => { let s = ($("#session-select") || {}).value || ""; if (!s) { try { s = JSON.parse(localStorage.getItem("nexus_session")) || ""; } catch { s = ""; } } return s; };
  async function abrirHilo() {
    const sid = sesion();
    if (!sid) return toast("Abre o empieza una conversación primero", true);
    const h = await api(`/api/sessions/${sid}/hilo`);
    $("#hilo-notas").value = h.notas || ""; $("#hilo-resumen").value = h.resumen || "";
    $("#hilo-info").textContent = `${h.mensajes} mensajes · ${h.plegados} ya resumidos · notas ${h.tokens_notas} tokens · resumen ${h.tokens_resumen} tokens · contexto del modelo ${h.n_ctx} tokens`;
    $("#hilo-dialog").showModal();
  }
  on("#hilo-open", abrirHilo);
  on("#hilo-cerrar", () => $("#hilo-dialog").close());
  on("#hilo-guardar", async () => {
    await api(`/api/sessions/${sesion()}/hilo`, { method: "PUT", body: { notas: $("#hilo-notas").value, resumen: $("#hilo-resumen").value } });
    $("#hilo-dialog").close(); toast("Hilo guardado");
  });
  on("#hilo-resumir", async () => {
    toast("Resumiendo…");
    const h = await api(`/api/sessions/${sesion()}/hilo/resumir`, { body: { todo: true } });
    $("#hilo-resumen").value = h.resumen || "";
    toast(h.plegados_ahora ? `${h.plegados_ahora} mensajes pasados al resumen` : "No hay nada más que resumir");
  });

  /* ---------------------------------------------------------------- rastreador: lista de URLs */
  async function rastrearLista(texto) {
    const max = Math.max(1, Math.min(100000, +($("#rag-crawl-max") || {}).value || 40));
    const scope = ($("#rag-crawl-scope") || {}).value || "path", agent = ($("#rag-target") || {}).value || "";
    const r = await api("/api/rag/crawl", { body: { urls: texto, max_pages: max, max_depth: max > 500 ? 50 : 3, scope, agent, background: true } });
    toast(`${r.urls} direcciones en cola: se rastrean de dos en dos`);
    $("#rag-lista-info").textContent = `${r.urls} direcciones enviadas`;
    document.querySelector('.rail [data-view="memory"]')?.click();
  }
  on("#rag-lista-open", () => $("#lista-dialog").showModal());
  on("#lista-cerrar", () => $("#lista-dialog").close());
  on("#lista-go", async () => { const t = $("#lista-texto").value.trim(); if (!t) return toast("Pega al menos una dirección", true); await rastrearLista(t); $("#lista-texto").value = ""; $("#lista-dialog").close(); });
  on("#rag-lista-subir", () => $("#rag-lista-file").click());
  if ($("#rag-lista-file")) $("#rag-lista-file").onchange = guard(async (e) => { const f = e.target.files[0]; e.target.value = ""; if (f) await rastrearLista(await f.text()); });

  /* ---------------------------------------------------------------- ubicación */
  let vigia = null;
  function vigilarNavegador(activo) {
    if (vigia !== null) { navigator.geolocation.clearWatch(vigia); vigia = null; }
    if (!activo || !navigator.geolocation) return;
    vigia = navigator.geolocation.watchPosition((p) => {
      api("/api/ubicacion", { body: { lat: p.coords.latitude, lon: p.coords.longitude, precision: p.coords.accuracy, altitud: p.coords.altitude, velocidad: p.coords.speed, rumbo: p.coords.heading, t: p.timestamp } }).catch(() => {});
    }, () => {}, { enableHighAccuracy: true, maximumAge: 30000, timeout: 20000 });
  }
  async function cargarUbicacion() {
    const box = $("#ubi-box"); if (!box) return;
    const u = await api("/api/ubicacion");
    vigilarNavegador(u.activa && u.navegador);
    box.innerHTML = `
      <div class="row"><label><input type="checkbox" id="ubi-activa" ${u.activa ? "checked" : ""}> Los agentes pueden consultar la ubicación</label>
        <label title="El navegador pedirá permiso; la posición solo se manda a tu Agente IA"><input type="checkbox" id="ubi-nav" ${u.navegador ? "checked" : ""}> Usar la ubicación de este navegador</label></div>
      <div class="list">${u.fuentes.map((f) => `<div class="item row"><span class="tag ${f.disponible ? "ok" : "warn"}">${f.disponible ? "disponible" : "no disponible"}</span><b>${esc(f.nombre)}</b><span class="muted">${esc(f.nota || "")}</span></div>`).join("")}</div>
      <div class="row"><input class="input" id="ubi-manual" value="${esc(u.manual || "")}" placeholder="Posición fija: «43.26, -2.93» o una ciudad (Bilbao)" style="flex:1">
        <input class="input" id="ubi-serie" value="${esc(u.serie || "")}" placeholder="Puerto del GPS (COM3, /dev/ttyACM0)" style="width:230px">
        <button class="btn" id="ubi-guardar">GUARDAR</button><button class="btn primary" id="ubi-probar">¿DÓNDE ESTOY?</button></div>
      <div class="row"><input class="input" id="ubi-lugar" placeholder="Guardar un lugar: nombre (Casa, Oficina…)" style="flex:1"><input class="input" id="ubi-lugar-pos" placeholder="latitud, longitud (vacío = donde estoy ahora)" style="flex:1"><button class="btn sm" id="ubi-lugar-add">AÑADIR LUGAR</button></div>
      <div class="row" id="ubi-lugares">${(u.lugares || []).map((l, i) => `<span class="tag accent">${esc(l.nombre)} <a href="#" data-quitar="${i}" title="Quitar">✕</a></span>`).join("") || `<span class="muted">Sin lugares guardados.</span>`}</div>
      <div id="ubi-res" class="muted">${u.ultima ? `Última posición: ${esc(u.ultima.descripcion || "")} (${u.ultima.lat}, ${u.ultima.lon})` : esc(u.privacidad)}</div>`;
    const poner = (cambios) => api("/api/ubicacion", { method: "PUT", body: cambios });
    $("#ubi-activa").onchange = guard(async (e) => { await poner({ activa: e.target.checked }); cargarUbicacion(); });
    $("#ubi-nav").onchange = guard(async (e) => { await poner({ navegador: e.target.checked }); toast(e.target.checked ? "El navegador pedirá permiso de ubicación" : "Ubicación del navegador desactivada"); cargarUbicacion(); });
    on("#ubi-guardar", async () => { await poner({ manual: $("#ubi-manual").value, serie: $("#ubi-serie").value }); toast("Guardado"); cargarUbicacion(); });
    on("#ubi-probar", async () => {
      $("#ubi-res").textContent = "Buscando la posición…";
      const r = await api("/api/ubicacion/consultar", { body: {} });
      $("#ubi-res").innerHTML = r.error ? `<span class="tag bad">sin posición</span> ${esc(r.error)}` :
        `<span class="tag ok">${esc(r.fuente)}</span> <b>${esc(r.descripcion || "")}</b> · ${r.lat}, ${r.lon}${r.precision ? ` · ±${num(r.precision, 0)} m` : ""} · hace ${r.edad_s} s${r.aviso ? `<br>${esc(r.aviso)}` : ""}`;
    });
    on("#ubi-lugar-add", async () => {
      const nombre = $("#ubi-lugar").value.trim(); if (!nombre) return toast("Pon un nombre al lugar", true);
      let pos = $("#ubi-lugar-pos").value.trim().split(/[,; ]+/).map(Number);
      if (pos.length < 2 || pos.some(Number.isNaN)) { const r = await api("/api/ubicacion/consultar", { body: {} }); if (r.error) return toast(r.error, true); pos = [r.lat, r.lon]; }
      await poner({ lugares: [...(u.lugares || []), { nombre, lat: pos[0], lon: pos[1] }] }); cargarUbicacion();
    });
    $$("#ubi-lugares [data-quitar]").forEach((a) => (a.onclick = guard(async (e) => { e.preventDefault(); await poner({ lugares: u.lugares.filter((_, i) => i !== +a.dataset.quitar) }); cargarUbicacion(); })));
  }

  /* ---------------------------------------------------------------- MCP */
  async function cargarMcp() {
    const box = $("#mcp-box"); if (!box || !UI().admin()) return;
    const m = await api("/api/mcp");
    const filas = (m.servidores || []).map((s) => { const c = (m.clientes || {})[s.nombre] || {};
      return `<div class="item row"><span class="tag ${c.conectado ? "ok" : s.activo === false ? "warn" : "bad"}">${c.conectado ? "conectado" : s.activo === false ? "apagado" : "sin conexión"}</span><b>${esc(s.nombre)}</b>
        <span class="muted mono">${esc(s.url || (Array.isArray(s.orden) ? s.orden.join(" ") : s.orden) || "")}</span><span class="muted">${c.conectado ? `${c.herramientas.length} herramientas` : esc(c.error || "")}</span>
        <button class="btn sm ghost" data-mcp-quitar="${esc(s.nombre)}" style="margin-left:auto">QUITAR</button></div>`; }).join("");
    box.innerHTML = `
      <div class="row"><label><input type="checkbox" id="mcp-servidor" ${m.servidor ? "checked" : ""}> Ofrecer las ${m.herramientas} herramientas de Agente IA por MCP</label>
        <span class="tag ${m.con_token ? "ok" : "warn"}">${m.con_token ? "con token para otros equipos" : "solo desde este equipo"}</span>
        <button class="btn sm" id="mcp-token">${m.con_token ? "CAMBIAR TOKEN" : "CREAR TOKEN"}</button></div>
      <div class="muted">En otro programa con MCP añade un servidor HTTP con la dirección <span class="mono">${esc(m.url)}</span> o uno de tipo orden (stdio) con <span class="mono">${esc(m.orden)}</span>. Las herramientas de riesgo medio o alto piden aprobación aquí, como con cualquier agente.</div>
      <h3 style="margin-top:8px">Servidores MCP externos <span class="sub">sus herramientas las usan tus agentes como mcp_servidor_herramienta</span></h3>
      <div class="list">${filas || `<span class="muted">Ninguno todavía.</span>`}</div>
      <div class="row"><input class="input" id="mcp-nombre" placeholder="Nombre (ficheros)" style="width:160px"><input class="input" id="mcp-orden" placeholder="Orden (npx -y @modelcontextprotocol/server-filesystem C:\\docs) o URL (https://…/mcp)" style="flex:1">
        <select class="input" id="mcp-riesgo" style="width:auto" title="Riesgo de sus herramientas: decide si piden aprobación"><option value="medium">riesgo medio</option><option value="low">riesgo bajo</option><option value="high">riesgo alto</option></select>
        <button class="btn primary" id="mcp-add">AÑADIR</button><button class="btn" id="mcp-reconectar">RECONECTAR</button></div>`;
    const guardar = async (lista) => { await api("/api/mcp/servidores", { method: "PUT", body: { servidores: lista } }); toast("Conectando…"); setTimeout(() => cargarMcp().catch(() => {}), 2500); };
    $("#mcp-servidor").onchange = guard(async (e) => { await api("/api/mcp", { method: "PUT", body: { servidor: e.target.checked } }); cargarMcp(); });
    on("#mcp-token", async () => { const r = await api("/api/mcp", { method: "PUT", body: { nuevo_token: true } }); prompt("Token de MCP (cópialo ahora: no se vuelve a mostrar). Va en la cabecera Authorization: Bearer …", r.token); cargarMcp(); });
    on("#mcp-add", async () => {
      const nombre = $("#mcp-nombre").value.trim(), orden = $("#mcp-orden").value.trim(); if (!nombre || !orden) return toast("Pon el nombre y la orden o la URL", true);
      const nuevo = /^https?:\/\//.test(orden) ? { nombre, url: orden } : { nombre, orden };
      await guardar([...(m.servidores || []).filter((s) => s.nombre !== nombre), { ...nuevo, riesgo: $("#mcp-riesgo").value }]);
    });
    on("#mcp-reconectar", async () => { await api("/api/mcp/reconectar", { body: {} }); cargarMcp(); });
    $$("[data-mcp-quitar]", box).forEach((b) => (b.onclick = guard(() => guardar(m.servidores.filter((s) => s.nombre !== b.dataset.mcpQuitar)))));
  }

  /* ---------------------------------------------------------------- laboratorio beta */
  function pintarBanco(b) {
    if (!b || !(b.pruebas || []).length) return "";
    return `<div class="muted">${esc(b.sistema || "")} · ${new Date(b.ts * 1000).toLocaleString("es-ES")}</div><div class="list">` + b.pruebas.map((p) =>
      `<div class="item row"><span class="tag ${p.ok ? "ok" : "warn"}">${p.ok ? "ok" : "sin medir"}</span><b>${esc(p.prueba)}</b><span class="mono">${p.ok ? `${num(p.valor, 1)} ${esc(p.unidad)}` : esc(p.error || "")}</span>
       <span class="muted mono" style="margin-left:auto">${p.primer_token_ms ? `primer token ${num(p.primer_token_ms, 0)} ms · ` : ""}${num(p.ms, 0)} ms</span></div>`).join("") + "</div>";
  }
  async function cargarBeta() { const r = $("#beta-result"); if (r && !r.dataset.tocado) r.innerHTML = pintarBanco(await api("/api/beta/bench")); }
  on("#beta-bench", async () => { const r = $("#beta-result"); r.dataset.tocado = "1"; r.innerHTML = `<span class="muted">Midiendo base de datos, embeddings, memoria, biblioteca y modelo…</span>`; r.innerHTML = pintarBanco(await api("/api/beta/bench", { body: { llm: true } })); });
  on("#beta-http", async () => {
    const t = await api("/api/beta/http"), r = $("#beta-result"); r.dataset.tocado = "1";
    r.innerHTML = `<div class="muted">${t.peticiones} peticiones en ${t.total_rutas} rutas desde el arranque. Cada respuesta lleva además la cabecera Server-Timing.</div><div class="list">` +
      t.rutas.slice(0, 25).map((f) => `<div class="item row"><b class="mono">${esc(f.ruta)}</b><span class="muted">${f.n} veces</span><span class="mono" style="margin-left:auto">media ${num(f.media_ms, 1)} ms · p95 ${num(f.p95_ms, 1)} ms · máx. ${num(f.max_ms, 0)} ms${f.errores ? ` · ${f.errores} errores` : ""}</span></div>`).join("") + "</div>";
  });
  on("#beta-informe", async () => { const res = await api("/api/beta/informe", { raw: true }); UI().saveFile("informe-beta-agente-ia.md", await res.text(), "text/markdown"); });
  on("#beta-opinar", async () => { await api("/api/beta/opinion", { body: { texto: $("#beta-opinion").value, nota: +$("#beta-nota").value, vista: "sistema" } }); $("#beta-opinion").value = ""; toast("Gracias: opinión guardada en este equipo"); });

  /* ---------------------------------------------------------------- laboratorio de bolsa */
  const labDatos = () => { const simbolos = $("#lab-syms").value.trim() || ($("#bo-sym") || {}).value || ""; if (!simbolos) throw new Error("Indica al menos un valor"); return { simbolos, rango: $("#lab-rango").value, estrategia: $("#lab-estrategia").value }; };
  const labOut = (html) => { $("#lab-result").innerHTML = html; };
  // un fallo (sin historico, sin conexion...) se explica en el propio panel, no solo en un aviso que desaparece
  const lab = (sel, fn) => { const el = $(sel); if (el) el.onclick = async () => { try { await fn(); } catch (e) { labOut(`<span class="tag bad">no se pudo</span> ${esc(e.message)}`); } }; };
  const fila = (m) => m && m.periodos ? [["Rentabilidad", `${num(m.rentabilidad_pct)} %`], ["Anualizada", `${num(m.anual_pct)} %`], ["Volatilidad", `${num(m.volatilidad_pct)} %`], ["Sharpe", num(m.sharpe)], ["Sortino", num(m.sortino)], ["Caída máxima", `${num(m.caida_maxima_pct)} %`]] : [];
  function curvaSvg(curva) {
    if (!curva || curva.length < 2) return "";
    const W = 640, H = 160, vs = curva.flatMap((c) => [c.valor, c.referencia]), lo = Math.min(...vs), hi = Math.max(...vs), dx = W / (curva.length - 1);
    const linea = (k) => curva.map((c, i) => `${(i * dx).toFixed(1)},${(H - 4 - ((c[k] - lo) / (hi - lo || 1)) * (H - 8)).toFixed(1)}`).join(" ");
    return `<svg viewBox="0 0 ${W} ${H}" style="width:100%;height:160px"><polyline points="${linea("referencia")}" fill="none" stroke="var(--muted)" stroke-width="1.2" stroke-dasharray="4 3"/><polyline points="${linea("valor")}" fill="none" stroke="var(--accent)" stroke-width="2"/></svg>
      <div class="row mono muted"><span style="color:var(--accent)">━ estrategia</span><span>┄ comprar y mantener</span></div>`;
  }
  lab("#lab-backtest", async () => {
    labOut(`<span class="muted">Probando la estrategia con el histórico…</span>`);
    const r = await api("/api/bolsa/lab/backtest", { body: labDatos() });
    labOut(`<div class="row"><span class="tag ${r.datos_reales ? "ok" : "warn"}">${r.datos_reales ? "datos reales" : "datos simulados (sin conexión)"}</span><b>${esc(r.veredicto || "")}</b></div>${curvaSvg(r.curva)}
      <div class="grid g3"><div><h3>Estrategia</h3>${kv([...fila(r.metricas), ["Valor final", `${num(r.valor_final)} €`], ["Gastos", `${num(r.gastos)} €`]])}</div>
      <div><h3>Comprar y mantener</h3>${kv(fila(r.comprar_mantener))}</div>
      <div><h3>Operaciones</h3>${kv([["Cerradas", r.operaciones.operaciones], r.operaciones.operaciones ? ["Aciertos", `${num(r.operaciones.aciertos_pct, 1)} %`] : null, r.operaciones.operaciones ? ["Media por operación", `${num(r.operaciones.esperanza)} €`] : null,
        r.fuera_muestra ? ["Fuera de muestra", `${num(r.fuera_muestra.rentabilidad_pct)} %`] : null, r.en_muestra ? ["En muestra", `${num(r.en_muestra.rentabilidad_pct)} %`] : null])}</div></div>
      <div class="muted">${esc(r.aviso)}</div>`);
  });
  lab("#lab-senales", async () => {
    const d = labDatos(), sym = d.simbolos.split(/[,; ]+/)[0];
    const r = await api(`/api/bolsa/lab/senales/${encodeURIComponent(sym)}?rango=${d.rango}`);
    labOut(`<div class="row"><b>${esc(r.simbolo)}</b><span class="tag ${r.lectura === "alcista" ? "ok" : r.lectura === "bajista" ? "bad" : "warn"}">${esc(r.lectura)}</span><span class="mono">señal ${num(r.combinada)} · confianza ${num(r.confianza)}</span></div>
      <div class="list">${Object.entries(r.estrategias).map(([k, v]) => `<div class="item row"><b>${esc(k)}</b><span class="mono">${v.senal > 0 ? "+" : ""}${num(v.senal)}</span><span class="muted">${esc(v.detalle)}</span><span class="mono muted" style="margin-left:auto">confianza ${num(v.confianza)}</span></div>`).join("")}</div>`);
  });
  lab("#lab-mesa", async () => {
    const d = labDatos();
    labOut(`<span class="muted">La mesa de analistas está trabajando…</span>`);
    const r = await api("/api/bolsa/lab/mesa", { body: { simbolo: d.simbolos.split(/[,; ]+/)[0], rango: d.rango } }), x = r.decision;
    labOut(`<div class="row"><b>${esc(r.simbolo)}</b><span class="tag ${x.accion === "comprar" ? "ok" : x.accion === "vender" ? "bad" : "warn"}">${esc(x.accion)}</span><span class="mono">confianza ${num(x.confianza)} · peso ${num(x.peso_pct, 1)} % · parada ${num(x.parada_pct, 1)} %</span><span class="tag accent">${r.con_ia ? "con IA" : "por reglas"}</span></div>
      <div>${esc(x.motivo)}</div>
      <div class="list"><div class="item"><b>Técnico</b><br>${esc(r.analistas.tecnico)}</div><div class="item"><b>Noticias</b><br>${esc(r.analistas.noticias)}</div>
      <div class="item"><b>Alcista</b><br>${esc(r.debate.alcista)}</div><div class="item"><b>Bajista</b><br>${esc(r.debate.bajista)}</div>
      <div class="item"><b>Riesgo</b><br>Volatilidad anual ${num(r.riesgo.volatilidad_anual_pct, 1)} % · a ${num(r.riesgo.distancia_maximo_pct, 1)} % del máximo · peso agresivo ${num(r.riesgo.pesos_por_perfil.agresivo, 1)} %, neutral ${num(r.riesgo.pesos_por_perfil.neutral, 1)} %, conservador ${num(r.riesgo.pesos_por_perfil.conservador, 1)} %</div></div>
      <div class="muted">${esc(r.aviso)}</div>`);
  });
  lab("#lab-mc", async () => {
    const d = labDatos(), r = await api("/api/bolsa/lab/montecarlo", { body: { simbolo: d.simbolos.split(/[,; ]+/)[0], rango: d.rango, simulaciones: 1000 } });
    labOut(`<div class="row"><b>${esc(r.simbolo)}</b><span class="muted">${r.simulaciones} simulaciones a ${r.horizonte} sesiones</span></div>` + kv([["Resultado mediano", `${num(r.mediana_pct)} %`], ["Entre (5 % – 95 %)", `${num(r.p5_pct)} % … ${num(r.p95_pct)} %`], ["Probabilidad de perder", `${num(r.prob_perder_pct, 1)} %`], ["Caída máxima habitual", `${num(r.caida_maxima_mediana_pct)} %`], ["Caída máxima en el peor 5 %", `${num(r.caida_maxima_p5_pct)} %`]]));
  });
  lab("#lab-opt", async () => {
    const d = labDatos(); let html = "";
    for (const [metodo, nombre] of [["paridad_riesgo", "Paridad de riesgo"], ["min_varianza", "Varianza mínima"], ["inversa_vol", "Inversa de la volatilidad"], ["igual", "A partes iguales"]]) {
      const r = await api("/api/bolsa/lab/optimizar", { body: { ...d, metodo } });
      html += `<div class="item"><b>${nombre}</b> <span class="mono muted">volatilidad ${num(r.volatilidad_anual_pct, 1)} % · Sharpe ${num(r.sharpe)}</span><br><span class="mono">${Object.entries(r.pesos).map(([s, p]) => `${esc(s)} ${num(p, 1)} %`).join(" · ")}</span></div>`;
    }
    labOut(`<div class="list">${html}</div>`);
  });

  /* ---------------------------------------------------------------- vídeo */
  const elegidas = new Set();
  const galeria = $("#gallery");
  if (galeria) galeria.addEventListener("click", (e) => {
    if (!(e.ctrlKey || e.metaKey) || e.target.tagName !== "IMG") return;
    e.stopPropagation(); e.preventDefault();
    const u = new URL(e.target.src).pathname; elegidas.has(u) ? elegidas.delete(u) : elegidas.add(u);
    e.target.style.outline = elegidas.has(u) ? "3px solid var(--accent)" : "";
  }, true);
  async function cargarVideos() {
    const box = $("#vid-list"); if (!box) return;
    const vs = await api("/api/video");
    box.innerHTML = vs.map((v) => `<div class="item row"><a href="${esc(v.url)}" download>${esc(v.nombre)}</a><span class="muted mono" style="margin-left:auto">${UI().fmtB(v.bytes)} · ${UI().ago(v.ts)}</span></div>`).join("");
  }
  on("#vid-go", async () => {
    $("#vid-result").innerHTML = `<span class="muted">Componiendo el vídeo…</span>`;
    const r = await api("/api/video/rapido", { body: { imagenes: [...elegidas], ultimas: +$("#vid-n").value || 6, segundos: +$("#vid-seg").value || 3, formato: $("#vid-formato").value, titulo: $("#vid-titulo").value } });
    $("#vid-result").innerHTML = `<span class="tag ok">listo</span> ${r.imagenes} imágenes · ${num(r.seconds, 1)} s · ${r.width}×${r.height} · <a href="${esc(r.url)}" download>descargar vídeo</a><br><span class="muted">${esc(r.nota)}</span>`;
    elegidas.clear(); $$("#gallery img").forEach((i) => (i.style.outline = "")); cargarVideos();
  });

  /* ---------------------------------------------------------------- carga al entrar en cada vista */
  const porVista = { settings: () => { cargarUbicacion().catch(() => {}); cargarMcp().catch(() => {}); }, system: () => cargarBeta().catch(() => {}), studio: () => cargarVideos().catch(() => {}) };
  const mirar = new MutationObserver((ms) => ms.forEach((m) => { const v = m.target; if (v.classList.contains("on")) { const fn = porVista[v.id.replace("view-", "")]; if (fn && UI()) fn(); } }));
  $$("section.view").forEach((v) => mirar.observe(v, { attributes: true, attributeFilter: ["class"] }));
  // la ubicacion del navegador sigue mandandose aunque no se abra Ajustes
  setTimeout(() => { if (UI()) api("/api/ubicacion").then((u) => vigilarNavegador(u.activa && u.navegador)).catch(() => {}); }, 4000);
})();
