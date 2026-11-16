/* Agente IA — Oficina Herrero: estructuras metálicas (cálculo, despiece, planos, soldaduras e imagen). Usa window.NexusUI. */
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
  const num = (v, d = 1) => (v === null || v === undefined || Number.isNaN(+v) ? "—" : (+v).toLocaleString("es-ES", { maximumFractionDigits: d }));
  const H = { info: null, actual: null, ws: null, burbuja: null, sid: "" };
  const EJEMPLOS = [
    ["Porche 6×4", "Porche para dos coches de 6 x 4 m y 2,6 m de altura, galvanizado"],
    ["Nave 20×12", "Nave de 20 x 12 m con 6 m de alero a 700 m de altitud"],
    ["Entreplanta 6×4", "Entreplanta de 6 x 4 m a 2,8 m de altura para almacén con tramex"],
    ["Escalera 3 m", "Escalera de 3 m de altura y 1 m de ancho en oxirón"],
    ["Barandilla 8 m", "Barandilla de 8 m para una terraza con 7 m de desnivel"],
    ["Pasarela 8 m", "Pasarela de 8 m de largo y 1,2 m de ancho a 3 m de altura"],
    ["Pérgola 4×3", "Pérgola de 4 x 3 m de lamas, pintada en blanco"],
  ];

  function opciones(sel, mapa, vacio) {
    $(sel).innerHTML = `<option value="">${esc(vacio)}</option>` + Object.entries(mapa).map(([k, v]) => `<option value="${esc(k)}">${esc(v)}</option>`).join("");
  }
  async function cargar() {
    const d = await api("/api/herrero");
    H.info = d;
    $("#hr-aviso").textContent = d.aviso;
    $("#hr-motor").textContent = d.imagen_ia ? "· motor de imagen listo" : "· sin modelo de imagen: se hace un render de la geometría";
    if (!$("#hr-tipo").childElementCount) {
      opciones("#hr-tipo", d.tipos, "Tipo: lo que diga el texto");
      opciones("#hr-acero", Object.fromEntries(d.aceros.map((a) => [a, a + "JR"])), "Acero: S275");
      opciones("#hr-acabado", d.acabados, "Acabado: del texto o galvanizado");
      opciones("#hr-cubierta", d.cubiertas, "Cubierta: del texto");
      opciones("#hr-uso", d.usos, "Uso: del texto");
      opciones("#hr-forjado", d.forjados, "Suelo: del texto");
      $("#hr-ejemplos").innerHTML = '<span class="muted">Ejemplos:</span>' + EJEMPLOS.map(([t, e]) => `<button class="chip" data-ej="${esc(e)}" title="${esc(e)}">${esc(t)}</button>`).join("");
      $$("[data-ej]").forEach((b) => (b.onclick = () => { $("#hr-encargo").value = b.dataset.ej; $("#hr-encargo").focus(); }));
    }
    lista(d.proyectos);
  }
  function lista(ps) {
    $("#hr-lista").innerHTML = ps.length ? `<table class="tbl"><tr><th></th><th>Proyecto</th><th>Medidas</th><th>Peso</th><th>Fecha</th><th></th></tr>${ps.map((p) =>
      `<tr><td>${p.render ? `<img src="${esc(p.render)}" alt="" style="width:72px;border-radius:6px">` : ""}</td><td><b>${esc(p.titulo)}</b><div class="muted">${esc((p.encargo || "").slice(0, 90))}</div></td>
       <td class="mono">${esc(p.dimensiones)}</td><td class="mono">${num(p.kg_total, 0)} kg</td><td class="mono muted">${esc(p.creado)}</td>
       <td><button class="btn sm" data-ver="${esc(p.id)}">ABRIR</button> <button class="btn sm danger admin-only" data-borrar="${esc(p.id)}">✕</button></td></tr>`).join("")}</table>`
      : '<p class="muted">Aún no hay proyectos. Escribe un encargo y pulsa CALCULAR PROYECTO.</p>';
    $$("[data-ver]").forEach((b) => (b.onclick = guard(async () => { mostrar(await api(`/api/herrero/proyecto/${b.dataset.ver}`)); $("#hr-res").scrollIntoView({ behavior: "smooth" }); })));
    $$("[data-borrar]").forEach((b) => (b.onclick = guard(async () => { if (!confirm("¿Borrar el proyecto y sus ficheros?")) return; await api(`/api/herrero/proyecto/${b.dataset.borrar}`, { method: "DELETE" }); toast("Proyecto borrado"); cargar(); })));
  }

  function params() {
    const p = {};
    for (const [k, id] of [["tipo", "tipo"], ["acero", "acero"], ["acabado", "acabado"], ["cubierta", "cubierta"], ["uso", "uso"], ["forjado", "forjado"], ["zona_viento", "zona"]]) {
      const v = $(`#hr-${id}`).value; if (v) p[k] = v;
    }
    for (const k of ["largo", "ancho", "alto", "altitud"]) { const v = $(`#hr-${k}`).value; if (v !== "") p[k] = +v; }
    return p;
  }
  on("#hr-go", async () => {
    const encargo = $("#hr-encargo").value.trim(), parametros = params();
    if (!encargo && !parametros.tipo) return toast("Describe la estructura o elige el tipo", true);
    $("#hr-res").innerHTML = '<div class="panel"><span class="spin"></span> La oficina está calculando, despiezando y dibujando…</div>';
    const r = await api("/api/herrero/proyecto", { body: { encargo, parametros, imagen_ia: $("#hr-ia").checked } });
    mostrar(r); cargar();
  });

  const barra = (u) => {
    const p = Math.round(u * 100), cls = u > 1 ? "bad" : u > 0.9 ? "warn" : "ok";
    return `<div style="display:flex;align-items:center;gap:6px"><div style="flex:1;min-width:60px;height:6px;border-radius:3px;background:var(--line)"><div style="width:${Math.min(100, p)}%;height:100%;border-radius:3px;background:var(--${cls})"></div></div><span class="mono">${p} %</span></div>`;
  };
  function tabla(cab, filas) {
    return `<div style="overflow-x:auto"><table class="tbl"><tr>${cab.map((c) => `<th>${esc(c)}</th>`).join("")}</tr>${filas.map((f) => `<tr>${f.map((c) => `<td>${c}</td>`).join("")}</tr>`).join("")}</table></div>`;
  }
  function mostrar(r) {
    H.actual = r;
    const f = r.ficheros, t = r.totales;
    const foto = f.foto_ia || f.render;
    const puestos = Object.values(r.puestos).map((p) => `<div class="panel" style="padding:10px"><b>${esc(p.titulo)}</b><div class="muted">${esc(p.texto)}</div>${(p.supuestos || []).map((s) => `<div class="tag warn" style="margin-top:4px;white-space:normal">${esc(s)}</div>`).join("")}</div>`).join("");
    $("#hr-res").innerHTML = `
      <div class="panel col"><h3>${esc(r.titulo)} <span class="sub">${esc(r.dimensiones)} · acero ${esc(r.parametros.acero)}JR · ${esc(r.clase_ejecucion)} · ${r.cumple ? '<span class="tag ok">cumple</span>' : '<span class="tag bad">revisar</span>'}</span></h3>
        <div class="grid g4">${[["Peso total", num(t.kg_total, 0) + " kg"], ["Piezas", t.piezas], ["Barras a comprar", t.barras_comerciales], ["Pintura / galvanizado", num(t.pintura_m2) + " m²"]]
          .map(([k, v]) => `<div class="panel stat"><span class="k">${k}</span><span class="v">${v}</span></div>`).join("")}</div>
        <div class="row" style="margin-top:8px"><a class="btn primary" href="${esc(f.pdf)}" target="_blank" rel="noopener">PDF (3 hojas)</a><a class="btn" href="${esc(f.plano_svg)}" target="_blank" rel="noopener">PLANO SVG</a>
          <a class="btn" href="${esc(f.isometrica_svg)}" target="_blank" rel="noopener">ISOMÉTRICA SVG</a><a class="btn" href="${esc(f.csv)}" download>DESPIECE CSV</a>${foto ? `<a class="btn" href="${esc(foto)}" target="_blank" rel="noopener">IMAGEN</a>` : ""}
          <button class="btn" id="hr-al-chat">EXPLICAR CON EL AGENTE</button></div>
      </div>
      <div class="grid g2" style="margin-top:16px">
        <div class="panel col"><h3>Imagen <span class="sub">${esc(r.puestos.fotografia.texto)}</span></h3>${foto ? `<img src="${esc(foto)}" alt="Imagen de la estructura" style="width:100%;border-radius:8px;cursor:zoom-in" data-zoom>` : '<p class="muted">Sin imagen.</p>'}</div>
        <div class="panel col"><h3>Vista isométrica <span class="sub">marcas del despiece</span></h3><img src="${esc(f.isometrica_svg)}" alt="Isométrica" style="width:100%;background:#f4f5f6;border-radius:8px;cursor:zoom-in" data-zoom></div>
      </div>
      <div class="panel col" style="margin-top:16px"><h3>Plano de montaje <span class="sub">alzados, planta, cotas, marcas y uniones soldadas · A3</span></h3><img src="${esc(f.plano_svg)}" alt="Plano de montaje" style="width:100%;background:#fff;border-radius:8px;cursor:zoom-in" data-zoom></div>
      <div class="panel col" style="margin-top:16px"><h3>La oficina <span class="sub">lo que ha hecho cada puesto</span></h3><div class="grid g2">${puestos}</div></div>
      <div class="panel col" style="margin-top:16px"><h3>Comprobaciones <span class="sub">aprovechamiento ≤ 100 % · ${esc(Object.entries(r.cargas).filter(([k, v]) => typeof v === "number" && v).map(([k, v]) => `${k.replace(/_kn_m2/, "").replace(/_/g, " ")} ${num(v, 2)}`).join(" · "))} kN/m²</span></h3>
        ${tabla(["Elemento", "Perfil", "Aprovechamiento", "Detalle", "Método"], r.comprobaciones.map((c) => [esc(c.elemento), `<b>${esc(c.perfil)}</b>`, barra(c.max),
          esc(Object.entries(c.aprovechamiento).map(([k, v]) => `${k.replace(/_/g, " ")} ${Math.round(v * 100)} %`).join(", ")), `<span class="muted">${esc(c.metodo)}</span>`]))}
        ${(r.cargas.notas || []).map((n) => `<div class="muted">· ${esc(n)}</div>`).join("")}</div>
      <div class="panel col" style="margin-top:16px"><h3>Despiece <span class="sub">${num(t.kg_perfiles, 0)} kg de perfiles + ${num(t.kg_chapas, 0)} kg de chapas</span></h3>
        ${tabla(["Marca", "Pieza", "Ud", "Perfil", "Longitud", "kg/ud", "kg"], r.despiece.map((d) => [`<b>${esc(d.marca)}</b>`, esc(d.rol), d.cantidad, esc(d.perfil), `${d.longitud_mm} mm`, num(d.kg_ud, 2), num(d.kg_total, 1)]))}
        ${r.placas.length ? tabla(["Marca", "Chapa", "Ud", "Medidas", "kg", "Anclaje"], r.placas.map((p) => [`<b>${esc(p.marca)}</b>`, esc(p.rol), p.cantidad, `${p.a}×${p.b}×${p.t} mm`, num(p.kg_total, 1), esc(p.pernos)])) : ""}</div>
      <div class="panel col" style="margin-top:16px"><h3>Lista de corte <span class="sub">barras comerciales, corte de sierra de 3 mm</span></h3>
        ${tabla(["Perfil", "Piezas", "Barras", "Largo barra", "Aprovechamiento", "kg a comprar"], r.lista_corte.map((c) => [esc(c.perfil), c.piezas, `<b>${c.barras}</b>${c.empalmes ? ` <span class="tag warn">${c.empalmes} empalmes</span>` : ""}`, `${c.largo_barra_mm} mm`, `${num(c.aprovechamiento_pct, 0)} %`, num(c.kg_compra, 1)]))}</div>
      <div class="panel col" style="margin-top:16px"><h3>Uniones soldadas <span class="sub">${num(r.soldadura_total.metros_cordon)} m de cordón · ${num(r.soldadura_total.hilo_kg)} kg de hilo · ISO 2553</span></h3>
        ${tabla(["Unión", "Ud", "Símbolo", "Garganta", "Cordón", "Nota"], r.soldaduras.map((s) => [esc(s.union), s.cantidad, `<span class="mono">${esc(s.simbolo)}</span>`, `a = ${num(s.garganta_mm)} mm`, `${s.cordon_mm_ud} mm/ud · ${num(s.metros, 2)} m`, `<span class="muted">${esc(s.nota || s.aviso || "")}</span>`]))}</div>
      <div class="panel col" style="margin-top:16px"><h3>Normas y avisos</h3>${r.avisos.map((a) => `<div class="tag warn" style="white-space:normal;margin:2px 0">${esc(a)}</div>`).join("")}${r.normas.map((n) => `<div class="muted">· ${esc(n)}</div>`).join("")}</div>`;
    $$("[data-zoom]", $("#hr-res")).forEach((im) => (im.onclick = () => { const d = document.createElement("div"); d.className = "lightbox"; d.innerHTML = `<img src="${im.src}" style="background:#fff">`; d.onclick = () => d.remove(); document.body.appendChild(d); }));
    on("#hr-al-chat", () => preguntar(`Explícame el proyecto ${r.id} (${r.titulo}, ${r.dimensiones}): perfiles elegidos, por qué, y qué debo vigilar al fabricarlo y montarlo.`, true));
  }

  /* ---------------------------------------------------------------- chat con el agente Herrero */
  function burbuja(quien, html) {
    const d = document.createElement("div"); d.className = "panel"; d.style.padding = "10px";
    d.innerHTML = `<div class="muted mono">${esc(quien)}</div><div class="hr-txt">${html}</div>`;
    $("#hr-chat").appendChild(d); $("#hr-chat").scrollTop = $("#hr-chat").scrollHeight; return d;
  }
  function ws() {
    if (H.ws && H.ws.readyState <= 1) return H.ws;
    H.ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
    H.ws.onmessage = (ev) => {
      const e = JSON.parse(ev.data);
      if (e.sub) return;
      if (e.type === "session") H.sid = e.session_id;
      else if (e.type === "token" && H.burbuja) { H.burbuja.dataset.txt = (H.burbuja.dataset.txt || "") + e.text; $(".hr-txt", H.burbuja).innerHTML = UI().md(H.burbuja.dataset.txt); $("#hr-chat").scrollTop = $("#hr-chat").scrollHeight; }
      else if (e.type === "tool_call" && H.burbuja) { $(".hr-txt", H.burbuja).innerHTML = `<span class="spin"></span> ${esc(e.name === "herrero_proyecto" ? "La oficina está calculando…" : e.name)}`; }
      else if (e.type === "tool_result" && e.name === "herrero_proyecto") { cargar().catch(() => {}); }
      else if (e.type === "done") { H.burbuja = null; }
      else if (e.type === "error") { burbuja("Error", esc(e.message)); H.burbuja = null; }
    };
    return H.ws;
  }
  function preguntar(texto, contexto = false) {
    texto = (texto || "").trim(); if (!texto) return;
    burbuja("Tú", esc(texto));
    let msg = texto;
    if (!contexto && H.actual) msg += `\n\n(Proyecto abierto en la pestaña: ${H.actual.id}, ${H.actual.titulo}, ${H.actual.dimensiones}. Resumen:\n${H.actual.comprobaciones.map((c) => `${c.elemento}: ${c.perfil} ${Math.round(c.max * 100)} %`).join("; ")})`;
    else if (contexto && H.actual) msg += `\n\nResumen del cálculo:\n${H.actual.comprobaciones.map((c) => `${c.elemento}: ${c.perfil} ${Math.round(c.max * 100)} % (${c.metodo})`).join("\n")}\nPeso ${H.actual.totales.kg_total} kg. Avisos: ${H.actual.avisos.join(" ")}`;
    const w = ws();
    const enviar = () => w.send(JSON.stringify({ type: "chat", agent: "herrero", text: msg, session_id: H.sid || undefined }));
    H.burbuja = burbuja("Herrero", '<span class="spin"></span>');
    if (w.readyState === 1) enviar(); else w.addEventListener("open", enviar, { once: true });
  }
  on("#hr-preguntar", () => { preguntar($("#hr-pregunta").value); $("#hr-pregunta").value = ""; });
  if ($("#hr-pregunta")) $("#hr-pregunta").onkeydown = (e) => { if (e.key === "Enter") $("#hr-preguntar").click(); };
  if ($("#hr-encargo")) $("#hr-encargo").onkeydown = (e) => { if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) $("#hr-go").click(); };

  function registrar() {
    const L = UI() && UI().loaders;
    if (!L) return setTimeout(registrar, 200);
    L.herrero = cargar;
    if (UI().view && UI().view() === "herrero") cargar();
  }
  registrar();
})();
