/* Nexus-Office — webui */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const safeUrl = (u) => (/^(https?:\/\/|\/(outputs|static)\/)/i.test(u) ? u : "#");
  const fmtB = (b) => (b > 1 << 30 ? (b / (1 << 30)).toFixed(2) + " GB" : b > 1 << 20 ? (b / (1 << 20)).toFixed(0) + " MB" : (b / 1024).toFixed(0) + " KB");
  const ago = (ts) => { const s = Date.now() / 1000 - ts; return s < 60 ? "ahora" : s < 3600 ? `${(s / 60) | 0} min` : s < 86400 ? `${(s / 3600) | 0} h` : `${(s / 86400) | 0} d`; };
  const store = { get(k, d) { try { return JSON.parse(sessionStorage.getItem(k)) ?? d; } catch { return d; } }, set(k, v) { try { sessionStorage.setItem(k, JSON.stringify(v)); } catch {} } };
  const local = { get(k, d) { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } }, set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} } };

  // js/device.js detecta el dispositivo; si no hubiera cargado, la interfaz sigue funcionando como escritorio
  const NexusDevice = window.NexusDevice || { apk: !!window.NexusApp, pwa: false, touch: false, lowPower: false, preference: "auto",
    detected: () => "desktop", setPreference() {}, setAvatarBand() {}, haptic() {} };
  const S = { token: store.get("nexus_admin", ""), session: local.get("nexus_session", null), status: null, agents: [], busy: false,
              speak: local.get("nexus_speak", false), attachments: [], projects: [], view: "chat", cfg: {} };

  async function api(path, opts = {}) {
    const headers = { ...(opts.body && !(opts.body instanceof FormData) ? { "Content-Type": "application/json" } : {}), ...(S.token ? { "X-Admin-Token": S.token } : {}) };
    const res = await fetch(path, { method: opts.method || (opts.body ? "POST" : "GET"), headers,
      body: opts.body instanceof FormData ? opts.body : opts.body ? JSON.stringify(opts.body) : undefined });
    if (opts.raw) return res;
    if (!res.ok) { let msg = res.statusText; try { msg = (await res.json()).detail || msg; } catch {} throw new Error(msg); }
    return res.status === 204 ? null : res.json();
  }
  function toast(msg, err = false) {
    const el = document.createElement("div"); el.className = "toast" + (err ? " err" : ""); el.textContent = msg;
    $("#toasts").appendChild(el); setTimeout(() => el.remove(), err ? 6000 : 3500);
  }

  /* ---- descargar ficheros (navegador, PWA y app Android) */
  function b64(bytes) { let s = ""; for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000)); return btoa(s); }
  function saveFile(name, data, mime = "text/plain") {
    const bytes = typeof data === "string" ? new TextEncoder().encode(data) : new Uint8Array(data);
    if (window.NexusApp && NexusApp.saveFile) { NexusApp.saveFile(name, mime, b64(bytes)); return; }   // Android: el sistema pregunta dónde guardarlo
    const url = URL.createObjectURL(new Blob([bytes], { type: mime }));
    const a = Object.assign(document.createElement("a"), { href: url, download: name });
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 5000);
    toast(`Descargado: ${name}`);
  }
  window.__nexusSaved = (ok, name) => toast(ok ? `Guardado: ${name}` : `No se guardó ${name}`, !ok);
  const EXT = { python: "py", py: "py", python3: "py", javascript: "js", js: "js", node: "js", typescript: "ts", ts: "ts", tsx: "tsx", jsx: "jsx",
    java: "java", kotlin: "kt", kt: "kt", c: "c", h: "h", cpp: "cpp", "c++": "cpp", cc: "cpp", hpp: "hpp", csharp: "cs", cs: "cs", "c#": "cs",
    go: "go", golang: "go", rust: "rs", rs: "rs", ruby: "rb", rb: "rb", php: "php", swift: "swift", dart: "dart", lua: "lua", r: "r", perl: "pl",
    html: "html", xml: "xml", svg: "svg", css: "css", scss: "scss", sass: "sass", less: "less", json: "json", jsonc: "json", yaml: "yml", yml: "yml",
    toml: "toml", ini: "ini", sql: "sql", bash: "sh", sh: "sh", shell: "sh", zsh: "sh", console: "sh", powershell: "ps1", ps1: "ps1", pwsh: "ps1",
    bat: "bat", batch: "bat", cmd: "bat", markdown: "md", md: "md", text: "txt", txt: "txt", plaintext: "txt", csv: "csv", vue: "vue", svelte: "svelte",
    gradle: "gradle", groovy: "groovy", scala: "scala", haskell: "hs", elixir: "ex", erlang: "erl", tex: "tex", latex: "tex", graphql: "graphql",
    proto: "proto", asm: "asm", nginx: "conf", apache: "conf", diff: "diff", patch: "patch", wireguard: "conf", conf: "conf" };
  const MIME = { html: "text/html", css: "text/css", js: "text/javascript", json: "application/json", xml: "application/xml", svg: "image/svg+xml", md: "text/markdown", csv: "text/csv" };
  let codeN = 0;
  function codeFileName(lang, info, code) {
    const okName = (n) => /^[\w.@+-]+\.[A-Za-z0-9]{1,8}$/.test(n || "") ? n : "";
    let name = okName(info) || okName(lang);
    if (!name) { const m = code.split("\n", 3).join("\n").match(/(?:archivo|fichero|file(?:name)?)\s*[:=]\s*([\w.@+-]+\.[A-Za-z0-9]{1,8})/i); name = m ? okName(m[1]) : ""; }
    if (!name) { const l = (lang || "").toLowerCase();
      name = l === "dockerfile" ? "Dockerfile" : l === "makefile" ? "Makefile" : `nexus-codigo-${++codeN}.${EXT[l] || (/^\w{1,6}$/.test(l) ? l : "txt")}`; }
    return name;
  }
  function codeBlock(info, code) {
    const [lang = "", fname = ""] = info.trim().split(/\s+/);
    return `<div class="codebox"><div class="codebar"><span class="lang">${esc(fname || lang || "código")}</span>` +
      `<button type="button" data-code-copy title="Copiar el código">⧉ COPIAR</button><button type="button" data-code-save data-lang="${esc(lang)}" data-name="${esc(fname)}" title="Descargar como archivo">⤓ DESCARGAR</button></div>` +
      `<pre><code>${esc(code)}</code></pre></div>`;
  }
  document.addEventListener("click", (e) => {
    const b = e.target.closest("[data-code-copy],[data-code-save]"); if (!b) return;
    const code = b.closest(".codebox").querySelector("pre code").textContent;
    if (b.hasAttribute("data-code-copy")) return copyText(code);
    const name = codeFileName(b.dataset.lang, b.dataset.name, code); const ext = name.split(".").pop().toLowerCase();
    saveFile(name, code, (MIME[ext] || "text/plain") + ";charset=utf-8");
  });
  const guard = (fn) => async (...a) => { try { return await fn(...a); } catch (e) { toast(e.message, true); } };

  /* ================================================================ modo usuario / admin */
  function setAdmin(on) {
    document.body.classList.toggle("admin", on);
    $("#mode-admin").classList.toggle("on", on); $("#mode-user").classList.toggle("on", !on);
    if (!on) { S.token = ""; store.set("nexus_admin", ""); }
    refreshView();
  }
  $("#mode-user").onclick = () => { if (S.token) api("/api/admin/logout", { body: {} }).catch(() => {}); setAdmin(false); };
  $("#mode-admin").onclick = async () => {
    if (S.token) return;
    const hasPin = S.status ? S.status.has_pin : true;
    $("#pin-title").textContent = hasPin ? "ACCESO ADMINISTRADOR" : "CREAR PIN DE ADMINISTRADOR";
    $("#pin-help").textContent = hasPin ? "Introduce el PIN de administrador." : "Primer acceso: elige un PIN (mínimo 4 caracteres). Protege la configuración avanzada.";
    $("#pin-input").value = ""; $("#pin-dialog").showModal(); $("#pin-input").focus();
  };
  $("#pin-dialog").addEventListener("close", guard(async () => {
    if ($("#pin-dialog").returnValue !== "ok") return;
    const pin = $("#pin-input").value, hasPin = S.status ? S.status.has_pin : true;
    const r = await api(hasPin ? "/api/admin/login" : "/api/admin/setup", { body: { pin } });
    S.token = r.token; store.set("nexus_admin", r.token); setAdmin(true); toast("Modo administrador activado"); loadStatus();
  }));

  /* ================================================================ navegacion */
  const loaders = {};
  function go(view) {
    if (!$("#view-" + view)) view = "chat";
    S.view = view;
    if (location.hash.slice(1) !== view) history.replaceState(null, "", "#" + view);
    $$("#rail button[data-view]").forEach((b) => b.classList.toggle("on", b.dataset.view === view));
    $("#rail-more").classList.toggle("on", !$(`#rail button[data-view="${view}"]`).hasAttribute("data-pri"));
    $$(".view").forEach((v) => v.classList.toggle("on", v.id === "view-" + view));
    closeMore();
    refreshView();
  }
  function refreshView() { const fn = loaders[S.view]; if (fn) guard(fn)(); }
  $$("#rail button[data-view]").forEach((b) => (b.onclick = () => { NexusDevice.haptic(); go(b.dataset.view); }));

  /* ================================================================ movil y tablet */
  // hoja «Más»: en el movil la barra inferior muestra 4 secciones y el resto se abre aqui
  function openMore() {
    $("#more-grid").innerHTML = $$("#rail button[data-view]:not([data-pri])").map((b) => {
      const badge = b.querySelector(".badge:not(.hidden)");
      return `<button data-go="${b.dataset.view}" class="${b.dataset.view === S.view ? "on" : ""}">${b.querySelector("svg").outerHTML}${b.querySelector(".tip").textContent}${badge ? `<span class="badge">${badge.textContent}</span>` : ""}</button>`;
    }).join("");
    $$("#more-grid [data-go]").forEach((b) => (b.onclick = () => { NexusDevice.haptic(); go(b.dataset.go); }));
    $("#more-extra").innerHTML = "";
    if (window.NexusApp) {
      const srv = document.createElement("button"); srv.className = "btn ghost"; srv.textContent = "⇄ CAMBIAR DE SERVIDOR"; srv.onclick = () => NexusApp.reset();
      $("#more-extra").appendChild(srv);
    }
    $("#more-sheet").classList.remove("hidden");
  }
  function closeMore() { $("#more-sheet").classList.add("hidden"); }
  $("#rail-more").onclick = () => { NexusDevice.haptic(); $("#more-sheet").classList.contains("hidden") ? openMore() : closeMore(); };
  $("#more-sheet").onclick = (e) => { if (e.target.id === "more-sheet") closeMore(); };
  // franja del avatar sobre el chat (se recuerda en este dispositivo)
  $("#avatar-toggle").onclick = () => { NexusDevice.haptic(); NexusDevice.setAvatarBand(document.documentElement.dataset.avatar !== "on"); };
  addEventListener("nexus-layout", closeMore);
  // boton «atras» de Android: cierra lo que este abierto y vuelve al chat antes de salir de la app
  window.__nexusBack = () => {
    const lb = $(".lightbox"); if (lb) { lb.remove(); return true; }
    const dlg = $("dialog[open]"); if (dlg) { dlg.close("cancel"); return true; }
    if (!$("#more-sheet").classList.contains("hidden")) { closeMore(); return true; }
    if (S.view !== "chat") { go("chat"); return true; }
    return false;
  };

  /* ================================================================ avatar y fondo */
  starfield($("#stars"));
  // avatar intercambiable: grafo neuronal (2D) · geisha del eclipse (3D) · modelo GLB/GLTF/VRM propio
  const graphAvatar = new NeuralAvatar($("#avatar"), { maxLabels: 9 });
  let avatarImpl = graphAvatar, avatarStyle = "grafo", lastGraph = null;
  const avatar = {
    setState: (s) => avatarImpl.setState(s), setLevel: (v) => { avatarImpl.setLevel(v); if (devSync.last === "speaking" || devSync.last === "listening") syncDevices(devSync.last, v); },
    update: (g) => { lastGraph = g; avatarImpl.update(g); }, setAccent: (c) => avatarImpl.setAccent(c),
    get level() { return avatarImpl.level || 0; },
  };
  const when3d = () => new Promise((res) => (window.NexusAvatar3D ? res() : addEventListener("nexus-avatar3d-ready", res, { once: true })));
  async function applyAvatar(style, modelUrl) {
    const key = style + "|" + (modelUrl || "");
    if (key === avatarStyle) return;
    const wrap = $("#avatar-wrap"), canvas2d = $("#avatar");
    if (avatarImpl !== graphAvatar) avatarImpl.destroy();
    avatarImpl = graphAvatar;
    avatarStyle = key;
    if (style === "grafo" || (style === "glb" && !modelUrl)) {
      canvas2d.classList.remove("hidden"); graphAvatar.paused = false;
    } else {
      try {
        await when3d();
        canvas2d.classList.add("hidden"); graphAvatar.paused = true;
        avatarImpl = NexusAvatar3D.create(style, wrap, { url: modelUrl, onError: (e) => { toast("No se pudo cargar el modelo 3D: " + (e.message || e), true); applyAvatar("grafo"); } });
      } catch (e) { toast("WebGL no disponible: se usa el avatar de grafo", true); avatarStyle = ""; return applyAvatar("grafo"); }
    }
    avatarImpl.setAccent(S.cfg.accent_color || "#00f0ff");
    if (lastGraph) avatarImpl.update(lastGraph);
  }
  let explorer = null;
  const STATE_SUB = { idle: "núcleo cognitivo", thinking: "procesando", tool: "ejecutando", speaking: "síntesis de voz", listening: "captando audio" };
  const devSync = { last: "", t: 0, timer: null };
  function syncDevices(s, level) {   // las luces, el reloj y el Pico W siguen al avatar
    const now = Date.now();
    if (s === devSync.last && (level == null || now - devSync.t < 250)) return;
    devSync.last = s; devSync.t = now;
    fetch("/api/dispositivos/estado", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ estado: s, nivel: level ?? null, origen: "web" }) }).catch(() => {});
  }
  function setState(s) {
    avatar.setState(s); explorer && explorer.setState(s); syncDevices(s);
    const map = { idle: "EN LÍNEA", thinking: "PENSANDO", tool: "HERRAMIENTA", speaking: "HABLANDO", listening: "ESCUCHANDO" };
    $("#avatar-state").textContent = map[s] || "EN LÍNEA"; $("#avatar-sub").textContent = STATE_SUB[s] || "";
  }
  // grafo general con el subgrafo aislado de cada agente; el explorador puede mostrar uno solo
  let graphScope = local.get("nexus_graph_scope", "*"), graphScopes = [];
  const scopeLabel = (id) => (id ? agentName(id) : "General");
  function tintAgents(g) {   // el ancla de cada subgrafo con el color de su agente
    g.nodes.forEach((n) => { const c = (S.agents.find((a) => a.id === n.scope) || {}).color;
      if ((n.type === "agente" || n.agent_core) && /^#[0-9a-f]{6}$/i.test(c || "")) n.color = c; });
    return g;
  }
  async function loadGraph() {
    const g = tintAgents(await api("/api/graph"));
    avatar.update(g);
    if (explorer) explorer.update(graphScope === "*" ? g : tintAgents(await api(`/api/graph?scope=${encodeURIComponent(graphScope)}`).catch(() => { graphScope = "*"; return g; })));
    $("#hud-left").textContent = `GRAFO · ${g.nodes.length} NODOS · ${g.links.length} ENLACES`;
  }
  async function loadScopes() {
    graphScopes = await api("/api/graph/scopes");
    if (graphScope !== "*" && !graphScopes.some((s) => s.id === graphScope)) graphScope = "*";
    $("#graph-scope").innerHTML = `<option value="*">◎ Todo · general + ${graphScopes.length - 1} agentes</option>` + graphScopes.map((s) =>
      `<option value="${esc(s.id)}" ${s.id === graphScope ? "selected" : ""}>${esc(s.icon)} ${esc(s.name)}${s.id ? "" : " (común)"} · ${s.entities} nodos${s.documents ? ` · ${s.documents} docs` : ""}</option>`).join("");
    $("#graph-scope").value = graphScope;
    const t = $("#rag-target"), keep = t.value;
    t.innerHTML = graphScopes.map((s) => `<option value="${esc(s.id)}">${s.id ? `${esc(s.icon)} solo ${esc(s.name)} (su grafo individual)` : "◎ grafo general (todos los agentes)"}</option>`).join("");
    if (graphScopes.some((s) => s.id === keep)) t.value = keep;
  }
  $("#graph-scope").onchange = guard(async (e) => { graphScope = e.target.value; local.set("nexus_graph_scope", graphScope); await loadGraph(); });
  const ragTarget = () => $("#rag-target").value || "";

  /* ================================================================ websocket */
  let ws, current = null;
  function connect() {
    ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
    ws.onopen = () => $("#dot-llm").classList.add("ok");
    ws.onclose = () => { $("#dot-llm").classList.remove("ok"); setTimeout(connect, 2000); };
    ws.onmessage = (ev) => onEvent(JSON.parse(ev.data));
  }
  function onEvent(e) {
    switch (e.type) {
      case "session": S.session = e.session_id; local.set("nexus_session", e.session_id); break;
      case "agent": if (current && !e.sub) current.setAgent(e); break;
      case "state": setState(e.state); break;
      case "token": current && current.append(e.text); if (Live.on && !e.sub) Live.token(e.text); break;
      case "reasoning": current && current.reason(e.text); break;
      case "tool_call": current && current.tool(e); break;
      case "tool_result": current && current.toolDone(e); break;
      case "learned": addLearned(e.items); break;
      case "media": break;
      case "assist": if (current && !e.sub) current.note(`🧭 ${e.tools.length}/${e.of} herramientas`, `IA de apoyo (embeddings): herramientas al caso para esta petición: ${e.tools.join(", ")}`); break;
      case "progress":
        if (current) current.progress(e.text);
        if ($("#rag-progress") && S.ragBusy) $("#rag-progress").innerHTML = `<div class="chip"><span class="spin"></span>${esc(e.text)}…</div>`;
        if (/^(Lote \d|Rastreando \d|Leyendo el sitemap)/.test(e.text || "")) crawlStep = e.text;
        break;
      case "crawl_job": if (S.view === "memory") { loadCrawls().catch(() => {}); loadRag().catch(() => {}); } break;
      case "kg_job": if (window.NexusKG) NexusKG.onJob(e.job); break;
      case "done": if (current) { current.finish(e); current = null; } S.busy = false; setBusy(false); loadSessions(); if (Live.on) Live.done(e.text); break;
      case "error": if (current) { current.error(e.message); current = null; } S.busy = false; setBusy(false); setState("idle"); if (Live.on) Live.listen(); break;
      case "graph_update": loadGraph().catch(() => {}); if (explorer) loadScopes().catch(() => {}); break;
      case "approval_required": showApproval(e.approval); break;
      case "approval_resolved": $(`#ap-${e.id}`)?.remove(); break;
      case "lounge": onLounge(e.message); break;
      case "coop": onCoop(e.event); break;
      case "remote": if (e.event === "paired") { toast(`Dispositivo emparejado: ${e.device.name}`); if (S.view === "net") remoteBox(); } break;
    }
  }

  /* ================================================================ markdown */
  function md(src) {
    src = String(src || "").replace(/<tool>[\s\S]*?<\/tool>/g, "").replace(/<tool>[\s\S]*$/, "").replace(/<tool_result[\s\S]*?<\/tool_result>/g, "");
    const blocks = [];
    src = src.replace(/```([^\n`]*)\n?([\s\S]*?)(```|$)/g, (m, l, c) => { blocks.push(codeBlock(l, c.replace(/\n$/, ""))); return `\u0000${blocks.length - 1}\u0000`; });
    const inline = (t) => esc(t)
      .replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (m, a, u) => `<img src="${safeUrl(u)}" alt="${a}" loading="lazy">`)
      .replace(/\[audio\]\(([^)\s]+)\)/g, (m, u) => `<audio controls src="${safeUrl(u)}"></audio>`)
      .replace(/\[([^\]]+)\]\((\/outputs\/docs\/[^)\s]+\.pdf)\)/g, (m, a, u) => `<a class="doc-card" href="${safeUrl(u)}" target="_blank" rel="noopener"><span class="ic">PDF</span><span><b>${a.replace(/^📄\s*/, "")}</b><small>abrir · descargar · archivado en la biblioteca RAG</small></span></a>`)
      .replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (m, a, u) => `<a href="${safeUrl(u)}" target="_blank" rel="noopener">${a}</a>`)
      .replace(/`([^`]+)`/g, "<code>$1</code>").replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>").replace(/(^|[\s(])\*([^*\n]+)\*/g, "$1<i>$2</i>");
    const out = []; let list = null;
    for (const line of src.split("\n")) {
      const li = line.match(/^\s*(?:[-*•]|(\d+)\.)\s+(.*)/);
      if (li) { const tag = li[1] ? "ol" : "ul"; if (list !== tag) { if (list) out.push(`</${list}>`); out.push(`<${tag}>`); list = tag; } out.push(`<li>${inline(li[2])}</li>`); continue; }
      if (list) { out.push(`</${list}>`); list = null; }
      const h = line.match(/^(#{1,3})\s+(.*)/);
      if (h) out.push(`<h${h[1].length}>${inline(h[2])}</h${h[1].length}>`);
      else if (/^\u0000\d+\u0000$/.test(line.trim())) out.push(line.trim());
      else if (line.trim()) out.push(`<p>${inline(line)}</p>`);
    }
    if (list) out.push(`</${list}>`);
    return out.join("").replace(/\u0000(\d+)\u0000/g, (m, i) => blocks[+i]);
  }

  /* ================================================================ chat */
  const box = $("#messages");
  const agentColor = (id) => (S.agents.find((a) => a.id === id) || {}).color || "var(--accent)";
  const agentName = (id) => (S.agents.find((a) => a.id === id) || {}).name || id || "Nexus-Office";
  function scroll() { box.scrollTop = box.scrollHeight; }
  function welcome() {
    box.innerHTML = `<div class="welcome"><h1>Nexus-Office</h1><div class="muted">Inteligencia multimodal con memoria, aprendizaje y agentes</div>
      <div class="sugg">${["Hola, me llamo … y me gusta …", "¿Qué sabes de mí?", "Genera una imagen de una ciudad de neón bajo la lluvia",
        "Compón una música épica", "Cuánto es 2^16 / 7", "Info del sistema", "Busca noticias sobre IA"].map((s) => `<button>${esc(s)}</button>`).join("")}</div></div>`;
    $$(".sugg button", box).forEach((b) => (b.onclick = () => { $("#input").value = b.textContent; $("#input").focus(); }));
  }
  function userMsg(text, images = []) {
    $(".welcome", box)?.remove();
    const el = document.createElement("div"); el.className = "msg user";
    el.innerHTML = `<div class="who">TÚ</div><div class="bubble">${md(text)}${images.map((i) => `<img src="${i}">`).join("")}</div>`;
    box.appendChild(el); scroll();
  }
  function aiMsg(agent = "nexus", initial = "", mid = null, done = false) {
    $(".welcome", box)?.remove();
    const el = document.createElement("div"); el.className = "msg ai"; el.style.setProperty("--agent", agentColor(agent));
    el.innerHTML = `<div class="who"><span class="aname">${esc(agentName(agent)).toUpperCase()}</span></div>
      <div class="bubble"><div class="chips"></div><details class="think hidden"><summary>💭 Razonando…</summary><div class="think-text"></div></details><div class="text ${done ? "" : "cursor"}"></div></div><div class="msg-actions"></div>`;
    box.appendChild(el);
    let raw = initial; const text = $(".text", el), chips = $(".chips", el);
    let pend = false, thinkOn = false; const render = () => { pend = false; text.innerHTML = md(raw); scroll(); };
    const api_ = {
      setAgent(e) { el.style.setProperty("--agent", e.color); $(".aname", el).textContent = `${e.icon || ""} ${e.name}`.toUpperCase(); },
      append(t) { raw += t; if (!pend) { pend = true; requestAnimationFrame(render); } if (thinkOn) { thinkOn = false; $(".think summary", el).textContent = "💭 Razonamiento"; } },
      reason(t) { const d = $(".think", el); d.classList.remove("hidden"); thinkOn = true; const tt = $(".think-text", el); tt.textContent += t; if (d.open) tt.scrollTop = tt.scrollHeight; scroll(); },
      note(label, title) { const c = document.createElement("span"); c.className = "chip done assist"; c.textContent = label; c.title = title; chips.appendChild(c); },
      tool(e) { const c = document.createElement("span"); c.className = "chip"; c.dataset.tool = e.name;
        c.innerHTML = `<span class="spin"></span>${e.sub ? esc(e.sub) + " › " : ""}${esc(e.name)}`; c.title = JSON.stringify(e.args); chips.appendChild(c); scroll(); },
      progress(t) { const c = [...chips.querySelectorAll(".chip:not(.done)")].pop(); if (c) { c.innerHTML = `<span class="spin"></span>${esc(c.dataset.tool)} › ${esc(t)}`; } },
      toolDone(e) { const c = [...chips.querySelectorAll(".chip:not(.done)")].reverse().find((x) => x.dataset.tool === e.name);
        if (c) { const err = /"error"/.test(e.result); c.className = "chip " + (err ? "err" : "done"); c.querySelector(".spin")?.remove(); c.insertAdjacentText("afterbegin", err ? "✕ " : "✓ "); c.title = e.result; } },
      finish(e) { raw = e.text; render(); text.classList.remove("cursor"); actions(e.message_id, e.text); if (S.speak && !Live.on) speak(e.text); },
      error(m) { raw += `\n\n**Error:** ${m}`; render(); text.classList.remove("cursor"); },
    };
    function actions(id, content) {
      const a = $(".msg-actions", el);
      a.innerHTML = `<button title="Me gusta">👍</button><button title="No me gusta">👎</button><button title="Leer en voz alta">🔊</button><button title="Copiar">⧉</button><button title="Descargar la respuesta (.md)">⤓</button>`;
      const [up, down, sp, cp, dl] = a.children;
      const fb = (score, b) => guard(async () => { if (!id) return; await api("/api/feedback", { body: { message_id: id, score, comment: "" } });
        up.classList.remove("on"); down.classList.remove("on"); b.classList.add("on"); toast(score > 0 ? "Aprendido: respuesta valorada" : "Aprendido: lo tendré en cuenta"); })();
      up.onclick = () => fb(1, up); down.onclick = () => fb(-1, down);
      sp.onclick = () => speak(content); cp.onclick = () => copyText(content);
      dl.onclick = () => saveFile(`nexus-respuesta-${new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "")}.md`, content, "text/markdown;charset=utf-8");
    }
    if (done) { render(); actions(mid, initial); }
    return api_;
  }
  function addLearned(items) {
    const el = document.createElement("div"); el.className = "learned";
    el.textContent = "⟡ aprendido: " + items.map((i) => `${i.s} ${i.r.replace(/_/g, " ")} ${i.o}`).join(" · ");
    box.appendChild(el); scroll();
  }
  function setBusy(b) { $("#btn-send").disabled = b; }

  async function send(opts = {}) {
    const text = $("#input").value.trim();
    const ready = S.projects.filter((p) => p.doc);
    if ((!text && !S.attachments.length && !ready.length) || S.busy) return;
    if (S.projects.some((p) => !p.doc && !p.error)) return toast("Espera a que termine de importarse el archivo", true);
    if (!ws || ws.readyState !== 1) return toast("Sin conexión con el núcleo", true);
    const images = S.attachments.slice(); S.attachments = []; S.projects = []; renderAttachments();
    // los ZIP ya estan en la biblioteca y en el sandbox: el agente recibe donde encontrarlos
    const note = ready.map((p) => p.doc.meta.sandbox ? `[Archivo ${(p.doc.meta.format || "zip").toUpperCase()} adjunto «${p.doc.title}»: ${p.doc.meta.files} ficheros descomprimidos en el sandbox en ` +
      `${p.doc.meta.sandbox}/ (file_list/file_read) e indexados en la biblioteca RAG (rag_search)]`
      : `[Documento adjunto «${p.doc.title}» (${p.name}): ${p.doc.pages} páginas indexadas en la biblioteca RAG (rag_search)]`).join("\n");
    const body = text || (ready.length ? "Revisa el contenido de lo que te adjunto y resúmeme qué contiene." : "Describe esta imagen.");
    userMsg((text || (ready.length ? "" : "(imagen)")) + (ready.length ? `${text ? "\n\n" : ""}${ready.map((p) => `📦 ${p.name}`).join("  ")}` : ""), images);
    current = aiMsg($("#agent-select").value === "auto" ? "nexus" : $("#agent-select").value);
    S.busy = true; setBusy(true); setState("thinking");
    ws.send(JSON.stringify({ type: "chat", text: note ? `${body}\n\n${note}` : body, agent: $("#agent-select").value, session_id: S.session, images, live: !!opts.live }));
    $("#input").value = ""; autoGrow();
  }
  const autoGrow = () => { const t = $("#input"); t.style.height = "auto"; t.style.height = Math.min(t.scrollHeight, 180) + "px"; };
  $("#input").addEventListener("input", autoGrow);
  // con teclado fisico Intro envia; en pantallas tactiles Intro es salto de linea y se envia con el boton
  $("#input").addEventListener("keydown", (e) => { if (e.key === "Enter" && !e.shiftKey && !e.isComposing && !NexusDevice.touch) { e.preventDefault(); send(); } });
  const placeholder = () => { $("#input").placeholder = NexusDevice.layout === "phone" ? "Mensaje…" : NexusDevice.touch ? "Escribe a Nexus-Office…" : "Escribe a Nexus-Office… (Shift+Enter: nueva línea)"; };
  placeholder(); addEventListener("nexus-layout", placeholder);
  $("#btn-send").onclick = () => { NexusDevice.haptic(); send(); };
  $("#btn-attach").onclick = () => $("#file-attach").click();
  $("#file-attach").onchange = (e) => { const fs = [...e.target.files]; e.target.value = "";
    fs.filter((f) => f.type.startsWith("image/")).slice(0, 4).forEach(addImage);
    fs.filter((f) => !f.type.startsWith("image/")).forEach(addZip); };
  const ARCHIVE_RE = /\.(zip|rar|7z|tar|tgz|tbz2|txz|tar\.(gz|bz2|xz))$/i;
  async function addZip(file) { // documento o comprimido (proyecto/documentacion): se importa al momento y el mensaje lleva su ubicacion
    const p = { name: file.name }; S.projects.push(p); renderAttachments();
    try { const fd = new FormData(); fd.append("file", file); p.doc = await api("/api/rag/upload", { body: fd });
      toast(p.doc.meta.sandbox ? `${(p.doc.meta.format || "zip").toUpperCase()} importado: ${p.doc.meta.files} ficheros, ${p.doc.meta.indexed} indexados` : `Documento indexado: ${p.doc.title}`); }
    catch (err) { p.error = err.message; toast(`No se pudo importar ${file.name}: ${err.message}`, true); }
    renderAttachments();
  }
  $("#input").addEventListener("paste", (e) => { [...(e.clipboardData?.files || [])].filter((f) => f.type.startsWith("image/")).forEach(addImage); });
  function addImage(file) {
    const img = new Image(), url = URL.createObjectURL(file);
    img.onload = () => { // reducimos en el cliente: menos tokens de vision, mas rapido en CPU
      const k = Math.min(1, 1024 / Math.max(img.width, img.height)), c = document.createElement("canvas");
      c.width = img.width * k; c.height = img.height * k; c.getContext("2d").drawImage(img, 0, 0, c.width, c.height);
      S.attachments.push(c.toDataURL("image/jpeg", 0.88)); renderAttachments(); URL.revokeObjectURL(url);
    };
    img.src = url;
  }
  function renderAttachments() {
    $("#attachments").innerHTML = S.attachments.map((a, i) => `<div class="att"><img src="${a}"><button data-i="${i}">×</button></div>`).join("") +
      S.projects.map((p, i) => `<div class="att zip ${p.error ? "err" : ""}" title="${esc(p.error || p.name)}">${p.doc || p.error ? "" : `<span class="spin"></span>`}<span class="zi">${ARCHIVE_RE.test(p.name) ? "📦" : "📄"}</span>` +
        `<span class="zn">${esc(p.name)}</span>${p.doc ? `<span class="zn">${p.doc.meta.sandbox ? p.doc.meta.files + " fich." : p.doc.pages + " pág."}</span>` : ""}<button data-p="${i}">×</button></div>`).join("");
    $$("#attachments button[data-i]").forEach((b) => (b.onclick = () => { S.attachments.splice(+b.dataset.i, 1); renderAttachments(); }));
    $$("#attachments button[data-p]").forEach((b) => (b.onclick = () => { S.projects.splice(+b.dataset.p, 1); renderAttachments(); }));
  }
  box.addEventListener("click", (e) => { if (e.target.tagName === "IMG") lightbox(e.target.src); });
  function lightbox(src) { const d = document.createElement("div"); d.className = "lightbox"; d.innerHTML = `<img src="${src}">`; d.onclick = () => d.remove(); document.body.appendChild(d); }

  async function loadSessions() {
    const list = await api("/api/sessions");
    $("#session-select").innerHTML = `<option value="">— conversaciones —</option>` +
      list.map((s) => `<option value="${s.id}" ${s.id === S.session ? "selected" : ""}>${esc(s.title || "sin título").slice(0, 40)} · ${s.n}</option>`).join("");
  }
  async function openSession(id) {
    S.session = id || null; local.set("nexus_session", S.session); box.innerHTML = "";
    if (!id) return welcome();
    const msgs = await api(`/api/sessions/${id}`);
    if (!msgs.length) return welcome();
    for (const m of msgs) m.role === "user" ? userMsg(m.content) : aiMsg(m.agent, m.content, m.id, true);
  }
  $("#session-select").onchange = (e) => guard(openSession)(e.target.value);
  $("#new-chat").onclick = () => { openSession(null); loadSessions(); };

  /* ================================================================ voz */
  let actx = null;
  function levelLoop(analyser, until) {
    const data = new Uint8Array(analyser.fftSize);
    (function f() { if (until()) { avatar.setLevel(0); return; } analyser.getByteTimeDomainData(data);
      let s = 0; for (const v of data) s += ((v - 128) / 128) ** 2; avatar.setLevel(Math.sqrt(s / data.length) * 5); requestAnimationFrame(f); })();
  }
  const plain = (t) => t.replace(/<tool>[\s\S]*?<\/tool>/g, "").replace(/!\[[^\]]*\]\([^)]*\)|\[audio\]\([^)]*\)/g, "").replace(/```[\s\S]*?```/g, " código ").replace(/[*_#`>|]/g, "").replace(/\[([^\]]+)\]\([^)]*\)/g, "$1").trim();
  async function speak(text) {
    const t = plain(text); if (!t) return;
    try {
      const res = await api("/api/voice/tts", { body: { text: t, rate: S.cfg.speech_rate || 1 }, raw: true });
      if (res.status === 200) {
        const audio = new Audio(URL.createObjectURL(await res.blob()));
        actx = actx || new (window.AudioContext || window.webkitAudioContext)();
        const src = actx.createMediaElementSource(audio), an = actx.createAnalyser(); an.fftSize = 512; src.connect(an); an.connect(actx.destination);
        let ended = false; audio.onended = () => { ended = true; setState("idle"); };
        setState("speaking"); await audio.play(); levelLoop(an, () => ended); return;
      }
    } catch (e) { /* fallback al navegador */ }
    if (window.NexusApp && NexusApp.speak && NexusApp.speak(t, S.cfg.speech_rate || 1)) {   // app Android: voz del sistema
      setState("speaking"); const iv = setInterval(() => avatar.setLevel(0.3 + Math.random() * 0.5), 110);
      setTimeout(() => { clearInterval(iv); avatar.setLevel(0); setState("idle"); }, Math.min(30000, 300 + t.length * 65)); return;
    }
    if (!("speechSynthesis" in window)) return;
    const u = new SpeechSynthesisUtterance(t); u.lang = (S.cfg.language || "es") + "-" + ((S.cfg.language || "es") === "en" ? "US" : "ES"); u.rate = S.cfg.speech_rate || 1;
    const v = speechSynthesis.getVoices().find((v) => v.lang.startsWith(S.cfg.language || "es") && (!S.cfg.voice_name || v.name.includes(S.cfg.voice_name))); if (v) u.voice = v;
    let on = true; u.onend = u.onerror = () => { on = false; avatar.setLevel(0); setState("idle"); };
    u.onboundary = () => avatar.setLevel(0.5 + Math.random() * 0.5);
    setState("speaking"); speechSynthesis.cancel(); speechSynthesis.speak(u);
    (function f() { if (!on) return; avatar.setLevel(Math.max(0, avatar.level * 0.9)); requestAnimationFrame(f); })();
  }
  $("#btn-speak").classList.toggle("on", S.speak);
  $("#btn-speak").onclick = () => { S.speak = !S.speak; local.set("nexus_speak", S.speak); $("#btn-speak").classList.toggle("on", S.speak); toast(S.speak ? "Respuestas en voz alta" : "Voz desactivada"); };

  /* ---- captura de micrófono en PCM -> WAV de 16 kHz: funciona igual en Chrome, Edge, Firefox, Safari (macOS/iOS) y
     WebView, a diferencia de MediaRecorder (webm en unos, mp4 en otros). Incluye detector de voz (VAD) para el modo en vivo */
  const micOK = () => !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia && window.isSecureContext !== false);
  function wavBlob(chunks, rate) {
    const len = chunks.reduce((a, c) => a + c.length, 0), ratio = rate / 16000, n = Math.floor(len / ratio);
    const all = new Float32Array(len); let o = 0; for (const c of chunks) { all.set(c, o); o += c.length; }
    const buf = new ArrayBuffer(44 + n * 2), v = new DataView(buf);
    const w = (p, str) => [...str].forEach((ch, i) => v.setUint8(p + i, ch.charCodeAt(0)));
    w(0, "RIFF"); v.setUint32(4, 36 + n * 2, true); w(8, "WAVE"); w(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
    v.setUint32(24, 16000, true); v.setUint32(28, 32000, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true); w(36, "data"); v.setUint32(40, n * 2, true);
    for (let i = 0; i < n; i++) { const a = Math.floor(i * ratio), b = Math.min(len - 1, Math.floor((i + 1) * ratio)); let sum = 0, k = 0;   // media del tramo: antialias basico
      for (let j = a; j <= b; j++) { sum += all[j]; k++; } const x = Math.max(-1, Math.min(1, sum / Math.max(1, k))); v.setInt16(44 + i * 2, x < 0 ? x * 0x8000 : x * 0x7fff, true); }
    return new Blob([buf], { type: "audio/wav" });
  }
  async function openMic(onFrame) {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true, channelCount: 1 } });
    actx = actx || new (window.AudioContext || window.webkitAudioContext)();
    if (actx.state === "suspended") await actx.resume();
    const src = actx.createMediaStreamSource(stream), proc = actx.createScriptProcessor(4096, 1, 1), mute = actx.createGain(); mute.gain.value = 0;
    proc.onaudioprocess = (e) => { const d = new Float32Array(e.inputBuffer.getChannelData(0)); let s = 0; for (const x of d) s += x * x; onFrame(d, Math.sqrt(s / d.length)); };
    src.connect(proc); proc.connect(mute); mute.connect(actx.destination);
    return { rate: actx.sampleRate, close: () => { try { proc.disconnect(); src.disconnect(); } catch {} stream.getTracks().forEach((t) => t.stop()); } };
  }
  async function transcribe(blob) {
    const fd = new FormData(); fd.append("file", blob, "voz.wav");
    return api("/api/voice/stt", { body: fd });
  }

  let rec = null;
  // app Android: voz nativa del sistema (el microfono web no esta disponible en http dentro de la app)
  window.__nexusVoice = (t) => { setState("idle"); $("#btn-mic").classList.remove("rec"); if (t) { $("#input").value = t; send(); } };
  $("#btn-mic").onclick = guard(async () => {
    if (window.NexusApp && NexusApp.listen) { setState("listening"); $("#btn-mic").classList.add("rec"); NexusApp.listen((S.cfg.language || "es") + "-ES"); return; }
    if (rec) { rec.stop(); return; }
    const engine = S.status?.voice?.stt_active;
    if (engine === "faster-whisper" && micOK()) {
      const chunks = []; let mic = null, quietFor = 0, heard = false;
      const stop = guard(async () => {
        if (!rec) return; rec = null; mic && mic.close(); $("#btn-mic").classList.remove("rec"); avatar.setLevel(0); setState("thinking");
        const r = await transcribe(wavBlob(chunks, mic.rate)); setState("idle");
        if (r.text) { $("#input").value = r.text; send(); } else toast(r.error || "No se entendió el audio", true);
      });
      rec = { stop };
      mic = await openMic((d, rms) => { chunks.push(d); avatar.setLevel(rms * 6);
        if (rms > 0.02) { heard = true; quietFor = 0; } else if (heard && (quietFor += d.length / mic.rate) > 1.6) stop(); });   // se para solo tras 1,6 s de silencio
      $("#btn-mic").classList.add("rec"); setState("listening"); return;
    }
    const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (!SR) return toast(micOK() ? "Este navegador no reconoce voz; instala la voz local (Sistema > faster-whisper)" : "El micrófono del navegador solo funciona en https o en este mismo equipo (localhost)", true);
    const r = new SR(); r.lang = (S.cfg.language || "es") === "en" ? "en-US" : (S.cfg.language || "es") + "-ES"; r.interimResults = true;
    rec = { stop: () => r.stop() }; $("#btn-mic").classList.add("rec"); setState("listening");
    const pulse = setInterval(() => avatar.setLevel(0.2 + Math.random() * 0.4), 120);
    r.onresult = (e) => { const t = [...e.results].map((x) => x[0].transcript).join(""); $("#input").value = t; if (e.results[e.results.length - 1].isFinal) { r.stop(); send(); } };
    r.onend = r.onerror = () => { clearInterval(pulse); avatar.setLevel(0); rec = null; $("#btn-mic").classList.remove("rec"); if (!S.busy) setState("idle"); };
    r.start();
  });

  /* ================================================================ modo en vivo: conversación por voz manos libres
     Escucha -> transcribe -> responde (corto y sin razonamiento largo) -> lo dice frase a frase mientras se genera ->
     vuelve a escuchar. Motores, por este orden: app Android (reconocimiento continuo del sistema), Whisper local con
     detector de voz (cualquier navegador con micrófono) y el reconocimiento del navegador (Chrome, Edge, Safari). */
  const Live = {
    on: false, paused: false, engine: "", mic: null, sr: null, phase: "idle", buf: "", said: "", queue: [], playing: null, gen: 0,
    langTag() { const l = S.cfg.language || "es"; return l === "en" ? "en-US" : l + "-ES"; },
    ui(st, label) { this.phase = st; const el = $("#live"); el.dataset.st = st; $("#live-state").textContent = label;
      setState({ listening: "listening", thinking: "thinking", speaking: "speaking" }[st] || "idle"); },
    level(v) { $("#live-orb").style.setProperty("--lv", Math.max(0, Math.min(1, v)).toFixed(3)); avatar.setLevel(v); },
    pickEngine() {
      if (window.NexusApp && NexusApp.listenLive && (!NexusApp.canListenLive || NexusApp.canListenLive())) return "android";
      if (S.status?.voice?.stt_active === "faster-whisper" && micOK()) return "whisper";
      if (window.SpeechRecognition || window.webkitSpeechRecognition) return "browser";
      return "";
    },
    async start() {
      if (this.on) return;
      this.engine = this.pickEngine();
      if (!this.engine) return toast(micOK() ? "Para el modo en vivo instala la voz local (Sistema > faster-whisper) o usa Chrome, Edge o Safari" : "El micrófono del navegador solo funciona en https o en este mismo equipo; usa la app o abre Nexus-Office en localhost", true);
      this.on = true; this.paused = false; this.said = ""; $("#live").classList.remove("hidden"); $("#btn-live").classList.add("on");
      $("#live-heard").textContent = ""; $("#live-said").textContent = "";
      $("#live-engine").textContent = { android: "voz del sistema Android", whisper: "Whisper local · " + (S.status?.voice?.stt_model || ""), browser: "reconocimiento del navegador" }[this.engine] +
        " · " + (S.status?.voice?.tts_active === "browser" ? "voz del navegador" : "voz " + (S.status?.voice?.tts_active || ""));
      try { await this.listen(); } catch (e) { toast("No se pudo abrir el micrófono: " + (e.message || e), true); this.end(); }
    },
    end() {
      this.on = false; this.gen++; this.stopSpeaking(); this.stopListening();
      if (this.mic) { this.mic.close(); this.mic = null; }
      $("#live").classList.add("hidden"); $("#btn-live").classList.remove("on"); this.level(0); setState("idle");
    },
    pause() { this.paused = !this.paused; $("#live-pause").textContent = this.paused ? "SEGUIR" : "PAUSAR";
      if (this.paused) { this.stopListening(); this.stopSpeaking(); this.ui("paused", "EN PAUSA"); } else this.listen(); },
    async listen() {
      if (!this.on || this.paused) return;
      this.ui("listening", "ESCUCHANDO"); $("#live-heard").textContent = "";
      if (this.engine === "android") return NexusApp.listenLive(this.langTag());
      if (this.engine === "browser") return this.listenBrowser();
      if (!this.mic) await this.openVad();
      this.vad.armed = true;
    },
    stopListening() {
      if (this.engine === "android" && window.NexusApp && NexusApp.stopListening) NexusApp.stopListening();
      if (this.sr) { const r = this.sr; this.sr = null; try { r.abort(); } catch {} }
      if (this.vad) this.vad.armed = false;
    },
    listenBrowser() {
      const SR = window.SpeechRecognition || window.webkitSpeechRecognition, r = new SR(); this.sr = r;
      r.lang = this.langTag(); r.interimResults = true; r.continuous = false;
      r.onresult = (e) => { const t = [...e.results].map((x) => x[0].transcript).join(""); $("#live-heard").textContent = t; this.level(0.4 + Math.random() * 0.3);
        if (e.results[e.results.length - 1].isFinal) { this.sr = null; this.heard(t); } };
      r.onerror = (e) => { if (e.error === "not-allowed") { toast("Permiso de micrófono denegado", true); this.end(); } };
      r.onend = () => { if (this.sr === r) { this.sr = null; setTimeout(() => this.on && this.phase === "listening" && this.listen(), 250); } };
      r.start();
    },
    async openVad() {
      // detector de voz por energia con suelo de ruido adaptativo; 0,3 s de audio previo para no comerse el principio
      const v = this.vad = { armed: true, noise: 0.01, speaking: false, loud: 0, quiet: 0, pre: [], chunks: [], len: 0 };
      this.mic = await openMic((d, rms) => {
        const rate = this.mic ? this.mic.rate : 48000, dur = d.length / rate;
        if (!v.speaking) v.noise = v.noise * 0.95 + Math.min(rms, 0.05) * 0.05;
        const thr = Math.max(0.012, v.noise * 2.8) * (this.phase === "speaking" ? 2.2 : 1);   // con el altavoz sonando, mas exigente (eco)
        if (this.phase === "listening" || this.phase === "speaking") this.level(this.phase === "listening" ? rms * 7 : this.level.v || 0);
        if (this.phase === "speaking" && $("#live-barge").checked && rms > thr) { if ((v.loud += dur) > 0.3) { v.loud = 0; this.bargeIn(); } return; }
        if (!v.armed || this.phase !== "listening") { v.pre = []; return; }
        if (!v.speaking) {
          v.pre.push(d); while (v.pre.length > Math.ceil(0.3 / dur)) v.pre.shift();
          if (rms > thr) { if ((v.loud += dur) > 0.15) { v.speaking = true; v.chunks = [...v.pre]; v.len = 0; v.quiet = 0; $("#live-heard").textContent = "…"; } } else v.loud = 0;
          return;
        }
        v.chunks.push(d); v.len += dur;
        if (rms < thr) v.quiet += dur; else v.quiet = 0;
        if (v.quiet > 0.8 || v.len > 30) {   // fin de la frase: 0,8 s de silencio (o 30 s seguidos)
          v.speaking = false; v.loud = 0; v.armed = false; const chunks = v.chunks; v.chunks = [];
          if (v.len - v.quiet < 0.35) { v.armed = true; return; }   // un golpe o un ruido corto
          this.ui("thinking", "TRANSCRIBIENDO");
          transcribe(wavBlob(chunks, rate)).then((r) => { if (r.text && r.text.trim()) this.heard(r.text.trim()); else this.listen(); })
            .catch(() => this.listen());
        }
      });
    },
    heard(text) {
      if (!this.on) return;
      text = text.trim(); if (!text) return this.listen();
      if (/^(termina|terminar|adiós|adios|fin del modo en vivo|stop|para el modo en vivo)\.?$/i.test(text)) return this.end();
      $("#live-heard").textContent = "«" + text + "»"; $("#live-said").textContent = "";
      if (S.busy) { toast("Espera a que termine la respuesta anterior", true); return this.listen(); }
      this.buf = ""; this.queue = []; this.gen++; this.ui("thinking", "PENSANDO");
      $("#input").value = text; send({ live: true });
    },
    token(t) {   // frase a frase mientras se genera: empieza a hablar antes de que acabe la respuesta
      if (!this.on) return;
      this.buf += t; $("#live-said").textContent = plain(md2text(this.buf)).slice(-600);
      if (this.engine === "android") return;   // la voz del sistema Android se lanza al final, de una vez
      let m;
      while ((m = this.buf.match(/^([\s\S]{20,}?[.!?…:;])(\s|$)|^([\s\S]*?\n\s*\n)/))) { const part = m[1] || m[3]; this.buf = this.buf.slice(part.length); this.say(part); }
    },
    done(text) {
      if (!this.on) return;
      $("#live-said").textContent = plain(md2text(text)).slice(-900);
      if (this.engine === "android") { const t = plain(text); if (t && NexusApp.speak(t, S.cfg.speech_rate || 1)) { this.ui("speaking", "HABLANDO"); this.fake(); } else this.listen(); return; }
      if (this.buf.trim()) this.say(this.buf); this.buf = "";
      if (!this.queue.length && !this.playing) this.listen();
    },
    fake() { clearInterval(this.fakeIv); this.fakeIv = setInterval(() => this.phase === "speaking" ? this.level(0.3 + Math.random() * 0.5) : clearInterval(this.fakeIv), 110); },
    say(part) { const t = plain(md2text(part)); if (!t) return; this.queue.push({ text: t, gen: this.gen, audio: null }); this.prefetch(); if (!this.playing) this.next(); },
    prefetch() {   // la siguiente frase se sintetiza mientras suena la actual
      if (S.status?.voice?.tts_active === "browser") return;
      for (const q of this.queue.slice(0, 2)) if (!q.audio) q.audio = api("/api/voice/tts", { body: { text: q.text, rate: S.cfg.speech_rate || 1 }, raw: true })
        .then((r) => (r.status === 200 ? r.blob() : null)).catch(() => null);
    },
    async next() {
      const q = this.queue.shift();
      if (!q) { this.playing = null; if (this.on && this.phase !== "thinking") this.listen(); else if (this.on && !S.busy) this.listen(); return; }
      if (q.gen !== this.gen || !this.on || this.paused) return this.next();
      this.ui("speaking", "HABLANDO"); this.playing = q; this.prefetch();
      const blob = q.audio ? await q.audio : null;
      if (q.gen !== this.gen || !this.on) return;
      if (blob) {
        const a = new Audio(URL.createObjectURL(blob)); this.audio = a;
        actx = actx || new (window.AudioContext || window.webkitAudioContext)();
        try { const src = actx.createMediaElementSource(a), an = actx.createAnalyser(); an.fftSize = 512; src.connect(an); an.connect(actx.destination);
          const data = new Uint8Array(an.fftSize); const f = () => { if (this.audio !== a) return; an.getByteTimeDomainData(data); let s = 0; for (const v of data) s += ((v - 128) / 128) ** 2; this.level(Math.sqrt(s / data.length) * 5); requestAnimationFrame(f); }; f();
        } catch {}
        a.onended = a.onerror = () => { if (this.audio === a) { this.audio = null; this.next(); } };
        try { await a.play(); } catch { this.audio = null; this.next(); }
        return;
      }
      if (!("speechSynthesis" in window)) return this.next();
      const u = new SpeechSynthesisUtterance(q.text); u.lang = this.langTag(); u.rate = S.cfg.speech_rate || 1;
      const v = speechSynthesis.getVoices().find((v) => v.lang.startsWith(S.cfg.language || "es") && (!S.cfg.voice_name || v.name.includes(S.cfg.voice_name))); if (v) u.voice = v;
      u.onboundary = () => this.level(0.5 + Math.random() * 0.5);
      u.onend = u.onerror = () => { if (this.utt === u) { this.utt = null; this.level(0); this.next(); } };
      this.utt = u; speechSynthesis.speak(u);
    },
    stopSpeaking() {
      this.queue = []; this.playing = null; clearInterval(this.fakeIv);
      if (this.audio) { const a = this.audio; this.audio = null; a.pause(); }
      if (this.utt) { this.utt = null; try { speechSynthesis.cancel(); } catch {} }
      if (window.NexusApp && NexusApp.stopSpeaking) NexusApp.stopSpeaking();
    },
    bargeIn() { if (this.phase !== "speaking") return; this.gen++; this.stopSpeaking(); this.listen(); },
  };
  const md2text = (t) => String(t || "").replace(/<think>[\s\S]*?<\/think>/g, "").replace(/^\s*[-*•]\s+/gm, "").replace(/^\s*\d+\.\s+/gm, "").replace(/\|/g, " ");
  // puente con la app Android (SpeechRecognizer sin dialogo y aviso de fin de voz)
  window.__nexusLiveResult = (t) => Live.on && (t ? Live.heard(t) : Live.listen());
  window.__nexusLivePartial = (t) => Live.on && ($("#live-heard").textContent = t);
  window.__nexusLiveLevel = (v) => Live.on && Live.phase === "listening" && Live.level(+v);
  window.__nexusLiveState = () => {};
  window.__nexusLiveError = (code) => { if (!Live.on) return;
    if (typeof code === "string") { toast(code, true); return Live.end(); }
    if (code === 9) { toast("Permiso de micrófono denegado", true); return Live.end(); }   // ERROR_INSUFFICIENT_PERMISSIONS
    setTimeout(() => Live.on && Live.phase === "listening" && Live.listen(), code === 7 || code === 6 ? 150 : 600); };   // sin coincidencias / silencio: se sigue escuchando
  window.__nexusSpoken = () => { if (Live.on && Live.phase === "speaking") { clearInterval(Live.fakeIv); Live.level(0); Live.listen(); } };
  $("#btn-live").onclick = () => (Live.on ? Live.end() : Live.start());
  $("#live-end").onclick = () => Live.end();
  $("#live-pause").onclick = () => Live.pause();
  addEventListener("keydown", (e) => { if (e.key === "Escape" && Live.on) Live.end(); });
  window.NexusLive = Live;

  /* ================================================================ aprobaciones */
  function showApproval(a) {
    if ($(`#ap-${a.id}`)) return;
    const el = document.createElement("div"); el.className = "approval"; el.id = `ap-${a.id}`;
    el.innerHTML = `<h4>⚠ APROBACIÓN REQUERIDA ${a.level === "admin" ? "· ADMIN" : ""}</h4>
      <div><b>${esc(a.agent)}</b>${a.remote ? " (nodo remoto)" : ""} quiere usar <b>${esc(a.tool)}</b> <span class="tag ${a.risk}">${a.risk}</span></div>
      <pre class="code" style="max-height:140px;margin:8px 0">${esc(JSON.stringify(a.args, null, 1))}</pre>
      <div class="row" style="justify-content:flex-end"><button class="btn danger sm">DENEGAR</button><button class="btn ok sm">APROBAR</button></div>`;
    const [deny, ok] = $$("button", el);
    const resolve = (v) => guard(async () => { await api(`/api/approvals/${a.id}`, { body: { approve: v } }); el.remove(); })();
    deny.onclick = () => resolve(false); ok.onclick = () => resolve(true);
    if (a.level === "admin" && !S.token) ok.title = "Requiere modo administrador";
    $("#approvals").appendChild(el); setState("tool");
  }

  /* ================================================================ estado general */
  async function loadStatus() {
    const s = await api("/api/status"); S.status = s;
    $("#ver").textContent = "v" + s.version;
    $("#pill-llm").textContent = `${s.llm.backend}${s.llm.model ? " · " + s.llm.model : ""}`;
    $("#chat-model").textContent = `${s.llm.backend} · ${s.llm.model || "—"}`;
    $("#pill-profile").textContent = `${s.profile.tier} · ${s.profile.accelerator}`;
    const mem = Object.values(s.memory.by_kind).reduce((a, b) => a + b, 0);
    $("#pill-mem").textContent = `${mem} recuerdos · ${s.graph.entities} nodos · ${s.rag.documents} docs`; $("#dot-mem").className = "dot ok";
    $("#pill-net").textContent = s.coop.running ? `${s.coop.vip} · ${s.coop.peers} nodos` : "off"; $("#dot-net").className = "dot " + (s.coop.running ? "ok live" : "");
    $("#hud-right").textContent = s.profile.accelerator.toUpperCase() + (s.npu.present ? " · NPU" : "");
    const tune = s.suggest_smaller_model ? "Rendimiento bajo: prueba un modelo más pequeño" : (s.tuning[0] || "");
    $("#pill-tune").classList.toggle("hidden", !tune); $("#pill-tune-t").textContent = tune.slice(0, 60);
    const bt = $("#badge-tools"), n = s.pending_approvals + (S.token ? s.pending_proposals : 0);
    bt.textContent = n; bt.classList.toggle("hidden", !n);
    $("#badge-more").textContent = n; $("#badge-more").classList.toggle("hidden", !n);   // herramientas vive en «Más» en el movil
    if (s.admin !== !!S.token) setAdmin(s.admin);
  }
  async function loadConfig() {
    const c = await api("/api/config"); S.cfg = c.user; S.fullCfg = c;
    const th = c.user.theme && Object.keys(c.user.theme).length ? c.user.theme
      : { preset: "neon", ...(c.user.accent_color && c.user.accent_color !== "#00f0ff" ? { accent: c.user.accent_color } : {}) };
    const applied = NexusThemes.apply(th);
    S.cfg.accent_color = applied.accent;
    avatar.setAccent(applied.accent);
    $("#avatar-wrap").parentElement.classList.toggle("hidden", c.user.avatar_enabled === false);
    if (c.user.avatar_enabled !== false) applyAvatar(c.user.avatar_style || "geisha", c.user.avatar_model || "");
  }
  async function loadAgents() {
    S.agents = await api("/api/agents");
    const sel = $("#agent-select"), cur = sel.value || S.cfg.default_agent || "auto";
    sel.innerHTML = `<option value="auto">◇ Automático</option>` + S.agents.filter((a) => a.enabled).map((a) => `<option value="${a.id}">${esc(a.icon)} ${esc(a.name)}</option>`).join("");
    sel.value = [...sel.options].some((o) => o.value === cur) ? cur : "auto";
  }
  const spark = []; async function sideStats() {
    const m = await api("/api/monitor"), c = m.current || {};
    $("#ss-cpu").textContent = c.cpu != null ? Math.round(c.cpu) + "%" : "--";
    $("#ss-ram").textContent = c.ram_pct != null ? Math.round(c.ram_pct) + "%" : "--";
    const last = m.llm_calls[m.llm_calls.length - 1]; $("#ss-tps").textContent = last ? last.tps.toFixed(1) : "--";
    spark.push(c.cpu || 0); if (spark.length > 60) spark.shift();
    lines($("#spark"), [spark], ["#00f0ff"], 100, false);
    if (S.view === "system") drawMonitor(m);
  }
  function lines(canvas, series, colors, max = 100, grid = true) {
    const r = canvas.getBoundingClientRect(), d = devicePixelRatio || 1; canvas.width = r.width * d; canvas.height = r.height * d;
    const ctx = canvas.getContext("2d"); ctx.scale(d, d); const w = r.width, h = r.height;
    if (grid) { ctx.strokeStyle = "rgba(0,240,255,.08)"; for (let i = 1; i < 4; i++) { ctx.beginPath(); ctx.moveTo(0, (h * i) / 4); ctx.lineTo(w, (h * i) / 4); ctx.stroke(); } }
    series.forEach((s, k) => {
      if (!s.length) return; ctx.strokeStyle = colors[k]; ctx.lineWidth = 1.8; ctx.shadowColor = colors[k]; ctx.shadowBlur = 8; ctx.beginPath();
      s.forEach((v, i) => { const x = (i / Math.max(s.length - 1, 1)) * w, y = h - (Math.min(v, max) / max) * (h - 4) - 2; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
      ctx.stroke(); ctx.shadowBlur = 0;
      const g = ctx.createLinearGradient(0, 0, 0, h); g.addColorStop(0, colors[k] + "33"); g.addColorStop(1, colors[k] + "00");
      ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.fillStyle = g; ctx.fill();
    });
  }

  /* ================================================================ vista: SALON */
  let room = null;
  const kindLabel = { task: "tarea", intro: "presentación", insight: "aprendizaje", delegation: "delegación", proposal: "propuesta", summary: "resumen", system: "reunión", chat: "", feedback: "feedback" };
  function loungeLine(m) {
    const a = S.agents.find((x) => x.id === m.agent), color = m.agent === "usuario" || m.agent === "admin" ? "#ffffff" : (a?.color || "#00f0ff");
    return `<div class="lm ${m.kind}" style="--c:${color}"><span class="k">${kindLabel[m.kind] || m.kind} · ${ago(m.ts)}</span><b>${esc(a?.name || m.agent).toUpperCase()}</b>${esc(m.content).replace(/\n/g, "<br>")}</div>`;
  }
  function onLounge(m) {
    if (room) room.say(m);
    if (m.meta && m.meta.event === "meeting_start") meetBusy(true);
    if (m.meta && m.meta.event === "meeting_end") meetBusy(false);
    const log = $("#lounge-log");
    if (log && S.view === "lounge") { log.insertAdjacentHTML("beforeend", loungeLine(m)); log.scrollTop = log.scrollHeight; }
    else if (m.kind !== "task") { const b = $("#badge-lounge"); b.textContent = (+b.textContent || 0) + 1; b.classList.remove("hidden"); }
    if (m.kind === "proposal" || m.kind === "summary") setTimeout(() => S.view === "lounge" && loaders.lounge(), 400);
  }
  loaders.lounge = async () => {
    $("#badge-lounge").classList.add("hidden"); $("#badge-lounge").textContent = "";
    const d = await api("/api/lounge");
    if (!room) window.__lounge = room = new LoungeRoom($("#lounge-canvas"));
    room.setAgents(d.agents);
    $("#lounge-log").innerHTML = d.messages.map(loungeLine).join("") || `<div class="muted">El salón está tranquilo. Convoca una reunión.</div>`;
    $("#lounge-log").scrollTop = 1e9; $("#lounge-count").textContent = `${d.messages.length} mensajes`; meetBusy(!!d.busy);
    d.messages.filter((m) => m.kind !== "task").slice(-4).forEach((m) => room.say(m));
    // matriz de habilidades
    const topics = {}; Object.values(d.skills).flat().forEach((r) => (topics[r.topic] = (topics[r.topic] || 0) + r.ok + r.fail));
    const cols = Object.entries(topics).sort((a, b) => b[1] - a[1]).slice(0, 8).map((x) => x[0]);
    const rows = Object.keys(d.skills).filter((a) => a !== "usuario");
    $("#lounge-skills").innerHTML = rows.length ? `<div class="skill-matrix"><table class="tbl"><tr><th>AGENTE</th>${cols.map((c) => `<th>${esc(c)}</th>`).join("")}</tr>${rows.map((a) => {
      const by = Object.fromEntries(d.skills[a].map((r) => [r.topic, r]));
      return `<tr><td style="color:${agentColor(a)}">${esc(agentName(a))}</td>${cols.map((c) => { const r = by[c]; if (!r) return `<td class="h muted">·</td>`;
        const hue = r.score >= 0.6 ? "var(--ok)" : r.score < 0.45 ? "var(--bad)" : "var(--warn)"; return `<td class="h" style="color:${hue}" title="${r.ok} ok / ${r.fail} fallos">${Math.round(r.score * 100)}%</td>`; }).join("")}</tr>`; }).join("")}</table></div>`
      : `<span class="muted">Aún no hay tareas registradas. El directorio se construye con el uso y tu feedback.</span>`;
    $("#lounge-gaps").innerHTML = d.gaps.length ? d.gaps.map((g) => `<div class="item"><span class="tag warn">carencia</span> <b>${esc(g.topic)}</b> <span class="muted">· ${g.count} tareas${g.struggling ? ` · ${g.struggling} flojas` : ""}${g.already_proposed ? " · ya propuesto" : ""}</span></div>`).join("") : `<span class="muted">Sin carencias detectadas.</span>`;
    $("#lounge-proposals").innerHTML = d.proposals.map((p) => `<div class="item"><div class="row"><span style="color:${p.spec.color};font-size:20px">${esc(p.spec.icon)}</span><div style="flex:1"><div class="t">${esc(p.spec.name)} <span class="tag ${p.status === "aprobada" ? "ok" : p.status === "rechazada" ? "bad" : "warn"}">${p.status}</span></div><div class="d">${esc(p.reason)}</div></div></div>
      <div class="d mono" style="margin:6px 0">${esc(p.spec.description)} · claves: ${esc((p.spec.keywords || []).join(", "))}</div>
      ${p.status === "pendiente" ? `<div class="row admin-only"><button class="btn ok sm" data-ap="${p.id}">APROBAR Y CREAR</button><button class="btn danger sm" data-rj="${p.id}">RECHAZAR</button></div><div class="user-only muted">Un administrador debe aprobarlo.</div>` : ""}</div>`).join("");
    $$("[data-ap]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/lounge/proposals/${b.dataset.ap}`, { body: { approve: true } }); toast("Agente creado"); await loadAgents(); loaders.lounge(); })));
    $$("[data-rj]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/lounge/proposals/${b.dataset.rj}`, { body: { approve: false, note: prompt("Motivo (opcional)") || "" } }); loaders.lounge(); })));
  };
  function meetBusy(on) { const b = $("#lounge-meet"); b.disabled = on; b.classList.toggle("busy", on); b.textContent = on ? "REUNIÓN EN CURSO…" : "CONVOCAR REUNIÓN"; }
  $("#lounge-meet").onclick = guard(async () => {
    const r = await api("/api/lounge/meet", { body: { topic: $("#lounge-topic").value } });
    if (r.busy) { toast("Ya hay una reunión en curso"); meetBusy(true); return; }
    toast("Reunión convocada"); meetBusy(true); $("#lounge-topic").value = "";
  });
  const loungeSay = guard(async () => { const t = $("#lounge-say").value.trim(); if (!t) return; await api("/api/lounge/say", { body: { text: t } }); $("#lounge-say").value = ""; });
  $("#lounge-say-btn").onclick = loungeSay; $("#lounge-say").onkeydown = (e) => e.key === "Enter" && loungeSay();

  /* ================================================================ vista: NUCLEO (memoria) */
  let memFilter = "";
  loaders.memory = async () => {
    if (!explorer) explorer = new NeuralAvatar($("#graph-canvas"), { explore: true, maxLabels: 40, radius: 0.4 });
    explorer.setAccent(S.cfg.accent_color || "#00f0ff"); await loadScopes(); await loadGraph();
    $("#legend").innerHTML = Object.entries(NEXUS_TYPE_COLORS).map(([k, c]) => `<span><i style="background:${c};color:${c}"></i>${k}</span>`).join("");
    const d = await api(`/api/memory?limit=150${memFilter ? "&kind=" + memFilter : ""}`);
    const st = d.stats.by_kind;
    $("#mem-stats").innerHTML = [["mensajes", d.stats.messages], ["hechos", st.fact || 0], ["preferencias", st.preference || 0], ["resúmenes", st.summary || 0], ["ejemplos", st.example || 0], ["lecciones", st.lesson || 0]]
      .map(([k, v]) => `<div class="stat"><span class="v">${v}</span><span class="k">${k}</span></div>`).join("") + `<div class="muted mono" style="grid-column:1/-1">embeddings: ${esc(d.stats.embedder)}</div>`;
    $("#mem-filters").innerHTML = ["", "fact", "preference", "summary", "example", "lesson", "note", "episode"].map((k) => `<button class="btn sm ${k === memFilter ? "primary" : "ghost"}" data-k="${k}">${k || "todos"}</button>`).join("");
    $$("#mem-filters button").forEach((b) => (b.onclick = () => { memFilter = b.dataset.k; loaders.memory(); }));
    renderMem(d.items);
    await loadRag();
    await loadCrawls();
  };
  async function loadRag() {
    const d = await api("/api/rag");
    $("#rag-stats").textContent = `${d.stats.documents} documentos · ${d.stats.chunks} fragmentos`;
    const moveOpts = (cur) => graphScopes.map((s) => `<option value="${esc(s.id)}" ${s.id === (cur || "") ? "selected" : ""}>${s.id ? `${esc(s.icon)} ${esc(s.name)}` : "◎ general"}</option>`).join("");
    // lotes de un rastreo y partes de un archivo, juntos y en su orden (1, 2, 3...) donde esta el mas reciente
    const grp = (doc) => doc.meta.job ? "j" + doc.meta.job : doc.meta.archive_root ? "a" + doc.meta.archive_root : doc.meta.part_ids ? "a" + doc.id : "";
    const seen = new Set(), docs = [];
    d.documents.forEach((doc) => { const g = grp(doc); if (!g) return docs.push(doc); if (seen.has(g)) return; seen.add(g);
      docs.push(...d.documents.filter((x) => grp(x) === g).sort((a, b) => a.id - b.id)); });
    $("#rag-list").innerHTML = docs.map((doc) => `<div class="item row"><span class="tag accent">${esc(doc.kind)}</span><div style="flex:1"><div class="t">${esc(doc.title)} <span class="tag ${doc.agent ? "warn" : ""}" title="Grafo en el que está">${doc.agent ? esc(scopeLabel(doc.agent)) : "general"}</span></div>
      <div class="d mono">${esc(doc.source).slice(0, 90)} · ${doc.kind === "zip" ? `${doc.meta.files} ficheros (${doc.meta.indexed} indexados) en sandbox/${esc(doc.meta.sandbox)}` : doc.kind === "web" ? `${doc.pages} páginas web rastreadas` : `${doc.pages} pág.`} · ${doc.chunks} fragmentos · ${ago(doc.added)}</div>${doc.meta?.summary ? `<div class="d">${esc(doc.meta.summary).slice(0, 220)}…</div>` : ""}</div>
      ${graphScopes.length > 1 ? `<select class="input" data-ragmv="${doc.id}" title="Mover a otro grafo" style="width:auto">${moveOpts(doc.agent)}</select>` : ""}<a class="btn sm" href="/api/rag/${doc.id}/file" target="_blank">ABRIR</a><button class="btn danger sm" data-ragdel="${doc.id}">✕</button></div>`).join("") || `<span class="muted">Biblioteca vacía: procesa una URL o añade documentos.</span>`;
    $$("[data-ragmv]").forEach((sel) => (sel.onchange = guard(async () => { await api(`/api/rag/${sel.dataset.ragmv}`, { method: "PATCH", body: { agent: sel.value } }); toast(`Documento movido al grafo ${sel.value ? "de " + scopeLabel(sel.value) : "general"}`); await loadScopes(); loadRag(); })));
    $$("[data-ragdel]").forEach((b) => (b.onclick = guard(async () => { if (!confirm("¿Quitar el documento de la biblioteca?")) return; await api(`/api/rag/${b.dataset.ragdel}`, { method: "DELETE" }); loadRag(); })));
  }
  $("#rag-url-go").onclick = guard(async () => {
    const url = $("#rag-url").value.trim(); if (!/^https?:\/\//.test(url)) return toast("Introduce una URL http(s)", true);
    S.ragBusy = true; $("#rag-url-go").disabled = true; setState("tool");
    $("#rag-progress").innerHTML = `<div class="chip"><span class="spin"></span>procesando…</div>`;
    try { const r = await api("/api/rag/url", { body: { url, agent: ragTarget() } });
      $("#rag-progress").innerHTML = md(`[📄 ${r.title}](${r.url})`) + `<div class="muted mono">${r.pages} páginas · ${r.sections} secciones · ${r.words} palabras · ${r.images_embedded} imágenes · ${r.rag_chunks} fragmentos RAG · ${r.learned} conocimientos · ${r.seconds} s</div><p>${esc(r.summary)}</p>`;
      $("#rag-url").value = ""; toast("Documento creado, archivado y aprendido"); loaders.memory();
    } finally { S.ragBusy = false; $("#rag-url-go").disabled = false; setState("idle"); }
  });
  $("#rag-crawl-scope").onchange = () => { const d = $("#rag-crawl-scope").value === "domain"; $("#rag-crawl-max").disabled = d; $("#rag-crawl-max").title = d ? "Todo el dominio: sin límite de páginas" : "Máximo de páginas en total. Si no cabe en un lote, se rastrea por lotes correlativos y reanudables"; };
  $("#rag-crawl-go").onclick = guard(async () => {
    const url = $("#rag-crawl").value.trim(); if (!/^https?:\/\//.test(url)) return toast("Introduce una URL http(s)", true);
    const max_pages = Math.max(1, Math.min(100000, +$("#rag-crawl-max").value || 40)), scope = $("#rag-crawl-scope").value;
    const max_depth = max_pages > 500 ? 50 : 3;   // sitio entero: hay que poder llegar a todas sus páginas
    const r = await api("/api/rag/crawl", { body: { url, max_pages, max_depth, scope, agent: ragTarget(), background: true } });
    $("#rag-crawl").value = "";
    toast(r.job.complete ? `Rastreando TODO el dominio por lotes de ${r.job.batch_pages}: al terminar se crea su grafo de conocimiento`
          : r.job.max_pages > r.job.batch_pages ? `Rastreo en marcha por lotes de ${r.job.batch_pages} páginas` : "Rastreo en marcha");
    loadCrawls();
  });
  let crawlTimer = null, crawlStep = "";
  const crawlLabel = { "en curso": "ok", "completado": "accent", "pausado": "warn", "interrumpido": "warn", "error": "bad" };
  async function loadCrawls() {
    const jobs = await api("/api/rag/crawls");
    const live = jobs.some((j) => j.running);
    $("#rag-jobs").innerHTML = jobs.slice(0, 12).map((j) => {
      const pct = Math.min(100, Math.round(100 * j.pages_done / Math.max(1, j.pending ? j.pages_done + Math.min(j.pending, j.max_pages - j.pages_done) : j.pages_done)));
      const last = j.batches[j.batches.length - 1];
      return `<div class="item"><div class="row"><span class="tag ${crawlLabel[j.status] || ""}">${esc(j.status)}</span><div style="flex:1"><div class="t">${esc(j.title || j.url)}</div>
        <div class="d mono">${j.pages_done} págs. en ${j.batches.length} lote${j.batches.length === 1 ? "" : "s"} de ${j.batch_pages}${j.pending ? ` · ${j.pending} pendientes` : ""} · ${j.complete ? "sin límite" : `máx. ${j.max_pages}`} · ${esc(j.scope === "domain" ? "todo el dominio y subdominios" : "sección")}${j.reason ? ` · ${esc(j.reason)}` : ""}${j.error ? ` · ${esc(j.error)}` : ""}</div>
        ${j.knowledge ? `<div class="d">🧠 Grafo del dominio: ${j.knowledge.nodes} nodos · ${j.knowledge.concepts} conceptos · ${j.knowledge.facts} afirmaciones (${(j.knowledge.facts_by_status || {})["contrastada"] || 0} contrastadas) · ${j.knowledge.topics} temas${j.knowledge.reasoned_topics ? ` (${j.knowledge.reasoned_topics} razonados)` : ""}</div>` : j.knowledge_error ? `<div class="d bad">${esc(j.knowledge_error)}</div>` : ""}
        ${j.running && crawlStep ? `<div class="d"><span class="spin"></span> ${esc(crawlStep)}</div>` : last ? `<div class="d">Último: ${esc(last.title)}</div>` : ""}</div>
        ${j.running ? `<button class="btn sm" data-cjpause="${j.id}">PAUSAR</button>` : j.status !== "completado" ? `<button class="btn sm primary" data-cjresume="${j.id}">REANUDAR</button>` : ""}
        ${j.complete && j.pages_done && !j.running ? `<button class="btn sm" data-cjkg="${j.id}" title="Razonar ya el grafo multicapa con lo leído">${j.knowledge ? "REHACER GRAFO" : "CREAR GRAFO"}</button>` : ""}
        ${j.knowledge ? `<a class="btn sm" href="/api/rag/crawls/${j.id}/knowledge" title="Paquete importable: grafo + conocimiento + manifiesto">⬇ ZIP</a><select class="input sm" data-cjto="${j.id}" style="width:auto">${[["", "general"], ...S.agents.map((a) => [a.id, a.name])].map(([v, n]) => `<option value="${esc(v)}">${esc(n)}</option>`).join("")}</select><button class="btn sm primary" data-cjimp="${j.id}" title="Darle a ese agente el conocimiento del dominio">DAR A AGENTE</button>` : ""}
        <button class="btn danger sm" data-cjdel="${j.id}" title="Quitar el rastreo (los lotes ya indexados se pueden conservar)">✕</button></div>
        <div class="bar"><i style="width:${j.status === "completado" ? 100 : pct}%"></i></div></div>`;
    }).join("");
    $$("[data-cjpause]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/rag/crawls/${b.dataset.cjpause}/pause`, { body: {} }); toast("Se pausará al terminar la página en curso"); setTimeout(loadCrawls, 800); })));
    $$("[data-cjresume]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/rag/crawls/${b.dataset.cjresume}/resume`, { body: {} }); toast("Rastreo reanudado por el siguiente lote"); loadCrawls(); })));
    $$("[data-cjkg]").forEach((b) => (b.onclick = guard(async () => { b.disabled = true; toast("Razonando el grafo del dominio…"); try { const k = await api(`/api/rag/crawls/${b.dataset.cjkg}/knowledge`, { body: {} }); toast(`Grafo listo: ${k.nodes} nodos, ${k.facts} afirmaciones`); } finally { loadCrawls(); } })));
    $$("[data-cjimp]").forEach((b) => (b.onclick = guard(async () => { const agent = $(`[data-cjto="${b.dataset.cjimp}"]`).value; const r = await api(`/api/rag/crawls/${b.dataset.cjimp}/knowledge/import`, { body: { agent } }); toast(`Conocimiento dado a ${agent || "general"}: ${r.graph.nodes} nodos y ${r.rag_chunks} fragmentos`); loadRag(); })));
    $$("[data-cjdel]").forEach((b) => (b.onclick = guard(async () => { const docs = confirm("¿Borrar también de la biblioteca los lotes ya indexados?\n(Cancelar = conservarlos)"); await api(`/api/rag/crawls/${b.dataset.cjdel}?docs=${docs}`, { method: "DELETE" }); loadCrawls(); loadRag(); })));
    clearTimeout(crawlTimer);
    if (live && S.view === "memory") crawlTimer = setTimeout(() => loadCrawls().catch(() => {}), 2500);
    if (!live) crawlStep = "";
  }
  $("#rag-kg-import").onclick = () => $("#rag-kg-file").click();
  $("#rag-kg-file").onchange = guard(async (e) => {
    const f = e.target.files[0]; e.target.value = "";
    if (!f) return;
    const fd = new FormData(); fd.append("file", f); fd.append("agent", ragTarget());
    const r = await api("/api/rag/domain-knowledge/import", { body: fd });
    toast(`Conocimiento importado en ${r.agent || "general"}: ${r.graph.nodes} nodos y ${r.rag_chunks} fragmentos`); loadRag();
  });
  const ragDrop = $("#rag-drop");
  ragDrop.onclick = () => $("#rag-file").click();
  ragDrop.ondragover = (e) => { e.preventDefault(); ragDrop.classList.add("over"); }; ragDrop.ondragleave = () => ragDrop.classList.remove("over");
  const ragUpload = guard(async (files) => {   // varios a la vez: el servidor los lee en paralelo (un fichero por núcleo)
    const out = []; let secs = 0;
    try {
      for (let i = 0; i < files.length; i += 20) {
        const part = files.slice(i, i + 20);
        $("#rag-progress").innerHTML = `<div class="chip"><span class="spin"></span>importando ${files.length > 1 ? `${i + 1}–${i + part.length} de ${files.length}` : esc(part[0].name)}…</div>`;
        const fd = new FormData(); part.forEach((f) => fd.append("files", f)); fd.append("agent", ragTarget());
        const r = await api("/api/rag/import", { body: fd }); out.push(...r.results); secs += r.seconds;
      }
    } finally { loadRag(); }
    const ok = out.filter((r) => r.doc), bad = out.filter((r) => r.error);
    $("#rag-progress").innerHTML = `<div class="muted mono">${ok.length} importados en el grafo ${ragTarget() ? "de " + esc(scopeLabel(ragTarget())) : "general"}${bad.length ? ` · ${bad.length} con error` : ""} · ${secs.toFixed(1)} s</div>` +
      ok.map((r) => `<div class="muted">✓ ${esc(r.name)}: ${r.doc.meta.sandbox ? `${r.doc.meta.files} ficheros, ${r.doc.meta.indexed} indexados` : `${r.doc.pages} pág.`} · ${r.doc.chunks} fragmentos</div>`).join("") +
      bad.map((r) => `<div class="err">✕ ${esc(r.name)}: ${esc(r.error)}</div>`).join("");
    toast(bad.length ? `${ok.length} importados, ${bad.length} con error` : `${ok.length} ${ok.length === 1 ? "archivo importado" : "archivos importados"}`, !ok.length);
  });
  ragDrop.ondrop = (e) => { e.preventDefault(); ragDrop.classList.remove("over"); ragUpload([...e.dataTransfer.files]); };
  $("#rag-file").onchange = (e) => { ragUpload([...e.target.files]); e.target.value = ""; };
  let rt; $("#rag-search").oninput = (e) => { clearTimeout(rt); rt = setTimeout(guard(async () => { const q = e.target.value.trim(); if (!q) return loadRag();
    const hits = await api(`/api/rag/search?q=${encodeURIComponent(q)}`);
    $("#rag-list").innerHTML = hits.map((h) => `<div class="item rag-hit"><div class="src">${esc(h.title)} · p. ${h.page} · relevancia ${h.score}</div>${esc(h.text).slice(0, 400)}…</div>`).join("") || `<span class="muted">Sin resultados.</span>`; }), 300); };
  function renderMem(items) {
    $("#mem-list").innerHTML = items.map((m) => `<div class="item row"><span class="tag accent">${m.kind}</span><div style="flex:1">${esc(m.content)}<div class="d">importancia ${(m.importance ?? 0).toFixed(2)}${m.score ? " · relevancia " + m.score : ""} · ${m.source || ""}</div></div><button class="btn danger sm" data-del="${m.id}">✕</button></div>`).join("") || `<span class="muted">Sin recuerdos.</span>`;
    $$("[data-del]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/memory/${b.dataset.del}`, { method: "DELETE" }); b.closest(".item").remove(); })));
  }
  let mt; $("#mem-search").oninput = (e) => { clearTimeout(mt); mt = setTimeout(guard(async () => { const q = e.target.value.trim(); if (!q) return loaders.memory(); renderMem(await api(`/api/memory/search?q=${encodeURIComponent(q)}`)); }), 300); };
  $("#mem-add").onclick = guard(async () => { const c = $("#mem-new").value.trim(); if (!c) return; await api("/api/memory", { body: { content: c, kind: $("#mem-kind").value } }); $("#mem-new").value = ""; toast("Memorizado"); loaders.memory(); });

  /* ================================================================ vista: ESTUDIO */
  loaders.studio = async () => {
    const st = S.status || (await api("/api/status"));
    $("#img-engine").textContent = `${st.image.engine}${st.image.model ? " · " + st.image.model : ""}${st.image.runtime ? " · " + st.image.runtime : ""}`;
    $("#vision-engine").textContent = st.llm.backend === "builtin" ? "análisis básico (sin LLM)" : "visión con " + st.llm.model;
    const o = await api("/api/outputs");
    $("#gallery").innerHTML = o.images.map((u) => `<img src="${u}" loading="lazy">`).join("") || `<span class="muted">Aún no hay imágenes.</span>`;
    $$("#gallery img").forEach((i) => (i.onclick = () => lightbox(i.src)));
  };
  // ---- mejorador de prompts
  async function enhanceInto(el, target, style = "", negEl = null, btn = null) {
    const text = el.value.trim(); if (!text) return toast("Escribe primero una idea", true);
    if (btn) { btn.disabled = true; btn.dataset.t = btn.textContent; btn.textContent = "…"; }
    try {
      const r = await api("/api/prompt/enhance", { body: { text, target, style } });
      el.value = r.prompt; if (negEl && r.negative && !negEl.value) negEl.value = r.negative;
      el.dispatchEvent(new Event("input")); toast(`Prompt mejorado (${r.engine === "llm" ? "con IA" : "por reglas"})`);
    } finally { if (btn) { btn.disabled = false; btn.textContent = btn.dataset.t; } }
  }
  api("/api/prompt/styles").then((st) => { $("#img-style").insertAdjacentHTML("beforeend", Object.keys(st).map((k) => `<option value="${k}">${k}</option>`).join("")); }).catch(() => {});
  $("#img-enh").onclick = guard(() => enhanceInto($("#img-prompt"), "image", $("#img-style").value, $("#img-neg"), $("#img-enh")));
  $("#mus-enh").onclick = guard(() => enhanceInto($("#mus-prompt"), "music", "", null, $("#mus-enh")));
  $("#btn-enhance").onclick = guard(() => enhanceInto($("#input"), "chat", "", null, $("#btn-enhance")));
  $("#img-go").onclick = guard(async () => {
    const p = $("#img-prompt").value.trim(); if (!p) return;
    $("#img-go").disabled = true; $("#img-result").innerHTML = `<div class="chip"><span class="spin"></span>generando…</div>`; setState("tool");
    try { const r = await api("/api/image/generate", { body: { prompt: p, negative: $("#img-neg").value } });
      $("#img-result").innerHTML = `<img class="preview-img" src="${r.url}"><div class="muted mono">${esc(r.engine)}${r.seconds ? " · " + r.seconds + " s" : ""} · semilla ${r.seed}</div>${r.note ? `<div class="muted">${esc(r.note)}</div>` : ""}`;
      $("#img-result img").onclick = (e) => lightbox(e.target.src); loaders.studio();
    } finally { $("#img-go").disabled = false; setState("idle"); }
  });
  $("#mus-go").onclick = guard(async () => {
    const p = $("#mus-prompt").value.trim() || "relajante"; $("#mus-go").disabled = true; setState("tool");
    $("#mus-result").innerHTML = `<div class="chip"><span class="spin"></span>componiendo…</div>`;
    try { const r = await api("/api/music/generate", { body: { prompt: p, seconds: +$("#mus-sec").value || 20 } });
      $("#mus-result").innerHTML = `<audio controls autoplay src="${r.url}" style="width:100%"></audio><div class="row mono muted"><span>${esc(r.mood)}</span><span>${esc(r.key)}</span><span>${r.tempo} BPM</span><span>${r.duration} s</span>${r.midi_url ? `<a href="${r.midi_url}" download>MIDI</a>` : ""}<a href="${r.url}" download>WAV</a></div>`;
      const audio = $("#mus-result audio"); actx = actx || new AudioContext(); const an = actx.createAnalyser(); an.fftSize = 512;
      actx.createMediaElementSource(audio).connect(an); an.connect(actx.destination); let end = false; audio.onended = () => (end = true); levelLoop(an, () => end || audio.paused);
    } finally { $("#mus-go").disabled = false; setState("idle"); }
  });
  const drop = $("#ana-drop");
  drop.onclick = () => $("#ana-file").click();
  drop.ondragover = (e) => { e.preventDefault(); drop.classList.add("over"); }; drop.ondragleave = () => drop.classList.remove("over");
  drop.ondrop = (e) => { e.preventDefault(); drop.classList.remove("over"); if (e.dataTransfer.files[0]) analyze(e.dataTransfer.files[0]); };
  $("#ana-file").onchange = (e) => e.target.files[0] && analyze(e.target.files[0]);
  const analyze = guard(async (file) => {
    $("#ana-result").innerHTML = `<div class="chip"><span class="spin"></span>analizando…</div>`; setState("thinking");
    const fd = new FormData(); fd.append("file", file); fd.append("question", $("#ana-q").value);
    try { const r = await api("/api/image/analyze", { body: fd });
      $("#ana-result").innerHTML = `<img class="preview-img" src="${URL.createObjectURL(file)}" style="max-height:220px"><div class="bubble" style="margin-top:8px">${md(r.description)}</div>
        <div class="row" style="margin-top:6px">${r.stats.dominant_colors.map((c) => `<span class="tag" style="color:${c};border-color:${c}">${c}</span>`).join("")}<span class="muted mono">${esc(r.engine)}</span></div>`;
    } finally { setState("idle"); }
  });

  /* ================================================================ vista: DISPOSITIVOS */
  const NOMBRE_ESTADO = { idle: "En línea", listening: "Escuchando", thinking: "Pensando", tool: "Herramienta", speaking: "Hablando", success: "Hecho", error: "Error", alert: "Aviso" };
  async function ledCard(d) {
    let st = null, fx = [];
    try { st = await api(`/api/dispositivos/leds/${encodeURIComponent(d.id)}/api/state`); const e = await api(`/api/dispositivos/leds/${encodeURIComponent(d.id)}/api/effects`); fx = Array.isArray(e) ? e : e.effects || []; } catch (e) { st = null; }
    const id = esc(d.id), fxName = (f, i) => (typeof f === "string" ? f : f.name || f.id || "efecto " + i);
    return `<div class="panel" data-led="${id}"><div class="row"><span class="live-dot ${st ? "" : "off"}"></span><b>${esc(d.name)}</b><span class="muted">${esc(d.host)}:${d.port} · ${d.leds || "?"} LED${d.version ? " · v" + esc(d.version) : ""}</span><span class="spacer"></span>
      <button class="btn sm" data-led-del="${id}">QUITAR</button></div>
      ${st ? `<div class="row" style="margin-top:8px"><button class="btn sm ${st.on ? "primary" : ""}" data-led-on="${id}" data-v="${st.on ? 0 : 1}">${st.on ? "ENCENDIDA" : "APAGADA"}</button>
        <input type="range" min="1" max="255" value="${st.bri}" data-led-bri="${id}" style="flex:1 1 120px">
        <select class="input" data-led-fx="${id}" style="flex:1 1 140px;min-width:0">${fx.map((f, i) => `<option value="${i}" ${st.fx === i || (st.seg && st.seg[0] && st.seg[0].fx === i) ? "selected" : ""}>${esc(fxName(f, i))}</option>`).join("")}</select>
        <a class="btn sm" href="http://${esc(d.host)}:${d.port}/" target="_blank" rel="noopener">SU PÁGINA</a></div>
        <label class="row muted" style="margin-top:6px"><input type="checkbox" data-led-follow="${id}" ${d.follow_state !== false ? "checked" : ""}> Mostrar el estado de Nexus (escucha, piensa, habla con vúmetro…)</label>` : '<p class="muted">No responde. Comprueba que está encendido y en la misma red.</p>'}</div>`;
  }
  loaders.devices = async () => {
    const d = await api("/api/dispositivos");
    $("#dv-modo").textContent = "activo: " + (d.modos.find((m) => m.id === d.modo)?.nombre || d.modo);
    $("#dv-modos").innerHTML = d.modos.map((m) => `<button class="btn ${m.id === d.modo ? "primary" : ""}" data-modo="${esc(m.id)}" title="${esc(m.descripcion || "")}">${esc(m.icono || "")} ${esc(m.nombre)}</button>`).join("");
    $$("[data-modo]").forEach((b) => (b.onclick = guard(async () => { await api("/api/dispositivos/modo", { body: { modo: b.dataset.modo, origen: "escritorio" } }); toast("Modo " + b.textContent.trim()); loaders.devices(); })));
    $("#dv-leds").innerHTML = d.leds.length ? (await Promise.all(d.leds.map(ledCard))).join("") : '<p class="muted">Sin luces. Graba el firmware NexusLED en un ESP32 (panel Firmware), conéctalo a tu Wi-Fi y pulsa BUSCAR EN LA RED.</p>';
    const L = (id) => `/api/dispositivos/leds/${encodeURIComponent(id)}/api/state`;
    $$("[data-led-del]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/dispositivos/leds/${encodeURIComponent(b.dataset.ledDel)}`, { method: "DELETE" }); loaders.devices(); })));
    $$("[data-led-on]").forEach((b) => (b.onclick = guard(async () => { await api(L(b.dataset.ledOn), { body: { on: b.dataset.v === "1" } }); loaders.devices(); })));
    $$("[data-led-bri]").forEach((r) => (r.onchange = guard(() => api(L(r.dataset.ledBri), { body: { bri: Number(r.value) } }))));
    $$("[data-led-fx]").forEach((r) => (r.onchange = guard(() => api(L(r.dataset.ledFx), { body: { fx: Number(r.value) } }))));
    $$("[data-led-follow]").forEach((c) => (c.onchange = guard(() => api(`/api/dispositivos/leds/${encodeURIComponent(c.dataset.ledFollow)}`, { method: "PUT", body: { follow_state: c.checked } }))));
    $("#dv-clients").innerHTML = d.clientes.length ? `<table class="tbl"><tr><th>Dispositivo</th><th>Tipo</th><th>IP</th><th>Visto</th></tr>${d.clientes.map((c) => `<tr><td>${esc(c.id)}</td><td>${esc(c.tipo)}</td><td class="mono">${esc(c.ip || "")}</td><td class="mono muted">hace ${Math.round(Date.now() / 1000 - c.visto)} s</td></tr>`).join("")}</table>` : '<p class="muted">Ningún reloj ni Pico W conectado ahora. Al grabarlos se les configura la Wi-Fi y la dirección de este equipo.</p>';
    if (!$("#dv-pico").childElementCount) $("#dv-pico").innerHTML = ["idle", "listening", "thinking", "tool", "speaking", "error"].map((s) => `<figure style="margin:0;text-align:center"><img src="/api/dispositivos/pico/${s}.png" alt="${s}" style="width:150px;image-rendering:pixelated;border:1px solid var(--line);border-radius:4px;background:#fff"><figcaption class="muted" style="font-size:.75rem">${NOMBRE_ESTADO[s]}</figcaption></figure>`).join("");
    if (window.NexusFirmware) NexusFirmware.render($("#dv-fw")); else $("#dv-fw").innerHTML = '<p class="muted">El firmware y su código fuente están en la carpeta <code>firmware/</code> de la instalación.</p>';
  };
  $("#dv-scan").onclick = guard(async () => {
    $("#dv-found").innerHTML = '<span class="chip"><span class="spin"></span>Buscando…</span>';
    const r = await api("/api/dispositivos/leds/buscar" + ($("#dv-ip").value.trim() ? "?ip=" + encodeURIComponent($("#dv-ip").value.trim()) : ""));
    $("#dv-found").innerHTML = r.encontrados.length ? r.encontrados.map((f) => `<div class="row"><b>${esc(f.name)}</b><span class="muted">${esc(f.host)} · ${f.leds} LED · v${esc(f.version)}</span>${f.added ? '<span class="tag ok">añadido</span>' : `<button class="btn sm primary" data-found="${esc(f.host)}:${f.http_port || 80}">AÑADIR</button>`}</div>`).join("") : '<p class="muted">No se ha encontrado ningún NexusLED. Escribe su IP y pulsa AÑADIR.</p>';
    $$("[data-found]").forEach((b) => (b.onclick = guard(async () => { await api("/api/dispositivos/leds", { body: { host: b.dataset.found } }); toast("Luces añadidas"); $("#dv-found").innerHTML = ""; loaders.devices(); })));
  });
  $("#dv-add").onclick = guard(async () => { const ip = $("#dv-ip").value.trim(); if (!ip) return; await api("/api/dispositivos/leds", { body: { host: ip } }); toast("Luces añadidas"); loaders.devices(); });

  /* ================================================================ vista: BOLSA (mesa simulada) */
  const eur = (x) => (x ?? 0).toLocaleString("es-ES", { style: "currency", currency: "EUR" });
  const num = (x, d = 2) => (x ?? 0).toLocaleString("es-ES", { maximumFractionDigits: d });
  const pct = (x) => `<span style="color:var(${x > 0 ? "--ok" : x < 0 ? "--bad" : "--muted"})">${x > 0 ? "+" : ""}${num(x)} %</span>`;
  const EST = { pendiente: "warn", por_confirmar: "warn", confirmada: "accent", ejecutada: "ok", cancelada: "", caducada: "", rechazada: "bad" };
  const bolsaState = { cartera: local.get("nexus_bolsa_cartera", 0), data: null };
  function priceSpark(canvas, ticks) {
    const ctx = canvas.getContext("2d"), w = (canvas.width = canvas.clientWidth * devicePixelRatio), h = (canvas.height = 60 * devicePixelRatio);
    ctx.clearRect(0, 0, w, h); if (ticks.length < 2) return;
    const ps = ticks.map((t) => t.precio), lo = Math.min(...ps), hi = Math.max(...ps), span = hi - lo || 1;
    ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--accent") || "#0ff"; ctx.lineWidth = 2 * devicePixelRatio; ctx.beginPath();
    ps.forEach((p, i) => { const x = (i / (ps.length - 1)) * w, y = h - ((p - lo) / span) * (h - 6) - 3; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); }); ctx.stroke();
  }
  async function bolsaQuote() {
    const sym = $("#bo-sym").value.trim(); if (!sym) return;
    const q = await api("/api/bolsa/cotizacion/" + encodeURIComponent(sym));
    $("#bo-q").innerHTML = `<div class="row"><b>${esc(q.simbolo)}</b><span class="muted">${esc(q.nombre)} · ${esc(q.mercado)}</span><span class="tag ${q.abierto ? "ok" : ""}">${q.abierto ? "abierto" : "cerrado"}</span></div>
      <div class="row mono"><span>${num(q.precio, 4)} ${esc(q.divisa)}</span>${pct(q.variacion_pct)}<span class="muted">compra ${num(q.compra, 4)} · venta ${num(q.venta, 4)}</span></div><canvas style="width:100%;height:60px"></canvas>`;
    priceSpark($("#bo-q canvas"), q.ticks);
  }
  const orderBody = () => ({ cartera: bolsaState.cartera || null, simbolo: $("#bo-sym").value.trim(), lado: $("#bo-lado").value, cantidad: Number($("#bo-qty").value),
    tipo: $("#bo-tipo").value, limite: $("#bo-lim").value ? Number($("#bo-lim").value) : null, stop: $("#bo-stop").value ? Number($("#bo-stop").value) : null, validez: $("#bo-val").value });
  loaders.bolsa = async () => {
    const d = await api("/api/bolsa" + (bolsaState.cartera ? `?cartera=${bolsaState.cartera}` : "")), r = d.resumen; bolsaState.data = d;
    if (!d.carteras.some((c) => c.id === bolsaState.cartera)) bolsaState.cartera = r.cartera;
    $("#bolsa-aviso").textContent = d.aviso;
    $("#bolsa-modo").innerHTML = r.modo === "real" ? '<span class="tag bad">CARTERA REAL · solo seguimiento</span>' : '<span class="tag ok">SIMULADA · sin dinero</span>';
    $("#bolsa-stats").innerHTML = [["Valor total", eur(r.total_eur)], ["Efectivo", eur(r.efectivo_eur)], ["Rentabilidad", pct(r.rentabilidad_pct)], ["Gastos pagados", eur(r.gastos_eur)]]
      .map(([k, v]) => `<div class="panel stat"><span class="k">${k}</span><span class="v">${v}</span></div>`).join("");
    $("#bo-cartera").innerHTML = d.carteras.map((c) => `<option value="${c.id}" ${c.id === r.cartera ? "selected" : ""}>${esc(c.nombre)} · ${c.modo === "real" ? "REAL" : "simulada"} · ${esc(d.tarifas[c.tarifa] || c.tarifa)}</option>`).join("");
    $("#bc-tarifa").innerHTML = Object.entries(d.tarifas).map(([k, v]) => `<option value="${k}">${esc(v)}</option>`).join("");
    $("#bn-mkt").innerHTML = d.mercados.map((m) => `<option value="${m.id}">${esc(m.id)}</option>`).join("");
    $("#bolsa-mercados").innerHTML = `<table class="tbl"><tr><th>Bolsa</th><th>Estado</th><th>Hora local</th><th>Próximo</th></tr>${d.mercados.map((m) =>
      `<tr><td>${esc(m.nombre)}</td><td><span class="tag ${m.abierto ? "ok" : ""}">${m.abierto ? "abierta" : "cerrada"}</span></td><td class="mono">${esc(m.hora_local)}</td><td class="mono muted">${m.abierto ? "cierra " + esc(m.cierra) : "abre " + esc(m.abre)}</td></tr>`).join("")}</table>`;
    $("#bolsa-pos").innerHTML = r.posiciones.length ? `<table class="tbl"><tr><th>Valor</th><th>Cantidad</th><th>Coste medio</th><th>Precio</th><th>Valor €</th><th>Plusvalía</th><th>Día</th><th>Peso</th><th></th></tr>${r.posiciones.map((p) =>
      `<tr><td><b>${esc(p.simbolo)}</b><div class="muted">${esc(p.nombre)}</div></td><td class="mono">${num(p.cantidad, 6)}</td><td class="mono">${eur(p.coste_medio_eur)}</td><td class="mono">${num(p.precio, 4)} ${esc(p.divisa)}</td><td class="mono">${eur(p.valor_eur)}</td><td class="mono">${eur(p.plusvalia_eur)} ${pct(p.plusvalia_pct)}</td><td>${pct(p.variacion_dia_pct)}</td><td class="mono">${num(p.peso_pct)} %</td>
       <td><button class="btn sm" data-sell="${esc(p.simbolo)}" data-q="${p.cantidad}">VENDER</button></td></tr>`).join("")}</table>` : '<p class="muted">Sin posiciones. Busca un valor y da tu primera orden.</p>';
    $("#bolsa-ord").innerHTML = d.ordenes.length ? `<table class="tbl"><tr><th>#</th><th>Orden</th><th>Estado</th><th>Creada</th><th></th></tr>${d.ordenes.map((o) => {
      const open = ["pendiente", "por_confirmar", "confirmada"].includes(o.estado);
      return `<tr><td class="mono">${o.id}</td><td>${o.lado === "compra" ? "Compra" : "Venta"} ${num(o.cantidad, 6)} <b>${esc(o.simbolo)}</b> · ${esc(o.tipo.replace("_", " "))}${o.limite ? " lím. " + num(o.limite, 4) : ""}${o.stop ? " stop " + num(o.stop, 4) : ""}${o.precio_ejec ? `<div class="muted">a ${num(o.precio_ejec, 4)}</div>` : ""}${o.motivo ? `<div class="muted">${esc(o.motivo)}</div>` : ""}</td>
        <td><span class="tag ${EST[o.estado] || ""}">${esc(o.estado.replace("_", " "))}</span></td><td class="mono muted">${esc((o.creada || "").replace("T", " ").slice(0, 16))}</td>
        <td>${o.estado === "por_confirmar" ? `<button class="btn sm primary admin-only" data-confirm="${o.id}">CONFIRMAR</button>` : ""}${o.estado === "confirmada" ? `<button class="btn sm admin-only" data-fill="${o.id}">REGISTRAR EJECUCIÓN</button>` : ""}${open ? `<button class="btn sm" data-cancel="${o.id}">CANCELAR</button>` : ""}</td></tr>`; }).join("")}</table>` : '<p class="muted">Sin órdenes.</p>';
    $("#bolsa-ops").innerHTML = d.operaciones.length ? `<table class="tbl"><tr><th>Fecha</th><th>Operación</th><th>Efectivo</th><th>Comisión</th><th>Impuestos</th><th>Cambio</th><th>Deslizam.</th><th>Total</th><th>Resultado</th></tr>${d.operaciones.map((o) =>
      `<tr><td class="mono muted">${esc(o.fecha.replace("T", " ").slice(0, 16))}</td><td>${o.lado === "compra" ? "Compra" : "Venta"} ${num(o.cantidad, 6)} <b>${esc(o.simbolo)}</b> a ${num(o.precio, 4)}${o.aviso ? `<div class="tag warn" title="${esc(o.aviso)}">regla 2 meses</div>` : ""}</td><td class="mono">${eur(o.efectivo_eur)}</td><td class="mono">${eur(o.comision_eur)}</td><td class="mono">${eur(o.impuestos_eur)}</td><td class="mono">${eur(o.cambio_eur)}</td><td class="mono">${eur(o.deslizamiento_eur)}</td><td class="mono">${eur(o.total_eur)}</td><td class="mono">${o.ganancia_eur == null ? "" : eur(o.ganancia_eur)}</td></tr>`).join("")}</table>` : '<p class="muted">Aún no hay operaciones.</p>';
    await bolsaAgent().catch(() => {}); bolsaLive();
    if (!liveState.timer) liveState.timer = setInterval(() => { guard(bolsaLive)(); if (Date.now() - liveState.agentT > 15000) { liveState.agentT = Date.now(); guard(bolsaAgent)(); } }, 5000);
    if (!$("#bo-syms").childElementCount) $("#bo-syms").innerHTML = (await api("/api/bolsa/instrumentos")).map((i) => `<option value="${esc(i.simbolo)}">${esc(i.nombre)} · ${esc(i.mercado)}</option>`).join("");
    $$("[data-sell]").forEach((b) => (b.onclick = () => { $("#bo-sym").value = b.dataset.sell; $("#bo-lado").value = "venta"; $("#bo-qty").value = b.dataset.q; bolsaQuote().catch(() => {}); $("#bo-sym").scrollIntoView({ behavior: "smooth", block: "center" }); }));
    $$("[data-cancel]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/bolsa/orden/${b.dataset.cancel}/cancelar`, { body: {} }); loaders.bolsa(); })));
    $$("[data-confirm]").forEach((b) => (b.onclick = guard(async () => {
      const o = d.ordenes.find((x) => String(x.id) === b.dataset.confirm);
      const t = prompt(`Orden REAL: ${o.lado} ${o.cantidad} ${o.simbolo} (${o.tipo}). Nexus-Office no la envía a ningún bróker: la ejecutas tú después.\nEscribe ${d.confirmacion} para confirmarla.`);
      if (t == null) return; await api(`/api/bolsa/orden/${o.id}/confirmar`, { body: { confirmacion: t } }); toast("Orden confirmada: ejecútala en tu bróker"); loaders.bolsa(); })));
    $$("[data-fill]").forEach((b) => (b.onclick = guard(async () => {
      const px = prompt("Precio de ejecución en tu bróker (divisa del valor):"); if (!px) return;
      const com = prompt("Comisiones y gastos en € (0 si no hubo):", "0") ?? "0";
      await api(`/api/bolsa/orden/${b.dataset.fill}/ejecucion`, { body: { precio: Number(px.replace(",", ".")), comision_eur: Number(com.replace(",", ".")) } }); loaders.bolsa(); })));
  };
  /* ---- graficos en vivo (canvas propio, sin librerias: funciona sin internet) */
  const css = (v, f) => (getComputedStyle(document.documentElement).getPropertyValue(v) || "").trim() || f;
  const LEVEL_COLORS = { limite: "--accent", stop: "--warn", ejecucion: "--ok", entrada: "--muted", objetivo: "--ok" };
  function liveChart(canvas, pts, niveles = [], opt = {}) {
    const dp = devicePixelRatio || 1, W = canvas.clientWidth || 300, H = canvas.clientHeight || 180;
    canvas.width = W * dp; canvas.height = H * dp;
    const ctx = canvas.getContext("2d"); ctx.scale(dp, dp); ctx.clearRect(0, 0, W, H);
    const ps = pts.map((p) => p.precio).filter((p) => p != null);
    if (ps.length < 2) { ctx.fillStyle = css("--muted", "#789"); ctx.font = "12px sans-serif"; ctx.fillText(opt.vacio || "Esperando datos…", 12, H / 2); return; }
    const lv = niveles.map((n) => n.precio).filter((x) => x != null);
    let lo = Math.min(...ps, ...lv), hi = Math.max(...ps, ...lv); const pad = (hi - lo) * 0.08 || hi * 0.01 || 1; lo -= pad; hi += pad;
    const L = 6, R = 64, T = 8, B = 18, cw = W - L - R, ch = H - T - B;
    const t0 = pts[0].ts, t1 = pts[pts.length - 1].ts, span = t1 - t0 || 1;
    const X = (t) => L + ((t - t0) / span) * cw, Y = (p) => T + (1 - (p - lo) / (hi - lo)) * ch;
    const dec = hi >= 100 ? 2 : hi >= 1 ? 3 : 5, f = (x) => x.toLocaleString("es-ES", { maximumFractionDigits: dec, minimumFractionDigits: Math.min(2, dec) });
    ctx.strokeStyle = css("--line", "rgba(0,240,255,.15)"); ctx.lineWidth = 1; ctx.fillStyle = css("--muted", "#789"); ctx.font = "10px monospace";
    for (let i = 0; i <= 4; i++) { const p = lo + ((hi - lo) * i) / 4, y = Y(p); ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + cw, y); ctx.stroke(); ctx.fillText(f(p), L + cw + 4, y + 3); }
    const tf = (t) => { const d = new Date(t * 1000); return span > 3 * 86400 ? d.toLocaleDateString("es-ES", { day: "2-digit", month: "short" }) : d.toLocaleTimeString("es-ES", { hour: "2-digit", minute: "2-digit" }); };
    ctx.fillText(tf(t0), L, H - 4); const lt = tf(t1); ctx.fillText(lt, L + cw - ctx.measureText(lt).width, H - 4);
    const up = ps[ps.length - 1] >= ps[0], col = css(up ? "--ok" : "--bad", up ? "#7f7" : "#f57");
    const g = ctx.createLinearGradient(0, T, 0, T + ch); g.addColorStop(0, col + "55"); g.addColorStop(1, col + "00");
    ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(X(p.ts), Y(p.precio)) : ctx.moveTo(X(p.ts), Y(p.precio))));
    ctx.lineTo(X(t1), T + ch); ctx.lineTo(X(t0), T + ch); ctx.closePath(); ctx.fillStyle = g; ctx.fill();
    ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(X(p.ts), Y(p.precio)) : ctx.moveTo(X(p.ts), Y(p.precio)))); ctx.strokeStyle = col; ctx.lineWidth = 1.8; ctx.stroke();
    niveles.forEach((n) => {
      if (n.precio == null) return; const y = Y(n.precio), c = css(LEVEL_COLORS[n.tipo] || "--accent", "#0ff");
      ctx.setLineDash([5, 4]); ctx.strokeStyle = c; ctx.lineWidth = 1.2; ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + cw, y); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = c; ctx.font = "10px sans-serif"; ctx.fillText(`${n.texto} ${f(n.precio)}`, L + 4, y - 3);
      if (n.ts && n.ts >= t0 && n.ts <= t1) { ctx.beginPath(); ctx.arc(X(n.ts), y, 4, 0, 7); ctx.fill(); }
    });
    const last = ps[ps.length - 1], y = Y(last); ctx.fillStyle = col; ctx.fillRect(L + cw + 1, y - 8, R - 2, 16); ctx.fillStyle = "#000"; ctx.font = "bold 10px monospace"; ctx.fillText(f(last), L + cw + 4, y + 3);
    ctx.beginPath(); ctx.arc(X(t1), y, 3.5, 0, 7); ctx.fillStyle = col; ctx.fill();
  }
  const liveState = { timer: null, agentT: 0, orders: [] };
  async function bolsaLive() {
    if (S.view !== "bolsa") { clearInterval(liveState.timer); liveState.timer = null; return; }
    const d = bolsaState.data; if (!d) return;
    const ag = liveState.agent, agOrders = ag && ag.cartera !== d.resumen.cartera ? (ag.ordenes_activas || []) : [];
    const open = [...d.ordenes.filter((o) => ["pendiente", "por_confirmar", "confirmada"].includes(o.estado)), ...agOrders].slice(0, 12);
    const box = $("#bolsa-live");
    if (!open.length) { box.innerHTML = '<p class="muted">No hay órdenes activas. Las órdenes pendientes (limitadas, stop, las del agente) aparecen aquí con su gráfico en vivo.</p>'; $("#bl-sub").textContent = ""; return; }
    const ids = open.map((o) => o.id).join(",");
    if (box.dataset.ids !== ids) {
      box.dataset.ids = ids;
      box.innerHTML = open.map((o) => `<div class="panel live-card" data-live="${o.id}"><div class="row"><span class="live-dot"></span><b>${esc(o.simbolo)}</b><span class="muted">#${o.id} · ${o.lado} ${num(o.cantidad, 6)} · ${esc(o.tipo.replace("_", " "))}${o.origen === "agente-autonomo" ? " · agente" : ""}</span><span class="spacer"></span><span class="px mono"></span><span class="chg"></span></div><canvas class="live-chart"></canvas><div class="muted info" style="margin-top:4px"></div></div>`).join("");
    }
    let real = false;
    await Promise.all(open.map(async (o) => {
      const card = box.querySelector(`[data-live="${o.id}"]`); if (!card) return;
      try {
        const g = await api(`/api/bolsa/orden/${o.id}/grafico`), q = g.cotizacion; real ||= g.real;
        card.querySelector(".px").textContent = `${num(q.precio, 4)} ${q.divisa}`; card.querySelector(".chg").innerHTML = pct(q.variacion_pct);
        card.querySelector(".live-dot").classList.toggle("off", !q.abierto);
        const dist = g.niveles.filter((n) => ["limite", "stop", "objetivo"].includes(n.tipo)).map((n) => `${n.texto}: ${num(n.precio, 4)} (${num(((n.precio / q.precio) - 1) * 100)} %)`).join(" · ");
        card.querySelector(".info").textContent = `${q.real ? q.fuente + (q.retraso ? " · " + q.retraso : "") : "simulado (sin conexión)"} · ${q.abierto ? "mercado abierto" : "mercado cerrado"}${g.orden.estado !== "pendiente" ? " · " + g.orden.estado : ""}${dist ? " · " + dist : ""}`;
        liveChart(card.querySelector("canvas"), g.puntos, g.niveles);
      } catch (e) { card.querySelector(".info").textContent = String(e.message || e); }
    }));
    $("#bl-sub").innerHTML = real ? "datos reales · se actualiza cada 5 s" : "sin conexión: precios simulados";
  }
  async function bolsaAgent() {
    const a = await api("/api/bolsa/agente"); liveState.agent = a; const r = a.resumen, st = a.estadisticas;
    a.ordenes_activas = (bolsaState.data && a.cartera !== bolsaState.data.resumen.cartera) ? (await api(`/api/bolsa?cartera=${a.cartera}`)).ordenes.filter((o) => o.estado === "pendiente") : [];
    $("#ba-state").innerHTML = a.activo ? '<span class="tag ok"><span class="live-dot"></span> TRABAJANDO</span>' : `<span class="tag">${a.configurado ? "ARRANCANDO" : "EN PAUSA"}</span>`;
    $("#ba-datos").innerHTML = a.datos.en_linea ? `<span class="tag accent">datos reales · ${esc(a.datos.ultima_fuente || "")}</span>` : '<span class="tag warn">sin conexión · simulación</span>';
    $("#ba-toggle").textContent = a.configurado ? "PAUSAR" : "ACTIVAR";
    $("#ba-stats").innerHTML = [["Patrimonio", eur(r.total_eur)], ["Rentabilidad", pct(r.rentabilidad_pct)], ["Operaciones cerradas", `${st.cerradas} · ${st.cerradas ? Math.round((st.ganadoras / st.cerradas) * 100) : 0} % aciertos`], ["Posiciones", `${r.posiciones.length} · efectivo ${eur(r.efectivo_eur)}`]]
      .map(([k, v]) => `<div class="panel stat"><span class="k">${k}</span><span class="v" style="font-size:1rem">${v}</span></div>`).join("");
    liveChart($("#ba-curve"), a.curva.map((c) => ({ ts: c.ts, precio: c.total_eur })), [{ tipo: "entrada", precio: r.capital_inicial_eur, texto: "Capital inicial" }], { vacio: "La curva aparece tras los primeros ciclos" });
    $("#ba-rank").innerHTML = a.ranking.length ? `<table class="tbl"><tr><th>Valor</th><th>Puntuación</th><th>Precio</th><th>RSI</th><th>20 sesiones</th></tr>${a.ranking.slice(0, 8).map((x) => `<tr><td><b>${esc(x.simbolo)}</b>${x.real ? "" : ' <span class="muted">sim.</span>'}</td><td class="mono">${num(x.puntuacion)}</td><td class="mono">${num(x.precio, 4)}</td><td class="mono">${x.rsi ?? "–"}</td><td>${pct(x.roc20_pct)}</td></tr>`).join("")}</table>` : '<p class="muted">Analizando el mercado…</p>';
    $("#ba-diario").innerHTML = a.diario.length ? a.diario.map((e) => `<div class="e ${esc(e.tipo)}"><div class="t">${new Date(e.ts * 1000).toLocaleString("es-ES")} · ${esc(e.tipo)}${e.simbolo ? " · " + esc(e.simbolo) : ""}</div>${esc(e.texto)}</div>`).join("") : '<p class="muted">Todavía sin decisiones.</p>';
    if (a.error) $("#ba-diario").insertAdjacentHTML("afterbegin", `<div class="e error">${esc(a.error)}</div>`);
    const c = a.ajustes; $("#ba-riesgo").value = c.riesgo_pct; $("#ba-maxpos").value = c.max_posiciones; $("#ba-peso").value = c.max_peso_pct; $("#ba-reserva").value = c.reserva_pct; $("#ba-int").value = c.intervalo_s;
    if (document.activeElement !== $("#ba-universo")) $("#ba-universo").value = (c.universo || []).join(", ");
  }
  $("#ba-toggle").onclick = guard(async () => { await api("/api/bolsa/agente", { body: { activo: !liveState.agent?.configurado } }); bolsaAgent(); });
  $("#ba-cycle").onclick = guard(async () => { toast("El agente está analizando el mercado…"); await api("/api/bolsa/agente", { body: { ciclo: true } }); await loaders.bolsa(); });
  $("#ba-view").onclick = () => { if (!liveState.agent) return; bolsaState.cartera = liveState.agent.cartera; local.set("nexus_bolsa_cartera", bolsaState.cartera); refreshView(); };
  $("#ba-save").onclick = guard(async () => {
    await api("/api/bolsa/agente", { body: { riesgo_pct: Number($("#ba-riesgo").value), max_posiciones: Number($("#ba-maxpos").value), max_peso_pct: Number($("#ba-peso").value), reserva_pct: Number($("#ba-reserva").value), intervalo_s: Number($("#ba-int").value), universo: $("#ba-universo").value.split(/[,\s]+/).filter(Boolean) } });
    toast("Ajustes del agente guardados"); bolsaAgent(); });
  $("#bo-cartera").onchange = () => { bolsaState.cartera = Number($("#bo-cartera").value); local.set("nexus_bolsa_cartera", bolsaState.cartera); refreshView(); };
  $("#bo-quote").onclick = guard(bolsaQuote);
  $("#bo-sym").onchange = () => guard(bolsaQuote)();
  $("#bo-fees").onclick = guard(async () => {
    const b = orderBody(), g = await api("/api/bolsa/costes", { body: { ...b, precio: b.limite } });
    $("#bo-fees-box").innerHTML = `Efectivo ${eur(g.efectivo_eur)} · comisión ${eur(g.comision_eur)} · impuestos ${eur(g.impuestos_eur)} · cambio de divisa ${eur(g.cambio_eur)} <span class="muted">(${esc(g.tramo)}; sin contar el deslizamiento)</span>`;
  });
  $("#bo-send").onclick = guard(async () => {
    const b = orderBody(), real = bolsaState.data?.resumen.modo === "real";
    if (!confirm(`${real ? "CARTERA REAL: la orden quedará por confirmar, no se envía a ningún bróker.\n" : "Cartera simulada (sin dinero).\n"}${b.lado === "compra" ? "Comprar" : "Vender"} ${b.cantidad} ${b.simbolo} · ${b.tipo}?`)) return;
    const o = await api("/api/bolsa/orden", { body: b });
    toast(o.estado === "ejecutada" ? `Ejecutada a ${num(o.precio_ejec, 4)}` : o.estado === "rechazada" ? `Rechazada: ${o.motivo}` : o.nota || o.estado, o.estado === "rechazada");
    loaders.bolsa();
  });
  $("#bc-new").onclick = guard(async () => {
    const c = await api("/api/bolsa/cartera", { body: { nombre: $("#bc-nombre").value.trim(), capital: Number($("#bc-cap").value), tarifa: $("#bc-tarifa").value, modo: $("#bc-modo").value } });
    bolsaState.cartera = c.id; local.set("nexus_bolsa_cartera", c.id); toast("Cartera creada"); loaders.bolsa();
  });
  $("#bc-reset").onclick = guard(async () => {
    if (!confirm("¿Reiniciar la cartera simulada? Se borran sus órdenes, operaciones y posiciones.")) return;
    await api(`/api/bolsa/cartera/${bolsaState.cartera}/reiniciar`, { body: {} }); loaders.bolsa();
  });
  $("#bi-import").onclick = guard(async () => {
    const r = await api("/api/bolsa/importar", { body: { simbolo: $("#bi-sym").value.trim(), csv: $("#bi-csv").value } });
    toast(`${r.simbolo}: ${r.filas} cierres (${r.desde} a ${r.hasta})`); loaders.bolsa();
  });
  $("#bn-add").onclick = guard(async () => {
    await api("/api/bolsa/instrumento", { body: { simbolo: $("#bn-sym").value.trim(), nombre: $("#bn-name").value.trim(), mercado: $("#bn-mkt").value, precio: Number($("#bn-px").value) } });
    $("#bo-syms").innerHTML = ""; toast("Valor añadido"); loaders.bolsa();
  });

  /* ================================================================ vista: AGENTES */
  async function officePanel() {
    const profs = await api("/api/office/profiles"), act = profs.find((p) => p.active), box = $("#office-panel");
    const chips = (p) => p.agents.map((a) => `<span class="tag${a.gremio ? " accent" : ""}" title="${esc(a.description)}">${esc(a.icon)} ${esc(a.name)}</span>`).join("");
    box.innerHTML = `<h3>PERFIL DE OFICINA</h3>
      <p class="muted">${act ? `<b>${esc(act.icon)} ${esc(act.name)}</b> · ${esc(act.sector)} · ${esc(act.description)}` : "Sin perfil: agentes por defecto de Nexus-Office. Elige el tipo de pyme para convertir esta instalación en su oficina, con agentes de oficina y del gremio, cada uno con su propio grafo de conocimiento."}</p>
      ${act ? `<div class="row">${chips(act)}</div>` : ""}
      <div class="row admin-only" style="margin-top:12px"><select class="input" id="office-select" style="flex:1 1 260px;min-width:0">
        <option value="">— Sin perfil (agentes por defecto de Nexus-Office) —</option>${profs.map((p) => `<option value="${p.id}" ${p.active ? "selected" : ""}>${esc(p.icon)} ${esc(p.name)} · ${p.agents.length} agentes</option>`).join("")}</select>
        <button class="btn primary" id="office-apply">APLICAR</button></div>
      <div class="row admin-only" id="office-preview" style="margin-top:8px"></div>`;
    const sel = $("#office-select"), prev = () => { const p = profs.find((x) => x.id === sel.value); $("#office-preview").innerHTML = p ? chips(p) : ""; };
    sel.onchange = prev; prev();
    $("#office-apply").onclick = guard(async () => {
      if (!confirm(sel.value ? "¿Aplicar este perfil? Los agentes que no formen parte de la oficina se desactivan (no se borran)." : "¿Quitar el perfil de oficina?")) return;
      const r = await api("/api/office/profile", { body: { id: sel.value } });
      toast(r.profile ? `Oficina «${r.profile.assistant_name}» lista` : "Perfil quitado");
      await loadConfig(); loaders.agents();
    });
  }
  loaders.agents = async () => {
    await loadAgents(); officePanel().catch((e) => toast(e.message, true)); const tools = (await api("/api/tools")).tools;
    $("#agents-grid").innerHTML = S.agents.map((a) => `<div class="panel agent-card" style="--c:${a.color}"><div class="glow"></div>
      <div class="row"><div class="ico">${esc(a.icon)}</div><div style="flex:1"><div class="t" style="font:700 14px var(--font-hud);letter-spacing:.1em">${esc(a.name)}</div><div class="muted mono">${esc(a.id)} · ${esc(a.model)}</div></div>${a.enabled ? "" : '<span class="tag bad">off</span>'}</div>
      <p class="muted">${esc(a.description)}</p>
      <div class="row">${(a.tools[0] === "*" ? ["todas"] : a.tools).slice(0, 7).map((t) => `<span class="tag">${esc(t)}</span>`).join("")}</div>
      <div class="row" style="margin-top:8px">${a.keywords.slice(0, 6).map((k) => `<span class="tag accent">${esc(k)}</span>`).join("")}</div>
      <div class="row admin-only" style="margin-top:12px"><button class="btn sm" data-edit="${a.id}">EDITAR</button></div></div>`).join("");
    $$("[data-edit]").forEach((b) => (b.onclick = () => editAgent(S.agents.find((a) => a.id === b.dataset.edit), tools)));
    $("#agent-new").onclick = () => editAgent({ id: "", name: "", description: "", system_prompt: "Eres un agente especialista de {assistant_name}.", model: "default", temperature: 0.7, max_tokens: null, tools: [], memory: true, can_delegate_to: [], keywords: [], specialty: "", color: "#00f0ff", icon: "◆", enabled: true }, tools);
  };
  function editAgent(a, tools) {
    const ed = $("#agent-editor"); ed.classList.remove("hidden");
    ed.innerHTML = `<h3>${a.id ? "Editar agente · " + esc(a.name) : "Nuevo agente"}</h3><div class="grid g3">
      <label class="field">ID<input class="input" name="id" value="${esc(a.id)}" ${a.id ? "readonly" : ""}></label>
      <label class="field">Nombre<input class="input" name="name" value="${esc(a.name)}"></label>
      <label class="field">Modelo<input class="input" name="model" value="${esc(a.model)}" placeholder="default · ollama:… · peer:…"></label>
      <label class="field">Temperatura<input class="input" name="temperature" type="number" step="0.1" min="0" max="2" value="${a.temperature ?? ""}"></label>
      <label class="field">Máx. tokens<input class="input" name="max_tokens" type="number" value="${a.max_tokens ?? ""}"></label>
      <label class="field">Icono / color<div class="row"><input class="input" name="icon" value="${esc(a.icon)}" style="width:70px"><input type="color" name="color" value="${esc(a.color)}"></div></label>
    </div>
    <label class="field" style="margin-top:10px">Descripción<input class="input" name="description" value="${esc(a.description)}"></label>
    <label class="field" style="margin-top:10px">Prompt de sistema<textarea class="input" name="system_prompt" rows="5">${esc(a.system_prompt)}</textarea></label>
    <label class="field" style="margin-top:10px">Palabras clave de enrutado (coma)<input class="input" name="keywords" value="${esc(a.keywords.join(", "))}"></label>
    <label class="field" style="margin-top:10px">Especialidad: su saber hacer (áreas y términos, coma). Lo que encaja aquí va a su memoria propia; lo demás es común<textarea class="input" name="specialty" rows="2" placeholder="Vacío: generalista (todo lo que aprende es común)">${esc(a.specialty || "")}</textarea></label>
    <label class="field" style="margin-top:10px">Puede delegar en (coma, * = todos)<input class="input" name="can_delegate_to" value="${esc(a.can_delegate_to.join(", "))}"></label>
    <div class="field" style="margin-top:10px">Herramientas<div class="row"><label class="row"><input type="checkbox" name="tool" value="*" ${a.tools.includes("*") ? "checked" : ""}> todas</label>${tools.map((t) => `<label class="row"><input type="checkbox" name="tool" value="${t.name}" ${a.tools.includes(t.name) ? "checked" : ""}> ${t.name} <span class="tag ${t.risk}">${t.risk}</span></label>`).join("")}</div></div>
    <div class="row" style="margin-top:12px"><label class="row"><span class="switch"><input type="checkbox" name="memory" ${a.memory ? "checked" : ""}><span></span></span>memoria</label>
      <label class="row"><span class="switch"><input type="checkbox" name="enabled" ${a.enabled ? "checked" : ""}><span></span></span>activo</label>
      <span style="flex:1"></span>${a.id && a.id !== "nexus" ? '<button class="btn danger" id="ag-del">BORRAR</button>' : ""}<button class="btn ghost" id="ag-cancel">CANCELAR</button><button class="btn primary" id="ag-save">GUARDAR</button></div>`;
    const v = (n) => $(`[name=${n}]`, ed);
    const list = (s) => s.split(",").map((x) => x.trim()).filter(Boolean);
    $("#ag-cancel").onclick = () => ed.classList.add("hidden");
    $("#ag-save").onclick = guard(async () => {
      await api("/api/agents", { method: "PUT", body: { id: v("id").value, name: v("name").value, description: v("description").value, system_prompt: v("system_prompt").value,
        model: v("model").value || "default", temperature: v("temperature").value === "" ? null : +v("temperature").value, max_tokens: v("max_tokens").value === "" ? null : +v("max_tokens").value,
        tools: $$("[name=tool]:checked", ed).map((c) => c.value), memory: v("memory").checked, enabled: v("enabled").checked,
        keywords: list(v("keywords").value), specialty: v("specialty").value.trim(), can_delegate_to: list(v("can_delegate_to").value), color: v("color").value, icon: v("icon").value || "◆" } });
      toast("Agente guardado"); ed.classList.add("hidden"); loaders.agents();
    });
    $("#ag-del") && ($("#ag-del").onclick = guard(async () => { if (!confirm("¿Borrar agente?")) return; await api(`/api/agents/${a.id}`, { method: "DELETE" }); ed.classList.add("hidden"); loaders.agents(); }));
    ed.scrollIntoView({ behavior: "smooth" });
  }

  /* ================================================================ vista: MODELOS */
  const FIT = { ideal: "ideal", ok: "compatible", pesado: "pesado", no_cabe: "no cabe" };
  let jobsTimer = null;
  loaders.models = async () => {
    const [rec, m] = await Promise.all([api("/api/models/recommended"), api("/api/models")]);
    $("#rec-profile").textContent = `perfil ${rec.profile.tier} · hasta ${rec.profile.max_model_b}B · ideal ${rec.profile.recommended_model_b}B`;
    const active = Object.values(m.active);
    const items = rec.items.slice().sort((a, b) => (b.sector ? 1 : 0) - (a.sector ? 1 : 0));   // los de tu oficina, primero
    $("#rec-grid").innerHTML = items.map((r) => `<div class="item col" style="gap:6px"><div class="row"><span class="tag accent">${r.kind}</span><b style="flex:1">${esc(r.name)}</b>${r.best ? '<span class="tag ok">★ recomendado</span>' : ""}${r.sector ? '<span class="tag warn">◆ para tu oficina</span>' : ""}</div>
      ${r.sector ? `<div class="d"><b>Para tu oficina:</b> ${esc(r.sector)}</div>` : ""}<div class="d">${esc(r.desc)}</div><div class="row mono muted"><span class="tag ${r.fit}">${FIT[r.fit]}</span><span>${r.size_mb >= 1000 ? (r.size_mb / 1024).toFixed(1) + " GB" : r.size_mb + " MB"}</span>${r.vision ? '<span class="tag">visión</span>' : ""}${r.baseline ? '<span class="tag">base CPU</span>' : ""}</div>
      <div>${r.installed ? (active.includes(r.id) ? '<span class="tag ok">● activo</span>' : `<button class="btn sm" data-act="${r.id}">ACTIVAR</button>`) : `<button class="btn sm ${r.fit === "no_cabe" ? "danger" : "primary"}" data-rec="${r.id}">DESCARGAR</button>`}</div></div>`).join("");
    $("#catalog-list").innerHTML = m.catalog.map((c) => `<div class="item row"><span class="tag accent">${c.kind}</span><div style="flex:1"><div class="t">${esc(c.name)}</div><div class="d mono">${c.format} · ${fmtB(c.size)} · ${esc(c.source)}${c.vision ? " · visión" : ""}${c.meta?.quant ? " · " + c.meta.quant : ""}</div></div>
      ${active.includes(c.id) ? '<span class="tag ok">activo</span>' : ["llm", "image", "tts", "embed"].includes(c.kind) ? `<button class="btn sm" data-act="${c.id}">ACTIVAR</button>` : ""}<button class="btn danger sm admin-only" data-delm="${c.id}">✕</button></div>`).join("") || `<span class="muted">Ningún modelo instalado. Descarga uno recomendado.</span>`;
    if (m.ollama.length) $("#catalog-list").insertAdjacentHTML("beforeend", `<div class="item"><b>Ollama local:</b> ${m.ollama.map((o) => `<button class="btn sm" data-spec="ollama:${esc(o)}">${esc(o)}</button>`).join(" ")}</div>`);
    $("#src-select").innerHTML = m.sources.map((s) => `<option value="${s.id}">${esc(s.name)}</option>`).join("");
    $$("[data-rec]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/models/recommended/${b.dataset.rec}`, { body: {} }); toast("Descarga iniciada"); watchJobs(); })));
    $$("[data-act]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/models/activate/${b.dataset.act}`, { body: {} }); toast("Modelo activado"); loadStatus(); loaders.models(); })));
    $$("[data-delm]").forEach((b) => (b.onclick = guard(async () => { if (!confirm("¿Quitar del catálogo y borrar ficheros?")) return; await api(`/api/models/${b.dataset.delm}?files=true`, { method: "DELETE" }); loaders.models(); })));
    $$("[data-spec]").forEach((b) => (b.onclick = guard(async () => { await api("/api/models/activate-spec", { body: { spec: b.dataset.spec } }); toast("LLM cambiado"); loadStatus(); })));
    renderJobs(m.jobs); if (m.jobs.some((j) => !["completado", "error", "cancelado"].includes(j.status))) watchJobs();
  };
  function renderJobs(jobs) {
    $("#jobs-list").innerHTML = jobs.slice().reverse().map((j) => { const pct = j.total ? (100 * j.done) / j.total : 0;
      return `<div class="item"><div class="row"><b style="flex:1">${esc(j.title)}</b><span class="tag ${j.status === "completado" ? "ok" : j.status === "error" ? "bad" : "warn"}">${esc(j.status.split(" ")[0])}</span></div>
        <div class="bar" style="margin:6px 0"><i style="width:${pct.toFixed(1)}%"></i></div><div class="d mono">${fmtB(j.done)} / ${j.total ? fmtB(j.total) : "?"} · ${pct.toFixed(1)}% ${j.error ? "· " + esc(j.error) : ""}</div></div>`; }).join("") || `<span class="muted">Sin descargas.</span>`;
  }
  function watchJobs() {
    if (jobsTimer) return;
    jobsTimer = setInterval(guard(async () => { const m = await api("/api/models"); renderJobs(m.jobs);
      if (!m.jobs.some((j) => !["completado", "error", "cancelado"].includes(j.status))) { clearInterval(jobsTimer); jobsTimer = null; if (S.view === "models") loaders.models(); loadStatus(); } }), 1500);
  }
  $("#src-go").onclick = guard(async () => {
    const src = $("#src-select").value, q = $("#src-q").value.trim(); if (!q) return;
    $("#src-results").innerHTML = `<div class="chip"><span class="spin"></span>buscando en ${src}…</div>`; $("#src-files").innerHTML = "";
    const res = await api(`/api/models/search?source=${src}&q=${encodeURIComponent(q)}&kind=${$("#src-kind").value}`);
    $("#src-results").innerHTML = res.map((r) => `<div class="item" data-repo="${esc(r.id)}" style="cursor:pointer"><div class="t">${esc(r.name)}</div><div class="d mono">${r.downloads ? "↓ " + r.downloads.toLocaleString() : ""} ${esc(r.task || "")}</div></div>`).join("") || `<span class="muted">Sin resultados.</span>`;
    $$("#src-results [data-repo]").forEach((el) => (el.onclick = guard(async () => {
      $("#src-files").innerHTML = `<div class="chip"><span class="spin"></span>listando ficheros…</div>`;
      const files = await api(`/api/models/files?source=${src}&repo=${encodeURIComponent(el.dataset.repo)}`);
      const isRepo = files.some((f) => /(^|\/)(config|model_index)\.json$/.test(f.path)) && ["huggingface", "modelscope"].includes(src);
      const FITN = { ideal: "ideal", ok: "cabe", pesado: "pesado", "?": "" };
      $("#src-files").innerHTML = `<div class="row"><b style="flex:1">${esc(el.dataset.repo)}</b>${isRepo ? '<button class="btn sm primary" id="dl-repo">DESCARGAR REPOSITORIO COMPLETO</button>' : ""}</div>` +
        files.filter((f) => !/\.(md|txt|gitattributes|json|png|jpg)$/i.test(f.path) || /onnx\.json$/.test(f.path)).slice(0, 120).map((f) => `<div class="item"><div class="row">
          <span style="flex:1" class="mono">${f.recommended ? "★ " : ""}${esc(f.path)}</span><span class="muted mono">${f.size ? fmtB(f.size) : ""}</span>
          <button class="btn sm ${f.recommended ? "primary" : ""}" data-dl="${esc(f.path)}">↓</button></div>
          <div class="row" style="margin-top:4px">${f.kind ? `<span class="tag accent">${esc(f.kind)}</span>` : ""}${f.quant ? `<span class="tag">${esc(f.quant)}</span>` : ""}
          ${f.fit && f.fit !== "?" ? `<span class="tag ${f.fit}">${FITN[f.fit]}</span>` : ""}${f.split ? '<span class="tag">multiparte</span>' : ""}${f.slow ? '<span class="tag warn">puede ir lento</span>' : ""}
          ${(f.engines || []).map((e) => `<span class="tag ok">${esc(e)}</span>`).join("")}</div></div>`).join("");
      $$("#src-files [data-dl]").forEach((b) => (b.onclick = guard(async () => { await api("/api/models/download", { body: { source: src, repo: el.dataset.repo, file: b.dataset.dl, kind: $("#src-kind").value } }); toast("Descarga iniciada (con partes y proyector de visión si los hay)"); watchJobs(); })));
      $("#dl-repo") && ($("#dl-repo").onclick = guard(async () => { await api("/api/models/download-repo", { body: { source: src, repo: el.dataset.repo, kind: $("#src-kind").value } }); toast("Descargando el repositorio completo"); watchJobs(); }));
    })));
  });
  $("#imp-go").onclick = guard(async () => { const r = await api("/api/models/import", { body: { path: $("#imp-path").value.trim(), copy_file: $("#imp-copy").checked } }); toast(`Importado: ${r.name} (${r.kind}, ${r.format})`); loaders.models(); });
  $("#spec-go").onclick = guard(async () => { await api("/api/models/activate-spec", { body: { spec: $("#spec-in").value.trim() } }); toast("LLM externo activado"); loadStatus(); });

  /* ================================================================ vista: HERRAMIENTAS */
  loaders.tools = async () => {
    const d = await api("/api/tools");
    $("#pending-list").innerHTML = d.pending.map((p) => `<div class="item"><b>${esc(p.agent)}</b> → ${esc(p.tool)} <span class="tag ${p.risk}">${p.risk}</span><span class="muted"> · requiere ${p.level}</span></div>`).join("") || `<span class="muted">Nada pendiente.</span>`;
    d.pending.forEach(showApproval);
    const pol = d.policy, opts = ["auto", "ask", "admin", "deny"], names = { auto: "automático", ask: "preguntar", admin: "solo admin", deny: "bloquear" };
    $("#policy-box").innerHTML = ["low", "medium", "high"].map((r) => `<div class="setting"><div class="lbl"><b>Riesgo <span class="tag ${r}">${r}</span></b></div>
      <select class="input" data-pol="${r}" style="width:auto" ${S.token ? "" : "disabled"}>${opts.map((o) => `<option value="${o}" ${pol[r] === o ? "selected" : ""}>${names[o]}</option>`).join("")}</select></div>`).join("") + `<div class="muted">Las peticiones de nodos remotos se elevan un nivel.</div>`;
    $$("[data-pol]").forEach((s) => (s.onchange = guard(async () => { await api("/api/config", { method: "PUT", body: { "tools.policy": { ...pol, [s.dataset.pol]: s.value } } }); toast("Política actualizada"); })));
    $("#tools-table").innerHTML = `<table class="tbl"><tr><th>HERRAMIENTA</th><th>DESCRIPCIÓN</th><th>RIESGO</th><th>PERMISOS</th><th>ORIGEN</th><th class="admin-only">ACTIVA</th></tr>${d.tools.map((t) => `<tr><td class="mono">${esc(t.name)}</td><td>${esc(t.description)}</td><td><span class="tag ${t.risk}">${t.risk}</span></td><td class="mono muted">${esc(t.permissions.join(", "))}</td><td>${t.source === "community" ? '<span class="tag accent">comunidad</span>' : "integrada"}</td>
      <td class="admin-only"><label class="switch"><input type="checkbox" data-ten="${t.name}" ${t.enabled ? "checked" : ""}><span></span></label>${t.source === "community" ? ` <button class="btn danger sm" data-unin="${t.name}">✕</button>` : ""}</td></tr>`).join("")}</table>`;
    $$("[data-ten]").forEach((c) => (c.onchange = guard(() => api(`/api/tools/${c.dataset.ten}/enabled`, { body: { enabled: c.checked } }))));
    $$("[data-unin]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/tools/community/${b.dataset.unin}`, { method: "DELETE" }); loaders.tools(); })));
    if (!S.token) return;
    const props = await api("/api/proposals");
    $("#proposals-list").innerHTML = props.map((p) => `<details class="item"><summary class="row" style="cursor:pointer"><b>${esc(p.name)}</b><span class="tag ${p.status === "aprobada" ? "ok" : p.status === "rechazada" ? "bad" : "warn"}">${p.status}</span><span class="tag ${p.analysis.suggested_risk}">riesgo ${p.analysis.suggested_risk}</span><span class="muted">${esc(p.author)} · ${ago(p.created)}</span></summary>
      <p>${esc(p.description)}</p>${p.rationale ? `<p class="muted">Motivo: ${esc(p.rationale)}</p>` : ""}
      ${p.analysis.errors.map((e) => `<div class="tag bad">✕ ${esc(e)}</div>`).join(" ")} ${p.analysis.warnings.map((w) => `<div class="tag warn">⚠ ${esc(w)}</div>`).join(" ")}
      <textarea class="input" data-code="${p.id}" rows="12" style="margin-top:8px">${esc(p.code)}</textarea>
      <div class="row" style="margin-top:6px;justify-content:flex-end"><button class="btn sm ghost" data-pcopy="${p.id}">⧉ COPIAR CÓDIGO</button><button class="btn sm ghost" data-pdl="${p.id}" data-pname="${esc(p.name)}">⤓ DESCARGAR .py</button></div>
      ${p.status === "pendiente" ? `<div class="row" style="margin-top:8px"><input class="input mono" data-targs="${p.id}" value="{}" style="flex:1" title="Argumentos de prueba (JSON)"><button class="btn sm" data-psave="${p.id}">GUARDAR CÓDIGO</button><button class="btn sm" data-ptest="${p.id}">PROBAR EN SANDBOX</button>
        <button class="btn sm" data-pshare="${p.id}">COMPARTIR EN LAN</button><button class="btn danger sm" data-prej="${p.id}">RECHAZAR</button><button class="btn ok sm" data-papp="${p.id}">APROBAR E INSTALAR</button></div><pre class="code hidden" data-tout="${p.id}"></pre>` : `<div class="muted">Revisión: ${esc(p.review || "-")}</div>`}</details>`).join("") || `<span class="muted">Los agentes aún no han propuesto herramientas.</span>`;
    const id = (b, k) => b.dataset[k];
    $$("[data-pcopy]").forEach((b) => (b.onclick = () => copyText($(`[data-code="${b.dataset.pcopy}"]`).value)));
    $$("[data-pdl]").forEach((b) => (b.onclick = () => saveFile(`${(b.dataset.pname || "herramienta").replace(/[^\w.-]+/g, "_")}.py`, $(`[data-code="${b.dataset.pdl}"]`).value, "text/x-python;charset=utf-8")));
    $$("[data-psave]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/proposals/${id(b, "psave")}`, { method: "PUT", body: { code: $(`[data-code="${id(b, "psave")}"]`).value } }); toast("Código actualizado y re-analizado"); loaders.tools(); })));
    $$("[data-ptest]").forEach((b) => (b.onclick = guard(async () => { const pid = id(b, "ptest"), out = $(`[data-tout="${pid}"]`);
      const r = await api(`/api/proposals/${pid}/test`, { body: { args: JSON.parse($(`[data-targs="${pid}"]`).value || "{}") } }); out.classList.remove("hidden"); out.textContent = JSON.stringify(r, null, 2); })));
    $$("[data-papp]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/proposals/${id(b, "papp")}/approve`, { body: { note: "" } }); toast("Herramienta instalada en el repositorio"); loaders.tools(); })));
    $$("[data-prej]").forEach((b) => (b.onclick = guard(async () => { const reason = prompt("Motivo del rechazo (el agente lo aprenderá):"); if (reason === null) return; await api(`/api/proposals/${id(b, "prej")}/reject`, { body: { reason } }); loaders.tools(); })));
    $$("[data-pshare]").forEach((b) => (b.onclick = guard(async () => { await api(`/api/coop/share-proposal/${id(b, "pshare")}`, { body: {} }); toast("Compartida con la red"); })));
    const audit = await api("/api/tools/audit?limit=80");
    $("#audit-table").innerHTML = `<table class="tbl"><tr><th>CUÁNDO</th><th>AGENTE</th><th>HERRAMIENTA</th><th>DECISIÓN</th><th>ESTADO</th><th>DURACIÓN</th></tr>${audit.map((a) => `<tr><td class="mono">${ago(a.ts)}</td><td>${esc(a.agent)}</td><td class="mono" title="${esc(a.args)}">${esc(a.tool)}</td><td>${esc(a.decision)} · ${esc(a.approver)}</td><td><span class="tag ${a.status === "ok" ? "ok" : "bad"}">${esc(a.status)}</span></td><td class="mono">${a.duration}s</td></tr>`).join("")}</table>`;
  };

  /* ================================================================ vista: RED COOPERATIVA */
  function onCoop(ev) {
    if (ev.type === "lan_chat") addLan(ev.message);
    if (ev.type === "peer_join") toast(`Nodo conectado: ${ev.peer?.name || "?"} (${ev.peer?.vip || ""})`);
    if (ev.type === "remote_use") toast(`${ev.from} está usando tu ${ev.what}`);
    if (ev.type === "tool_shared") toast(`${ev.from} compartió una herramienta (pendiente de revisión)`);
    if (ev.type === "approval_required") showApproval(ev.approval);
    if (S.view === "net") loaders.net();
  }
  function addLan(m) { const el = $("#lan-chat"); el.insertAdjacentHTML("beforeend", `<div class="m"><b>${esc(m.from)}</b> ${esc(m.text)} <span class="muted mono">${ago(m.ts)}</span></div>`); el.scrollTop = el.scrollHeight; }
  loaders.net = async () => {
    const d = await api("/api/coop");
    $("#net-state").innerHTML = d.running ? '<span class="tag ok">activo</span>' : '<span class="tag">inactivo</span>';
    $("#net-kv").innerHTML = [["Nodo", d.name], ["ID", d.node_id], ["Red", d.network_id || "— sin red —"], ["IP virtual", d.vip || "—"], ["Escucha", d.listen ? `sí · puerto ${d.port}` : "no (solo saliente)"],
      ["Enlaces directos", d.links.length], ["Direcciones", d.addresses.map((a) => `${a.ip} (${a.kind})`).join(", ")]].map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("");
    $("#net-count").textContent = `${d.peers.length} nodos remotos`;
    $("#net-peers-list").innerHTML = d.peers.map((p) => `<div class="item peer"><span class="dot ${p.direct ? "ok" : "warn"}"></span><div><div class="t">${esc(p.name)} <span class="vip">${esc(p.vip)}</span></div>
      <div class="d mono">${esc(p.hardware.tier)} · ${esc(p.hardware.accelerator)} · ${esc(p.hardware.device).slice(0, 40)} · LLM ${esc(p.llm.backend)}${p.llm.model ? ":" + esc(p.llm.model) : ""} · ${p.hops} salto(s)</div></div>
      <div class="row"><button class="btn sm" data-ping="${p.node_id}">PING</button>${p.offers.inference ? `<button class="btn sm admin-only" data-usepeer="${esc(p.name)}">USAR SU IA</button>` : ""}</div></div>`).join("") || `<span class="muted">${d.running ? "Esperando a otros nodos…" : "Activa la red para ver nodos."}</span>`;
    $$("[data-ping]").forEach((b) => (b.onclick = guard(async () => { const r = await api(`/api/coop/ping/${b.dataset.ping}`, { body: {} }); toast(`Latencia: ${r.ms} ms`); })));
    $$("[data-usepeer]").forEach((b) => (b.onclick = guard(async () => { await api("/api/models/activate-spec", { body: { spec: "peer:" + b.dataset.usepeer } }); toast("Usando la IA del nodo " + b.dataset.usepeer); loadStatus(); })));
    $("#lan-chat").innerHTML = ""; d.chat.forEach(addLan);
    if (S.token && S.fullCfg?.coop) {
      $$("[data-cfg]").forEach((c) => { const [s, k] = c.dataset.cfg.split("."); c.checked = !!S.fullCfg[s][k]; c.onchange = guard(() => api("/api/config", { method: "PUT", body: { [c.dataset.cfg]: c.checked } })); });
      $("#net-peers").value = (S.fullCfg.coop.peers || []).join("\n"); $("#net-public").value = (S.fullCfg.coop.public_urls || []).join("\n");
    }
    drawNet(d);
    api("/api/lan").then((l) => {
      const term = l.urls[0] ? `curl -L "${l.urls[0].replace("/?key=", "/api/package/termux?key=")}" | tar -xz && bash Nexus-Office/install-termux.sh` : "";
      $("#lan-box").innerHTML = `<div class="row"><span class="tag ${l.listening_lan ? "ok" : "warn"}">${l.listening_lan ? "accesible desde la red" : "solo este equipo"}</span>
          <span class="muted">Clave de red: <b class="mono">${esc(l.key)}</b></span>
          <button class="btn sm admin-only" id="lan-toggle">${l.listening_lan ? "DESACTIVAR ACCESO DE RED" : "PERMITIR ACCESO DESDE LA RED"}</button></div>
        ${l.listening_lan ? "" : '<div class="muted">Activa el acceso de red (modo admin) y reinicia Nexus-Office para usarlo desde otros dispositivos.</div>'}
        <div class="muted">Abre una de estas direcciones en el móvil, tablet o consola (o búscalas con la app Android Nexus-Office):</div>
        ${l.urls.map((u) => `<div class="invite row"><span style="flex:1">${esc(u)}</span><button class="btn sm" data-copy="${esc(u)}">COPIAR</button></div>`).join("")}
        ${term ? `<div class="muted">Nexus-Office completo en Android (Termux):</div><div class="invite row"><span style="flex:1">${esc(term)}</span><button class="btn sm" data-copy="${esc(term)}">COPIAR</button></div>` : ""}`;
      $$("[data-copy]").forEach((b) => (b.onclick = () => navigator.clipboard?.writeText(b.dataset.copy).then(() => toast("Copiado"))));
      $("#lan-toggle") && ($("#lan-toggle").onclick = guard(async () => { const r = await api("/api/lan/enable", { body: { enabled: !l.listening_lan } });
        toast(`Guardado (${r.host}). Reinicia Nexus-Office para aplicarlo.`); loaders.net(); }));
    }).catch(() => ($("#lan-box").innerHTML = '<span class="muted">Disponible solo desde el propio equipo.</span>'));
    if (S.token) { guard(remoteBox)(); guard(vpnBox)(); }
  };
  /* ---------------------------------------------------------------- VPN cifrada (WireGuard) */
  async function vpnBox() {
    const box = $("#vpn-box"); if (!S.token || !box) return;
    const v = await api("/api/vpn");
    const plat = v.platform, ins = v.instructions, order = [plat, ...["windows", "linux", "macos", "wsl"].filter((p) => p !== plat)];
    const state = !v.enabled ? ["", "desactivada"] : v.tunnel_up ? ["ok", `túnel activo en este PC (${v.server_ip})`] : ["warn", "activada: falta levantar el túnel en este PC"];
    const cmds = (list) => list.map((c) => `<div class="invite row"><span class="mono" style="flex:1;white-space:pre-wrap">${esc(c)}</span><button class="btn sm" data-copy="${esc(c)}">COPIAR</button></div>`).join("");
    box.innerHTML = `
      <div class="row"><span class="tag ${state[0]}">${state[1]}</span><span class="muted mono">Curve25519 · ChaCha20‑Poly1305 · clave precompartida por dispositivo</span></div>
      <div class="muted">La app <b>Nexus-Office VPN</b> para Android crea un túnel WireGuard cifrado solo hacia este PC: funciona en casa y fuera (datos móviles), sin exponer Nexus-Office a internet. Solo viaja por el túnel el tráfico de la app.</div>
      <div class="setting"><div class="lbl"><b>Activar VPN</b><small>Puerto UDP del túnel (ábrelo en el router para usarla fuera de casa)</small></div>
        <div class="row"><input class="input" id="vpn-port" type="number" min="1" max="65535" value="${v.port}" style="width:110px">
          <label class="switch"><input type="checkbox" id="vpn-on" ${v.enabled ? "checked" : ""}><span></span></label></div></div>
      ${v.enabled ? `
      ${v.listening_lan ? "" : '<div class="notice bad">Nexus-Office solo escucha en este equipo: reinícialo para que la app lo alcance por el túnel.</div>'}
      <div class="remote-sub">1 · Túnel en este PC (${esc(ins[plat].title)})</div>
      <div class="row"><button class="btn sm primary" id="vpn-apply">LEVANTAR / RECARGAR TÚNEL</button><button class="btn sm" id="vpn-conf">⤓ DESCARGAR ${esc("nexusvpn.conf")}</button>
        <button class="btn sm" id="vpn-upnp">ABRIR UDP ${v.port} EN EL ROUTER (UPnP)</button></div>
      <div id="vpn-apply-res"></div>
      ${v.wireguard ? "" : `<div class="muted">Instala WireGuard:</div>${cmds(ins[plat].install)}`}
      <div class="muted">${v.tunnel_up ? "Tras añadir o quitar dispositivos, recarga el túnel:" : "Para levantarlo a mano (si Nexus-Office no tiene permisos de administrador):"}</div>
      ${cmds(v.tunnel_up ? ins[plat].reload : ins[plat].up)}
      <details><summary class="muted">Otros sistemas (Windows, Linux, macOS, WSL)</summary>${order.slice(1).map((p) => `<div class="remote-sub">${esc(ins[p].title)}</div>${cmds([...ins[p].install, ...ins[p].up])}`).join("")}</details>
      <div class="remote-sub">2 · Añadir un móvil</div>
      <div class="row"><input class="input" id="vpn-name" placeholder="Nombre (p. ej. Móvil de Ana)" maxlength="40" style="flex:1">
        <label class="row muted" style="gap:6px"><input type="checkbox" id="vpn-pubip" checked> incluir IP pública</label><button class="btn primary" id="vpn-add">GENERAR CÓDIGO VPN</button></div>
      <div id="vpn-add-box"></div>
      <div class="remote-sub">Dispositivos de la VPN (${v.peers.length})</div>
      <div class="list">${v.peers.length ? v.peers.map((p) => `<div class="item row"><div style="flex:1"><div class="t">${esc(p.name)} <span class="vip">${esc(p.ip)}</span></div>
          <div class="d mono">${esc(p.public.slice(0, 16))}… · ${new Date(p.created * 1000).toLocaleDateString()}</div></div><button class="btn sm danger" data-vpndel="${esc(p.id)}">QUITAR</button></div>`).join("")
        : '<span class="muted">Ninguno todavía.</span>'}</div>` : ""}`;
    $$("#vpn-box [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
    const save = guard(async () => { const x = await api("/api/vpn/enable", { body: { enabled: $("#vpn-on").checked, port: +$("#vpn-port").value } });
      toast(x.restart_required ? "VPN activada. Reinicia Nexus-Office para que escuche por el túnel." : "Guardado"); vpnBox(); });
    $("#vpn-on").onchange = save; $("#vpn-port").onchange = save;
    if (!v.enabled) return;
    $("#vpn-conf").onclick = guard(async () => { const r = await api("/api/vpn/server.conf", { raw: true }); if (!r.ok) throw new Error(r.statusText); saveFile("nexusvpn.conf", await r.text(), "text/plain;charset=utf-8"); });
    $("#vpn-apply").onclick = guard(async () => { const r = await api("/api/vpn/apply", { body: {} });
      $("#vpn-apply-res").innerHTML = r.applied ? `<div class="notice" style="border-color:var(--ok)">Túnel ${r.tunnel_up ? "activo" : "aplicado"} en este PC.</div>`
        : `<div class="notice bad">${esc(r.reason)}. Ejecuta estos comandos:</div>${cmds(r.commands)}`;
      $$("#vpn-apply-res [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy))); if (r.applied) setTimeout(vpnBox, 1500); });
    $("#vpn-upnp").onclick = guard(async () => { toast("Buscando el router…"); const r = await api("/api/vpn/upnp", { body: { open: true } }); toast(`UDP ${r.port} abierto hacia ${r.internal}. IP pública: ${r.external_ip}`); });
    $$("[data-vpndel]").forEach((b) => (b.onclick = guard(async () => {
      if (!confirm("¿Quitar este dispositivo de la VPN? Recarga después el túnel del PC.")) return;
      await api(`/api/vpn/peers/${b.dataset.vpndel}`, { method: "DELETE" }); toast("Quitado. Recarga el túnel para aplicarlo."); vpnBox(); })));
    $("#vpn-add").onclick = guard(async () => {
      const x = await api("/api/vpn/peers", { body: { name: $("#vpn-name").value.trim(), lookup_public_ip: $("#vpn-pubip").checked } });
      $("#vpn-add-box").innerHTML = `<div class="panel col" style="margin-top:8px">
          <div class="muted"><b>App Nexus-Office VPN:</b> «Conectar por VPN» → pega este código. Contiene la clave privada del móvil: <b>se muestra una sola vez</b> y no lo compartas.</div>
          <div class="invite row"><span style="flex:1;word-break:break-all">${esc(x.code)}</span><button class="btn sm" data-copy="${esc(x.code)}">COPIAR</button></div>
          <div class="row"><button class="btn sm" id="vpn-dl-code">⤓ DESCARGAR CÓDIGO</button><button class="btn sm" id="vpn-dl-conf">⤓ DESCARGAR .conf (app WireGuard oficial)</button></div>
          <div class="muted">Direcciones incluidas: ${x.endpoints.map(esc).join(" · ")}. Ahora <b>recarga el túnel</b> del PC para que acepte el móvil nuevo.</div></div>`;
      $$("#vpn-add-box [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
      const slug = (x.peer.name || "movil").replace(/[^\w-]+/g, "_");
      $("#vpn-dl-code").onclick = () => saveFile(`nexus-vpn-${slug}.txt`, x.code + "\n");
      $("#vpn-dl-conf").onclick = () => saveFile(`nexus-vpn-${slug}.conf`, x.client_conf);
      guard(async () => { const r = await api("/api/vpn/apply", { body: {} }); if (r.applied) toast("Túnel recargado con el móvil nuevo"); })();
    });
  }
  /* ---------------------------------------------------------------- acceso remoto por internet */
  let pairTimer = 0;
  async function remoteBox() {
    const box = $("#remote-box"); if (!S.token || !box) return;
    const r = await api("/api/remote");
    const up = r.upnp || {}, ago = (t) => (t ? (Date.now() / 1000 - t < 120 ? "ahora" : new Date(t * 1000).toLocaleString()) : "—");
    const state = r.listening ? ["ok", `escuchando en el puerto ${r.port}`] : r.enabled ? ["warn", "activado: reinicia Nexus-Office para abrir el puerto"] : ["", "desactivado"];
    box.innerHTML = `
      <div class="row"><span class="tag ${state[0]}">${state[1]}</span><span class="muted mono">${esc(r.crypto)}</span></div>
      <div class="setting"><div class="lbl"><b>Permitir acceso desde internet</b><small>Puerto HTTPS propio; el puerto local (${S.fullCfg?.server?.port || 8775}) no cambia</small></div>
        <div class="row"><input class="input" id="rm-port" type="number" min="1024" max="65535" value="${r.port}" style="width:110px" title="Puerto HTTPS">
          <label class="switch"><input type="checkbox" id="rm-on" ${r.enabled ? "checked" : ""}><span></span></label></div></div>
      <div class="remote-sub">Huella del certificado (SHA-256)</div>
      <div class="invite">${esc(r.fingerprint)}</div>
      <div class="muted">La app Android la comprueba sola. En un navegador, el primer acceso avisará de «certificado no válido»: es tu certificado propio; continúa solo si esta huella coincide con la que muestra el navegador.</div>
      <div class="remote-sub">Llegar desde internet</div>
      <div class="row"><button class="btn sm primary" id="rm-wizard">ASISTENTE DEL ROUTER</button><button class="btn sm" id="rm-upnp-open">ABRIR PUERTO EN EL ROUTER (UPnP)</button><button class="btn sm ghost" id="rm-upnp-close">CERRAR PUERTO</button>
        <button class="btn sm" id="rm-ip">DETECTAR IP PÚBLICA</button></div>
      ${up.error ? `<div class="notice bad">UPnP: ${esc(up.error)}. Abre el puerto TCP ${r.port} a mano en el router hacia este equipo, o usa Tailscale/ZeroTier.</div>` : ""}
      ${up.mapped ? `<div class="notice" style="border-color:var(--ok)">Puerto ${r.port} abierto en el router hacia ${esc(up.internal || "este equipo")}. IP pública: <b>${esc(up.external_ip)}</b></div>` : ""}
      ${up.cgnat || r.behind_cgnat ? `<div class="notice bad">Tu operador comparte la IP pública (CGNAT): abrir puertos no basta. Usa Tailscale o ZeroTier (Nexus-Office detecta su dirección) o un túnel como Cloudflare Tunnel apuntando a https://localhost:${r.port}.</div>` : ""}
      ${r.urls.length ? r.urls.map((u) => `<div class="invite row"><span class="tag ${u.kind === "internet" ? "accent" : ""}">${esc(u.kind)}</span><span style="flex:1">${esc(u.url)}</span><button class="btn sm" data-copy="${esc(u.url)}">COPIAR</button></div>`).join("") : '<span class="muted">Sin direcciones todavía.</span>'}
      <div class="remote-sub">Emparejar un dispositivo nuevo</div>
      <div class="row"><input class="input" id="rm-name" placeholder="Nombre (p. ej. Móvil de Ana)" maxlength="40" style="flex:1"><button class="btn primary" id="rm-pair">GENERAR CÓDIGO</button></div>
      <div id="rm-pair-box"></div>
      <div class="remote-sub">Dispositivos autorizados (${r.devices.length})</div>
      <div class="list">${r.devices.length ? r.devices.map((d) => `<div class="item row"><div style="flex:1"><div class="t">${esc(d.name)}</div>
          <div class="d">último acceso ${esc(ago(d.last_seen))} · ${esc(d.last_ip || "")}</div></div><button class="btn sm danger" data-revoke="${esc(d.id)}">REVOCAR</button></div>`).join("")
        : '<span class="muted">Ninguno. Genera un código para emparejar el primero.</span>'}</div>
      <details><summary class="muted">Avanzado</summary><div class="row" style="margin-top:8px"><button class="btn sm danger" id="rm-cert">REGENERAR CERTIFICADO</button>
        <span class="muted">Cambia la huella: las apps tendrán que volver a emparejarse.</span></div></details>`;
    $$("#remote-box [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
    $("#rm-wizard").onclick = () => { go("settings"); setTimeout(() => { $("#router-panel").scrollIntoView({ behavior: "smooth" }); guard(routerWizard)(); }, 300); };
    const save = guard(async () => {
      const x = await api("/api/remote/enable", { body: { enabled: $("#rm-on").checked, port: +$("#rm-port").value } });
      toast(x.restart_required ? "Guardado. Reinicia Nexus-Office para aplicarlo." : "Guardado"); remoteBox();
    });
    $("#rm-on").onchange = save; $("#rm-port").onchange = save;
    const upnp = (open) => guard(async () => { toast(open ? "Buscando el router…" : "Cerrando el puerto…"); await api("/api/remote/upnp", { body: { open } }); remoteBox(); });
    $("#rm-upnp-open").onclick = upnp(true); $("#rm-upnp-close").onclick = upnp(false);
    $("#rm-ip").onclick = guard(async () => { const x = await api("/api/remote/public-ip", { body: {} }); toast("IP pública: " + x.public_ip); remoteBox(); });
    $("#rm-cert").onclick = guard(async () => {
      if (!confirm("¿Regenerar el certificado? Las apps emparejadas dejarán de conectar hasta volver a emparejarlas.")) return;
      await api("/api/remote/certificate", { body: {} }); toast("Certificado nuevo. Reinicia Nexus-Office."); remoteBox();
    });
    $$("[data-revoke]").forEach((b) => (b.onclick = guard(async () => {
      if (!confirm("¿Revocar el acceso de este dispositivo? Se desconecta al momento.")) return;
      await api(`/api/remote/devices/${b.dataset.revoke}`, { method: "DELETE" }); toast("Acceso revocado"); remoteBox();
    })));
    $("#rm-pair").onclick = guard(async () => {
      const x = await api("/api/remote/pairing", { body: { name: $("#rm-name").value.trim() } });
      const end = x.expires * 1000;
      $("#rm-pair-box").innerHTML = `<div class="panel col" style="margin-top:8px">
          <div class="muted">Código de un solo uso · caduca en <b id="rm-left">10:00</b></div>
          <div class="pair-code">${esc(x.code)}</div>
          <div class="muted"><b>App Android:</b> «Conectar desde internet» → pega este código de acceso (incluye la dirección y la huella del certificado):</div>
          <div class="invite row"><span style="flex:1">${esc(x.invite)}</span><button class="btn sm" data-copy="${esc(x.invite)}">COPIAR</button></div>
          ${x.links.length ? `<div class="muted"><b>Navegador:</b> abre en el otro dispositivo uno de estos enlaces (o la dirección y escribe el código):</div>` +
            x.links.map((l) => `<div class="invite row"><span style="flex:1">${esc(l)}</span><button class="btn sm" data-copy="${esc(l)}">COPIAR</button></div>`).join("") : ""}
        </div>`;
      $$("#rm-pair-box [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
      clearInterval(pairTimer);
      pairTimer = setInterval(() => {
        const left = Math.max(0, Math.round((end - Date.now()) / 1000)), el = $("#rm-left");
        if (!el) return clearInterval(pairTimer);
        el.textContent = `${(left / 60) | 0}:${String(left % 60).padStart(2, "0")}`;
        if (!left) { clearInterval(pairTimer); $("#rm-pair-box").innerHTML = '<span class="muted">El código caducó. Genera otro.</span>'; }
      }, 1000);
    });
  }
  /* ---------------------------------------------------------------- asistente del router */
  function openExternal(url) {
    if (window.NexusApp && NexusApp.openUrl) return NexusApp.openUrl(url);
    window.open(url, "_blank", "noopener");
  }
  async function routerWizard() {
    const box = $("#router-box");
    box.innerHTML = '<div class="chip"><span class="spin"></span>Buscando tu router en la red…</div>';
    const d = await api("/api/remote/router"), rule = d.rule, port = d.port;
    const step = (n, cls, title, body) => `<div class="step ${cls}"><div class="n">${cls === "ok" ? "✓" : n}</div><div class="body"><h4>${title}</h4>${body}</div></div>`;
    const copyRow = (k, v) => `<dt>${k}</dt><dd>${esc(v)}</dd><button class="btn sm ghost" data-copy="${esc(v)}">COPIAR</button>`;
    const s1 = d.listening ? ["ok", `Escuchando en el puerto ${port} con TLS 1.3.`]
      : d.enabled ? ["warn", "Activado, falta <b>reiniciar Nexus-Office</b> para abrir el puerto."]
      : ["bad", `Desactivado. <button class="btn sm primary" id="rw-enable">ACTIVAR (PUERTO ${port})</button>`];
    box.innerHTML = `<div class="steps">
      ${step(1, s1[0], "Acceso remoto en Nexus-Office", `<div>${s1[1]}</div>`)}
      ${step(2, d.gateway ? "ok" : "bad", "Tu router", d.gateway ? `
        <div><b>${esc(d.brand_name)}</b>${d.page_title && d.page_title !== d.brand_name ? ` · «${esc(d.page_title)}»` : ""} en <span class="mono">${esc(d.gateway)}</span>
          · este equipo: <span class="mono">${esc(d.local_ip)}</span>${d.interface.name ? ` (${esc(d.interface.name)})` : ""}</div>
        <div class="row">${d.admin_urls.map((u, i) => `<button class="btn sm ${i ? "ghost" : "primary"}" data-open="${esc(u)}">ABRIR ${esc(u.replace(/\/$/, ""))}</button>`).join("")}</div>
        <div class="muted">Se abre la configuración del router en el navegador. Entra con su usuario y contraseña (suelen venir en la pegatina del router). Si el navegador avisa del certificado del router, es normal en la mayoría de routers domésticos.</div>`
        : '<div class="notice bad">No se encontró la puerta de enlace. ¿Está este equipo conectado al router por Wi‑Fi o cable?</div>')}
      ${step(3, "", "Abrir el puerto " + port, `
        ${d.upnp_igd ? `<div class="row"><button class="btn sm primary" id="rw-upnp">ABRIR AUTOMÁTICAMENTE (UPnP)</button><span class="muted">Tu router lo permite: un clic y listo.</span></div>`
          : `<div class="notice">Tu router no ofrece la apertura automática (UPnP desactivado o no disponible). Hazlo a mano, son un par de minutos${d.brand === "generic" ? "" : ""}:</div>`}
        <div><b>Dónde:</b> ${esc(d.menu_path)}${d.tip ? ` <span class="muted">${esc(d.tip)}</span>` : ""}</div>
        <div><b>Crea esta regla</b> (el nombre de cada campo varía según el router):</div>
        <dl class="rule">${copyRow("Nombre / servicio", rule.name)}${copyRow("Protocolo", rule.protocol)}${copyRow("Puerto externo (WAN)", String(rule.external_port))}
          ${copyRow("IP interna (este equipo)", rule.internal_ip)}${copyRow("Puerto interno (LAN)", String(rule.internal_port))}</dl>
        ${d.interface.mac ? `<div class="muted">Recomendado: en el apartado DHCP del router, <b>reserva la IP ${esc(rule.internal_ip)}</b> para este equipo (MAC <span class="mono">${esc(d.interface.mac)}</span>
          <button class="btn sm ghost" data-copy="${esc(d.interface.mac)}">COPIAR MAC</button>) para que la regla siga valiendo tras reiniciar.</div>` : ""}
        ${d.upnp_igd ? "" : '<div class="muted">Alternativa sin tocar el router: Tailscale o ZeroTier en el PC y en el móvil (Nexus-Office muestra su dirección en Red COOP → Acceso remoto).</div>'}`)}
      ${step(4, "", "Comprobar desde internet", `<div class="row"><button class="btn sm" id="rw-check">COMPROBAR AHORA</button><span class="muted">Consulta tu IP pública e intenta conectar a ella.</span></div><div id="rw-check-res"></div>`)}
      ${step(5, "", "Conectar la app Android", `<div class="row"><button class="btn sm primary" id="rw-pair">GENERAR CÓDIGO PARA LA APP</button>
        <span class="muted">En el móvil: app Nexus-Office → «Conectar desde internet» → pega el código. Para probarlo de verdad, desactiva la Wi‑Fi y usa datos móviles.</span></div><div id="rw-pair-res"></div>`)}
      </div><div class="row"><button class="btn sm ghost" id="router-scan">BUSCAR DE NUEVO</button></div>`;
    $$("#router-box [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
    $$("#router-box [data-open]").forEach((b) => (b.onclick = () => openExternal(b.dataset.open)));
    $("#router-scan").onclick = guard(routerWizard);
    $("#rw-enable") && ($("#rw-enable").onclick = guard(async () => {
      await api("/api/remote/enable", { body: { enabled: true, port } }); toast("Acceso remoto activado: reinicia Nexus-Office para abrir el puerto"); routerWizard();
    }));
    $("#rw-upnp") && ($("#rw-upnp").onclick = guard(async () => {
      const x = await api("/api/remote/upnp", { body: { open: true } });
      if (x.upnp.error) toast("UPnP: " + x.upnp.error, true); else toast(`Puerto ${port} abierto en el router (IP pública ${x.upnp.external_ip})`);
    }));
    $("#rw-check").onclick = guard(async () => {
      const res = $("#rw-check-res"); res.innerHTML = '<div class="chip"><span class="spin"></span>Comprobando…</div>';
      const x = await api("/api/remote/check", { body: {} });
      res.innerHTML = x.reachable
        ? `<div class="notice" style="border-color:var(--ok)">Accesible desde internet: <b class="mono">${esc(x.url)}</b> responde con TLS 1.3. Ya puedes emparejar la app.</div>`
        : x.behind_cgnat ? `<div class="notice bad">Tu operador comparte la IP pública (CGNAT, IP ${esc(x.public_ip)}): abrir puertos no basta. Pide a tu operador una IP pública (a veces gratis, «salir de CGNAT») o usa Tailscale / ZeroTier.</div>`
        : !x.listening ? '<div class="notice bad">Nexus-Office aún no escucha en el puerto remoto: actívalo (paso 1) y reinicia Nexus-Office.</div>'
        : `<div class="notice">Tu IP pública es <b class="mono">${esc(x.public_ip)}</b>, pero desde este equipo no responde el puerto ${x.port}. O la regla del router aún no está bien, o tu router no permite la prueba desde dentro de casa (es frecuente): la prueba definitiva es el móvil con datos móviles en <span class="mono">${esc(x.url)}</span>.</div>`;
    });
    $("#rw-pair").onclick = guard(async () => {
      const x = await api("/api/remote/pairing", { body: { name: "" } });
      $("#rw-pair-res").innerHTML = `<div class="pair-code">${esc(x.code)}</div>
        <div class="invite row"><span style="flex:1">${esc(x.invite)}</span><button class="btn sm" data-copy="${esc(x.invite)}">COPIAR CÓDIGO DE ACCESO</button></div>
        <div class="muted">Sirve una sola vez y caduca en 10 minutos. Pásalo al móvil (por ejemplo, copiándolo en un mensaje para ti) y pégalo en la app.</div>`;
      $$("#rw-pair-res [data-copy]").forEach((b) => (b.onclick = () => copyText(b.dataset.copy)));
    });
  }
  $("#router-scan").onclick = guard(routerWizard);
  function copyText(t) {
    if (window.NexusApp && NexusApp.copy) { NexusApp.copy(t); return toast("Copiado"); }
    navigator.clipboard?.writeText(t).then(() => toast("Copiado"), () => toast("No se pudo copiar", true));
  }
  function drawNet(d) {
    const c = $("#net-canvas"), r = c.getBoundingClientRect(), dp = devicePixelRatio || 1; c.width = r.width * dp; c.height = r.height * dp;
    const ctx = c.getContext("2d"); ctx.scale(dp, dp); const cx = r.width / 2, cy = r.height / 2, R = Math.min(cx, cy) * 0.72;
    const pos = d.peers.map((p, i) => ({ p, x: cx + Math.cos((i / Math.max(d.peers.length, 1)) * 6.283 - 1.57) * R * (0.6 + 0.4 * Math.min(p.hops, 3) / 3), y: cy + Math.sin((i / Math.max(d.peers.length, 1)) * 6.283 - 1.57) * R * (0.6 + 0.4 * Math.min(p.hops, 3) / 3) }));
    for (const q of pos) { ctx.strokeStyle = q.p.direct ? "rgba(0,240,255,.6)" : "rgba(255,176,32,.45)"; ctx.setLineDash(q.p.direct ? [] : [5, 5]); ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(q.x, q.y); ctx.stroke(); }
    ctx.setLineDash([]);
    const node = (x, y, col, label, sub, r0) => { ctx.fillStyle = col; ctx.shadowColor = col; ctx.shadowBlur = 18; ctx.beginPath(); ctx.arc(x, y, r0, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      ctx.fillStyle = "#d8f6ff"; ctx.font = '700 11px "Orbitron"'; ctx.textAlign = "center"; ctx.fillText(label, x, y + r0 + 14); ctx.fillStyle = "#6f8aa3"; ctx.font = '10px "JetBrains Mono"'; ctx.fillText(sub, x, y + r0 + 27); };
    node(cx, cy, d.running ? "#00f0ff" : "#33475a", d.name, d.vip || "sin red", 14);
    pos.forEach((q) => node(q.x, q.y, q.p.hardware.tier === "alto" ? "#ff3df2" : q.p.hardware.tier === "medio" ? "#ffb020" : "#7cff6b", q.p.name, q.p.vip, 9));
  }
  $("#net-start").onclick = guard(async () => { await api("/api/coop/start", { body: {} }); toast("Red cooperativa activada"); loaders.net(); loadStatus(); });
  $("#net-stop").onclick = guard(async () => { await api("/api/coop/stop", { body: {} }); loaders.net(); loadStatus(); });
  $("#net-create").onclick = guard(async () => { const n = prompt("Nombre de la red (opcional):") ?? ""; const r = await api("/api/coop/create", { body: { name: n } }); toast("Red creada: " + r.network_id); await loadConfig(); loaders.net(); });
  $("#net-invite").onclick = guard(async () => { const r = await api("/api/coop/invite"); $("#net-invite-box").innerHTML = `<div class="invite">${esc(r.code)}</div><div class="muted">Comparte este código solo con quien quieras en tu red: incluye la clave de cifrado.</div>`; navigator.clipboard?.writeText(r.code); toast("Invitación copiada"); });
  $("#net-join").onclick = guard(async () => { const r = await api("/api/coop/join", { body: { code: $("#net-code").value.trim() } }); toast("Unido a " + r.network_id); $("#net-code").value = ""; await loadConfig(); loaders.net(); });
  $("#net-save").onclick = guard(async () => { const ls = (id) => $(id).value.split("\n").map((x) => x.trim()).filter(Boolean);
    await api("/api/config", { method: "PUT", body: { "coop.peers": ls("#net-peers"), "coop.public_urls": ls("#net-public") } }); await loadConfig(); toast("Guardado. Reinicia la red para aplicar."); });
  const lanSend = guard(async () => { const t = $("#lan-input").value.trim(); if (!t) return; const m = await api("/api/coop/chat", { body: { text: t } }); addLan(m); $("#lan-input").value = ""; });
  $("#lan-send").onclick = lanSend; $("#lan-input").onkeydown = (e) => e.key === "Enter" && lanSend();

  /* ================================================================ vista: SISTEMA */
  loaders.system = async () => {
    const d = await api("/api/hardware"), hw = d.hardware, p = d.profile;
    const gpus = hw.gpus.map((g) => `${g.name} · ${g.vram_gb} GB ${g.integrated ? "(integrada)" : ""}`).join("<br>") || "—";
    const npus = hw.npus.map((n) => `${n.name} · ${n.runtime_hint}`).join("<br>") || "No detectada";
    $("#hw-cards").innerHTML = [["CPU", hw.cpu_name, `${hw.cores_physical} núcleos / ${hw.cores_logical} hilos · ${hw.cpu_flags.join(" ")}`], ["GPU", gpus, `acelerador: ${p.accelerator}`],
      ["NPU", npus, p.npu.hint || (p.npu.ready ? "lista: " + p.npu.offload.join(", ") : "")], ["MEMORIA", `${hw.ram_gb} GB RAM`, `${hw.disk_free_gb} GB libres · ${hw.os} ${hw.arch} · Python ${hw.python}`]]
      .map(([k, v, s]) => `<div class="panel hw-card"><h3>${k}</h3><div class="big">${v}</div><div class="muted mono" style="margin-top:6px">${esc(s)}</div></div>`).join("");
    const L = p.llm;
    $("#profile-kv").innerHTML = [["Perfil", `${p.id} (${p.tier})`], ["Dispositivo", p.device_name], ["LLM recomendado", `≤ ${L.recommended_model_b}B (máx ${L.max_model_b}B) ${L.quant}`],
      ["Contexto", `${L.n_ctx} (máx ${L.max_ctx})`], ["Hilos / lote", `${L.n_threads} / ${L.n_batch}`], ["Capas GPU", L.gpu_layers === -1 ? "todas" : L.gpu_layers], ["Runtime llama.cpp", L.llamacpp_wheel],
      ["Imagen", `${p.image.device} · ${p.image.resolution}px · ${p.image.neural ? "neural GPU" : "CPU"}`], ["Voz (STT)", `whisper ${p.stt.model} · ${p.stt.compute_type} · ${p.stt.device}`],
      ["Embeddings", p.embeddings.device], ["NPU", p.npu.present ? `${p.npu.name} · ${p.npu.ready ? "en uso" : "sin runtime"}` : "—"]].map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("");
    const log = (d.tuning_state.log || []).slice().reverse();
    $("#tune-list").innerHTML = (d.tuning.length ? `<div class="item"><b>Este arranque:</b><ul>${d.tuning.map((r) => `<li>${esc(r)}</li>`).join("")}</ul></div>` : `<div class="item muted">Este arranque: configuración estable, sin cambios.</div>`) +
      log.slice(0, 6).map((l) => `<div class="item"><span class="muted mono">${new Date(l.ts * 1000).toLocaleString()}</span><ul>${l.reasons.map((r) => `<li>${esc(r)}</li>`).join("")}</ul></div>`).join("") +
      (() => { const tr = Object.entries(d.tuning_state.thread_trials || {}).sort((a, b) => +a[0] - +b[0]);
        return `<div class="item mono muted">Pruebas de hilos: ${tr.length ? tr.map(([n, v]) => `${esc(n)} hilos ≈ ${(v.reduce((x, y) => x + y, 0) / v.length).toFixed(1)} tok/s (${v.length})`).join(" · ") : "aún sin medir (se mide con tus respuestas)"}</div>`; })();
    const rt = await api("/api/runtimes");
    $("#runtimes-box").innerHTML = Object.keys(rt.recommended).map((k) => `<div class="item"><div class="t">${esc(rt.descriptions[k])}</div>
      <div class="row" style="margin-top:6px">${(rt.bundled?.[k] || []).map((v) => `<span class="tag ok" title="Viene en el paquete: no hace falta descargarlo">✓ ${esc(v)} incluido${k === "llama" && rt.active.llama === (v.startsWith("cpu") ? "cpu" : v) && !rt.installed[k].includes(v) ? " · en uso" : ""}</span>`).join("")}${rt.recommended[k].filter((v) => !(rt.bundled?.[k] || []).includes(v)).map((v, i) => { const inst = rt.installed[k].includes(v), prog = rt.progress[`${k}:${v}`];
        return inst ? `<span class="tag ok">✓ ${v}${rt.active.llama === v && k === "llama" ? " · en uso" : ""}</span>` : prog && prog.status !== "error" && prog.status !== "instalado" ? `<span class="tag warn">${esc(prog.status)} ${prog.total ? Math.round((100 * prog.done) / prog.total) + "%" : ""}</span>`
          : `<button class="btn sm ${i === 0 ? "primary" : "admin-only"}" data-rt="${k}" data-v="${v}">INSTALAR ${v}${i === 0 ? " ★" : ""}</button>`; }).join("")}</div></div>`).join("");
    $$("[data-rt]").forEach((b) => (b.onclick = guard(async () => { await api("/api/runtimes/install", { body: { runtime: b.dataset.rt, variant: b.dataset.v } }); toast("Instalando runtime…"); setTimeout(loaders.system, 1500); })));
    if (Object.values(rt.progress).some((p) => !["instalado", "error"].includes(p.status))) setTimeout(() => S.view === "system" && loaders.system(), 2000);
    serviceBox(); downloadsBox();
    api("/api/drivers").then((dr) => {
      $("#drivers-box").innerHTML = dr.gpus.map((g) => `<div class="item"><div class="row"><b style="flex:1">${esc(g.name)}</b>
        <span class="tag ${g.status === "ok" ? "ok" : g.status === "actualizar" ? "warn" : ""}">${g.status === "ok" ? "al día" : g.status}</span></div>
        <div class="d mono">versión ${esc(g.version)}${g.date ? " · " + esc(g.date) : ""}${g.age_days ? ` · ${Math.floor(g.age_days / 30)} meses` : ""}</div>
        <ul>${g.reasons.map((r) => `<li>${esc(r)}</li>`).join("")}</ul>
        <div class="row admin-only">${g.options.map((o) => `<button class="btn sm" data-drv="${o.id}" title="${esc(o.detail)}">${esc(o.label)}</button>`).join("")}</div>
        <div class="user-only muted">Entra en modo administrador para lanzar el actualizador oficial.</div></div>`).join("") || `<span class="muted">No se detectaron drivers de GPU.</span>`;
      $$("[data-drv]").forEach((b) => (b.onclick = guard(async () => {
        if (!confirm(`Se abrirá: ${b.textContent}\n\nLa instalación la confirmas tú en el actualizador oficial (pedirá permisos de administrador). ¿Continuar?`)) return;
        const r = await api("/api/drivers/update", { body: { option: b.dataset.drv } }); toast("Abierto: " + r.how); })));
    }).catch(() => ($("#drivers-box").innerHTML = '<span class="muted">No se pudo comprobar.</span>'));
    $("#history-table").innerHTML =`<table class="tbl"><tr><th>SESIÓN</th><th>DURACIÓN</th><th>LLAMADAS</th><th>TOK/S</th><th>RAM PICO</th><th>CPU MEDIA</th><th>AJUSTES</th><th>CIERRE</th></tr>${d.history.slice().reverse().map((h) => `<tr><td class="mono">${h.session}</td><td>${Math.round(h.duration_s / 60)} min</td><td>${h.llm_calls}</td><td>${h.avg_tps}</td><td>${h.peak_ram_pct}%</td><td>${h.avg_cpu}%</td><td class="mono muted">${esc(JSON.stringify(h.settings))}</td><td>${h.clean_exit ? '<span class="tag ok">ok</span>' : '<span class="tag bad">abrupto</span>'}</td></tr>`).join("")}</table>`;
    if (S.token) $("#logs").textContent = (await api("/api/admin/logs?n=150")).lines.join("\n");
    drawMonitor(await api("/api/monitor"));
  };
  async function downloadsBox() {
    const d = await api("/api/downloads").catch(() => null), box = $("#downloads-box");
    if (!d) { box.innerHTML = '<span class="muted">No disponible.</span>'; return; }
    const ua = navigator.userAgent, mine = /Android/i.test(ua) ? "android" : /Windows/i.test(ua) ? (/ARM64|aarch64/i.test(ua) ? "windows-arm64" : "windows-x64")
      : /Mac OS X/i.test(ua) ? "macos-arm64" : /Linux/i.test(ua) ? (/aarch64|arm/i.test(ua) ? "linux-aarch64" : "linux-x86_64") : "";
    const mb = (n) => n > 1e9 ? (n / 1e9).toFixed(1) + " GB" : Math.max(1, Math.round(n / 1e6)) + " MB";
    if (!d.files.length) { box.innerHTML = `<span class="muted">Aún no hay instaladores. Copia la carpeta de instaladores (windows/, linux/, macos/, android/) en <span class="mono">${esc(d.folder)}</span> y aparecerán aquí para descargarlos desde cualquier equipo o móvil de la red.</span>`; return; }
    box.innerHTML = `<div class="muted" style="margin-bottom:6px">Sistema anfitrión: <b>${esc(d.host_label)}</b></div>` + d.files.map((f) => {
      const tags = (f.platform === d.host ? '<span class="tag ok">anfitrión</span> ' : "") + (f.platform === mine || (mine === "macos-arm64" && f.platform.startsWith("macos")) ? '<span class="tag warn">este dispositivo</span> ' : "");
      return `<div class="item row"><span style="flex:1"><b>${esc(f.label)}</b> · <span class="mono">${esc(f.name)}</span> ${tags}</span><span class="muted mono">${mb(f.size)}</span>
        <a class="btn sm primary" href="/descargas/${f.path.split("/").map(encodeURIComponent).join("/")}" download="${esc(f.name)}">DESCARGAR</a></div>`; }).join("");
  }
  async function serviceBox() {
    const box = $("#service-box");
    if (window.NexusApp && NexusApp.setBootStart) {   // app Android autonoma: servicio en primer plano al encender el movil
      const on = NexusApp.bootStart();
      box.innerHTML = `<div class="row"><span style="flex:1">${on ? "Nexus-Office arranca solo al encender el móvil." : "Nexus-Office arranca al abrir la app."}</span>
        <button class="btn sm ${on ? "" : "primary"}" id="svc-toggle">${on ? "DESACTIVAR" : "ARRANCAR AL ENCENDER"}</button></div>`;
      $("#svc-toggle").onclick = () => { NexusApp.setBootStart(!on); serviceBox(); };
      return;
    }
    const st = await api("/api/service").catch(() => null);
    if (!st || !st.supported) { box.innerHTML = '<span class="muted">No disponible en este dispositivo.</span>'; return; }
    const kinds = { systemd: "systemd", cron: "cron @reboot", launchd: "launchd", windows: "Programador de tareas", termux: "termux-services" };
    box.innerHTML = `<div class="row"><span style="flex:1">${st.registered ? `<span class="tag ok">activo</span> ${esc(kinds[st.kind] || st.kind)} · <span class="mono muted">${esc(st.registered)}</span>` : "Sin servicio: Nexus-Office arranca cuando lo abres."}</span>
      <span class="admin-only">${st.registered ? '<button class="btn sm danger" id="svc-off">QUITAR</button>' : '<label class="muted"><input type="checkbox" id="svc-red"> accesible desde la red local</label> <button class="btn sm primary" id="svc-on">INSTALAR SERVICIO</button>'}</span></div>
      <div class="muted" style="margin-top:6px">${st.admin_os ? "Con permisos de administrador: arrancará con el equipo." : "Sin permisos de administrador arranca al iniciar sesión; para que arranque con el equipo instálalo desde el instalador o con <span class=mono>sudo … -m nexus servicio instalar</span>."}</div>
      <div class="user-only muted">Entra en modo administrador para cambiarlo.</div>`;
    const act = (action) => guard(async () => { const r = await api("/api/service", { body: { action, red: !!($("#svc-red") || {}).checked } }); toast(r.detail); serviceBox(); });
    if ($("#svc-on")) $("#svc-on").onclick = act("instalar");
    if ($("#svc-off")) $("#svc-off").onclick = act("quitar");
  }
  function drawMonitor(m) {
    $("#mon-session").textContent = `sesión ${m.session} · ${Math.round(m.uptime_s / 60)} min · ${m.summary.llm_calls} llamadas · ${m.summary.avg_tps} tok/s`;
    lines($("#mon-chart"), [m.samples.map((s) => s.cpu), m.samples.map((s) => s.ram_pct), m.samples.map((s) => s.gpu_util || 0)], ["#00f0ff", "#ff3df2", "#7cff6b"]);
  }
  $("#shutdown").onclick = guard(async () => { if (!confirm("¿Apagar Nexus-Office? Se guardará el rendimiento de la sesión para el autoajuste.")) return;
    await api("/api/admin/shutdown", { body: {} }); toast("Nexus-Office se está apagando…"); });
  $("#redetect").onclick =guard(async () => { toast("Detectando hardware…"); await api("/api/hardware/redetect", { body: {} }); loaders.system(); loadStatus(); });

  /* ================================================================ vista: AJUSTES */
  const LAYOUT_NAMES = { phone: "Móvil", tablet: "Tablet", desktop: "Escritorio" };
  loaders.settings = async () => {
    await loadConfig(); const u = S.cfg;
    const voices = ("speechSynthesis" in window ? speechSynthesis.getVoices() : []).filter((v) => v.lang.startsWith(u.language || "es"));
    const row = (lbl, sub, ctrl) => `<div class="setting"><div class="lbl"><b>${lbl}</b>${sub ? `<small>${sub}</small>` : ""}</div>${ctrl}</div>`;
    const sw = (k) => `<label class="switch"><input type="checkbox" data-u="${k}" ${u[k] ? "checked" : ""}><span></span></label>`;
    $("#user-settings").innerHTML = row("Nombre del asistente", "", `<input class="input" data-u="assistant_name" value="${esc(u.assistant_name)}" style="width:180px">`) +
      row("Idioma", "", `<select class="input" data-u="language" style="width:auto">${[["es", "Español"], ["en", "English"], ["ca", "Català"], ["eu", "Euskara"], ["gl", "Galego"], ["fr", "Français"], ["pt", "Português"], ["de", "Deutsch"], ["it", "Italiano"]].map(([v, n]) => `<option value="${v}" ${u.language === v ? "selected" : ""}>${n}</option>`).join("")}</select>`) +
      row("Estilo de respuesta", "", `<select class="input" data-u="response_style" style="width:auto">${["breve", "equilibrado", "detallado"].map((v) => `<option ${u.response_style === v ? "selected" : ""}>${v}</option>`).join("")}</select>`) +
      row("Agente por defecto", "", `<select class="input" data-u="default_agent" style="width:auto"><option value="auto">automático</option>${S.agents.map((a) => `<option value="${a.id}" ${u.default_agent === a.id ? "selected" : ""}>${esc(a.name)}</option>`).join("")}</select>`) +
      row("Memoria a largo plazo", "Aprender de las conversaciones", sw("memory_enabled")) + row("Voz", "Entrada y salida por voz", sw("voice_enabled")) +
      row("Leer respuestas automáticamente", "", sw("auto_speak")) +
      row("Voz del navegador", "Si no hay voz local", `<select class="input" data-u="voice_name" style="width:200px"><option value="">automática</option>${voices.map((v) => `<option ${u.voice_name === v.name ? "selected" : ""}>${esc(v.name)}</option>`).join("")}</select>`) +
      row("Velocidad de voz", "", `<input type="range" min="0.6" max="1.6" step="0.1" data-u="speech_rate" value="${u.speech_rate}">`) +
      row("Diseño de la interfaz", `Solo en este dispositivo · detectado: ${LAYOUT_NAMES[NexusDevice.detected()]}${NexusDevice.apk ? " (app Android)" : NexusDevice.pwa ? " (app instalada)" : ""}`,
        `<select class="input" id="layout-pref" style="width:auto">${Object.entries({ auto: "Automático", ...LAYOUT_NAMES }).map(([v, n]) => `<option value="${v}" ${NexusDevice.preference === v ? "selected" : ""}>${n}</option>`).join("")}</select>`) +
      row("Mostrar avatar", "", sw("avatar_enabled")) +
      `<div class="setting" style="display:block"><div class="lbl"><b>Avatar</b><small>Todos reaccionan a la voz y al estado, y muestran tu grafo de memoria</small></div>
        <div class="avatar-pick" style="margin-top:8px">
          <button data-av="geisha"><span class="e">🌑</span>Geisha del eclipse<br><small>3D animado</small></button>
          <button data-av="grafo"><span class="e">◉</span>Grafo neuronal<br><small>2D, muy ligero</small></button>
          <button data-av="glb"><span class="e">⬢</span>Modelo propio<br><small>GLB · GLTF · VRM</small></button>
        </div>
        <div class="row" style="margin-top:8px"><span class="muted mono" style="flex:1">${u.avatar_model ? esc(u.avatar_model.split("/").pop()) : "sin modelo propio"}</span>
          <button class="btn sm" id="av-upload">SUBIR MODELO 3D</button><input type="file" id="av-file" accept=".glb,.gltf,.vrm" hidden></div></div>`;
    $$("[data-av]").forEach((b) => { b.classList.toggle("on", (u.avatar_style || "geisha") === b.dataset.av);
      b.onclick = guard(async () => {
        if (b.dataset.av === "glb" && !u.avatar_model) return $("#av-file").click();
        await api("/api/config", { method: "PUT", body: { "user.avatar_style": b.dataset.av } }); await loadConfig(); loaders.settings(); toast("Avatar cambiado"); }); });
    themeEditor();
    $("#layout-pref").onchange = (e) => { NexusDevice.setPreference(e.target.value); toast("Diseño: " + ({ auto: "automático", ...LAYOUT_NAMES })[e.target.value].toLowerCase()); };
    $("#av-upload").onclick = () => $("#av-file").click();
    $("#av-file").onchange = guard(async (e) => {
      const f = e.target.files[0]; if (!f) return; const fd = new FormData(); fd.append("file", f);
      toast("Subiendo modelo…"); const r = await api("/api/avatar/upload", { body: fd });
      await api("/api/config", { method: "PUT", body: { "user.avatar_model": r.url, "user.avatar_style": "glb" } });
      await loadConfig(); loaders.settings(); toast("Modelo 3D activado");
    });
    $$("[data-u]").forEach((el) => (el.onchange = guard(async () => {
      const v = el.type === "checkbox" ? el.checked : el.type === "range" ? +el.value : el.value;
      await api("/api/config", { method: "PUT", body: { ["user." + el.dataset.u]: v } });
      if (el.dataset.u === "auto_speak") { S.speak = v; local.set("nexus_speak", v); $("#btn-speak").classList.toggle("on", v); }
      await loadConfig(); toast("Guardado");
    })));
    if (window.NexusKG) guard(NexusKG.load)();   // gestor de conocimiento (js/kgadmin.js)
    if (!S.token) return;
    const c = S.fullCfg; const sections = Object.keys(c).filter((k) => k !== "user");
    $("#admin-settings").innerHTML = sections.map((sec) => `<details class="item"><summary style="cursor:pointer;font:700 12px var(--font-hud);letter-spacing:.15em">${sec.toUpperCase()}</summary>${Object.entries(c[sec]).map(([k, v]) => {
      const key = `${sec}.${k}`, t = typeof v;
      const ctrl = t === "boolean" ? `<label class="switch"><input type="checkbox" data-a="${key}" data-t="b" ${v ? "checked" : ""}><span></span></label>`
        : t === "number" ? `<input class="input" type="number" step="any" data-a="${key}" data-t="n" value="${v}" style="width:140px">`
        : t === "object" ? `<textarea class="input" data-a="${key}" data-t="j" rows="2" style="width:260px">${esc(JSON.stringify(v))}</textarea>`
        : `<input class="input" data-a="${key}" data-t="s" value="${esc(v)}" style="width:260px">`;
      return `<div class="setting"><div class="lbl mono">${k}</div>${ctrl}</div>`; }).join("")}</details>`).join("");
    $$("[data-a]").forEach((el) => (el.onchange = guard(async () => {
      const t = el.dataset.t, v = t === "b" ? el.checked : t === "n" ? +el.value : t === "j" ? JSON.parse(el.value) : el.value;
      if (v === "********") return; await api("/api/config", { method: "PUT", body: { [el.dataset.a]: v } }); toast(`${el.dataset.a} actualizado`); await loadConfig(); loadStatus();
    })));
  };
  /* ---------------- editor de temas */
  function themeEditor() {
    const TH = NexusThemes;
    S.themeDraft = TH.resolve(S.cfg.theme && Object.keys(S.cfg.theme).length ? S.cfg.theme : { preset: "neon" });
    const preview = () => { const t = TH.apply(S.themeDraft); avatar.setAccent(t.accent); };
    $("#theme-presets").innerHTML = Object.entries(TH.PRESETS).map(([id, p]) => `<button data-preset="${id}" class="${S.themeDraft.preset === id ? "on" : ""}"
      style="background:${p.bg};color:${p.text}"><div class="sw">${[p.accent, p.accent2, p.panel, p.ok, p.bad].map((c) => `<i style="background:${c}"></i>`).join("")}</div>${esc(p.name)}</button>`).join("");
    $$("[data-preset]").forEach((b) => (b.onclick = () => { S.themeDraft = { ...TH.PRESETS[b.dataset.preset], preset: b.dataset.preset }; preview(); themeEditor(); }));
    const colors = { accent: "Acento", accent2: "Acento 2", bg: "Fondo", panel: "Paneles", text: "Texto", muted: "Texto secundario", chrome: "Barras",
                     field: "Campos", ok: "Correcto", warn: "Aviso", bad: "Error", on_accent: "Texto sobre acento", code_bg: "Código" };
    $("#theme-colors").innerHTML = Object.entries(colors).map(([k, l]) => `<label>${l}<input type="color" data-tc="${k}" value="${S.themeDraft[k]}"></label>`).join("");
    const fontSel = (k, l) => `<label>${l}<select class="input" data-tf="${k}" style="width:150px">${Object.entries(TH.FONTS).map(([n, v]) => `<option value='${v}' ${S.themeDraft[k] === v ? "selected" : ""}>${n}</option>`).join("")}</select></label>`;
    const rng = (k, l, min, max, step) => `<label>${l}<input type="range" data-tr="${k}" min="${min}" max="${max}" step="${step}" value="${S.themeDraft[k]}"></label>`;
    const tog = (k, l) => `<label>${l}<span class="switch"><input type="checkbox" data-tt="${k}" ${S.themeDraft[k] ? "checked" : ""}><span></span></span></label>`;
    $("#theme-shape").innerHTML = fontSel("font_ui", "Fuente de texto") + fontSel("font_hud", "Fuente de títulos") + fontSel("font_mono", "Fuente de código") +
      rng("radius", "Redondeo", 0, 28, 1) + rng("glow", "Brillo neón", 0, 100, 5) + rng("blur", "Desenfoque", 0, 30, 1) + rng("panel_alpha", "Opacidad paneles", 0.2, 1, 0.02) +
      tog("grid", "Rejilla 3D") + tog("stars", "Partículas") + tog("scanlines", "Líneas CRT") + tog("vignette", "Viñeta") + tog("corners", "Esquinas HUD") + tog("light", "Modo claro");
    const custom = () => { S.themeDraft.preset = S.themeDraft.preset && !S.themeDraft.preset.endsWith("*") ? S.themeDraft.preset + "*" : S.themeDraft.preset || "custom"; };
    $$("[data-tc]").forEach((i) => (i.oninput = () => { S.themeDraft[i.dataset.tc] = i.value; custom(); preview(); }));
    $$("[data-tf]").forEach((i) => (i.onchange = () => { S.themeDraft[i.dataset.tf] = i.value; custom(); preview(); }));
    $$("[data-tr]").forEach((i) => (i.oninput = () => { S.themeDraft[i.dataset.tr] = +i.value; custom(); preview(); }));
    $$("[data-tt]").forEach((i) => (i.onchange = () => { S.themeDraft[i.dataset.tt] = i.checked; custom(); preview(); }));
  }
  const cleanTheme = (t) => { const o = { ...t }; if (o.preset) o.preset = o.preset.replace(/\*$/, ""); delete o.name; return o; };
  $("#theme-save").onclick = guard(async () => {
    await api("/api/config", { method: "PUT", body: { "user.theme": cleanTheme(S.themeDraft), "user.accent_color": S.themeDraft.accent } });
    await loadConfig(); toast("Tema guardado");
  });
  $("#theme-reset").onclick = guard(async () => { await api("/api/config", { method: "PUT", body: { "user.theme": {}, "user.accent_color": "#00f0ff" } }); await loadConfig(); themeEditor(); toast("Tema restablecido"); });
  $("#theme-export").onclick = () => {
    const blob = new Blob([JSON.stringify({ nexus_theme: 1, ...cleanTheme(S.themeDraft) }, null, 2)], { type: "application/json" });
    const a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = `nexus-tema-${(S.themeDraft.preset || "custom").replace(/\*$/, "")}.json`; a.click();
  };
  $("#theme-import").onclick = () => $("#theme-file").click();
  $("#theme-file").onchange = guard(async (e) => {
    const f = e.target.files[0]; if (!f) return; const t = JSON.parse(await f.text()); delete t.nexus_theme;
    S.themeDraft = { ...NexusThemes.resolve(t), ...t }; NexusThemes.apply(S.themeDraft); themeEditor(); toast("Tema importado: guárdalo para conservarlo"); e.target.value = "";
  });

  $("#pin-change-btn").onclick =guard(async () => { await api("/api/admin/pin", { body: { pin: $("#pin-change").value } }); $("#pin-change").value = ""; toast("PIN cambiado"); });

  loaders.chat = async () => { await loadSessions(); };

  // utilidades para los modulos de la interfaz que viven en otros ficheros (js/kgadmin.js)
  window.NexusUI = { api, toast, esc, guard, ago, fmtB, saveFile, agents: () => S.agents, admin: () => !!S.token,
                     agentName: (id) => agentName(id), refreshGraph: () => { loadGraph().catch(() => {}); } };

  /* ================================================================ selector de inicio: motor llama y CPU/GPU/NPU */
  const Accel = {
    d: null, engine: "auto", accel: "auto",
    async open(force) {
      const d = this.d = await api("/api/accel");
      if (!force && !d.ask) return;
      this.engine = d.engine; this.accel = d.accel;
      const h = d.hardware;
      $("#accel-hw").textContent = `${h.cpu} · ${h.cores} núcleos · ${h.ram_gb} GB` + (h.gpus.length ? ` · GPU: ${h.gpus.join(", ")}` : " · sin GPU") + (h.npus.length ? ` · NPU: ${h.npus.join(", ")}` : "");
      this.render(); this.result(d);
      const dlg = $("#accel-dialog"); if (!dlg.open) dlg.showModal();
    },
    render() {
      const opt = (list, cur, key) => list.map((o) => `<button type="button" data-${key}="${o.id}" class="${o.id === cur ? "on" : ""} ${o.available ? "" : "na"}">${esc(o.label)}${o.note ? `<small>${esc(o.note)}</small>` : ""}</button>`).join("");
      $("#accel-engines").innerHTML = opt(this.d.engines, this.engine, "eng");
      $("#accel-accels").innerHTML = opt(this.d.accels, this.accel, "acc");
      $$("[data-eng]").forEach((b) => (b.onclick = () => { this.engine = b.dataset.eng; this.render(); }));
      $$("[data-acc]").forEach((b) => (b.onclick = () => { this.accel = b.dataset.acc; this.render(); }));
      const a = this.d.accels.find((x) => x.id === this.accel), e = this.d.engines.find((x) => x.id === this.engine);
      $("#accel-note").textContent = [e && !e.available ? `${e.label}: ${e.note}; se usará el siguiente disponible` : "", a && !a.available ? a.note : ""].filter(Boolean).join(" · ");
    },
    result(r) {
      const eff = r.effective || {};
      $("#accel-result").innerHTML = `Ahora: <b>${esc(r.selected)}</b> · perfil ${esc(r.profile.accelerator)}${r.profile.gpu_layers ? " (capas en GPU)" : ""}` +
        (eff.device ? ` · el modelo está en <b>${esc(eff.device)}</b>${eff.offloaded ? ` (${esc(eff.offloaded)} capas)` : ""}${eff.detail ? ` · ${esc(eff.detail)}` : ""}${eff.ok === false ? ` <span class="tag warn">se pidió ${esc(eff.requested)}</span>` : ""}` : "") +
        (r.tokens_s ? ` · ${r.tokens_s} tok/s` : "") + (r.error ? `<div class="bad">${esc(r.error)}</div>` : "");
    },
    async apply() { await api("/api/accel", { body: { engine: this.engine, accel: this.accel, remember: $("#accel-remember").checked } }); },
  };
  $("#accel-go").onclick = guard(async () => { await Accel.apply(); $("#accel-dialog").close(); toast("Motor y acelerador aplicados"); loadStatus(); });
  $("#accel-test").onclick = guard(async () => {
    const b = $("#accel-test"); b.disabled = true; $("#accel-result").innerHTML = `<span class="spin"></span> Cargando el modelo y probando…`;
    try { await Accel.apply(); const r = await api("/api/accel/test", { body: {} }); Accel.d = { ...Accel.d, ...r }; Accel.result(r); toast(r.ok ? "Prueba correcta" : "La prueba ha fallado", !r.ok); }
    finally { b.disabled = false; }
  });
  $("#accel-open").onclick = guard(() => Accel.open(true));

  /* ================================================================ arranque */
  (async function boot() {
    connect();
    try { await loadConfig(); await loadStatus(); await loadAgents(); } catch (e) { toast("No se pudo contactar con el núcleo: " + e.message, true); }
    if (S.token) setAdmin(true);
    S.speak = S.speak || !!S.cfg.auto_speak; $("#btn-speak").classList.toggle("on", S.speak);
    await guard(openSession)(S.session); await guard(loadSessions)(); await guard(loadGraph)();
    if (location.hash.length > 1) go(location.hash.slice(1));
    guard(() => Accel.open(false))();
    addEventListener("hashchange", () => go(location.hash.slice(1)));
    setInterval(guard(loadStatus), 6000); setInterval(guard(sideStats), 2500); sideStats();
    setInterval(() => S.view === "memory" || guard(loadGraph)(), 30000);
    if ("serviceWorker" in navigator) navigator.serviceWorker.register("/sw.js").catch(() => {});
    if (window.NexusApp) {   // app Android: acceso a la pantalla de servidores
      const b = document.createElement("button"); b.className = "btn ghost sm"; b.textContent = "⇄ SERVIDOR"; b.onclick = () => NexusApp.reset();
      $(".topbar").insertBefore(b, $(".mode-switch"));
    }
  })();
})();
