/* Agente IA 4.2 — Mundo (fuentes por categorías y países), terminal de bolsa y Terminal IA. Usa window.NexusUI. */
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
  const num = (v, d = 2) => (v === null || v === undefined || v === "" || Number.isNaN(+v) ? (v ?? "—") : (+v).toLocaleString("es-ES", { maximumFractionDigits: d }));
  const store = { get: (k, d) => { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
                  set: (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* sin almacenamiento */ } } };
  const minutos = (s) => (s >= 3600 ? `${num(s / 3600, 1)} h` : `${Math.round(s / 60)} min`);

  /* tabla genérica a partir de filas (objetos) */
  function tabla(filas, max = 60, cols = null) {
    if (!filas || !filas.length) return '<div class="muted">Sin datos.</div>';
    const c = cols || [...new Set(filas.slice(0, 40).flatMap((f) => Object.keys(f)))].filter((k) => typeof filas[0][k] !== "object" || filas[0][k] === null).slice(0, 11);
    const celda = (v) => (typeof v === "number" ? num(v, Math.abs(v) < 10 ? 4 : 2) : typeof v === "string" && /^https?:\/\//.test(v) ? `<a href="${esc(v)}" target="_blank" rel="noopener">enlace</a>` : esc(v === null || v === undefined ? "" : typeof v === "object" ? JSON.stringify(v).slice(0, 80) : String(v).slice(0, 160)));
    return `<div style="overflow-x:auto"><table class="tbl"><tr>${c.map((k) => `<th>${esc(k.replace(/_/g, " "))}</th>`).join("")}</tr>${filas.slice(0, max).map((f) => `<tr>${c.map((k) => `<td>${celda(f[k])}</td>`).join("")}</tr>`).join("")}</table></div>` + (filas.length > max ? `<div class="muted">… y ${filas.length - max} filas más</div>` : "");
  }
  function marco(m, conBoton = true) {
    const pie = `<div class="muted mono">${esc(m.fuente || "")} · ${esc(m.recibido || "")}${m.motivo ? " · " + esc(m.motivo) : ""}</div>`;
    const extra = ["subyacente", "dias", "diferencial_10a_2a", "curva_invertida", "vix", "dominancia"].filter((k) => m[k] !== undefined && m[k] !== null)
      .map((k) => `<span class="tag accent">${esc(k.replace(/_/g, " "))}: ${esc(typeof m[k] === "object" ? Object.entries(m[k]).map(([a, b]) => `${a} ${b}`).join(", ") : m[k])}</span>`).join(" ");
    const boton = conBoton && m.receta ? `<button class="btn sm" data-anadir='${esc(JSON.stringify({ receta: m.receta, params: m.params || {}, titulo: m.titulo }))}'>+ ESCRITORIO</button>` : "";
    return `<div class="panel col" style="padding:12px"><div class="row"><b style="flex:1">${esc(m.titulo || m.receta || "")}</b>${boton}</div>${extra ? `<div class="row">${extra}</div>` : ""}${tabla(m.filas)}${pie}</div>`;
  }
  function kv(obj, salvo = []) {
    return `<dl class="kv">${Object.entries(obj).filter(([k, v]) => !salvo.includes(k) && (typeof v !== "object" || v === null)).map(([k, v]) => `<dt>${esc(k.replace(/_/g, " "))}</dt><dd>${esc(typeof v === "number" ? num(v, 4) : v ?? "—")}</dd>`).join("")}</dl>`;
  }

  /* ================================================================ terminal de bolsa */
  const tbOut = (html) => { $("#tb-result").innerHTML = html; enlazarAnadir(); };
  function enlazarAnadir() {
    $$("[data-anadir]", $("#tb-result")).forEach((b) => (b.onclick = guard(async () => {
      const w = JSON.parse(b.dataset.anadir);
      const actual = (await api("/api/mercados/escritorio?datos=false")).paneles;
      await api("/api/mercados/escritorio", { body: { paneles: [...actual, w] } });
      toast("Añadido al escritorio");
    })));
  }
  const herr = (nombre, args) => api(`/api/mercados/herramienta/${nombre}`, { body: args });
  on("#tb-go", async () => {
    const q = $("#tb-q").value.trim(); if (!q) return toast("Escribe una pregunta", true);
    tbOut('<div class="muted">Consultando…</div>');
    const r = await herr("mercado_panel", { pregunta: q });
    tbOut(r.marcos.map((m) => marco(m)).join("") + r.errores.map((e) => `<div class="muted">${esc(e.receta)}: ${esc(e.error)}</div>`).join("") + `<div class="muted">${esc(r.aviso)}</div>`);
  });
  if ($("#tb-q")) $("#tb-q").onkeydown = (e) => { if (e.key === "Enter") $("#tb-go").click(); };
  on("#tb-escritorio", async () => {
    tbOut('<div class="muted">Cargando el escritorio…</div>');
    const r = await api("/api/mercados/escritorio");
    tbOut(`<div class="grid g2">${r.paneles.map((w) => (w.marco ? marco({ ...w.marco, titulo: w.titulo }, false) : `<div class="panel"><b>${esc(w.titulo)}</b><div class="muted">${esc(w.error || "")}</div></div>`)).join("")}</div>
      <div class="muted">Para quitar paneles, edita la lista desde Ajustes avanzados o pide al agente de bolsa que reorganice el escritorio.</div>`);
  });
  const FORMS = {
    opciones: [`<div class="row"><input class="input" id="tf-sym" placeholder="Símbolo (AAPL) o vacío" style="width:140px"><input class="input" id="tf-sub" type="number" step="any" placeholder="Subyacente" style="width:120px">
      <select class="input" id="tf-estr" style="width:auto"></select><input class="input" id="tf-dias" type="number" placeholder="Días" value="30" style="width:80px"><input class="input" id="tf-vol" type="number" placeholder="Volatilidad %" value="25" style="width:110px"><button class="btn primary" id="tf-go">CALCULAR</button></div>`,
      async () => {
        const r = await herr("bolsa_opciones", { simbolo: $("#tf-sym").value.trim(), subyacente: +$("#tf-sub").value || undefined, estrategia: $("#tf-estr").value, dias: +$("#tf-dias").value || 30, volatilidad_pct: +$("#tf-vol").value || 25 });
        return kv(r, ["curva"]) + (r.griegas ? kv(r.griegas) : "") + tabla(r.patas) + `<div class="muted">Puntos de equilibrio: ${esc((r.puntos_equilibrio || []).join(", "))} · primas ${esc(r.primas)}</div>` + curva(r.curva);
      }],
    riesgo: [`<div class="row"><input class="input" id="tf-syms" placeholder="Valores (SAN.MC, BBVA.MC, AAPL)" style="flex:1 1 240px"><input class="input" id="tf-cap" type="number" value="10000" style="width:110px" title="Capital">
      <select class="input" id="tf-modo" style="width:auto"><option value="cartera">VaR y correlaciones</option><option value="estres">Escenarios de estrés (cartera de la mesa)</option></select><button class="btn primary" id="tf-go">CALCULAR</button></div>`,
      async () => {
        const modo = $("#tf-modo").value;
        const r = await herr("bolsa_riesgo", { modo, simbolos: $("#tf-syms").value.split(/[,; ]+/).filter(Boolean), capital: +$("#tf-cap").value || 10000 });
        return modo === "estres" ? tabla(r.escenarios, 20, ["nombre", "resultado", "resultado_pct"]) : kv(r) + (r.correlaciones ? tabla(r.correlaciones) : "");
      }],
    posicion: [`<div class="row"><input class="input" id="tf-cap" type="number" value="10000" style="width:110px" title="Capital"><input class="input" id="tf-rp" type="number" value="1" step="0.1" style="width:90px" title="Riesgo %">
      <input class="input" id="tf-ent" type="number" step="any" placeholder="Entrada" style="width:100px"><input class="input" id="tf-stop" type="number" step="any" placeholder="Stop" style="width:100px"><input class="input" id="tf-obj" type="number" step="any" placeholder="Objetivo" style="width:100px">
      <input class="input" id="tf-prob" type="number" placeholder="Acierto %" style="width:100px"><button class="btn primary" id="tf-go">CALCULAR</button></div>`,
      async () => {
        const r = await herr("bolsa_riesgo", { modo: "posicion", capital: +$("#tf-cap").value, riesgo_pct: +$("#tf-rp").value, entrada: +$("#tf-ent").value, stop: +$("#tf-stop").value, objetivo: +$("#tf-obj").value || undefined, prob_acierto: +$("#tf-prob").value || undefined });
        return kv(r) + (r.kelly ? kv(r.kelly) : "");
      }],
    criba: [`<div class="row"><select class="input" id="tf-criba" style="width:auto"></select><input class="input" id="tf-mkt" value="US" style="width:80px" title="Mercado (US, ES, DE…)">
      <input class="input" id="tf-filtros" placeholder="o filtros propios: per < 15, roe_pct > 20" style="flex:1 1 220px"><input class="input" id="tf-formula" placeholder="Orden: roe_pct / per" style="width:160px"><button class="btn primary" id="tf-go">CRIBAR</button></div>`,
      async () => {
        const r = await herr("bolsa_criba", { criba: $("#tf-filtros").value.trim() ? "" : $("#tf-criba").value, mercado: $("#tf-mkt").value, filtros: $("#tf-filtros").value.trim(), formula: $("#tf-formula").value.trim() });
        return marco({ ...r, receta: "" }, false);
      }],
    valoracion: [`<div class="row"><select class="input" id="tf-vm" style="width:auto"><option value="dcf">Valor por acción</option><option value="dcf_inverso">Crecimiento que descuenta el precio</option></select>
      <input class="input" id="tf-fcf" type="number" step="any" placeholder="Flujo de caja libre" style="width:150px"><input class="input" id="tf-g" type="number" step="any" placeholder="Crecimiento %" value="6" style="width:110px">
      <input class="input" id="tf-r" type="number" step="any" placeholder="Descuento %" value="9" style="width:100px"><input class="input" id="tf-acc" type="number" step="any" placeholder="Acciones" style="width:120px">
      <input class="input" id="tf-px" type="number" step="any" placeholder="Precio" style="width:90px"><input class="input" id="tf-deuda" type="number" step="any" placeholder="Deuda neta" style="width:110px"><button class="btn primary" id="tf-go">VALORAR</button></div>`,
      async () => {
        const r = await herr("bolsa_valoracion", { modo: $("#tf-vm").value, flujo_caja: +$("#tf-fcf").value, crecimiento_pct: +$("#tf-g").value, descuento_pct: +$("#tf-r").value, acciones: +$("#tf-acc").value || 0, precio: +$("#tf-px").value || 0, deuda_neta: +$("#tf-deuda").value || 0 });
        return kv(r) + (r.flujos ? tabla(r.flujos) : "");
      }],
    alertas: [`<div class="row"><input class="input" id="tf-sym" placeholder="Símbolo" style="width:120px"><input class="input" id="tf-cond" placeholder="precio > 200, rsi < 30" style="flex:1 1 220px">
      <input class="input" id="tf-nota" placeholder="Nota" style="width:160px"><button class="btn primary" id="tf-go">CREAR</button><button class="btn" id="tf-rev">REVISAR AHORA</button></div>`,
      async (accion = "listar") => {
        if (accion === "crear") await herr("bolsa_alerta", { accion: "crear", simbolo: $("#tf-sym").value, condiciones: $("#tf-cond").value, nota: $("#tf-nota").value });
        if (accion === "revisar") { const r = await herr("bolsa_alerta", { accion: "revisar" }); toast(`${r.saltan.length} alertas se cumplen ahora`); }
        const r = await herr("bolsa_alerta", { accion: "listar" });
        return tabla(r.alertas.map((a) => ({ id: a.id, simbolo: a.simbolo, condiciones: a.condiciones.map((c) => `${c.campo} ${c.op} ${c.valor}`).join(a.todas ? " y " : " o "), nota: a.nota, veces: a.veces }))) +
          '<div class="muted">Se revisan solas cada 5 minutos (fuente «alertas_bolsa» de Mundo) y aparecen en la categoría Mercados.</div>';
      }],
    expedientes: [`<div class="row"><input class="input" id="tf-sym" placeholder="Símbolo" style="width:120px"><input class="input" id="tf-tesis" placeholder="Tesis (opcional)" style="flex:1 1 220px">
      <input class="input" id="tf-ev" placeholder="Evidencia (opcional)" style="flex:1 1 200px"><input class="input" id="tf-fu" placeholder="Fuente" style="width:120px"><button class="btn primary" id="tf-go">GUARDAR / VER</button></div>`,
      async () => {
        const sym = $("#tf-sym").value.trim();
        if (!sym) return tabla((await herr("bolsa_expediente", { accion: "listar" })).expedientes);
        const args = { accion: $("#tf-tesis").value || $("#tf-ev").value ? "guardar" : "leer", simbolo: sym, tesis: $("#tf-tesis").value || undefined, evidencia: $("#tf-ev").value || undefined, fuente: $("#tf-fu").value || undefined };
        const e = await herr("bolsa_expediente", args);
        return `<div><b>${esc(e.simbolo)}</b>: ${esc(e.tesis || "sin tesis")}</div>` + tabla(e.evidencias || [], 30, ["texto", "fuente", "a_favor"]) + tabla(e.supuestos || [], 20, ["texto"]) + tabla(e.revisiones || [], 20, ["texto"]);
      }],
    informe: [`<div class="row"><input class="input" id="tf-sym" placeholder="Símbolo de EE. UU. (AAPL)" style="width:180px"><select class="input" id="tf-tipo" style="width:auto"><option>10-K</option><option>10-Q</option><option>20-F</option></select>
      <select class="input" id="tf-acc" style="width:auto"><option value="leer">Impulsores y frenos con citas</option><option value="cifras">Cifras contables</option><option value="lista">Lista de presentaciones</option></select><button class="btn primary" id="tf-go">LEER</button></div>`,
      async () => {
        const acc = $("#tf-acc").value;
        const r = await herr("bolsa_informe_oficial", { accion: acc, simbolo: $("#tf-sym").value.trim(), tipo: $("#tf-tipo").value, comparar: acc === "leer" });
        if (acc === "lista") return tabla(r.presentaciones);
        if (acc === "cifras") return tabla(r.filas) + `<div class="muted">${esc(r.nota)}</div>`;
        return `<div class="muted">${esc(r.nombre)} · ${esc(r.informe.tipo)} del ${esc(r.informe.fecha)} · ${esc(r.nota)}</div>` + r.impulsores.map((i) => `<div class="panel" style="padding:10px"><b>${esc(i.nombre)}</b> <span class="tag ${i.balance === "impulso" ? "ok" : i.balance === "freno" ? "bad" : "warn"}">${esc(i.balance)}</span> <span class="muted">${i.menciones} menciones</span>${i.citas.map((c) => `<blockquote class="muted" style="margin:6px 0 0 8px">“${esc(c.cita)}”</blockquote>`).join("")}</div>`).join("") + (r.riesgos_nuevos && r.riesgos_nuevos.length ? `<b>Riesgos nuevos frente al informe anterior</b>${r.riesgos_nuevos.map((x) => `<blockquote class="muted">“${esc(x)}”</blockquote>`).join("")}` : "");
      }],
  };
  function curva(pts) {
    if (!pts || pts.length < 2) return "";
    const xs = pts.map((p) => p.precio), ys = pts.map((p) => p.resultado), w = 600, h = 160;
    const x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(0, ...ys), y1 = Math.max(0, ...ys);
    const X = (x) => ((x - x0) / (x1 - x0 || 1)) * w, Y = (y) => h - ((y - y0) / (y1 - y0 || 1)) * h;
    return `<svg viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" style="width:100%;height:160px"><line x1="0" x2="${w}" y1="${Y(0)}" y2="${Y(0)}" stroke="currentColor" opacity=".3"/><polyline fill="none" stroke="var(--accent)" stroke-width="2" points="${pts.map((p) => `${X(p.precio)},${Y(p.resultado)}`).join(" ")}"/></svg>`;
  }
  let recetas = null;
  $$("#tb-tabs [data-tb]").forEach((b) => (b.onclick = guard(async () => {
    const [html, fn] = FORMS[b.dataset.tb];
    $("#tb-form").innerHTML = html; tbOut("");
    if (!recetas) recetas = await api("/api/mercados/recetas");
    if ($("#tf-estr")) $("#tf-estr").innerHTML = Object.entries(recetas.estrategias).map(([k, v]) => `<option value="${k}">${esc(v)}</option>`).join("");
    if ($("#tf-criba")) $("#tf-criba").innerHTML = Object.entries(recetas.cribas).map(([k, v]) => `<option value="${k}">${esc(v)}</option>`).join("");
    const correr = (arg) => guard(async () => { tbOut('<div class="muted">Calculando…</div>'); tbOut(await fn(arg)); });
    if ($("#tf-go")) $("#tf-go").onclick = correr(b.dataset.tb === "alertas" ? "crear" : undefined);
    if ($("#tf-rev")) $("#tf-rev").onclick = correr("revisar");
    if (b.dataset.tb === "alertas" || b.dataset.tb === "expedientes") correr()();
  })));

  /* ================================================================ Mundo */
  let muEstado = null, muPaises = [];
  async function muCargar() {
    const [e, r] = await Promise.all([api("/api/mundo"), api("/api/mundo/resumen")]);
    muEstado = e;
    if (!muPaises.length) {
      muPaises = (await api("/api/mundo/paises")).paises;
      $("#mu-pais").innerHTML = '<option value="">Todos los países</option>' + muPaises.map((p) => `<option value="${p.codigo}">${esc(p.nombre)}</option>`).join("");
      $("#mu-cat").innerHTML = '<option value="">Todas las categorías</option>' + Object.entries(e.categorias).map(([k, v]) => `<option value="${k}">${esc(v)}</option>`).join("");
    }
    const ok = e.fuentes.filter((f) => f.activa && f.ultimo_ok).length, mal = e.fuentes.filter((f) => f.activa && f.error).length;
    $("#mu-stats").innerHTML = [["Registros", e.total], ["Fuentes activas", e.fuentes.filter((f) => f.activa).length], ["Al día", ok], ["Con error", mal]]
      .map(([k, v]) => `<div class="panel stat"><span class="v">${num(v, 0)}</span><span class="k">${k}</span></div>`).join("");
    $("#mu-sub").textContent = e.activo ? `activo · países de interés ${e.paises.join(", ")}${e.sin_red ? " · sin red: en pausa" : ""}` : "apagado";
    const cats = r.categorias.map((c) => c.id), paises = r.paises.slice(0, 12);
    $("#mu-matriz").innerHTML = cats.length ? `<div style="overflow-x:auto"><table class="tbl"><tr><th>categoría</th>${paises.map((p) => `<th title="${esc(p.nombre)}">${esc(p.pais || "—")}</th>`).join("")}<th>total</th></tr>${r.categorias.map((c) => `<tr><td><a href="#" data-mu-cat="${c.id}">${esc(c.nombre.split(" (")[0])}</a></td>${paises.map((p) => { const n = (r.por_categoria_y_pais[c.id] || {})[p.pais] || 0; return `<td>${n ? `<a href="#" data-mu-cat="${c.id}" data-mu-pais="${p.pais}">${n}</a>` : ""}</td>`; }).join("")}<td>${c.registros}</td></tr>`).join("")}</table></div>` : '<div class="muted">Todavía no ha llegado nada. Las fuentes se piden solas cada cierto tiempo, o pulsa «Pedir las pendientes ahora».</div>';
    $$("[data-mu-cat]").forEach((a) => (a.onclick = (ev) => { ev.preventDefault(); $("#mu-cat").value = a.dataset.muCat; $("#mu-pais").value = a.dataset.muPais || ""; muBuscar(); }));
    // ajustes
    $("#mu-activo").checked = e.activo; $("#mu-contexto").checked = e.contexto; $("#mu-factor").value = e.factor; $("#mu-retencion").value = e.retencion_h;
    $("#mu-paises").value = e.paises.map((c) => (muPaises.find((p) => p.codigo === c) || { nombre: c }).nombre).join(", ");
    ["firms", "aisstream"].forEach((k) => { const el = $(`#mu-clave-${k}`); el.value = ""; el.placeholder = (e.claves[k] ? `Clave guardada ${e.claves[k]} · ` : "") + el.placeholder.replace(/^Clave guardada \S+ · /, ""); });
    $("#mu-tabla").innerHTML = `<tr><th>activa</th><th>fuente</th><th>categoría</th><th>cada (min)</th><th>estado</th><th></th></tr>` + e.fuentes.map((f) => `<tr>
      <td><input type="checkbox" data-mu-activa="${f.id}" ${f.activa ? "checked" : ""}></td>
      <td><b>${esc(f.nombre)}</b><div class="muted">${esc(f.origen)}${f.clave ? ` · necesita clave «${esc(f.clave)}»` : ""}${f.peso !== "ligera" ? ` · ${esc(f.peso)}` : ""}</div></td>
      <td>${esc(f.categoria_nombre.split(" (")[0])}</td>
      <td><input class="input" type="number" min="${Math.ceil(f.minimo_s / 60)}" data-mu-int="${f.id}" value="${Math.round(f.intervalo_base_s / 60)}" style="width:80px" title="Mínimo ${minutos(f.minimo_s)}; con el factor queda en ${minutos(f.intervalo_s)}"></td>
      <td>${f.en_curso ? '<span class="tag accent">pidiendo</span>' : f.error ? `<span class="tag bad" title="${esc(f.error)}">error</span>` : f.ultimo_ok ? `<span class="tag ok">${f.registros} reg</span>` : '<span class="tag">pendiente</span>'}<div class="muted">${f.hace_s === null ? "" : "hace " + minutos(f.hace_s)}</div></td>
      <td><button class="btn sm" data-mu-ahora="${f.id}">AHORA</button></td></tr>`).join("");
    $$("[data-mu-ahora]").forEach((b) => (b.onclick = guard(async () => { b.disabled = true; const r2 = await api("/api/mundo/actualizar", { body: { fuente: b.dataset.muAhora } }); toast(r2.error ? `Error: ${r2.error}` : `${r2.registros} registros`, !!r2.error); muCargar(); })));
  }
  async function muBuscar() {
    const q = new URLSearchParams({ categoria: $("#mu-cat").value, pais: $("#mu-pais").value, texto: $("#mu-texto").value.trim(), orden: $("#mu-orden").value, limite: 80 });
    const r = await api("/api/mundo/registros?" + q);
    $("#mu-registros").innerHTML = r.registros.length ? `<div style="overflow-x:auto"><table class="tbl"><tr><th>fecha</th><th>país</th><th>qué</th><th>detalle</th><th></th></tr>${r.registros.map((x) => `<tr><td class="mono">${esc(x.fecha || "")}</td><td>${esc(x.pais_nombre || "")}</td><td>${esc(x.titulo)}<div class="muted">${esc(x.fuente)}</div></td><td class="muted">${esc((x.detalle || "").slice(0, 200))}</td><td>${x.url ? `<a href="${esc(x.url)}" target="_blank" rel="noopener">abrir</a>` : ""}</td></tr>`).join("")}</table></div>` : '<div class="muted">Sin registros con esos filtros.</div>';
  }
  on("#mu-buscar", muBuscar);
  if ($("#mu-texto")) $("#mu-texto").onkeydown = (e) => { if (e.key === "Enter") muBuscar(); };
  on("#mu-guardar", async () => {
    if (!UI().admin()) return toast("Entra en modo administrador para cambiar las fuentes", true);
    const fuentes = {};
    $$("[data-mu-activa]").forEach((c) => { fuentes[c.dataset.muActiva] = { activa: c.checked }; });
    $$("[data-mu-int]").forEach((i) => { fuentes[i.dataset.muInt].intervalo_s = Math.round(+i.value * 60); });
    const claves = {};
    ["firms", "aisstream"].forEach((k) => { const v = $(`#mu-clave-${k}`).value.trim(); if (v) claves[k] = v; });
    await api("/api/mundo/config", { body: { activo: $("#mu-activo").checked, contexto: $("#mu-contexto").checked, factor: +$("#mu-factor").value || 1, retencion_h: +$("#mu-retencion").value || 48, paises: $("#mu-paises").value, claves, fuentes } });
    toast("Fuentes guardadas"); muCargar();
  });
  on("#mu-todas", async () => {
    if (!UI().admin()) return toast("Entra en modo administrador", true);
    $("#mu-nota").textContent = "Pidiendo las fuentes pendientes…";
    const r = await api("/api/mundo/actualizar", { body: { fuente: "" } });
    $("#mu-nota").textContent = `${r.pedidas.length} fuentes pedidas`; muCargar();
  });

  /* ================================================================ Terminal IA */
  const T = { sid: store.get("nexus_terminal_sid", ""), historial: store.get("nexus_terminal_hist", []), pos: -1, ws: null, burbuja: null, desde: {} };
  const salida = (texto, clase = "") => { const pre = $("#te-salida"); const span = document.createElement("span"); if (clase) span.className = clase; span.textContent = texto; pre.appendChild(span); if (pre.childNodes.length > 4000) pre.removeChild(pre.firstChild); pre.scrollTop = pre.scrollHeight; };
  function teSesion(nueva = false) {   // una sola carga a la vez: la vista puede pedirla dos veces al abrirse
    if (T.cargando && !nueva) return T.cargando;
    T.cargando = teSesionCargar(nueva).finally(() => { T.cargando = null; });
    return T.cargando;
  }
  async function teSesionCargar(nueva) {
    if (!T.sid || nueva) { const s = await api("/api/terminal/sesion", { body: { sesion: "nueva" } }); T.sid = s.id; store.set("nexus_terminal_sid", T.sid); $("#te-salida").textContent = ""; $("#te-chat").innerHTML = ""; }
    const e = await api(`/api/terminal?sesion=${encodeURIComponent(T.sid)}`);
    $("#te-carpeta").value = e.sesion.carpeta; $("#te-modo").value = e.sesion.modo;
    $("#te-consola-sub").textContent = `${e.consola} · ${e.ejecutable}`;
    $("#te-tareas").innerHTML = (e.sesion.tareas || []).map((t) => `${t.estado === "hecha" ? "[x]" : t.estado === "en_curso" ? "[~]" : "[ ]"} ${esc(t.texto)}`).join("<br>");
    $("#te-extra").innerHTML = [e.instrucciones ? `Instrucciones del proyecto: <span class="mono">${esc(e.instrucciones)}</span>` : "", e.ordenes.length ? `Órdenes: ${e.ordenes.map((o) => `<span class="mono">/${esc(o.nombre)}</span>`).join(" ")}` : "", e.habilidades.length ? `Habilidades: ${e.habilidades.map((h) => esc(h.nombre)).join(", ")}` : ""].filter(Boolean).join("<br>");
    $("#te-ficha-txt").textContent = e.con_ficha ? "hay una ficha creada" : "";
    $("#te-procesos").innerHTML = e.procesos.length ? tabla(e.procesos.map((p) => ({ id: p.id, nombre: p.nombre, vivo: p.vivo ? "sí" : `terminó (${p.codigo})`, segundos: p.segundos })), 20) + `<div class="row">${e.procesos.map((p) => `<button class="btn sm" data-te-ver="${p.id}">VER ${esc(p.id)}</button>${p.vivo ? `<button class="btn sm" data-te-parar="${p.id}">PARAR ${esc(p.id)}</button>` : ""}`).join("")}</div>` : '<div class="muted">Ningún proceso de fondo.</div>';
    $$("[data-te-ver]").forEach((b) => (b.onclick = guard(async () => { const id = b.dataset.teVer; const r = await api(`/api/terminal/procesos/${id}?desde=${T.desde[id] || 0}`); T.desde[id] = r.desde; salida(`\n── proceso ${id} ──\n${r.salida}`); })));
    $$("[data-te-parar]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/terminal/procesos/${b.dataset.teParar}`, { method: "DELETE" }); teSesion(); })));
    teFicheros(".");
  }
  async function teFicheros(ruta) {
    const r = await api(`/api/terminal/ficheros?sesion=${encodeURIComponent(T.sid)}&ruta=${encodeURIComponent(ruta)}`);
    $("#te-fich-sub").textContent = r.carpeta;
    $("#te-ficheros").innerHTML = `<div><a href="#" data-te-dir="${esc(r.padre)}">⬑ subir</a></div>` + r.entradas.map((f) => `<div class="row" style="gap:6px"><a href="#" ${f.carpeta ? `data-te-dir` : `data-te-fich`}="${esc(r.carpeta + (r.carpeta.includes("\\") ? "\\" : "/") + f.nombre)}">${f.carpeta ? "📁" : "📄"} ${esc(f.nombre)}</a><span class="muted mono" style="margin-left:auto">${f.carpeta ? "" : UI().fmtB(f.bytes)}</span></div>`).join("");
    $$("[data-te-dir]").forEach((a) => (a.onclick = guard(async (ev) => { ev.preventDefault(); teFicheros(a.dataset.teDir); })));
    $$("[data-te-fich]").forEach((a) => (a.onclick = guard(async (ev) => { ev.preventDefault(); teAbrir(a.dataset.teFich); })));
  }
  async function teAbrir(ruta) {
    const r = await api(`/api/terminal/fichero?sesion=${encodeURIComponent(T.sid)}&ruta=${encodeURIComponent(ruta)}`);
    $("#te-editor").style.display = ""; $("#te-editor-ruta").textContent = r.ruta; $("#te-editor-texto").value = r.texto;
    if (r.siguiente) toast(`Solo se muestran ${r.hasta} de ${r.total_lineas} líneas`, true);
  }
  on("#te-editor-guardar", async () => { await api("/api/terminal/fichero", { method: "PUT", body: { ruta: $("#te-editor-ruta").textContent, contenido: $("#te-editor-texto").value, sesion: T.sid } }); toast("Guardado (se puede deshacer)"); });
  on("#te-editor-deshacer", async () => { const r = await api("/api/terminal/deshacer", { body: { ruta: $("#te-editor-ruta").textContent, sesion: T.sid } }); toast(r.error || `Deshecho: ${r.hecho}`, !!r.error); if (!r.error) teAbrir($("#te-editor-ruta").textContent); });
  on("#te-editor-cerrar", () => { $("#te-editor").style.display = "none"; });
  async function teEjecutar() {
    const orden = $("#te-orden").value; if (!orden.trim()) return;
    T.historial = [orden, ...T.historial.filter((x) => x !== orden)].slice(0, 100); store.set("nexus_terminal_hist", T.historial); T.pos = -1;
    $("#te-orden").value = "";
    const leng = $("#te-leng").value;
    salida(`\n❯ ${orden}\n`, "muted");
    if (leng !== "consola") {
      const r = await api("/api/terminal/codigo", { body: { codigo: orden, lenguaje: leng, sesion: T.sid } });
      if (r.salida) salida(r.salida.endsWith("\n") ? r.salida : r.salida + "\n");
      if (r.valor) salida(r.valor + "\n");
      if (r.error) salida(r.error + "\n", "bad");
      return;
    }
    if ($("#te-fondo").checked) { const r = await api("/api/terminal/ejecutar", { body: { orden, sesion: T.sid, fondo: true } }); salida(`[proceso de fondo ${r.proceso.id}]\n`, "muted"); return teSesion(); }
    const res = await api("/api/terminal/ejecutar", { body: { orden, sesion: T.sid, flujo: true }, raw: true });
    if (!res.ok) { let m = res.statusText; try { m = (await res.json()).detail || m; } catch { /* nada */ } throw new Error(m); }
    const lector = res.body.getReader(), dec = new TextDecoder(); let resto = "";
    for (;;) {
      const { value, done } = await lector.read(); if (done) break;
      resto += dec.decode(value, { stream: true });
      const lineas = resto.split("\n"); resto = lineas.pop();
      for (const l of lineas) {
        if (!l.trim()) continue;
        const ev = JSON.parse(l);
        if (ev.t === "salida") salida(ev.texto);
        else if (ev.t === "fin") {
          if (ev.error) salida(`${ev.error}\n`, "bad");
          else salida(`\n[${ev.codigo === 0 ? "ok" : "código " + ev.codigo}${ev.agotada ? " · cortada por tiempo" : ""} · ${ev.ms} ms]\n`, "muted");
          if (ev.carpeta) $("#te-carpeta").value = ev.carpeta;
        }
      }
    }
  }
  on("#te-ejecutar", teEjecutar);
  if ($("#te-orden")) $("#te-orden").onkeydown = (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); guard(teEjecutar)(); }
    else if (e.key === "ArrowUp" && !$("#te-orden").value.includes("\n")) { T.pos = Math.min(T.historial.length - 1, T.pos + 1); if (T.historial[T.pos] !== undefined) $("#te-orden").value = T.historial[T.pos]; e.preventDefault(); }
    else if (e.key === "ArrowDown" && !$("#te-orden").value.includes("\n")) { T.pos = Math.max(-1, T.pos - 1); $("#te-orden").value = T.pos < 0 ? "" : T.historial[T.pos]; e.preventDefault(); }
  };
  on("#te-cortar", async () => { const r = await api("/api/terminal/interrumpir", { body: { sesion: T.sid } }); toast(r.interrumpida ? "Orden interrumpida" : "No había ninguna orden en curso"); });
  on("#te-ir", async () => { await api("/api/terminal/sesion", { body: { sesion: T.sid, carpeta: $("#te-carpeta").value } }); toast("Carpeta cambiada"); teSesion(); });
  on("#te-nueva", () => teSesion(true));
  if ($("#te-modo")) $("#te-modo").onchange = guard(async () => { await api("/api/terminal/sesion", { body: { sesion: T.sid, modo: $("#te-modo").value } }); toast($("#te-modo").value === "plan" ? "Modo plan: el agente solo lee y propone" : "Modo construir"); });
  on("#te-ficha", async () => { const r = await api("/api/terminal/ficha", { body: {} }); $("#te-ficha-txt").textContent = r.ficha; toast("Copia la ficha ahora: no se vuelve a mostrar"); });
  on("#te-ficha-quitar", async () => { await api("/api/terminal/ficha", { body: { quitar: true } }); $("#te-ficha-txt").textContent = ""; toast("Ficha quitada"); });
  function teBurbuja(quien, html) { const d = document.createElement("div"); d.className = "panel"; d.style.padding = "10px"; d.innerHTML = `<div class="muted mono">${esc(quien)}</div><div class="te-txt">${html}</div>`; $("#te-chat").appendChild(d); $("#te-chat").scrollTop = $("#te-chat").scrollHeight; return d; }
  function teWs() {
    if (T.ws && T.ws.readyState <= 1) return T.ws;
    T.ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
    T.ws.onmessage = (ev) => {
      const e = JSON.parse(ev.data);
      if (e.sub) return;
      if (e.type === "token" && T.burbuja) { T.burbuja.dataset.txt = (T.burbuja.dataset.txt || "") + e.text; $(".te-txt", T.burbuja).innerHTML = UI().md(T.burbuja.dataset.txt); $("#te-chat").scrollTop = $("#te-chat").scrollHeight; }
      else if (e.type === "tool_call") { salida(`\n[agente] ${e.name} ${JSON.stringify(e.args).slice(0, 300)}\n`, "muted"); if (T.burbuja && !T.burbuja.dataset.txt) T.burbuja.remove(); T.burbuja = null; }
      else if (e.type === "terminal" && e.evento === "salida") salida(e.texto);
      else if (e.type === "tool_result") { T.burbuja = teBurbuja("Terminal", ""); if (/terminal_(escribir|editar|tareas|ejecutar)/.test(e.name)) teSesion(); }
      else if (e.type === "done") { if (T.burbuja && !T.burbuja.dataset.txt) T.burbuja.remove(); T.burbuja = null; teSesion(); }
      else if (e.type === "error") { teBurbuja("Error", esc(e.message)); T.burbuja = null; }
    };
    return T.ws;
  }
  async function tePreguntar() {
    const texto = $("#te-pregunta").value.trim(); if (!texto) return;
    $("#te-pregunta").value = "";
    teBurbuja("Tú", esc(texto));
    const ex = texto.startsWith("/") ? (await api("/api/terminal/expandir", { body: { texto, sesion: T.sid } })).texto : texto;
    const w = teWs();
    const enviar = () => w.send(JSON.stringify({ type: "chat", agent: "terminal", text: ex, session_id: T.sid }));
    T.burbuja = teBurbuja("Terminal", "");
    if (w.readyState === 1) enviar(); else w.addEventListener("open", enviar, { once: true });
  }
  on("#te-preguntar", tePreguntar);
  if ($("#te-pregunta")) $("#te-pregunta").onkeydown = (e) => { if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); guard(tePreguntar)(); } };

  /* ---------------------------------------------------------------- registro de vistas */
  function registrar() {
    const L = UI() && UI().loaders;
    if (!L) return setTimeout(registrar, 200);
    L.mundo = async () => { await muCargar(); await muBuscar(); };
    L.terminal = async () => {
      const admin = UI().admin();
      $("#te-sin-admin").style.display = admin ? "none" : "";
      if (admin) await teSesion();
    };
    const v = UI().view && UI().view();
    if (v === "mundo" || v === "terminal") L[v]();
  }
  registrar();
})();
