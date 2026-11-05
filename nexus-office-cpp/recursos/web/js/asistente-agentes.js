/* Agente IA — asistente visual para crear y configurar agentes.
   Paso a paso: plantilla, identidad, personalidad, conocimiento, herramientas, equipo y revision. A la derecha, la
   ficha del agente y su «huella» de personalidad se actualizan al momento. Lo elegido se guarda en el campo
   «perfil» del agente para volver a abrir el asistente igual; el prompt de sistema se genera a partir de ello (y
   se puede retocar a mano en el ultimo paso).
   Usa window.NexusUI (app.js): api, toast, esc, agents. */
(() => {
  "use strict";
  const UI = () => window.NexusUI;
  const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const slug = (s) => String(s || "").normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40);

  /* ------------------------------------------------------------ catalogo */
  const GROUPS = [
    { id: "memoria", name: "Memoria y documentos", icon: "🧠", tools: ["memory_search", "memory_save", "knowledge_graph", "graph_import", "rag_search", "rag_list"] },
    { id: "web", name: "Internet", icon: "🌐", tools: ["web_search", "web_fetch", "url_to_pdf", "crawl_docs"] },
    { id: "crear", name: "Imagen, vídeo y música", icon: "🎨", tools: ["generate_image", "enhance_prompt", "crear_video", "compose_music", "imagen_editar", "imagen_info", "gimp_estado", "gimp_script"] },
    { id: "ficheros", name: "Ficheros y código", icon: "📁", tools: ["file_list", "file_read", "file_write", "python_exec"] },
    { id: "util", name: "Cálculo y sistema", icon: "🧮", tools: ["calculator", "datetime_now", "system_info"] },
    { id: "equipo", name: "Trabajo en equipo", icon: "🤝", tools: ["delegate", "list_agents", "consultar_profesor", "propose_tool"] },
    { id: "bolsa", name: "Bolsa (simulada)", icon: "📈", tools: ["bolsa_mercados", "bolsa_cotizacion", "bolsa_cartera", "bolsa_costes", "bolsa_orden", "bolsa_cancelar", "bolsa_informe_fiscal"] },
  ];
  const TOOL_ES = {
    memory_search: "Buscar en su memoria", memory_save: "Guardar recuerdos", knowledge_graph: "Grafo de conocimiento", graph_import: "Importar grafos",
    rag_search: "Buscar en documentos", rag_list: "Ver documentos", web_search: "Buscar en internet", web_fetch: "Leer páginas web",
    url_to_pdf: "Guardar web como PDF", crawl_docs: "Aprender una documentación web", generate_image: "Crear imágenes", enhance_prompt: "Mejorar prompts",
    crear_video: "Montar vídeos", compose_music: "Componer música", imagen_editar: "Retocar imágenes", imagen_info: "Analizar imágenes",
    gimp_estado: "Estado de GIMP", gimp_script: "Scripts de GIMP", file_list: "Ver ficheros", file_read: "Leer ficheros", file_write: "Escribir ficheros",
    python_exec: "Ejecutar código Python", calculator: "Calculadora", datetime_now: "Fecha y hora", system_info: "Info del equipo",
    delegate: "Delegar en otros agentes", list_agents: "Ver el equipo", consultar_profesor: "Consultar al profesor", propose_tool: "Proponer herramientas",
    bolsa_mercados: "Mercados", bolsa_cotizacion: "Cotizaciones", bolsa_cartera: "Cartera", bolsa_costes: "Costes", bolsa_orden: "Órdenes",
    bolsa_cancelar: "Cancelar órdenes", bolsa_informe_fiscal: "Informe fiscal",
  };
  const PRESETS = {
    charla: { name: "Solo conversación", tools: ["memory_search", "memory_save", "datetime_now", "calculator"] },
    investiga: { name: "Investigación", tools: ["memory_search", "memory_save", "rag_search", "rag_list", "web_search", "web_fetch", "url_to_pdf", "crawl_docs", "knowledge_graph", "datetime_now"] },
    creativo: { name: "Creativo", tools: ["generate_image", "enhance_prompt", "crear_video", "compose_music", "imagen_editar", "imagen_info", "memory_search", "file_list"] },
    oficina: { name: "Oficina", tools: ["memory_search", "memory_save", "knowledge_graph", "rag_search", "rag_list", "file_list", "file_read", "file_write", "calculator", "datetime_now", "delegate", "list_agents"] },
    todas: { name: "Todas", tools: ["*"] },
  };
  const TRAITS = ["empático", "directo", "didáctico", "con humor", "prudente", "persuasivo", "analítico", "creativo", "paciente", "resolutivo", "minucioso", "optimista"];
  const ICONS = ["◆", "✦", "★", "⚡", "☀", "☾", "♞", "⚖", "⚙", "✎", "✉", "☎", "⌘", "⚕", "♫", "❖", "🤖", "🧠", "💼", "📊", "📈", "🧾", "🛒", "🎨", "🎬", "📚", "🧪", "🛠", "🌐", "📣", "🤝", "🏛", "🩺", "🍳", "✈", "🏠"];
  const COLORS = ["#00f0ff", "#ff3df2", "#7cff6b", "#ffb020", "#ff4d6d", "#8b7bff", "#3da5ff", "#00e0a4", "#ff8a3d", "#f5f5f5"];
  const TEMPLATES = [
    { id: "cero", icon: "◆", name: "Desde cero", desc: "Un agente en blanco", p: {} },
    { id: "atencion", icon: "☎", name: "Atención al cliente", desc: "Responde dudas, incidencias y pedidos con amabilidad",
      p: { rol: "responsable de atención al cliente", tono: 70, creatividad: 0.4, longitud: 1, iniciativa: 50, rasgos: ["empático", "paciente", "resolutivo"],
        especialidad: "atención al cliente, incidencias, devoluciones, pedidos, garantías, quejas", keywords: ["cliente", "pedido", "devolución", "incidencia", "queja", "reclamación"],
        reglas: "Nunca prometas plazos ni compensaciones que no estén en la política de la empresa.\nSi el cliente está molesto, reconoce el problema antes de dar la solución.", preset: "oficina", color: "#3da5ff", sexo: "" } },
    { id: "comercial", icon: "💼", name: "Comercial", desc: "Presupuestos, seguimiento de clientes y propuestas",
      p: { rol: "comercial", tono: 60, creatividad: 0.7, longitud: 1, iniciativa: 80, rasgos: ["persuasivo", "optimista", "resolutivo"],
        especialidad: "ventas, presupuestos, propuestas comerciales, negociación, seguimiento de clientes, CRM", keywords: ["venta", "presupuesto", "oferta", "cliente", "propuesta", "comercial"],
        reglas: "Propón siempre un siguiente paso concreto (llamada, envío, fecha).", preset: "oficina", color: "#ffb020" } },
    { id: "contable", icon: "🧾", name: "Contabilidad y finanzas", desc: "Facturas, impuestos, tesorería y cuentas",
      p: { rol: "contable", tono: 25, creatividad: 0.2, longitud: 2, iniciativa: 40, rasgos: ["minucioso", "prudente", "analítico"],
        especialidad: "contabilidad, facturación, IVA, IRPF, impuesto de sociedades, tesorería, balances, cuentas anuales", keywords: ["factura", "iva", "impuesto", "contabilidad", "balance", "gasto", "tesorería"],
        reglas: "Muestra los cálculos paso a paso y usa la calculadora para las cifras.\nIndica cuándo conviene revisarlo con un asesor fiscal.", preset: "oficina", model: "especialidad:finanzas", color: "#7cff6b" } },
    { id: "marketing", icon: "📣", name: "Marketing y redes", desc: "Campañas, textos, publicaciones e imágenes",
      p: { rol: "responsable de marketing", tono: 80, creatividad: 1.0, longitud: 1, iniciativa: 70, rasgos: ["creativo", "con humor", "persuasivo"],
        especialidad: "marketing digital, redes sociales, copywriting, SEO, campañas, branding, newsletters", keywords: ["marketing", "redes", "instagram", "campaña", "publicación", "anuncio", "seo"],
        reglas: "Adapta el texto a cada red (longitud, tono, hashtags).", preset: "creativo", color: "#ff3df2" } },
    { id: "disenador", icon: "🎨", name: "Diseño y vídeo", desc: "Imágenes, carteles, logos y vídeos cortos",
      p: { rol: "diseñador gráfico", tono: 70, creatividad: 1.1, longitud: 0, iniciativa: 60, rasgos: ["creativo", "minucioso"],
        especialidad: "diseño gráfico, ilustración, color, tipografía, composición, vídeo corto, carteles, logos", keywords: ["imagen", "diseño", "logo", "cartel", "vídeo", "ilustración"],
        reglas: "Escribe los prompts de imagen en inglés y detallados.\nPara un vídeo, genera primero las imágenes en orden y luego usa crear_video.", preset: "creativo", color: "#8b7bff" } },
    { id: "investigador", icon: "🌐", name: "Investigador", desc: "Busca, contrasta y resume información",
      p: { rol: "investigador", tono: 40, creatividad: 0.3, longitud: 2, iniciativa: 50, rasgos: ["analítico", "minucioso", "didáctico"],
        especialidad: "investigación, documentación, fuentes, resúmenes, análisis de mercado, competencia", keywords: ["investiga", "busca", "compara", "resumen", "información", "fuentes"],
        reglas: "Cita las fuentes con su enlace.\nSepara lo comprobado de lo que es una suposición.", preset: "investiga", color: "#00e0a4" } },
    { id: "programador", icon: "⌘", name: "Programador", desc: "Código, scripts, hojas de cálculo y automatizaciones",
      p: { rol: "programador", tono: 35, creatividad: 0.3, longitud: 1, iniciativa: 60, rasgos: ["directo", "resolutivo", "minucioso"],
        especialidad: "programación, Python, scripts, automatización, hojas de cálculo, SQL, APIs", keywords: ["código", "programa", "script", "python", "excel", "automatiza", "error"],
        reglas: "Da el código completo y explica cómo ejecutarlo.\nPrueba el código con python_exec antes de darlo por bueno cuando puedas.", tools: ["file_list", "file_read", "file_write", "python_exec", "calculator", "web_search", "web_fetch", "memory_search"], color: "#00f0ff" } },
    { id: "rrhh", icon: "🤝", name: "Recursos humanos", desc: "Selección, nóminas, vacaciones y comunicación interna",
      p: { rol: "técnico de recursos humanos", tono: 55, creatividad: 0.5, longitud: 1, iniciativa: 40, rasgos: ["empático", "prudente", "paciente"],
        especialidad: "recursos humanos, selección, nóminas, vacaciones, convenio, contratos, formación", keywords: ["empleado", "nómina", "vacaciones", "contrato", "selección", "convenio"],
        reglas: "Trata los datos personales con discreción y no los repitas sin necesidad.", preset: "oficina", color: "#ff8a3d" } },
    { id: "legal", icon: "⚖", name: "Asesor legal", desc: "Contratos, RGPD, avisos legales y normativa",
      p: { rol: "asesor legal", tono: 20, creatividad: 0.2, longitud: 2, iniciativa: 30, rasgos: ["prudente", "analítico", "minucioso"],
        especialidad: "derecho mercantil, contratos, RGPD, protección de datos, condiciones de venta, aviso legal", keywords: ["contrato", "legal", "rgpd", "ley", "cláusula", "normativa"],
        reglas: "No sustituyes a un abogado: indícalo cuando el caso lo requiera.\nCita la norma en la que te basas.", preset: "investiga", color: "#f5f5f5" } },
  ];

  /* ------------------------------------------------------------ estado */
  const STEPS = ["Plantilla", "Identidad", "Personalidad", "Conocimiento", "Herramientas", "Equipo", "Revisar"];
  let W = null;   // { step, a (ficha), p (perfil), tools (catalogo), editing, promptTouched }

  const defaults = () => ({ plantilla: "cero", rol: "asistente", tono: 50, creatividad: 0.7, longitud: 1, iniciativa: 50, idioma: "es", rasgos: [], reglas: "", preset: "", ejemplos: "" });

  function fromAgent(a) {
    const p = Object.assign(defaults(), a.perfil || {});
    if (!a.perfil && a.temperature != null) p.creatividad = a.temperature;
    return p;
  }

  /* ------------------------------------------------------------ prompt de sistema */
  function tonoTxt(t) {
    return t < 25 ? "Hablas con un registro formal y profesional, tratando de usted salvo que te pidan lo contrario."
      : t < 50 ? "Hablas con un tono profesional y correcto, sin ser frío."
      : t < 75 ? "Hablas con un tono cercano y amable, tuteando."
      : "Hablas con un tono muy cercano, natural y desenfadado, tuteando.";
  }
  const LONG = ["Respondes de forma breve y al grano: pocas frases o una lista corta.", "Respondes con la extensión justa: completo pero sin rodeos.", "Respondes de forma detallada y estructurada (títulos, listas, ejemplos) cuando el tema lo merece."];
  function iniciativaTxt(i) {
    return i < 34 ? "Haces solo lo que se te pide; si falta información, preguntas antes de actuar."
      : i < 67 ? "Si ves algo importante que el usuario no ha pedido, lo mencionas brevemente al final."
      : "Eres proactivo: propones mejoras, siguientes pasos y alternativas, y te adelantas a lo que hará falta.";
  }
  const IDIOMA = { es: "Respondes siempre en español.", auto: "Respondes en el idioma en el que te escriban.", en: "You always answer in English." };
  function buildPrompt(a, p) {
    const lines = [`Eres ${a.name || "un agente"}, ${p.rol || "asistente"} de {assistant_name}.${a.description ? " " + a.description.replace(/\.?$/, ".") : ""}`];
    if (a.specialty) lines.push(`Tu especialidad: ${a.specialty}.`);
    lines.push(tonoTxt(p.tono) + " " + LONG[p.longitud ?? 1]);
    if (p.rasgos.length) lines.push(`Tu forma de ser: ${p.rasgos.join(", ")}.`);
    lines.push(iniciativaTxt(p.iniciativa));
    lines.push(IDIOMA[p.idioma] || IDIOMA.es);
    const reglas = (p.reglas || "").split("\n").map((r) => r.trim()).filter(Boolean);
    if (reglas.length) lines.push("Reglas:\n" + reglas.map((r) => "- " + r).join("\n"));
    const t = a.tools.includes("*") ? [] : a.tools;
    const hints = [];
    if (t.includes("memory_search")) hints.push("consulta tu memoria antes de responder sobre el usuario o la empresa");
    if (t.includes("rag_search")) hints.push("busca en los documentos de la biblioteca cuando la pregunta trate de ellos");
    if (t.includes("web_search")) hints.push("busca en internet lo que pueda haber cambiado o no sepas");
    if (t.includes("generate_image")) hints.push("para imágenes, escribe el prompt de generate_image en inglés y detallado");
    if (t.includes("crear_video")) hints.push("para un vídeo, genera las imágenes en orden y móntalas con crear_video");
    if (t.includes("delegate")) hints.push("delega en otro agente lo que sea de su especialidad");
    if (hints.length) lines.push("Herramientas: " + hints.join("; ") + ".");
    if ((p.ejemplos || "").trim()) lines.push("Ejemplos de cómo respondes:\n" + p.ejemplos.trim());
    return lines.join("\n\n");
  }

  /* ------------------------------------------------------------ huella de personalidad (radar SVG) */
  function radar(a, p) {
    const vals = [
      ["Creatividad", Math.min(1, p.creatividad / 1.4)],
      ["Cercanía", p.tono / 100],
      ["Detalle", (p.longitud ?? 1) / 2],
      ["Iniciativa", p.iniciativa / 100],
      ["Empatía", Math.min(1, (["empático", "paciente", "con humor", "optimista"].filter((r) => p.rasgos.includes(r)).length + 0.5) / 3)],
      ["Rigor", Math.min(1, (["minucioso", "analítico", "prudente", "directo"].filter((r) => p.rasgos.includes(r)).length + 0.5) / 3)],
    ];
    const n = vals.length, cx = 110, cy = 100, R = 70;
    const pt = (i, r) => [cx + Math.sin((2 * Math.PI * i) / n) * r, cy - Math.cos((2 * Math.PI * i) / n) * r];
    const ring = (f) => vals.map((_, i) => pt(i, R * f).join(",")).join(" ");
    const shape = vals.map(([, v], i) => pt(i, R * (0.12 + 0.88 * v)).join(",")).join(" ");
    const c = a.color || "#00f0ff";
    return `<svg viewBox="0 0 220 200" class="awz-radar" role="img" aria-label="Huella de personalidad">
      ${[0.33, 0.66, 1].map((f) => `<polygon points="${ring(f)}" fill="none" stroke="currentColor" stroke-opacity=".18"/>`).join("")}
      ${vals.map((_, i) => `<line x1="${cx}" y1="${cy}" x2="${pt(i, R)[0]}" y2="${pt(i, R)[1]}" stroke="currentColor" stroke-opacity=".12"/>`).join("")}
      <polygon points="${shape}" fill="${c}" fill-opacity=".28" stroke="${c}" stroke-width="2"/>
      ${vals.map(([l], i) => { const [x, y] = pt(i, R + 16); return `<text x="${x}" y="${y}" text-anchor="middle" dominant-baseline="middle" font-size="10" fill="currentColor" fill-opacity=".75">${l}</text>`; }).join("")}
    </svg>`;
  }

  /* ------------------------------------------------------------ interfaz */
  function css() {
    if (document.getElementById("awz-css")) return;
    const st = document.createElement("style");
    st.id = "awz-css";
    st.textContent = `
.awz{width:min(1100px,96vw);max-height:94vh;padding:0;display:flex;flex-direction:column;overflow:hidden}
.awz[open]{display:flex}
.awz-head{display:flex;align-items:center;gap:12px;padding:16px 20px;flex-shrink:0;border-bottom:1px solid var(--line)}
.awz-head h2{margin:0;flex:1}
.awz-steps{display:flex;gap:4px;padding:10px 20px;overflow-x:auto;flex-shrink:0;border-bottom:1px solid var(--line)}
.awz-steps button{background:none;border:1px solid var(--line);border-radius:999px;padding:4px 12px;color:var(--muted);cursor:pointer;white-space:nowrap;font:600 13px var(--font-ui)}
.awz-steps button.on{color:var(--on-accent,#001018);background:var(--accent);border-color:var(--accent)}
.awz-steps button.done{color:var(--accent);border-color:var(--line-strong)}
.awz-body{display:grid;grid-template-columns:minmax(0,1fr) 280px;gap:18px;padding:18px 20px;overflow:auto;flex:1;min-height:0}
@media (max-width:820px){.awz-body{grid-template-columns:1fr}.awz-side{order:-1}}
.awz-foot{display:flex;gap:8px;padding:12px 20px;border-top:1px solid var(--line);align-items:center;flex-shrink:0}
.awz-foot .sp{flex:1}
.awz h4{margin:14px 0 6px;font:700 12px var(--font-hud);letter-spacing:.14em;color:var(--muted)}
.awz-tpl{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:10px}
.awz-tpl button{text-align:left;background:var(--field);border:1px solid var(--line);border-radius:12px;padding:12px;cursor:pointer;color:var(--text)}
.awz-tpl button:hover,.awz-tpl button.on{border-color:var(--accent);box-shadow:var(--glow)}
.awz-tpl .i{font-size:24px}.awz-tpl b{display:block;margin-top:4px}.awz-tpl span{font-size:13px;color:var(--muted)}
.awz-chips{display:flex;flex-wrap:wrap;gap:6px}
.awz-chip{border:1px solid var(--line-strong);border-radius:999px;padding:3px 10px;cursor:pointer;background:none;color:var(--text);font:600 13px var(--font-ui)}
.awz-chip.on{background:color-mix(in srgb,var(--accent) 22%,transparent);border-color:var(--accent)}
.awz-chip .x{margin-left:6px;opacity:.6}
.awz-icons{display:grid;grid-template-columns:repeat(auto-fill,minmax(38px,1fr));gap:4px}
.awz-icons button{font-size:20px;height:38px;background:var(--field);border:1px solid var(--line);border-radius:8px;cursor:pointer;color:var(--text)}
.awz-icons button.on{border-color:var(--accent);box-shadow:var(--glow)}
.awz-colors{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.awz-colors button{width:30px;height:30px;border-radius:50%;border:2px solid transparent;cursor:pointer}
.awz-colors button.on{border-color:var(--text)}
.awz-slider{margin:8px 0 14px}
.awz-slider .lbl{display:flex;justify-content:space-between;font-size:13px;color:var(--muted)}
.awz-slider .lbl b{color:var(--text)}
.awz-slider input{width:100%;accent-color:var(--accent)}
.awz-slider .ends{display:flex;justify-content:space-between;font-size:12px;color:var(--muted)}
.awz-seg{display:inline-flex;border:1px solid var(--line-strong);border-radius:10px;overflow:hidden}
.awz-seg button{background:none;border:0;padding:6px 12px;color:var(--text);cursor:pointer}
.awz-seg button.on{background:var(--accent);color:var(--on-accent,#001018)}
.awz-group{border:1px solid var(--line);border-radius:12px;padding:10px;margin-bottom:10px}
.awz-group .gh{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.awz-group .gh b{flex:1}
.awz-tools{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:6px}
.awz-tool{display:flex;gap:8px;align-items:flex-start;padding:6px 8px;border:1px solid var(--line);border-radius:9px;cursor:pointer}
.awz-tool.on{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 10%,transparent)}
.awz-tool small{display:block;color:var(--muted);font-family:var(--font-mono);font-size:11px}
.awz-risk{font-size:10px;border-radius:6px;padding:0 5px;margin-left:auto}
.awz-risk.low{color:var(--ok);border:1px solid var(--ok)}.awz-risk.medium{color:var(--warn);border:1px solid var(--warn)}.awz-risk.high{color:var(--bad);border:1px solid var(--bad)}
.awz-card{border:1px solid var(--line-strong);border-radius:16px;padding:14px;position:relative;overflow:hidden;background:var(--field)}
.awz-card::before{content:"";position:absolute;inset:-40% -40% auto auto;width:180px;height:180px;background:radial-gradient(circle,var(--c) 0,transparent 70%);opacity:.25}
.awz-card .ico{width:54px;height:54px;border-radius:14px;display:flex;align-items:center;justify-content:center;font-size:28px;border:2px solid var(--c);box-shadow:0 0 18px color-mix(in srgb,var(--c) 50%,transparent)}
.awz-card .nm{font:700 15px var(--font-hud);letter-spacing:.08em;margin-top:8px}
.awz-card .rl{color:var(--muted);font-size:13px}
.awz-radar{width:100%;color:var(--text);margin-top:8px}
.awz-stat{display:flex;justify-content:space-between;font-size:13px;border-top:1px dashed var(--line);padding:4px 0}
.awz textarea.input,.awz input.input,.awz select.input{width:100%}
.awz-err{color:var(--bad);font-size:13px;min-height:1.2em}
`;
    document.head.appendChild(st);
  }

  function dialog() {
    let d = document.getElementById("awz");
    if (d) return d;
    d = document.createElement("dialog");
    d.id = "awz";
    d.className = "awz";
    d.innerHTML = `<div class="awz-head"><h2>✨ Asistente de agentes</h2><button class="btn ghost sm" data-x>✕</button></div>
      <div class="awz-steps" id="awz-steps"></div>
      <div class="awz-body"><div id="awz-main"></div><aside class="awz-side" id="awz-side"></aside></div>
      <div class="awz-foot"><span class="awz-err" id="awz-err"></span><span class="sp"></span><button class="btn ghost" data-prev>ATRÁS</button><button class="btn primary" data-next>SIGUIENTE</button></div>`;
    document.body.appendChild(d);
    d.querySelector("[data-x]").onclick = () => d.close();
    d.querySelector("[data-prev]").onclick = () => go(W.step - 1);
    d.querySelector("[data-next]").onclick = () => (W.step === STEPS.length - 1 ? save() : go(W.step + 1));
    return d;
  }

  function open(agent, tools) {
    css();
    const base = agent || { id: "", name: "", description: "", system_prompt: "", model: "default", temperature: 0.7, max_tokens: null, tools: [], memory: true,
      can_delegate_to: [], keywords: [], specialty: "", color: "#00f0ff", icon: "◆", enabled: true, sexo: "" };
    const a = JSON.parse(JSON.stringify(base));
    W = { step: agent ? 1 : 0, a, p: fromAgent(a), tools: tools || [], editing: !!agent && !!agent.id, promptTouched: false, models: [] };
    if (W.editing && a.perfil == null) W.promptTouched = true;   // agente sin asistente: su prompt se respeta salvo que lo regenere
    UI().api("/api/models").then((m) => { W.models = (m.catalog || []).filter((c) => c.kind === "llm"); if (W.step === 5) draw(); }).catch(() => {});
    const d = dialog();
    draw();
    d.showModal();
  }

  function go(n) {
    const err = validate(W.step);
    if (n > W.step && err) return showErr(err);
    showErr("");
    W.step = Math.max(0, Math.min(STEPS.length - 1, n));
    draw();
  }
  function showErr(t) { document.getElementById("awz-err").textContent = t; }
  function validate(step) {
    if (step === 1) {
      if (!W.a.name.trim()) return "Ponle un nombre al agente";
      if (!W.editing) {
        if (!W.a.id) W.a.id = slug(W.a.name);
        if (!/^[a-z0-9][a-z0-9-]{1,39}$/.test(W.a.id)) return "El identificador solo admite minúsculas, números y guiones";
        if ((UI().agents() || []).some((x) => x.id === W.a.id)) return `Ya hay un agente con el identificador «${W.a.id}»`;
      }
    }
    return "";
  }

  function applyTemplate(t) {
    const p = t.p || {};
    W.p = Object.assign(defaults(), { plantilla: t.id }, p);
    if (t.id !== "cero") {
      if (!W.a.name) W.a.name = t.name;
      W.a.description = W.a.description || t.desc;
      W.a.icon = t.icon;
      if (p.color) W.a.color = p.color;
      W.a.specialty = p.especialidad || W.a.specialty;
      W.a.keywords = p.keywords ? [...p.keywords] : W.a.keywords;
      W.a.tools = p.tools ? [...p.tools] : p.preset ? [...PRESETS[p.preset].tools] : W.a.tools;
      if (p.model) W.a.model = p.model;
      if (p.sexo != null) W.a.sexo = p.sexo;
    }
    W.a.temperature = W.p.creatividad;
  }

  function sync() {
    W.a.temperature = Math.round(W.p.creatividad * 10) / 10;
    W.a.max_tokens = [400, null, 2000][W.p.longitud ?? 1];
    W.a.perfil = W.p;
    if (!W.promptTouched) W.a.system_prompt = buildPrompt(W.a, W.p);
    side();
  }

  function side() {
    const a = W.a, p = W.p;
    const nt = a.tools.includes("*") ? "todas" : a.tools.length;
    document.getElementById("awz-side").innerHTML = `<div class="awz-card" style="--c:${esc(a.color)}">
      <div class="ico">${esc(a.icon)}</div>
      <div class="nm">${esc(a.name || "Sin nombre")}</div>
      <div class="rl">${esc(p.rol || "")}${a.id ? ` · <span class="mono">${esc(a.id)}</span>` : ""}</div>
      <div style="margin-top:6px;font-size:13px">${esc(a.description || "")}</div>
      ${radar(a, p)}
      <div class="awz-stat"><span>Creatividad</span><b>${a.temperature ?? "—"}</b></div>
      <div class="awz-stat"><span>Herramientas</span><b>${nt}</b></div>
      <div class="awz-stat"><span>Memoria</span><b>${a.memory ? "sí" : "no"}</b></div>
      <div class="awz-stat"><span>Modelo</span><b class="mono">${esc(a.model)}</b></div>
      <div class="awz-chips" style="margin-top:8px">${p.rasgos.map((r) => `<span class="awz-chip on">${esc(r)}</span>`).join("")}</div>
    </div>`;
  }

  function slider(key, label, min, max, step, ends, fmt) {
    const v = W.p[key];
    return `<div class="awz-slider"><div class="lbl"><span>${label}</span><b data-out="${key}">${fmt ? fmt(v) : v}</b></div>
      <input type="range" data-p="${key}" min="${min}" max="${max}" step="${step}" value="${v}"><div class="ends"><span>${ends[0]}</span><span>${ends[1]}</span></div></div>`;
  }
  const creaFmt = (v) => (v < 0.35 ? "precisa" : v < 0.8 ? "equilibrada" : v < 1.15 ? "creativa" : "muy creativa") + ` (${(+v).toFixed(1)})`;
  const tonoFmt = (v) => (v < 25 ? "formal" : v < 50 ? "profesional" : v < 75 ? "cercano" : "muy cercano");
  const iniFmt = (v) => (v < 34 ? "a demanda" : v < 67 ? "sugiere" : "proactivo");

  function chipsInput(id, list, placeholder, suggestions = []) {
    return `<div class="awz-chips" data-chips="${id}">${list.map((k, i) => `<button class="awz-chip on" data-del="${i}">${esc(k)}<span class="x">✕</span></button>`).join("")}</div>
      <input class="input" data-chipin="${id}" placeholder="${esc(placeholder)}" style="margin-top:6px">
      ${suggestions.length ? `<div class="awz-chips" style="margin-top:6px">${suggestions.filter((s) => !list.includes(s)).map((s) => `<button class="awz-chip" data-sug="${id}" data-v="${esc(s)}">＋ ${esc(s)}</button>`).join("")}</div>` : ""}`;
  }

  function draw() {
    const a = W.a, p = W.p, s = W.step;
    document.getElementById("awz-steps").innerHTML = STEPS.map((n, i) => `<button class="${i === s ? "on" : i < s ? "done" : ""}" data-step="${i}">${i + 1}. ${n}</button>`).join("");
    const m = document.getElementById("awz-main");
    if (s === 0) {
      const ag = (UI().agents() || []).filter((x) => x.id !== a.id);
      m.innerHTML = `<h4>EMPIEZA CON UNA PLANTILLA</h4><div class="awz-tpl">${TEMPLATES.map((t) => `<button data-tpl="${t.id}" class="${p.plantilla === t.id ? "on" : ""}"><div class="i">${t.icon}</div><b>${esc(t.name)}</b><span>${esc(t.desc)}</span></button>`).join("")}</div>
        ${ag.length ? `<h4>O PARTE DE UN AGENTE QUE YA TIENES</h4><div class="awz-chips">${ag.map((x) => `<button class="awz-chip" data-clone="${esc(x.id)}">${esc(x.icon)} ${esc(x.name)}</button>`).join("")}</div>` : ""}`;
    } else if (s === 1) {
      m.innerHTML = `<div class="grid g2" style="gap:10px">
        <label class="field">Nombre<input class="input" data-a="name" value="${esc(a.name)}" placeholder="p. ej. Lucía, Contable"></label>
        <label class="field">Identificador ${W.editing ? "(fijo)" : ""}<input class="input mono" data-a="id" value="${esc(a.id)}" ${W.editing ? "readonly" : ""} placeholder="se crea del nombre"></label></div>
        <label class="field" style="margin-top:10px">Su papel en la oficina<input class="input" data-p="rol" value="${esc(p.rol)}" placeholder="p. ej. responsable de atención al cliente"></label>
        <label class="field" style="margin-top:10px">Qué hace (una frase)<input class="input" data-a="description" value="${esc(a.description)}"></label>
        <h4>ICONO</h4><div class="awz-icons">${ICONS.map((i) => `<button data-icon="${i}" class="${a.icon === i ? "on" : ""}">${i}</button>`).join("")}</div>
        <h4>COLOR</h4><div class="awz-colors">${COLORS.map((c) => `<button data-color="${c}" class="${a.color === c ? "on" : ""}" style="background:${c}" aria-label="${c}"></button>`).join("")}<input type="color" data-a="color" value="${esc(a.color)}"></div>
        <h4>ASPECTO EN EL SALÓN Y SU DESPACHO</h4><div class="awz-seg">${[["", "Al azar"], ["mujer", "Mujer"], ["hombre", "Hombre"]].map(([v, l]) => `<button data-sexo="${v}" class="${(a.sexo || "") === v ? "on" : ""}">${l}</button>`).join("")}</div>`;
    } else if (s === 2) {
      m.innerHTML = `${slider("tono", "Tono", 0, 100, 1, ["formal", "muy cercano"], tonoFmt)}
        ${slider("creatividad", "Creatividad", 0, 1.4, 0.1, ["precisa", "muy creativa"], creaFmt)}
        ${slider("iniciativa", "Iniciativa", 0, 100, 1, ["a demanda", "proactivo"], iniFmt)}
        <h4>LONGITUD DE LAS RESPUESTAS</h4><div class="awz-seg">${["Breves", "Normales", "Detalladas"].map((l, i) => `<button data-long="${i}" class="${(p.longitud ?? 1) === i ? "on" : ""}">${l}</button>`).join("")}</div>
        <h4>IDIOMA</h4><div class="awz-seg">${[["es", "Español"], ["auto", "El del usuario"], ["en", "Inglés"]].map(([v, l]) => `<button data-lang="${v}" class="${p.idioma === v ? "on" : ""}">${l}</button>`).join("")}</div>
        <h4>RASGOS (HASTA 4)</h4><div class="awz-chips">${TRAITS.map((t) => `<button class="awz-chip ${p.rasgos.includes(t) ? "on" : ""}" data-trait="${esc(t)}">${esc(t)}</button>`).join("")}</div>
        <h4>REGLAS QUE SIEMPRE CUMPLE (UNA POR LÍNEA)</h4><textarea class="input" rows="4" data-p="reglas" placeholder="p. ej. No des precios sin consultar la tarifa vigente">${esc(p.reglas)}</textarea>`;
    } else if (s === 3) {
      const spec = (a.specialty || "").split(",").map((x) => x.trim()).filter(Boolean);
      const tpl = TEMPLATES.find((t) => t.id === p.plantilla);
      m.innerHTML = `<h4>ESPECIALIDAD · LO QUE APRENDE DE ESTO VA A SU MEMORIA PROPIA</h4>${chipsInput("specialty", spec, "Escribe un área y pulsa Intro", tpl?.p?.especialidad ? tpl.p.especialidad.split(",").map((x) => x.trim()) : [])}
        <div class="muted" style="font-size:13px;margin-top:4px">Vacío: generalista (todo lo que aprende es conocimiento común de la oficina).</div>
        <h4>PALABRAS QUE LE LLEGAN · ENRUTADO AUTOMÁTICO</h4>${chipsInput("keywords", a.keywords, "Palabra o expresión y pulsa Intro", tpl?.p?.keywords || [])}
        <h4>MEMORIA</h4><label class="row"><span class="switch"><input type="checkbox" data-mem ${a.memory ? "checked" : ""}><span></span></span>Recuerda las conversaciones y aprende de ellas</label>
        <h4>EJEMPLOS DE RESPUESTA (OPCIONAL)</h4><textarea class="input" rows="4" data-p="ejemplos" placeholder="Usuario: ¿Cuándo llega mi pedido?&#10;Agente: Déjame mirarlo. ¿Me dices el número de pedido?">${esc(p.ejemplos || "")}</textarea>`;
    } else if (s === 4) {
      const all = a.tools.includes("*");
      const known = new Set(GROUPS.flatMap((g) => g.tools));
      const extra = W.tools.filter((t) => !known.has(t.name)).map((t) => t.name);
      const groups = extra.length ? [...GROUPS, { id: "otras", name: "Otras (de la comunidad)", icon: "🧩", tools: extra }] : GROUPS;
      const info = Object.fromEntries(W.tools.map((t) => [t.name, t]));
      m.innerHTML = `<h4>PUNTO DE PARTIDA</h4><div class="awz-chips">${Object.entries(PRESETS).map(([k, v]) => `<button class="awz-chip ${p.preset === k ? "on" : ""}" data-preset="${k}">${v.name}</button>`).join("")}</div>
        <h4>HERRAMIENTAS ${all ? "· TODAS ACTIVADAS" : `· ${a.tools.length} ELEGIDAS`}</h4>
        ${groups.map((g) => { const avail = g.tools.filter((t) => info[t]); if (!avail.length) return ""; const on = avail.filter((t) => all || a.tools.includes(t)).length;
          return `<div class="awz-group"><div class="gh"><span>${g.icon}</span><b>${esc(g.name)}</b><span class="muted">${on}/${avail.length}</span><button class="btn sm ghost" data-grp="${g.id}">${on === avail.length ? "Quitar" : "Todas"}</button></div>
          <div class="awz-tools">${avail.map((t) => `<label class="awz-tool ${all || a.tools.includes(t) ? "on" : ""}"><input type="checkbox" data-tool="${t}" ${all || a.tools.includes(t) ? "checked" : ""} ${all ? "disabled" : ""}><span>${esc(TOOL_ES[t] || t)}<small>${esc(t)}</small></span><span class="awz-risk ${info[t].risk}">${info[t].risk === "low" ? "bajo" : info[t].risk === "medium" ? "medio" : "alto"}</span></label>`).join("")}</div></div>`; }).join("")}
        <div class="muted" style="font-size:13px">Riesgo medio o alto: el supervisor pide tu aprobación antes de usarlas según la política de herramientas.</div>`;
      m._groups = groups;
    } else if (s === 5) {
      const others = (UI().agents() || []).filter((x) => x.id !== a.id);
      const all = a.can_delegate_to.includes("*");
      const opts = [["default", "El de la oficina (activo)"], ["especialidad:finanzas", "IA especializada en finanzas"], ...W.models.map((x) => [x.id, x.name])];
      if (!opts.some(([v]) => v === a.model)) opts.push([a.model, a.model]);
      m.innerHTML = `<h4>MODELO DE IA</h4><select class="input" data-a="model">${opts.map(([v, l]) => `<option value="${esc(v)}" ${a.model === v ? "selected" : ""}>${esc(l)}</option>`).join("")}</select>
        <div class="muted" style="font-size:13px;margin-top:4px">«El de la oficina» usa el modelo activo; elige otro si este agente necesita uno distinto (más rápido, con visión, especializado…).</div>
        <h4>PUEDE PEDIR AYUDA A</h4><div class="awz-chips"><button class="awz-chip ${all ? "on" : ""}" data-deleg="*">Todo el equipo</button>${others.map((x) => `<button class="awz-chip ${all || a.can_delegate_to.includes(x.id) ? "on" : ""}" data-deleg="${esc(x.id)}" ${all ? "disabled" : ""}>${esc(x.icon)} ${esc(x.name)}</button>`).join("")}</div>
        ${a.can_delegate_to.length && !a.tools.includes("*") && !a.tools.includes("delegate") ? `<div class="awz-err" style="margin-top:6px">Para delegar necesita la herramienta «Delegar en otros agentes»: se activará al guardar.</div>` : ""}
        <h4>ESTADO</h4><label class="row"><span class="switch"><input type="checkbox" data-en ${a.enabled ? "checked" : ""}><span></span></span>Activo (aparece en la oficina y recibe mensajes)</label>`;
    } else {
      m.innerHTML = `<h4>PROMPT DE SISTEMA ${W.promptTouched ? "· EDITADO A MANO" : "· GENERADO"}</h4>
        <textarea class="input mono" rows="16" data-prompt style="font-size:13px">${esc(a.system_prompt)}</textarea>
        <div class="row" style="margin-top:6px"><button class="btn sm" data-regen>↺ Regenerar con lo elegido</button><span class="muted" style="font-size:13px">{assistant_name} se cambia por el nombre de tu asistente.</span></div>
        <h4>RESUMEN</h4><div class="muted" style="font-size:14px">${esc(a.name)} (${esc(a.id || slug(a.name))}) · ${a.tools.includes("*") ? "todas las herramientas" : a.tools.length + " herramientas"} · palabras clave: ${esc(a.keywords.join(", ") || "—")}</div>`;
    }
    const d = document.getElementById("awz");
    d.querySelector("[data-prev]").style.visibility = s === 0 ? "hidden" : "visible";
    d.querySelector("[data-next]").textContent = s === STEPS.length - 1 ? (W.editing ? "GUARDAR CAMBIOS" : "CREAR AGENTE") : "SIGUIENTE";
    bind(m);
    sync();
  }

  function bind(m) {
    const a = W.a, p = W.p;
    document.querySelectorAll("#awz-steps [data-step]").forEach((b) => (b.onclick = () => go(+b.dataset.step)));
    m.querySelectorAll("[data-tpl]").forEach((b) => (b.onclick = () => { applyTemplate(TEMPLATES.find((t) => t.id === b.dataset.tpl)); W.step = 1; draw(); }));
    m.querySelectorAll("[data-clone]").forEach((b) => (b.onclick = () => {
      const src = (UI().agents() || []).find((x) => x.id === b.dataset.clone); if (!src) return;
      const c = JSON.parse(JSON.stringify(src)); c.id = ""; c.name = src.name + " (copia)";
      Object.assign(W.a, c); W.p = fromAgent(c); W.promptTouched = !src.perfil; W.step = 1; draw();
    }));
    m.querySelectorAll("[data-a]").forEach((el) => (el.oninput = () => {
      a[el.dataset.a] = el.value;
      const idEl = m.querySelector('[data-a="id"]');
      if (el.dataset.a === "name" && !W.editing && idEl && !idEl._touched) { a.id = slug(el.value); idEl.value = a.id; }
      if (el.dataset.a === "id") el._touched = true;
      if (el.dataset.a === "color") m.querySelectorAll("[data-color]").forEach((x) => x.classList.toggle("on", x.dataset.color === el.value));
      sync();
    }));
    m.querySelectorAll("[data-p]").forEach((el) => (el.oninput = () => {
      p[el.dataset.p] = el.type === "range" ? +el.value : el.value;
      const out = m.querySelector(`[data-out="${el.dataset.p}"]`);
      if (out) out.textContent = { tono: tonoFmt, creatividad: creaFmt, iniciativa: iniFmt }[el.dataset.p](p[el.dataset.p]);
      sync();
    }));
    m.querySelectorAll("[data-icon]").forEach((b) => (b.onclick = () => { a.icon = b.dataset.icon; m.querySelectorAll("[data-icon]").forEach((x) => x.classList.toggle("on", x === b)); sync(); }));
    m.querySelectorAll("[data-color]").forEach((b) => (b.onclick = () => { a.color = b.dataset.color; m.querySelectorAll("[data-color]").forEach((x) => x.classList.toggle("on", x === b)); const ci = m.querySelector('input[type=color]'); if (ci) ci.value = a.color; sync(); }));
    m.querySelectorAll("[data-sexo]").forEach((b) => (b.onclick = () => { a.sexo = b.dataset.sexo; draw(); }));
    m.querySelectorAll("[data-long]").forEach((b) => (b.onclick = () => { p.longitud = +b.dataset.long; draw(); }));
    m.querySelectorAll("[data-lang]").forEach((b) => (b.onclick = () => { p.idioma = b.dataset.lang; draw(); }));
    m.querySelectorAll("[data-trait]").forEach((b) => (b.onclick = () => {
      const t = b.dataset.trait, i = p.rasgos.indexOf(t);
      if (i >= 0) p.rasgos.splice(i, 1); else if (p.rasgos.length < 4) p.rasgos.push(t); else return showErr("Como mucho 4 rasgos: quita uno antes");
      showErr(""); draw();
    }));
    // chips: especialidad y palabras clave
    const getList = (id) => (id === "specialty" ? (a.specialty || "").split(",").map((x) => x.trim()).filter(Boolean) : a.keywords);
    const setList = (id, l) => { if (id === "specialty") a.specialty = l.join(", "); else a.keywords = l; };
    m.querySelectorAll("[data-chipin]").forEach((el) => (el.onkeydown = (e) => {
      if (e.key !== "Enter" && e.key !== ",") return;
      e.preventDefault();
      const v = el.value.trim().replace(/,$/, ""); if (!v) return;
      const l = getList(el.dataset.chipin); v.split(",").map((x) => x.trim()).filter(Boolean).forEach((x) => !l.includes(x) && l.push(x));
      setList(el.dataset.chipin, l); draw(); setTimeout(() => document.querySelector(`[data-chipin="${el.dataset.chipin}"]`)?.focus(), 0);
    }));
    m.querySelectorAll("[data-chips]").forEach((box) => box.querySelectorAll("[data-del]").forEach((b) => (b.onclick = () => {
      const l = getList(box.dataset.chips); l.splice(+b.dataset.del, 1); setList(box.dataset.chips, l); draw();
    })));
    m.querySelectorAll("[data-sug]").forEach((b) => (b.onclick = () => { const l = getList(b.dataset.sug); l.push(b.dataset.v); setList(b.dataset.sug, l); draw(); }));
    const mem = m.querySelector("[data-mem]"); if (mem) mem.onchange = () => { a.memory = mem.checked; sync(); };
    const en = m.querySelector("[data-en]"); if (en) en.onchange = () => { a.enabled = en.checked; sync(); };
    // herramientas
    m.querySelectorAll("[data-preset]").forEach((b) => (b.onclick = () => { p.preset = b.dataset.preset; a.tools = [...PRESETS[p.preset].tools]; draw(); }));
    m.querySelectorAll("[data-tool]").forEach((c) => (c.onchange = () => {
      const t = c.dataset.tool; a.tools = a.tools.filter((x) => x !== t && x !== "*"); if (c.checked) a.tools.push(t); p.preset = ""; draw();
    }));
    m.querySelectorAll("[data-grp]").forEach((b) => (b.onclick = () => {
      const g = (m._groups || GROUPS).find((x) => x.id === b.dataset.grp); const info = new Set(W.tools.map((t) => t.name));
      const avail = g.tools.filter((t) => info.has(t));
      if (a.tools.includes("*")) a.tools = W.tools.map((t) => t.name);
      const allOn = avail.every((t) => a.tools.includes(t));
      a.tools = allOn ? a.tools.filter((t) => !avail.includes(t)) : [...new Set([...a.tools, ...avail])];
      p.preset = ""; draw();
    }));
    // equipo
    m.querySelectorAll("[data-deleg]").forEach((b) => (b.onclick = () => {
      const v = b.dataset.deleg;
      if (v === "*") a.can_delegate_to = a.can_delegate_to.includes("*") ? [] : ["*"];
      else a.can_delegate_to = a.can_delegate_to.includes(v) ? a.can_delegate_to.filter((x) => x !== v) : [...a.can_delegate_to.filter((x) => x !== "*"), v];
      draw();
    }));
    const pr = m.querySelector("[data-prompt]"); if (pr) pr.oninput = () => { W.promptTouched = true; a.system_prompt = pr.value; side(); };
    const rg = m.querySelector("[data-regen]"); if (rg) rg.onclick = () => { W.promptTouched = false; draw(); };
  }

  async function save() {
    for (const st of [1]) { const e = validate(st); if (e) { W.step = st; draw(); return showErr(e); } }
    const a = W.a;
    if (!a.id) a.id = slug(a.name);
    if (a.can_delegate_to.length && !a.tools.includes("*") && !a.tools.includes("delegate")) a.tools.push("delegate");
    sync();
    const body = { id: a.id, name: a.name.trim(), description: a.description, system_prompt: a.system_prompt, model: a.model || "default",
      temperature: a.temperature, max_tokens: a.max_tokens, tools: a.tools, memory: a.memory, enabled: a.enabled, keywords: a.keywords,
      specialty: a.specialty || "", can_delegate_to: a.can_delegate_to, color: a.color, icon: a.icon || "◆", sexo: a.sexo || "", perfil: W.p };
    try {
      await UI().api("/api/agents", { method: "PUT", body });
      UI().toast(W.editing ? `Agente ${a.name} actualizado` : `Agente ${a.name} creado`);
      document.getElementById("awz").close();
      if (UI().reloadAgents) UI().reloadAgents();
    } catch (e) { showErr(e.message); }
  }

  window.AgentWizard = { open, buildPrompt, TEMPLATES };
})();
