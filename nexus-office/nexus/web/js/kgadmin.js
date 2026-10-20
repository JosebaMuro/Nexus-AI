/* Nexus-Office — gestor de conocimiento (Ajustes): grafo general (común) y memoria propia de cada agente.
   Pestañas: resumen, explorar y editar nodos, importar/exportar grafos, leer un ZIP (razonarlo y procesarlo),
   mantenimiento (reorganizar, duplicados, recuerdos) y modelo de embeddings. Usa window.NexusUI (app.js). */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);

  const TABS = [["resumen", "Resumen"], ["nodos", "Explorar y editar"], ["grafos", "Importar / exportar"], ["zip", "Leer un ZIP"],
                ["mant", "Mantenimiento"], ["emb", "Embeddings"]];
  const CAT_ICON = { perfil: "👤", preferencia: "♥", comportamiento: "⚖", relacion: "⇄", interaccion: "☍", comun: "◎",
                     especialidad: "✦", procedimiento: "☰", documento: "▤", experiencia: "★" };
  const K = { tab: localStorage.getItem("nexus_kg_tab") || "resumen", ov: null, sel: null, jobs: {}, nodeQuery: { scope: "*", q: "", category: "", type: "", semantic: false, offset: 0 } };

  const scopeName = (id) => (id ? UI().agentName(id) : "General");
  const scopeOpts = (sel, { all = false, auto = false } = {}) =>
    (all ? `<option value="*" ${sel === "*" ? "selected" : ""}>Todo</option>` : "") +
    (auto ? `<option value="auto" ${sel === "auto" ? "selected" : ""}>Automático: cada cosa a su sitio</option>` : "") +
    `<option value="general" ${sel === "general" || sel === "" ? "selected" : ""}>◎ General (común)</option>` +
    UI().agents().map((a) => `<option value="${esc(a.id)}" ${sel === a.id ? "selected" : ""}>${esc(a.icon)} ${esc(a.name)} (solo suyo)</option>`).join("");
  const catOpts = (sel, blank = "cualquier categoría") => {
    const ov = K.ov || { general_categories: {}, agent_categories: {} };
    const opt = ([k, v]) => `<option value="${k}" ${sel === k ? "selected" : ""}>${CAT_ICON[k] || ""} ${esc(v)}</option>`;
    return `<option value="">${blank}</option><optgroup label="Comunes (grafo general)">${Object.entries(ov.general_categories).map(opt).join("")}</optgroup>` +
      `<optgroup label="De cada agente">${Object.entries(ov.agent_categories).map(opt).join("")}</optgroup>`;
  };
  const catTag = (c) => c ? `<span class="tag ${["especialidad", "procedimiento", "experiencia"].includes(c) ? "warn" : c === "documento" ? "" : "accent"}">${CAT_ICON[c] || ""} ${esc(c)}</span>` : "";
  const scopeTag = (s) => `<span class="tag ${s ? "warn" : "ok"}">${s ? esc(scopeName(s)) : "general"}</span>`;

  /* ---------------------------------------------------------------- armazón */
  async function load() {
    const root = $("#kg-admin"); if (!root) return;
    if (!UI().admin()) { root.innerHTML = '<span class="muted">Entra en modo administrador para gestionar el conocimiento.</span>'; return; }
    K.ov = await api("/api/kg/overview");
    (K.ov.jobs || []).forEach((j) => (K.jobs[j.id] = j));
    root.innerHTML = `<div class="kg-tabs" role="tablist">${TABS.map(([id, n]) => `<button role="tab" data-kgtab="${id}" class="${K.tab === id ? "on" : ""}">${n}</button>`).join("")}</div>
      <div id="kg-body" class="col"></div><div id="kg-jobs" class="list"></div>`;
    $$("[data-kgtab]", root).forEach((b) => (b.onclick = () => { K.tab = b.dataset.kgtab; localStorage.setItem("nexus_kg_tab", K.tab); load(); }));
    renderJobs();
    await guard(VIEWS[K.tab] || VIEWS.resumen)();
  }

  /* ---------------------------------------------------------------- resumen */
  const VIEWS = {};
  VIEWS.resumen = async () => {
    const ov = K.ov, e = ov.embeddings, mem = ov.memory.by_scope || {};
    const cats = (id) => Object.entries(ov.by_scope[id || "general"] || {}).sort((a, b) => b[1] - a[1])
      .map(([c, n]) => `<span class="chip">${CAT_ICON[c] || "·"} ${esc(c)} ${n}</span>`).join("");
    $("#kg-body").innerHTML = `
      <div class="muted">El <b>grafo general</b> es común a todos los agentes: cómo eres y qué te gusta, las pautas de comportamiento que pides, las relaciones e interacciones y el conocimiento general para razonar.
        Cada agente tiene dentro su <b>subgrafo aislado</b> con el saber hacer de su especialidad; ningún otro agente lo ve. El organizador decide dónde va cada cosa según la especialidad de cada agente (Agentes → Editar).</div>
      <div class="grid g3">${ov.scopes.map((s) => `<div class="item col" style="gap:6px"><div class="row"><b style="flex:1">${esc(s.icon)} ${esc(s.name)}</b>${s.id ? '<span class="tag warn">propio</span>' : '<span class="tag ok">común</span>'}</div>
        <div class="d mono">${s.entities} nodos · ${s.relations} relaciones · ${s.documents} documentos · ${mem[s.id || "general"] || 0} recuerdos</div>
        <div>${cats(s.id) || '<span class="muted">vacío</span>'}</div>
        <div class="row"><button class="btn sm" data-kgscope="${esc(s.id || "general")}">EXPLORAR</button><button class="btn sm ghost" data-kgexp="${esc(s.id || "general")}">EXPORTAR</button></div></div>`).join("")}</div>
      <div class="item"><b>Modelo de embeddings:</b> ${esc(e.embedder.model || e.embedder.id)} ${ov.semantic ? '<span class="tag ok">semántico</span>' : '<span class="tag warn">hashing (sin modelo)</span>'}
        ${e.total ? `<div class="d">Indexación semántica pendiente: ${e.total} vectores${e.running ? ` · ${e.rate || "…"} por segundo` : ""}</div>` : ""}
        ${ov.semantic ? "" : `<div class="d">Descarga <b>Qwen3-Embedding</b> en la pestaña Embeddings para que la búsqueda, la clasificación y la organización entiendan el significado.</div>`}</div>
      <div class="item"><b>Especialidades</b> <span class="muted">(lo que encaja aquí es memoria propia del agente)</span>
        ${ov.specialists.map((a) => `<div class="d"><b>${esc(a.name)}:</b> ${esc(a.profile).slice(0, 220)}</div>`).join("") || '<div class="d">Ningún agente especialista: todo lo aprendido es común.</div>'}
        <div class="d mono">${ov.bridges} relaciones puente (de un agente hacia lo común)</div></div>`;
    $$("[data-kgscope]").forEach((b) => (b.onclick = () => { K.nodeQuery = { ...K.nodeQuery, scope: b.dataset.kgscope, offset: 0 }; K.tab = "nodos"; load(); }));
    $$("[data-kgexp]").forEach((b) => (b.onclick = () => download(b.dataset.kgexp, "json")));
  };

  /* ---------------------------------------------------------------- explorar y editar */
  VIEWS.nodos = async () => {
    const q = K.nodeQuery;
    $("#kg-body").innerHTML = `<div class="row kg-filters">
        <select class="input" id="kgq-scope" style="width:auto">${scopeOpts(q.scope, { all: true })}</select>
        <select class="input" id="kgq-cat" style="width:auto">${catOpts(q.category)}</select>
        <input class="input" id="kgq-type" placeholder="tipo (concepto, persona…)" value="${esc(q.type)}" style="width:150px">
        <input class="input" id="kgq-q" placeholder="Buscar…" value="${esc(q.q)}" style="flex:1;min-width:140px">
        <label class="row" title="Busca por significado con el modelo de embeddings"><input type="checkbox" id="kgq-sem" ${q.semantic ? "checked" : ""}> por significado</label>
        <button class="btn primary sm" id="kgq-go">BUSCAR</button><button class="btn sm" id="kg-new">＋ NODO</button></div>
      <div class="kg-split"><div class="list" id="kg-results"></div><div id="kg-detail" class="col"></div></div>`;
    const read = () => ({ scope: $("#kgq-scope").value, category: $("#kgq-cat").value, type: $("#kgq-type").value.trim(),
                          q: $("#kgq-q").value.trim(), semantic: $("#kgq-sem").checked, offset: 0 });
    $("#kgq-go").onclick = guard(async () => { K.nodeQuery = read(); await results(); });
    $("#kgq-q").onkeydown = (e) => { if (e.key === "Enter") $("#kgq-go").click(); };
    ["#kgq-scope", "#kgq-cat"].forEach((s) => ($(s).onchange = () => $("#kgq-go").click()));
    $("#kg-new").onclick = () => newNode();
    await results();
    if (K.sel) await detail(K.sel);
  };
  async function results() {
    const q = K.nodeQuery;
    const params = new URLSearchParams({ q: q.q, category: q.category, type: q.type, limit: 60, offset: q.offset, semantic: q.semantic && q.q ? "true" : "false" });
    if (q.scope !== "*") params.set("scope", q.scope);
    const r = await api(`/api/kg/nodes?${params}`);
    $("#kg-results").innerHTML = (r.items.map((n) => `<div class="item kg-node ${K.sel === n.id ? "on" : ""}" data-node="${n.id}">
        <div class="row"><b style="flex:1">${esc(n.name)}</b>${catTag(n.category)}${scopeTag(n.scope)}</div>
        <div class="d mono">${esc(n.type)} · peso ${(+n.weight).toFixed(1)}${n.degree != null ? ` · ${n.degree} rel.` : ""}${n.similarity != null ? ` · similitud ${n.similarity}` : ""}</div>
        ${n.note ? `<div class="d">${esc(n.note).slice(0, 140)}</div>` : ""}</div>`).join("") || '<span class="muted">Sin resultados.</span>') +
      (!r.semantic && r.total > q.offset + 60 ? `<div class="row"><span class="muted">${r.total} en total</span><button class="btn sm" id="kg-more">MÁS</button></div>` : "");
    $$("[data-node]").forEach((el) => (el.onclick = guard(() => detail(+el.dataset.node))));
    $("#kg-more") && ($("#kg-more").onclick = guard(async () => { K.nodeQuery.offset += 60; await results(); }));
  }
  async function detail(id) {
    K.sel = id;
    $$(".kg-node").forEach((el) => el.classList.toggle("on", +el.dataset.node === id));
    let n;
    try { n = await api(`/api/kg/nodes/${id}`); } catch { $("#kg-detail").innerHTML = ""; K.sel = null; return; }
    const relRow = (r) => { const out = r.src === n.id; const other = out ? r.dst_name : r.src_name;
      return `<div class="row kg-rel"><span class="mono">${out ? "→" : "←"}</span><span style="flex:1">${out ? "" : `<b>${esc(other)}</b> `}<i>${esc(r.rel.replace(/_/g, " "))}</i>${out ? ` <b>${esc(other)}</b>` : ""}</span>
        ${r.scope !== n.scope ? `<span class="tag" title="Relación puente">${esc(r.scope ? scopeName(r.scope) : "general")}</span>` : ""}<span class="muted mono">${(+r.weight).toFixed(1)}</span>
        <button class="btn danger sm" data-delrel="${r.id}" title="Borrar relación">✕</button></div>`; };
    $("#kg-detail").innerHTML = `<div class="item col" style="gap:8px">
      <div class="row"><b style="flex:1">${esc(n.name)}</b>${scopeTag(n.scope)}${n.protected ? '<span class="tag">núcleo</span>' : ""}</div>
      <div class="grid g2" style="gap:8px">
        <label class="field">Nombre<input class="input" id="kgn-name" value="${esc(n.name)}" ${n.protected ? "readonly" : ""}></label>
        <label class="field">Tipo<input class="input" id="kgn-type" value="${esc(n.type)}" ${n.protected ? "readonly" : ""}></label>
        <label class="field">Categoría<select class="input" id="kgn-cat">${catOpts(n.category, "sin categoría")}</select></label>
        <label class="field">Peso<input class="input" id="kgn-w" type="number" step="0.1" min="0" value="${(+n.weight).toFixed(2)}"></label>
      </div>
      <label class="field">Nota / descripción<textarea class="input" id="kgn-note" rows="3">${esc(n.note || "")}</textarea></label>
      <div class="d mono">${n.mentions} menciones${n.source ? " · origen " + esc(n.source) : ""}</div>
      <div class="row"><button class="btn primary sm" id="kgn-save">GUARDAR</button>
        ${n.protected ? "" : `<select class="input" id="kgn-move" style="width:auto">${scopeOpts(n.scope || "general")}</select><button class="btn sm" id="kgn-mv">MOVER</button>
        <button class="btn danger sm" id="kgn-del">BORRAR</button>`}</div>
      <h4 class="kg-h">Relaciones (${n.relations.length})</h4><div class="col">${n.relations.map(relRow).join("") || '<span class="muted">Sin relaciones.</span>'}</div>
      <h4 class="kg-h">Añadir relación o fusionar</h4>
      <div class="row"><input class="input" id="kgr-rel" placeholder="relación (p.ej. usa)" style="width:150px"><input class="input" id="kgr-q" placeholder="buscar el otro nodo…" style="flex:1" list="kgr-list"><datalist id="kgr-list"></datalist></div>
      <div class="row"><button class="btn sm" id="kgr-add">＋ RELACIONAR</button>${n.protected ? "" : '<button class="btn sm ghost" id="kgr-merge" title="Este nodo desaparece y todo pasa al elegido">FUSIONAR EN EL ELEGIDO</button>'}</div></div>`;
    $("#kgn-save").onclick = guard(async () => {
      await api(`/api/kg/nodes/${n.id}`, { method: "PATCH", body: { name: $("#kgn-name").value, type: $("#kgn-type").value, category: $("#kgn-cat").value,
        note: $("#kgn-note").value, weight: +$("#kgn-w").value } }); toast("Nodo guardado"); await results(); await detail(n.id); });
    $("#kgn-mv") && ($("#kgn-mv").onclick = guard(async () => { const to = $("#kgn-move").value;
      const r = await api(`/api/kg/nodes/${n.id}`, { method: "PATCH", body: { scope: to } }); toast(`Movido a ${scopeName(r.scope)}`); await results(); await detail(r.id); }));
    $("#kgn-del") && ($("#kgn-del").onclick = guard(async () => { if (!confirm(`¿Borrar «${n.name}» y sus relaciones?`)) return;
      await api(`/api/kg/nodes/${n.id}`, { method: "DELETE" }); K.sel = null; $("#kg-detail").innerHTML = ""; await results(); }));
    $$("[data-delrel]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/kg/relations/${b.dataset.delrel}`, { method: "DELETE" }); await detail(n.id); })));
    let found = [];
    $("#kgr-q").oninput = async (e) => { const v = e.target.value.trim(); if (v.length < 2) return;
      try { found = (await api(`/api/kg/nodes?q=${encodeURIComponent(v.replace(/\s*\[.*$/, ""))}&limit=15`)).items.filter((x) => x.id !== n.id); } catch { found = []; }
      $("#kgr-list").innerHTML = found.map((x) => `<option value="${esc(x.name)} [${esc(x.scope ? scopeName(x.scope) : "general")} #${x.id}]">`).join(""); };
    const picked = () => { const m = $("#kgr-q").value.match(/#(\d+)\]$/); if (!m) throw new Error("Elige el otro nodo de la lista"); return +m[1]; };
    $("#kgr-add").onclick = guard(async () => { await api("/api/kg/relations", { body: { src: n.id, dst: picked(), rel: $("#kgr-rel").value || "relacionado_con" } }); toast("Relación creada"); await detail(n.id); });
    $("#kgr-merge") && ($("#kgr-merge").onclick = guard(async () => { const dst = picked(); if (!confirm(`¿Fusionar «${n.name}» en el nodo elegido?`)) return;
      const r = await api("/api/kg/merge", { body: { src: n.id, dst } }); toast("Fusionados"); await results(); await detail(r.id); }));
  }
  function newNode() {
    $("#kg-detail").innerHTML = `<div class="item col" style="gap:8px"><b>Nuevo nodo</b>
      <div class="grid g2" style="gap:8px"><label class="field">Nombre<input class="input" id="kgc-name"></label><label class="field">Tipo<input class="input" id="kgc-type" value="concepto"></label>
      <label class="field">Grafo<select class="input" id="kgc-scope">${scopeOpts(K.nodeQuery.scope === "*" ? "general" : K.nodeQuery.scope)}</select></label>
      <label class="field">Categoría<select class="input" id="kgc-cat">${catOpts("", "la de su grafo")}</select></label></div>
      <label class="field">Nota<textarea class="input" id="kgc-note" rows="2"></textarea></label>
      <div class="row"><button class="btn primary sm" id="kgc-go">CREAR</button></div></div>`;
    $("#kgc-go").onclick = guard(async () => {
      const r = await api("/api/kg/nodes", { body: { name: $("#kgc-name").value, type: $("#kgc-type").value, scope: $("#kgc-scope").value,
        category: $("#kgc-cat").value, note: $("#kgc-note").value } }); toast("Nodo creado"); await results(); await detail(r.id); });
  }

  /* ---------------------------------------------------------------- importar / exportar grafos */
  VIEWS.grafos = async () => {
    $("#kg-body").innerHTML = `<div class="grid g2">
      <div class="item col" style="gap:8px"><b>Insertar otro grafo de conocimiento</b>
        <div class="d">JSON (Nexus-Office, D3/vis.js, Cytoscape, JSON Graph Format, tripletas), JSON Lines, CSV/TSV (origen, destino, relación o sujeto, relación, objeto), GraphML, GEXF, Graphviz DOT y RDF (N-Triples, Turtle).</div>
        <label class="field">Insertar en<select class="input" id="kgi-target">${scopeOpts("auto", { auto: true })}</select></label>
        <div class="drop" id="kgi-drop">Suelta aquí el grafo (o haz clic)</div><input type="file" id="kgi-file" hidden accept=".json,.jsonl,.ndjson,.csv,.tsv,.graphml,.gexf,.dot,.gv,.nt,.ttl,.txt"></div>
      <div class="item col" style="gap:8px"><b>Exportar</b><div class="d">Para copias de seguridad, para otro Nexus-Office (respeta general y agentes) o para Gephi, yEd, Cytoscape, Neo4j…</div>
        <label class="field">Grafo<select class="input" id="kge-scope">${scopeOpts("*", { all: true })}</select></label>
        <label class="field">Formato<select class="input" id="kge-fmt"><option value="json">JSON de Nexus-Office</option><option value="graphml">GraphML</option><option value="csv">CSV (tripletas)</option></select></label>
        <div class="row"><button class="btn primary sm" id="kge-go">DESCARGAR</button></div></div></div>`;
    const send = guard(async (f) => { if (!f) return; const fd = new FormData(); fd.append("file", f); fd.append("target", $("#kgi-target").value);
      const j = await api("/api/kg/import", { body: fd }); onJob(j); toast(`Importando ${f.name}…`); });
    dropZone($("#kgi-drop"), $("#kgi-file"), send);
    $("#kge-go").onclick = () => download($("#kge-scope").value, $("#kge-fmt").value);
  };
  async function download(scope, fmt) {
    const params = new URLSearchParams({ format: fmt }); if (scope !== "*") params.set("scope", scope);
    const res = await api(`/api/kg/export?${params}`, { raw: true });
    if (!res.ok) return toast("No se pudo exportar", true);
    const name = (res.headers.get("Content-Disposition") || "").split("filename=")[1] || `nexus-conocimiento.${fmt}`;
    UI().saveFile(name, await res.arrayBuffer(), res.headers.get("Content-Type") || "application/octet-stream");
  }
  function dropZone(zone, input, onFile) {
    zone.onclick = () => input.click();
    input.onchange = () => { onFile(input.files[0]); input.value = ""; };
    zone.ondragover = (e) => { e.preventDefault(); zone.classList.add("over"); };
    zone.ondragleave = () => zone.classList.remove("over");
    zone.ondrop = (e) => { e.preventDefault(); zone.classList.remove("over"); onFile(e.dataTransfer.files[0]); };
  }

  /* ---------------------------------------------------------------- leer un ZIP */
  VIEWS.zip = async () => {
    const llm = ((await api("/api/status").catch(() => ({}))).llm || {}).backend;
    $("#kg-body").innerHTML = `<div class="item col" style="gap:8px">
      <div class="d">Nexus-Office descomprime el archivo de forma segura, inserta los grafos que traiga (JSON, GraphML, CSV, RDF, notas con [[enlaces]]), lo añade a la biblioteca RAG,
        agrupa los documentos por temas con el modelo de embeddings y <b>razona</b> los más importantes con el LLM: resumen, hechos y procedimientos. Lo común va al grafo general y el saber hacer, al especialista.</div>
      <div class="grid g2" style="gap:8px">
        <label class="field">Destino<select class="input" id="kgz-target">${scopeOpts("auto", { auto: true })}</select></label>
        <label class="field">Documentos que razona el LLM<input class="input" id="kgz-n" type="number" min="0" max="200" value="12"></label></div>
      <div class="row"><label class="row"><input type="checkbox" id="kgz-reason" checked> razonar con el LLM ${llm === "builtin" ? '<span class="tag warn">sin LLM: se usan reglas</span>' : ""}</label>
        <label class="row"><input type="checkbox" id="kgz-rag" checked> añadir a la biblioteca RAG</label></div>
      <div class="drop" id="kgz-drop">Suelta aquí un ZIP, RAR, 7z o tar.gz (o haz clic)</div><input type="file" id="kgz-file" hidden accept=".zip,.rar,.7z,.tar,.tgz,.gz,.bz2,.xz"></div>
      <div id="kgz-report"></div>`;
    dropZone($("#kgz-drop"), $("#kgz-file"), guard(async (f) => { if (!f) return;
      const fd = new FormData(); fd.append("file", f); fd.append("target", $("#kgz-target").value); fd.append("reason", $("#kgz-reason").checked);
      fd.append("rag", $("#kgz-rag").checked); fd.append("max_reason", +$("#kgz-n").value || 0);
      toast(`Subiendo ${f.name}…`); const j = await api("/api/kg/archive", { body: fd }); K.lastZip = j.id; onJob(j); }));
    const last = Object.values(K.jobs).filter((j) => j.kind === "zip" && j.status === "completado").sort((a, b) => b.started - a.started)[0];
    if (last) zipReport(last);
  };
  function zipReport(j) {
    const r = j.result || {}; const box = $("#kgz-report"); if (!box) return;
    box.innerHTML = `<div class="item col" style="gap:6px"><b>${esc(r.archive)}</b> <span class="muted">→ grafo ${esc(r.scope === "general" ? "general" : scopeName(r.scope))}</span>
      ${r.summary ? `<div>${esc(r.summary)}</div>` : ""}
      <div class="d mono">${r.files} ficheros · ${r.reasoned} razonados${r.llm ? "" : " (sin LLM)"} · ${r.facts} hechos · ${r.skills} procedimientos${r.rag_doc ? ` · biblioteca: ${r.rag_doc.chunks} fragmentos` : ""}</div>
      ${(r.graphs || []).map((g) => `<div class="d">⬡ ${esc(g.file)}: ${g.error ? `<span class="tag bad">${esc(g.error)}</span>` : `${g.nodes} nodos, ${g.relations} relaciones`}</div>`).join("")}
      ${(r.themes || []).length ? `<div>${r.themes.map((t) => `<span class="chip" title="${esc((t.examples || []).join("\n"))}">${esc(t.theme)} · ${t.documents}</span>`).join("")}</div>` : ""}
      ${(r.reasoning || []).slice(0, 12).map((x) => `<div class="d"><b>${esc(x.file)}</b> (${esc(x.ambito === "general" ? "general" : scopeName(x.ambito))}): ${esc(x.resumen)}</div>`).join("")}</div>`;
  }

  /* ---------------------------------------------------------------- mantenimiento */
  VIEWS.mant = async () => {
    $("#kg-body").innerHTML = `<div class="grid g2">
      <div class="item col" style="gap:8px"><b>Reorganizar lo aprendido</b><div class="d">Revisa cada nodo: lo del usuario, pautas, relaciones y conocimiento común al general; el saber hacer, a su especialista. Primero se muestra la propuesta.</div>
        <div class="row"><button class="btn sm" id="kgm-plan">REVISAR</button><button class="btn primary sm" id="kgm-apply">APLICAR</button></div><div class="list" id="kgm-moves"></div></div>
      <div class="item col" style="gap:8px"><b>Duplicados</b><div class="d">Nodos del mismo grafo que significan lo mismo (con el modelo de embeddings).</div>
        <div class="row"><select class="input" id="kgd-scope" style="width:auto">${scopeOpts("*", { all: true })}</select><button class="btn sm" id="kgd-go">BUSCAR</button></div><div class="list" id="kgd-list"></div></div>
      <div class="item col" style="gap:8px;grid-column:1/-1"><b>Recuerdos por ámbito</b><div class="d">Los recuerdos comunes los usan todos los agentes; los propios, solo su agente.</div>
        <div class="row"><select class="input" id="kgr-scope" style="width:auto">${scopeOpts("general")}</select><button class="btn sm" id="kgr-go">VER</button></div><div class="list" id="kgr-mems"></div></div></div>`;
    $("#kgm-plan").onclick = guard(async () => onJob(await api("/api/kg/reorganize", { body: { apply: false } })));
    $("#kgm-apply").onclick = guard(async () => { if (!confirm("¿Mover cada nodo a donde le corresponde?")) return; onJob(await api("/api/kg/reorganize", { body: { apply: true } })); });
    $("#kgd-go").onclick = guard(async () => { const sc = $("#kgd-scope").value; onJob(await api("/api/kg/duplicates", { body: { scope: sc === "*" ? null : sc } })); });
    $("#kgr-go").onclick = guard(memories);
    const last = (kind) => Object.values(K.jobs).filter((j) => j.kind === kind && j.status === "completado").sort((a, b) => b.started - a.started)[0];
    if (last("reorganize")) movesView(last("reorganize"));
    if (last("duplicates")) dupView(last("duplicates"));
  };
  function movesView(j) {
    const box = $("#kgm-moves"); if (!box) return; const r = j.result;
    box.innerHTML = (r.moved != null ? `<div class="chip done">${r.moved} nodos movidos · ${r.memories_shared} recuerdos pasados a comunes</div>` : `<div class="d">${r.total} cambios propuestos</div>`) +
      r.moves.slice(0, 80).map((m) => `<div class="d">${esc(m.name)}: ${esc(m.scope ? scopeName(m.scope) : "general")} → <b>${esc(m.to ? scopeName(m.to) : "general")}</b> ${catTag(m.to_category)} <span class="muted">${esc(m.reason || "")}</span></div>`).join("");
  }
  function dupView(j) {
    const box = $("#kgd-list"); if (!box) return;
    box.innerHTML = j.result.pairs.map((p, i) => `<div class="row"><span style="flex:1">${esc(p.drop.name)} ⟶ <b>${esc(p.keep.name)}</b> ${scopeTag(p.keep.scope)} <span class="muted mono">${p.similarity}</span></span>
      <button class="btn sm" data-merge="${i}">FUSIONAR</button></div>`).join("") || '<span class="muted">Sin duplicados.</span>';
    $$("[data-merge]", box).forEach((b) => (b.onclick = guard(async () => { const p = j.result.pairs[+b.dataset.merge];
      await api("/api/kg/merge", { body: { src: p.drop.id, dst: p.keep.id } }); b.closest(".row").remove(); toast("Fusionados"); })));
  }
  async function memories() {
    const sc = $("#kgr-scope").value; const items = await api(`/api/kg/memories?scope=${encodeURIComponent(sc)}&limit=150`);
    $("#kgr-mems").innerHTML = items.map((m) => `<div class="row"><span class="tag accent">${esc(m.kind)}</span><span style="flex:1">${esc(m.content).slice(0, 220)}</span>
      <select class="input" data-mem="${m.id}" style="width:auto">${scopeOpts(m.agent || "general")}</select></div>`).join("") || '<span class="muted">Sin recuerdos.</span>';
    $$("[data-mem]").forEach((s) => (s.onchange = guard(async () => { await api(`/api/kg/memories/${s.dataset.mem}`, { method: "PATCH", body: { scope: s.value } }); toast("Recuerdo movido"); })));
  }

  /* ---------------------------------------------------------------- embeddings */
  VIEWS.emb = async () => {
    const [st, rec] = await Promise.all([api("/api/kg/embeddings"), api("/api/models/recommended")]);
    const recs = rec.items.filter((r) => r.kind === "embed"); const models = K.ov.embed_models || [];
    const pct = st.total ? 100 - (100 * st.total) / Math.max(st.total + (st.done || 0), 1) : 100;
    $("#kg-body").innerHTML = `<div class="item col" style="gap:6px"><b>Activo:</b> ${esc(st.embedder.model || st.embedder.id)} ${st.embedder.slow ? '<span class="tag">neuronal en CPU</span>' : ""}${st.embedder.running ? '<span class="tag ok">en marcha</span>' : ""}
        <div class="d">Se usa para buscar en la biblioteca y en la memoria, decidir qué es común y qué es de cada agente, agrupar documentos por temas y detectar duplicados.
          Con un modelo neuronal los documentos se indexan al momento y la precisión semántica se completa en segundo plano, sin bloquear el chat.</div>
        ${st.total ? `<div class="bar"><i style="width:${pct.toFixed(1)}%"></i></div><div class="d mono">${st.total} vectores pendientes (${st.memories} recuerdos, ${st.chunks} fragmentos)${st.rate ? ` · ${st.rate}/s` : ""}${st.error ? " · " + esc(st.error) : ""}</div>` : '<div class="d mono">Todo indexado con el modelo activo.</div>'}
        <div class="row"><button class="btn sm" id="kge-reindex">REINDEXAR</button>${K.ov.embed_active ? '<button class="btn sm ghost" id="kge-off">USAR HASHING</button>' : ""}</div></div>
      ${models.map((m) => `<div class="item row"><span class="tag accent">embed</span><div style="flex:1"><div class="t">${esc(m.name)}</div><div class="d mono">${UI().fmtB(m.size)} · ${esc(m.path.split(/[\\/]/).pop())}</div></div>
        ${K.ov.embed_active === m.id ? '<span class="tag ok">activo</span>' : `<button class="btn sm" data-embact="${esc(m.id)}">ACTIVAR</button>`}</div>`).join("")}
      ${recs.filter((r) => !r.installed).map((r) => `<div class="item col" style="gap:6px"><div class="row"><b style="flex:1">${esc(r.name)}</b><span class="tag ok">recomendado</span></div><div class="d">${esc(r.desc)}</div>
        <div class="row"><span class="mono muted">${r.size_mb} MB · se descarga una vez y queda disponible sin conexión</span><button class="btn primary sm" data-embdl="${esc(r.id)}">DESCARGAR</button></div></div>`).join("")}`;
    $("#kge-reindex").onclick = guard(async () => { await api("/api/kg/embeddings/reindex", { body: {} }); toast("Reindexando en segundo plano"); VIEWS.emb(); });
    $("#kge-off") && ($("#kge-off").onclick = guard(async () => { await api("/api/kg/embeddings/model", { body: { model: "" } }); K.ov = await api("/api/kg/overview"); VIEWS.emb(); }));
    $$("[data-embact]").forEach((b) => (b.onclick = guard(async () => { await api("/api/kg/embeddings/model", { body: { model: b.dataset.embact } }); toast("Modelo de embeddings activado");
      K.ov = await api("/api/kg/overview"); VIEWS.emb(); })));
    $$("[data-embdl]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/models/recommended/${b.dataset.embdl}`, { body: {} });
      toast("Descargando: se activará solo al terminar (progreso en Modelos)"); })));
  };

  /* ---------------------------------------------------------------- trabajos */
  function renderJobs() {
    const box = $("#kg-jobs"); if (!box) return;
    const jobs = Object.values(K.jobs).sort((a, b) => b.started - a.started).slice(0, 6);
    box.innerHTML = jobs.map((j) => { const pct = j.total ? (100 * j.done) / j.total : j.status === "en curso" ? 0 : 100;
      return `<div class="item"><div class="row"><b style="flex:1">${esc(j.title)}</b><span class="tag ${j.status === "completado" ? "ok" : j.status === "error" ? "bad" : "warn"}">${esc(j.status)}</span></div>
        ${j.status === "en curso" ? `<div class="bar" style="margin:6px 0"><i style="width:${pct.toFixed(1)}%"></i></div>` : ""}
        <div class="d mono">${esc(j.error || j.step || "")}</div></div>`; }).join("");
  }
  function onJob(j) {
    if (!j || !j.id) return;
    K.jobs[j.id] = j; renderJobs();
    if (j.status !== "completado") return;
    if (j.kind === "zip" && K.tab === "zip") zipReport(j);
    if (j.kind === "reorganize" && K.tab === "mant") movesView(j);
    if (j.kind === "duplicates" && K.tab === "mant") dupView(j);
    if (j.kind === "import") toast(`Grafo importado: ${j.result.nodes} nodos, ${j.result.relations} relaciones`);
    if (j.kind === "zip") toast(`ZIP procesado: ${j.result.reasoned} documentos razonados`);
    UI().refreshGraph();
  }

  window.NexusKG = { load, onJob };
})();
