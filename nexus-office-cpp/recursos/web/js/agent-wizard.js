/* Nexus-Office — asistente visual para crear y configurar agentes (por pasos, con vista previa en vivo).
 * Usa NexusUI (app.js). Guarda con PUT /api/agents, igual que el editor avanzado. */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const U = () => window.NexusUI;
  const esc = (s) => U().esc(s);

  const ICONS = ["◆", "★", "⚙", "✎", "☎", "✉", "⌘", "☀", "☾", "♞", "⚖", "⚕", "✈", "⚡", "☘", "♫", "⌂", "✂", "⚑", "⛭", "◉", "▣", "✦", "❖"];
  const COLORS = ["#00f0ff", "#4d8dff", "#a06bff", "#ff5ad1", "#ff5d6c", "#ff9f45", "#ffd23f", "#4ee38a", "#2ec4b6", "#9aa7b8"];
  const PLANTILLAS = [
    { k: "generalista", icon: "◆", color: "#00f0ff", name: "Generalista", desc: "Responde de todo y deriva lo que no domina.", prompt: "Eres {assistant_name}, un agente generalista. Respondes con claridad y brevedad, pides un dato solo cuando es imprescindible y delegas en un especialista cuando el tema lo requiere.", temp: 0.7, kw: [], tools: ["*"], deleg: ["*"] },
    { k: "investigador", icon: "☾", color: "#4d8dff", name: "Investigador", desc: "Busca, contrasta fuentes y cita.", prompt: "Eres un investigador riguroso. Buscas en varias fuentes, contrastas los datos, indicas la fecha y la fuente de cada afirmación y dices claramente lo que no has podido confirmar.", temp: 0.3, kw: ["investiga", "busca", "fuentes", "noticias"], tools: [], deleg: [] },
    { k: "redactor", icon: "✎", color: "#ff5ad1", name: "Redactor", desc: "Escribe, corrige y adapta el tono.", prompt: "Eres un redactor profesional en español. Escribes textos claros y bien estructurados, adaptas el tono al destinatario y mantienes la voz del usuario cuando hay muestras suyas.", temp: 0.9, kw: ["redacta", "escribe", "correo", "carta", "texto"], tools: [], deleg: [] },
    { k: "analista", icon: "▣", color: "#4ee38a", name: "Analista de datos", desc: "Hojas de cálculo, cifras y gráficos.", prompt: "Eres un analista de datos. Compruebas las cifras ejecutando los cálculos, explicas el método y presentas conclusiones con tablas o gráficos sencillos. No inventas datos.", temp: 0.2, kw: ["datos", "excel", "análisis", "gráfico", "cifras"], tools: [], deleg: [] },
    { k: "programador", icon: "⌘", color: "#a06bff", name: "Programador", desc: "Escribe y revisa código.", prompt: "Eres un programador con criterio. Entregas código que funciona, lo pruebas cuando puedes, explicas los cambios en pocas líneas y señalas los riesgos.", temp: 0.2, kw: ["código", "programa", "script", "bug", "error"], tools: [], deleg: [] },
    { k: "vacio", icon: "★", color: "#ffd23f", name: "En blanco", desc: "Empieza de cero.", prompt: "Eres un agente especialista de {assistant_name}.", temp: 0.7, kw: [], tools: [], deleg: [] },
  ];
  const TEMPS = [[0.1, "Preciso", "Respuestas estables y repetibles"], [0.4, "Equilibrado", "Fiable con algo de matiz"], [0.7, "Natural", "El punto medio habitual"], [1.0, "Creativo", "Más variedad e ideas"]];
  const STEPS = ["Punto de partida", "Identidad", "Cometido", "Modelo", "Herramientas", "Equipo y memoria", "Revisar"];
  const slug = (s) => s.toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40);
  const list = (s) => s.split(",").map((x) => x.trim()).filter(Boolean);

  async function open(agent, tools, done) {
    const ui = U(), ed = $("#agent-editor");
    const edit = !!(agent && agent.id);
    const a = JSON.parse(JSON.stringify(agent || {}));
    Object.assign(a, { tools: a.tools || [], keywords: a.keywords || [], can_delegate_to: a.can_delegate_to || [], color: a.color || "#00f0ff", icon: a.icon || "◆",
      temperature: a.temperature ?? 0.7, memory: a.memory !== false, enabled: a.enabled !== false, model: a.model || "default", sexo: a.sexo || "", specialty: a.specialty || "",
      system_prompt: a.system_prompt || "Eres un agente especialista de {assistant_name}.", name: a.name || "", id: a.id || "", description: a.description || "", max_tokens: a.max_tokens ?? null });
    let ollama = [];
    try { ollama = ((await ui.api("/api/models")).ollama || []).map((m) => m.name || m.model || m).filter((x) => typeof x === "string"); } catch { /* sin Ollama */ }
    const others = ui.agents().filter((x) => x.id !== a.id);
    let step = edit ? 1 : 0, idTouched = edit;
    ed.classList.remove("hidden");

    const preview = () => `<div class="panel agent-card wz-preview" style="--c:${esc(a.color)}"><div class="glow"></div>
      <div class="row"><div class="ico">${esc(a.icon)}</div><div style="flex:1"><div class="t" style="font:700 14px var(--font-hud);letter-spacing:.1em">${esc(a.name || "Sin nombre")}</div>
      <div class="muted mono">${esc(a.id || "id")} · ${esc(a.model)}</div></div></div>
      <p class="muted">${esc(a.description || "Describe en una frase qué hace este agente.")}</p>
      <div class="row">${(a.tools[0] === "*" ? ["todas"] : a.tools).slice(0, 6).map((t) => `<span class="tag">${esc(t)}</span>`).join("") || '<span class="muted">sin herramientas</span>'}</div>
      <div class="row" style="margin-top:8px">${a.keywords.slice(0, 6).map((k) => `<span class="tag accent">${esc(k)}</span>`).join("")}</div></div>`;

    const body = () => {
      if (step === 0) return `<p class="muted">Elige una plantilla y ajústala en los pasos siguientes.</p><div class="wz-grid">${PLANTILLAS.map((p) =>
        `<button type="button" class="wz-card" data-tpl="${p.k}" style="--c:${p.color}"><span class="ico">${p.icon}</span><b>${esc(p.name)}</b><span class="muted">${esc(p.desc)}</span></button>`).join("")}</div>`;
      if (step === 1) return `<div class="grid g2"><label class="field">Nombre<input class="input" id="wz-name" value="${esc(a.name)}" placeholder="Ej. Marta, la de finanzas"></label>
        <label class="field">ID (minúsculas y guiones)<input class="input" id="wz-id" value="${esc(a.id)}" ${edit ? "readonly" : ""}></label></div>
        <label class="field" style="margin-top:10px">Descripción en una frase<input class="input" id="wz-desc" value="${esc(a.description)}"></label>
        <div class="field" style="margin-top:12px">Icono<div class="wz-icons">${ICONS.map((i) => `<button type="button" class="wz-ico ${i === a.icon ? "on" : ""}" data-icon="${i}">${i}</button>`).join("")}</div></div>
        <div class="field" style="margin-top:12px">Color<div class="wz-icons">${COLORS.map((c) => `<button type="button" class="wz-sw ${c === a.color ? "on" : ""}" data-color="${c}" style="background:${c}" aria-label="${c}"></button>`).join("")}<input type="color" id="wz-color" value="${esc(a.color)}"></div></div>
        <div class="field" style="margin-top:12px">Aspecto en el salón<div class="wz-icons">${[["", "Al azar"], ["mujer", "Mujer"], ["hombre", "Hombre"]].map(([v, l]) => `<button type="button" class="btn sm ${a.sexo === v ? "primary" : ""}" data-sexo="${v}">${l}</button>`).join("")}</div></div>`;
      if (step === 2) return `<label class="field">Prompt de sistema: cómo piensa y cómo responde<textarea class="input" id="wz-prompt" rows="8">${esc(a.system_prompt)}</textarea></label>
        <div class="muted" id="wz-count" style="margin-top:4px"></div>
        <label class="field" style="margin-top:10px">Palabras clave de enrutado (coma): si el usuario las dice, el mensaje llega a este agente<input class="input" id="wz-kw" value="${esc(a.keywords.join(", "))}"></label>
        <div class="field" style="margin-top:12px">Tono (temperatura)<div class="wz-grid">${TEMPS.map(([t, n, d]) => `<button type="button" class="wz-card ${Math.abs(a.temperature - t) < 0.15 ? "on" : ""}" data-temp="${t}"><b>${n}</b><span class="muted">${d}</span><span class="mono muted">${t}</span></button>`).join("")}</div></div>`;
      if (step === 3) return `<p class="muted">Qué modelo contesta por este agente. «default» usa el modelo activo de la oficina.</p><div class="wz-grid">${["default", ...ollama.map((o) => "ollama:" + o)].map((m) =>
        `<button type="button" class="wz-card ${a.model === m ? "on" : ""}" data-model="${esc(m)}"><b>${esc(m === "default" ? "Modelo activo" : m)}</b><span class="muted">${m === "default" ? "Cambia con el modelo de la oficina" : "Ollama local"}</span></button>`).join("")}</div>
        <label class="field" style="margin-top:10px">Otro (especialidad:finanzas · peer:… · ollama:…)<input class="input" id="wz-model" value="${esc(a.model)}"></label>
        <label class="field" style="margin-top:10px">Máximo de tokens por respuesta (vacío = el del modelo)<input class="input" id="wz-max" type="number" min="16" value="${a.max_tokens ?? ""}"></label>`;
      if (step === 4) return `<p class="muted">Lo que este agente puede hacer por sí mismo. El riesgo indica el daño posible si se equivoca.</p>
        <label class="row wz-all"><input type="checkbox" id="wz-all" ${a.tools.includes("*") ? "checked" : ""}> <b>Todas las herramientas</b> <span class="tag high">alto</span></label>
        <div class="wz-tools ${a.tools.includes("*") ? "off" : ""}">${tools.map((t) => `<label class="wz-tool"><input type="checkbox" data-tool="${esc(t.name)}" ${a.tools.includes(t.name) ? "checked" : ""}><span><b>${esc(t.name)}</b> <span class="tag ${esc(t.risk)}">${esc(t.risk)}</span><br><span class="muted">${esc(t.description)}</span></span></label>`).join("")}</div>`;
      if (step === 5) return `<div class="row" style="gap:18px"><label class="row"><span class="switch"><input type="checkbox" id="wz-mem" ${a.memory ? "checked" : ""}><span></span></span>Memoria propia</label>
        <label class="row"><span class="switch"><input type="checkbox" id="wz-on" ${a.enabled ? "checked" : ""}><span></span></span>Activo</label></div>
        <label class="field" style="margin-top:10px">Especialidad: sus áreas y términos (coma). Lo que encaja va a su memoria; lo demás es común<textarea class="input" id="wz-spec" rows="2" placeholder="Vacío: generalista">${esc(a.specialty)}</textarea></label>
        <div class="field" style="margin-top:12px">Puede delegar en<div class="wz-icons"><button type="button" class="btn sm ${a.can_delegate_to.includes("*") ? "primary" : ""}" data-deleg="*">Todos</button>${others.map((o) =>
          `<button type="button" class="btn sm ${a.can_delegate_to.includes(o.id) || a.can_delegate_to.includes("*") ? "primary" : ""}" data-deleg="${esc(o.id)}">${esc(o.icon)} ${esc(o.name)}</button>`).join("")}</div></div>`;
      const probs = problems();
      return `<div class="grid g2"><div>${preview()}</div><div class="muted"><b>Resumen</b><ul style="margin:6px 0 0 16px">
        <li>Tono: ${a.temperature}</li><li>Memoria: ${a.memory ? "sí" : "no"} · ${a.enabled ? "activo" : "desactivado"}</li><li>Herramientas: ${a.tools.includes("*") ? "todas" : a.tools.length}</li>
        <li>Delega en: ${a.can_delegate_to.join(", ") || "nadie"}</li></ul>${probs.length ? `<p class="tag bad" style="margin-top:10px">${esc(probs.join(" · "))}</p>` : ""}</div></div>`;
    };
    const problems = () => {
      const p = [];
      if (!a.name.trim()) p.push("Falta el nombre");
      if (!a.id.trim()) p.push("Falta el ID");
      if (!a.system_prompt.trim()) p.push("Falta el prompt");
      if (a.tools.includes("*")) p.push("Acceso total a herramientas");
      return p.filter((x) => !x.startsWith("Acceso")).concat(p.filter((x) => x.startsWith("Acceso")).map(() => "Aviso: acceso total a herramientas"));
    };

    const render = () => {
      ed.innerHTML = `<h3>${edit ? "Asistente · " + esc(a.name) : "Nuevo agente"} <span class="muted">paso ${step + 1} de ${STEPS.length}</span></h3>
        <div class="wz-steps">${STEPS.map((s, i) => `<button type="button" class="wz-step ${i === step ? "on" : i < step ? "done" : ""}" data-go="${i}"><i>${i + 1}</i>${esc(s)}</button>`).join("")}</div>
        <div class="wz-main"><div class="wz-body">${body()}</div>${step > 0 && step < 6 ? `<div class="wz-side">${preview()}</div>` : ""}</div>
        <div class="row" style="margin-top:14px"><button class="btn ghost" id="wz-cancel">CANCELAR</button><span style="flex:1"></span>
        ${step > (edit ? 1 : 0) ? '<button class="btn" id="wz-back">ATRÁS</button>' : ""}${step < 6 ? '<button class="btn primary" id="wz-next">SIGUIENTE</button>' : '<button class="btn primary" id="wz-save">GUARDAR AGENTE</button>'}</div>`;
      bind();
    };
    const refresh = () => { const s = $(".wz-side", ed); if (s) s.innerHTML = preview(); };
    const go = (n) => { step = Math.max(edit ? 1 : 0, Math.min(6, n)); render(); ed.scrollIntoView({ behavior: "smooth" }); };

    function bind() {
      $("#wz-cancel").onclick = () => ed.classList.add("hidden");
      $("#wz-back") && ($("#wz-back").onclick = () => go(step - 1));
      $("#wz-next") && ($("#wz-next").onclick = () => go(step + 1));
      $$("[data-go]", ed).forEach((b) => (b.onclick = () => go(+b.dataset.go)));
      $$("[data-tpl]", ed).forEach((b) => (b.onclick = () => {
        const p = PLANTILLAS.find((x) => x.k === b.dataset.tpl);
        Object.assign(a, { icon: p.icon, color: p.color, system_prompt: p.prompt, temperature: p.temp, keywords: [...p.kw], tools: [...p.tools], can_delegate_to: [...p.deleg] });
        if (!a.name) a.name = p.k === "vacio" ? "" : p.name;
        if (!idTouched) a.id = slug(a.name);
        if (!a.description) a.description = p.desc;
        go(1);
      }));
      const bindText = (id, fn) => $(id, ed) && ($(id, ed).oninput = (e) => { fn(e.target.value); refresh(); });
      bindText("#wz-name", (v) => { a.name = v; if (!idTouched) { a.id = slug(v); $("#wz-id", ed).value = a.id; } });
      bindText("#wz-id", (v) => { idTouched = true; a.id = slug(v); });
      bindText("#wz-desc", (v) => (a.description = v));
      bindText("#wz-prompt", (v) => { a.system_prompt = v; $("#wz-count").textContent = v.length + " caracteres · ~" + Math.ceil(v.length / 4) + " tokens"; });
      bindText("#wz-kw", (v) => (a.keywords = list(v)));
      bindText("#wz-model", (v) => (a.model = v.trim() || "default"));
      bindText("#wz-max", (v) => (a.max_tokens = v === "" ? null : +v));
      bindText("#wz-spec", (v) => (a.specialty = v));
      bindText("#wz-color", (v) => (a.color = v));
      if ($("#wz-count")) $("#wz-count").textContent = a.system_prompt.length + " caracteres · ~" + Math.ceil(a.system_prompt.length / 4) + " tokens";
      $$("[data-icon]", ed).forEach((b) => (b.onclick = () => { a.icon = b.dataset.icon; render(); }));
      $$("[data-color]", ed).forEach((b) => (b.onclick = () => { a.color = b.dataset.color; render(); }));
      $$("[data-sexo]", ed).forEach((b) => (b.onclick = () => { a.sexo = b.dataset.sexo; render(); }));
      $$("[data-temp]", ed).forEach((b) => (b.onclick = () => { a.temperature = +b.dataset.temp; render(); }));
      $$("[data-model]", ed).forEach((b) => (b.onclick = () => { a.model = b.dataset.model; render(); }));
      $("#wz-all") && ($("#wz-all").onchange = (e) => { a.tools = e.target.checked ? ["*"] : []; render(); });
      $$("[data-tool]", ed).forEach((c) => (c.onchange = () => { a.tools = $$("[data-tool]:checked", ed).map((x) => x.dataset.tool); refresh(); }));
      $("#wz-mem") && ($("#wz-mem").onchange = (e) => (a.memory = e.target.checked));
      $("#wz-on") && ($("#wz-on").onchange = (e) => (a.enabled = e.target.checked));
      $$("[data-deleg]", ed).forEach((b) => (b.onclick = () => {
        const id = b.dataset.deleg, cur = a.can_delegate_to;
        if (id === "*") a.can_delegate_to = cur.includes("*") ? [] : ["*"];
        else { const base = cur.includes("*") ? [] : cur; a.can_delegate_to = base.includes(id) ? base.filter((x) => x !== id) : [...base, id]; }
        render();
      }));
      $("#wz-save") && ($("#wz-save").onclick = ui.guard(async () => {
        const bad = problems().filter((x) => !x.startsWith("Aviso"));
        if (bad.length) { ui.toast(bad.join(" · "), true); return; }
        await ui.api("/api/agents", { method: "PUT", body: { id: a.id, name: a.name.trim(), description: a.description, system_prompt: a.system_prompt, model: a.model || "default",
          temperature: a.temperature, max_tokens: a.max_tokens, tools: a.tools, memory: a.memory, enabled: a.enabled, keywords: a.keywords, specialty: a.specialty.trim(),
          can_delegate_to: a.can_delegate_to, color: a.color, icon: a.icon || "◆", sexo: a.sexo } });
        ui.toast("Agente guardado"); ed.classList.add("hidden"); done && done();
      }));
    }
    render(); ed.scrollIntoView({ behavior: "smooth" });
  }
  window.NexusAgentWizard = { open };
})();
