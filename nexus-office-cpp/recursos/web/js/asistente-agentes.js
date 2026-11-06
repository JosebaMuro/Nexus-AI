/* Agente IA - asistente de configuracion de agentes (cuatro pasos).
   1 Puesto (plantilla del catalogo o desde cero) · 2 Forma de ser · 3 Que sabe y que hace · 4 Revisar y guardar.
   El servidor redacta la ficha y el prompt (POST /api/agents/asistente); aqui solo se recogen las respuestas.
   Usa window.NexusUI (app.js): api, toast, esc, guard, go, reloadAgents. */
(() => {
  "use strict";
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const PASOS = ["Puesto", "Forma de ser", "Qué sabe y qué hace", "Revisar"];
  const ICONOS = ["◆", "★", "⚡", "☎", "✉", "✎", "⚖", "⚙", "♫", "❖", "🤖", "💼", "📊", "🧾", "🛒", "🎨", "📚", "🛠", "🌐", "📣", "🤝", "🩺", "🍳", "🏠"];
  const OPC = {
    trato: [["cercano", "Cercano (tú)"], ["usted", "Formal (usted)"], ["desenfadado", "Desenfadado"]],
    longitud: [["breves", "Breves"], ["normales", "Normales"], ["detalladas", "Detalladas"]],
    creatividad: [["precisa", "Precisa"], ["equilibrada", "Equilibrada"], ["creativa", "Creativa"]],
    proactivo: [[true, "Propone ideas"], [false, "Solo lo que pidas"]],
    idioma: [["es", "Siempre en español"], ["auto", "En el idioma del usuario"]],
  };
  const PERMISO = { ask: "🔒 Te pide permiso para algunas acciones", admin: "🔒 Pide permiso al administrador para algunas acciones", deny: "⛔ Bloqueado por la política de herramientas" };
  let W = null;   // { d (catalogo), r (respuestas), editar, paso, grupo, q, vista }

  const $ = (s) => W.el.querySelector(s);
  const $$ = (s) => [...W.el.querySelectorAll(s)];
  const copia = (o) => JSON.parse(JSON.stringify(o));

  function dialogo() {
    let el = document.getElementById("awz-dialog");
    if (!el) {
      el = document.createElement("dialog");
      el.id = "awz-dialog";
      el.className = "awz";
      el.innerHTML = `<div class="awz-head"><h2>ASISTENTE DE AGENTES</h2><button class="btn sm ghost" data-x>✕</button></div>
        <div class="awz-pasos"></div><div class="awz-body"></div>
        <div class="awz-foot"><button class="btn ghost" data-atras>‹ ATRÁS</button><span class="awz-sp"></span>
          <button class="btn" data-sig>SIGUIENTE ›</button></div>`;
      document.body.appendChild(el);
      el.addEventListener("cancel", () => {});
    }
    return el;
  }

  /* ------------------------------------------------------------ abrir */
  async function open(agente) {
    const ui = UI();
    if (!ui.admin()) return ui.toast("Entra como administrador para configurar agentes", true);
    const d = await ui.api("/api/agents/asistente");
    const el = dialogo();
    W = { el, d, paso: 0, grupo: "", q: "", vista: null, editar: agente ? agente.id : "" };
    if (agente) {
      // agente ya hecho: sus respuestas guardadas o, si no las tiene, las de su plantilla con sus datos
      const base = agente.perfil && Object.keys(agente.perfil).length ? agente.perfil
        : (d.plantillas.find((p) => p.id === agente.id) || { perfil: d.vacio }).perfil;
      W.r = Object.assign(copia(d.vacio), copia(base));
      if (!agente.perfil || !Object.keys(agente.perfil).length) {
        Object.assign(W.r, { nombre: agente.name, descripcion: agente.description, icono: agente.icon, color: agente.color, sexo: agente.sexo || "",
          especialidad: agente.specialty || "", palabras: agente.keywords || [], modelo: agente.model || "default", memoria: agente.memory,
          delegar: agente.can_delegate_to || [], prompt_manual: agente.system_prompt || "" });
      }
      W.paso = 1;
    } else {
      W.r = copia(d.vacio);
    }
    pinta();
    el.showModal();
  }

  /* ------------------------------------------------------------ pintar */
  function pinta() {
    $(".awz-head h2").textContent = W.editar ? `CONFIGURAR · ${W.r.nombre || W.editar}`.toUpperCase() : "NUEVO AGENTE";
    $(".awz-pasos").innerHTML = PASOS.map((p, i) => `<button data-paso="${i}" class="${i === W.paso ? "on" : i < W.paso ? "hecho" : ""}">${i + 1}. ${p}</button>`).join("");
    $(".awz-body").innerHTML = [paso1, paso2, paso3, paso4][W.paso]();
    $("[data-atras]").style.visibility = W.paso ? "visible" : "hidden";
    $("[data-sig]").textContent = W.paso === 3 ? "GUARDAR" : "SIGUIENTE ›";
    $("[data-sig]").className = "btn " + (W.paso === 3 ? "primary" : "");
    enlaza();
    $(".awz-body").scrollTop = 0;
  }

  function seg(clave, txt) {
    return `<div class="awz-campo"><div class="lbl">${txt}</div><div class="awz-seg" data-seg="${clave}">${OPC[clave].map(([v, l]) =>
      `<button type="button" data-v='${JSON.stringify(v)}' class="${W.r[clave] === v ? "on" : ""}">${l}</button>`).join("")}</div></div>`;
  }

  function tarjetas() {
    const q = W.q.trim().toLowerCase();
    const lista = W.d.plantillas.filter((p) => (!W.grupo || p.grupo === W.grupo) &&
      (!q || (p.name + " " + p.description + " " + (p.perfil.especialidad || "")).toLowerCase().includes(q)));
    const cero = !q && !W.grupo ? `<button class="awz-tpl ${!W.r.plantilla ? "on" : ""}" data-tpl=""><span class="i">✚</span><b>Desde cero</b><span>Un agente en blanco: tú decides todo.</span></button>` : "";
    return cero + lista.map((p) => `<button class="awz-tpl ${W.r.plantilla === p.id ? "on" : ""}" data-tpl="${esc(p.id)}" style="--c:${esc(p.color)}">
      <span class="i">${esc(p.icon)}</span><b>${esc(p.name)}</b><span>${esc(p.description)}</span>${p.conocimiento ? `<small>📖 ${p.conocimiento} datos de su oficio</small>` : ""}</button>`).join("")
      || `<p class="muted">No hay ningún puesto con «${esc(W.q)}». Prueba con otra palabra o empieza desde cero.</p>`;
  }

  function paso1() {
    const grupos = ["", ...new Set(W.d.plantillas.map((p) => p.grupo))];
    return `<p class="muted">¿Qué trabajo hará? Elige un puesto parecido y lo ajustas en los pasos siguientes. Viene con su especialidad, herramientas y conocimiento.</p>
      <div class="row" style="flex-wrap:wrap;gap:8px;margin-bottom:10px"><input class="input" data-q placeholder="Buscar puesto (fontanero, contable, recepción…)" value="${esc(W.q)}" style="flex:1 1 220px;min-width:0">
      <div class="awz-seg">${grupos.map((g) => `<button type="button" data-grupo="${esc(g)}" class="${W.grupo === g ? "on" : ""}">${g || "Todos"}</button>`).join("")}</div></div>
      <div class="awz-tpls">${tarjetas()}</div>`;
  }

  function paso2() {
    const r = W.r;
    return `<div class="awz-grid">
      <label class="field">Nombre<input class="input" data-r="nombre" value="${esc(r.nombre)}" placeholder="Ej.: Lucía, Recepción, Contable"></label>
      <label class="field">Puesto o cargo<input class="input" data-r="puesto" value="${esc(r.puesto)}" placeholder="Ej.: responsable de atención al cliente"></label></div>
      <label class="field">Qué hace, en una frase<input class="input" data-r="descripcion" value="${esc(r.descripcion)}" placeholder="Ej.: Atiende a los clientes por chat y prepara los pedidos"></label>
      <div class="awz-campo"><div class="lbl">Icono y color</div><div class="row" style="flex-wrap:wrap;gap:4px">
        ${ICONOS.map((i) => `<button type="button" class="awz-ico ${r.icono === i ? "on" : ""}" data-ico="${i}">${i}</button>`).join("")}
        <input class="input" data-r="icono" value="${esc(r.icono)}" style="width:60px;text-align:center" title="Cualquier emoji o símbolo">
        <input type="color" data-r="color" value="${esc(r.color)}"></div></div>
      <label class="field">Aspecto en el salón y su despacho<select class="input" data-r="sexo">${[["", "Al azar"], ["mujer", "Mujer"], ["hombre", "Hombre"]].map(([v, l]) =>
        `<option value="${v}" ${(r.sexo || "") === v ? "selected" : ""}>${l}</option>`).join("")}</select></label>
      ${seg("trato", "Trato")}${seg("longitud", "Respuestas")}${seg("creatividad", "Creatividad")}${seg("proactivo", "Iniciativa")}${seg("idioma", "Idioma")}
      <label class="field">Reglas propias (una por línea, opcional)<textarea class="input" data-r="reglas" rows="3" placeholder="Ej.: No des precios sin consultar la tarifa.&#10;Despídete siempre con el horario de la tienda.">${esc(r.reglas)}</textarea></label>`;
  }

  function paso3() {
    const r = W.r, todas = r.paquetes.includes("todas"), equipo = todas || r.paquetes.includes("equipo");
    const tpl = W.d.plantillas.find((p) => p.id === r.plantilla);
    return `<label class="field">Especialidad: lo que sabe hacer (áreas y términos, separados por comas)<textarea class="input" data-r="especialidad" rows="2" placeholder="Vacío: generalista">${esc(r.especialidad)}</textarea></label>
      <label class="field">Palabras que le llaman (para que le lleguen solas las preguntas de su tema)<input class="input" data-r="palabras" value="${esc((r.palabras || []).join(", "))}" placeholder="Ej.: pedido, devolución, factura"></label>
      ${tpl && tpl.conocimiento ? `<p class="muted">📖 Al guardar recibe ${tpl.conocimiento} datos de su oficio en su grafo de conocimiento.</p>` : ""}
      <div class="awz-campo"><div class="lbl">Qué puede hacer</div><div class="awz-paqs">
        ${W.d.paquetes.map((p) => `<label class="awz-paq ${todas || r.paquetes.includes(p.id) ? "on" : ""}"><input type="checkbox" data-paq="${p.id}" ${todas || r.paquetes.includes(p.id) ? "checked" : ""} ${todas ? "disabled" : ""}>
          <span class="i">${p.icono}</span><span><b>${esc(p.nombre)}</b><small>${esc(p.desc)}</small>${PERMISO[p.permiso] ? `<small class="awz-perm">${PERMISO[p.permiso]}</small>` : ""}</span></label>`).join("")}
        <label class="awz-paq ${todas ? "on" : ""}"><input type="checkbox" data-paq="todas" ${todas ? "checked" : ""}><span class="i">∞</span><span><b>Todas</b><small>Cualquier herramienta, también las que se instalen más adelante.</small></span></label>
      </div></div>
      ${equipo ? `<div class="awz-campo"><div class="lbl">Puede pasar trabajo a</div><div class="row" style="flex-wrap:wrap;gap:6px">
        ${W.d.agentes.filter((a) => a.id !== W.editar && a.enabled).map((a) => `<button type="button" class="awz-chip ${r.delegar.includes(a.id) || r.delegar.includes("*") ? "on" : ""}" data-del="${esc(a.id)}" title="${esc(a.description)}">${esc(a.icon)} ${esc(a.name)}</button>`).join("")
          || '<span class="muted">Aún no hay otros agentes.</span>'}
        <button type="button" class="awz-chip ${r.delegar.includes("*") ? "on" : ""}" data-del="*">Todos</button></div></div>` : ""}
      <div class="awz-grid"><label class="field">Cerebro (modelo de IA)<select class="input" data-r="modelo">${modelos()}</select></label>
      <div class="awz-campo"><div class="lbl">Memoria propia</div><label class="row"><span class="switch"><input type="checkbox" data-r="memoria" ${r.memoria ? "checked" : ""}><span></span></span>Recuerda lo que aprende contigo</label></div></div>`;
  }

  function modelos() {
    const lista = [...W.d.modelos];
    if (W.r.modelo && !lista.some((m) => m.id === W.r.modelo)) lista.push({ id: W.r.modelo, nombre: W.r.modelo });
    return lista.map((m) => `<option value="${esc(m.id)}" ${m.id === (W.r.modelo || "default") ? "selected" : ""}>${esc(m.nombre)}</option>`).join("");
  }

  function paso4() {
    const v = W.vista;
    if (!v) return `<div class="chip"><span class="spin"></span> preparando la ficha…</div>`;
    const a = v.agente;
    const tools = a.tools[0] === "*" ? ["todas"] : a.tools;
    return `<div class="awz-ficha" style="--c:${esc(a.color)}"><div class="ico">${esc(a.icon)}</div><div style="flex:1;min-width:0">
        <b>${esc(a.name)}</b> <span class="muted mono">${esc(a.id)}</span><p class="muted" style="margin:4px 0">${esc(a.description || "")}</p>
        <div class="row" style="flex-wrap:wrap;gap:4px">${tools.map((t) => `<span class="tag">${esc(t)}</span>`).join("")}</div>
        <div class="row" style="flex-wrap:wrap;gap:4px;margin-top:6px">${a.keywords.slice(0, 10).map((k) => `<span class="tag accent">${esc(k)}</span>`).join("")}</div></div></div>
      ${v.avisos.length ? `<ul class="awz-avisos">${v.avisos.map((x) => `<li>${esc(x)}</li>`).join("")}</ul>` : ""}
      <label class="field">Instrucciones del agente (prompt). Se redactan solas con lo que has elegido; puedes retocarlas.
        <textarea class="input mono" data-prompt rows="12">${esc(a.system_prompt)}</textarea></label>
      <div class="row" style="flex-wrap:wrap;gap:8px">${W.r.prompt_manual ? '<button class="btn sm" data-regen>↺ VOLVER A REDACTARLAS</button><span class="muted">Usas tus instrucciones retocadas a mano.</span>' : ""}
        <label class="row"><input type="checkbox" data-probar checked> Abrir el chat con este agente al guardar</label></div>`;
  }

  /* ------------------------------------------------------------ eventos */
  function enlaza() {
    $("[data-x]").onclick = () => W.el.close();
    $("[data-atras]").onclick = () => ir(W.paso - 1);
    $("[data-sig]").onclick = UI().guard(() => (W.paso === 3 ? guardar() : ir(W.paso + 1)));
    $$("[data-paso]").forEach((b) => (b.onclick = () => ir(+b.dataset.paso)));
    $$("[data-r]").forEach((i) => {
      i.oninput = i.onchange = () => {
        const k = i.dataset.r;
        W.r[k] = i.type === "checkbox" ? i.checked : k === "palabras" ? i.value.split(",").map((x) => x.trim()).filter(Boolean) : i.value;
        if (k === "icono") $$("[data-ico]").forEach((b) => b.classList.toggle("on", b.dataset.ico === i.value));
      };
    });
    $$("[data-seg]").forEach((g) => g.querySelectorAll("button").forEach((b) => (b.onclick = () => {
      W.r[g.dataset.seg] = JSON.parse(b.dataset.v);
      g.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
    })));
    $$("[data-ico]").forEach((b) => (b.onclick = () => { W.r.icono = b.dataset.ico; $('[data-r="icono"]').value = b.dataset.ico; $$("[data-ico]").forEach((x) => x.classList.toggle("on", x === b)); }));
    // paso 1
    const q = $("[data-q]");
    if (q) q.oninput = () => { W.q = q.value; $(".awz-tpls").innerHTML = tarjetas(); enlazaTarjetas(); };
    $$("[data-grupo]").forEach((b) => (b.onclick = () => { W.grupo = b.dataset.grupo; pinta(); }));
    enlazaTarjetas();
    // paso 3
    $$("[data-paq]").forEach((c) => (c.onchange = () => {
      const id = c.dataset.paq;
      if (id === "todas") W.r.paquetes = c.checked ? ["todas"] : ["basico"];
      else W.r.paquetes = c.checked ? [...W.r.paquetes, id] : W.r.paquetes.filter((x) => x !== id);
      pinta();
    }));
    $$("[data-del]").forEach((b) => (b.onclick = () => {
      const id = b.dataset.del, d = W.r.delegar.filter((x) => x !== "*");
      W.r.delegar = id === "*" ? (W.r.delegar.includes("*") ? [] : ["*"]) : d.includes(id) ? d.filter((x) => x !== id) : [...d, id];
      pinta();
    }));
    // paso 4
    const pr = $("[data-prompt]");
    if (pr) pr.oninput = () => { W.r.prompt_manual = pr.value; };
    const rg = $("[data-regen]");
    if (rg) rg.onclick = UI().guard(async () => { W.r.prompt_manual = ""; await vista(); });
  }

  function enlazaTarjetas() {
    $$("[data-tpl]").forEach((b) => (b.onclick = () => {
      const id = b.dataset.tpl;
      if (W.editar && id !== W.r.plantilla && !confirm("¿Cambiar de puesto? Se cargan sus ajustes (el nombre se mantiene).")) return;
      const p = W.d.plantillas.find((x) => x.id === id);
      const nombre = W.editar ? W.r.nombre : "";
      W.r = copia(p ? p.perfil : W.d.vacio);
      if (nombre) W.r.nombre = nombre;
      ir(1);
    }));
  }

  async function ir(n) {
    if (n < 0 || n > 3) return;
    if (n > 1 && !String(W.r.nombre || "").trim()) { UI().toast("Ponle un nombre al agente", true); n = 1; }
    W.paso = n;
    W.vista = null;
    pinta();
    if (n === 3) await vista();
  }

  async function vista() {
    W.vista = await UI().api("/api/agents/asistente", { body: { respuestas: W.r, editar: W.editar } });
    if (W.paso === 3) pinta();
  }

  async function guardar() {
    const pr = $("[data-prompt]");
    if (pr && pr.value.trim() !== W.vista.agente.system_prompt.trim()) W.r.prompt_manual = pr.value;
    const probar = $("[data-probar]") && $("[data-probar]").checked;
    const r = await UI().api("/api/agents/asistente", { body: { respuestas: W.r, editar: W.editar, guardar: true } });
    UI().toast(`Agente «${r.agente.name}» guardado` + (r.sembradas ? ` con ${r.sembradas} datos de su oficio` : ""));
    W.el.close();
    UI().reloadAgents();
    if (probar) UI().probarAgente(r.agente.id);
  }

  window.AgentWizard = { open: (a) => UI().guard(() => open(a))() };
})();
