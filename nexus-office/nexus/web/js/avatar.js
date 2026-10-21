/* Nexus-Office — avatar 3D construido a partir de su propio grafo de memoria y conocimiento.
 * Nucleo central = la propia IA; nodos = entidades aprendidas (color por tipo, tamano por peso);
 * aristas = relaciones. El nucleo late con la voz, los impulsos viajan por las aristas al pensar. */
(function () {
  const TYPE_COLORS = {
    self: "#00f0ff", persona: "#ff3df2", preferencia: "#ffb020", lugar: "#7cff6b", concepto: "#5ea8ff",
    organizacion: "#ff8a3d", rol: "#b28cff", objeto: "#3dffd2", nota: "#e6f7ff", documento: "#c8b6ff", agente: "#ffe14d",
  };
  const STATE = {
    idle: { label: "EN LÍNEA", color: "#00f0ff", spin: 0.0016, pulse: 0.15 },
    thinking: { label: "PENSANDO", color: "#a66bff", spin: 0.006, pulse: 0.9 },
    tool: { label: "USANDO HERRAMIENTA", color: "#ffb020", spin: 0.004, pulse: 0.7 },
    speaking: { label: "HABLANDO", color: "#ff3df2", spin: 0.003, pulse: 0.4 },
    listening: { label: "ESCUCHANDO", color: "#7cff6b", spin: 0.002, pulse: 0.3 },
  };
  const hexRgb = (h) => { const n = parseInt(h.slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; };
  const rgba = (h, a) => { const [r, g, b] = hexRgb(h); return `rgba(${r},${g},${b},${a})`; };

  class NeuralAvatar {
    constructor(canvas, opts = {}) {
      this.c = canvas; this.ctx = canvas.getContext("2d");
      this.opts = Object.assign({ explore: false, maxLabels: 10, radius: 0.36 }, opts);
      this.nodes = []; this.links = []; this.byId = new Map();
      this.yaw = 0; this.pitch = -0.25; this.zoom = 1; this.state = "idle"; this.level = 0; this.levelS = 0;
      this.pulses = []; this.t = 0; this.hover = null; this.drag = null; this.accent = "#00f0ff";
      this.tip = document.createElement("div"); this.tip.className = "avatar-tip hidden";
      canvas.parentElement.appendChild(this.tip);
      this._bind(); this._resize();
      new ResizeObserver(() => this._resize()).observe(canvas.parentElement);
      this._loop = this._loop.bind(this); requestAnimationFrame(this._loop);
    }
    _resize() {
      const r = this.c.parentElement.getBoundingClientRect(), d = Math.min(window.devicePixelRatio || 1, 2);
      this.w = r.width; this.h = r.height; this.c.width = r.width * d; this.c.height = r.height * d;
      this.ctx.setTransform(d, 0, 0, d, 0, 0);
    }
    _bind() {
      const c = this.c;
      c.addEventListener("pointerdown", (e) => { this.drag = { x: e.clientX, y: e.clientY, yaw: this.yaw, pitch: this.pitch }; c.setPointerCapture(e.pointerId); });
      c.addEventListener("pointerup", () => (this.drag = null));
      c.addEventListener("pointermove", (e) => {
        const r = c.getBoundingClientRect(); this.mouse = { x: e.clientX - r.left, y: e.clientY - r.top };
        if (this.drag) { this.yaw = this.drag.yaw + (e.clientX - this.drag.x) * 0.008; this.pitch = Math.max(-1.3, Math.min(1.3, this.drag.pitch + (e.clientY - this.drag.y) * 0.006)); }
      });
      c.addEventListener("pointerleave", () => { this.mouse = null; });
      if (this.opts.explore) c.addEventListener("wheel", (e) => { e.preventDefault(); this.zoom = Math.max(0.5, Math.min(2.6, this.zoom * (e.deltaY > 0 ? 0.92 : 1.08))); }, { passive: false });
    }
    setAccent(c) { this.accent = c; STATE.idle.color = c; }
    setState(s) { this.state = STATE[s] ? s : "idle"; if (s === "thinking" || s === "tool") this._burst(6); }
    setLevel(v) { this.level = Math.max(0, Math.min(1, v)); }

    update(graph) {
      const old = this.byId; const nodes = []; const byId = new Map();
      const list = graph.nodes || [];
      const maxW = Math.max(1, ...list.map((n) => n.weight));
      const golden = Math.PI * (3 - Math.sqrt(5));
      // subgrafos de agente (vista completa): cada uno es un racimo propio alrededor de su ancla
      const anchors = list.filter((n) => n.type === "agente");
      const centers = new Map();
      anchors.forEach((a, i) => {
        const y = anchors.length > 1 ? 0.55 - (i / (anchors.length - 1)) * 1.1 : 0, rad = Math.sqrt(1 - y * y), th = golden * i * 2.4;
        centers.set(a.scope, { x: Math.cos(th) * rad * 0.8, y: y * 0.8, z: Math.sin(th) * rad * 0.8 });
      });
      const clustered = list.filter((n) => n.type !== "self" && n.scope && centers.has(n.scope));
      const others = list.filter((n) => n.type !== "self" && !clustered.includes(n));
      const N = Math.max(others.length, 1);
      others.sort((a, b) => b.weight - a.weight);
      for (const [scope, c] of centers) {
        const group = clustered.filter((n) => n.scope === scope && n.type !== "agente").sort((a, b) => b.weight - a.weight);
        const anchor = clustered.find((n) => n.scope === scope && n.type === "agente"), M = Math.max(group.length, 1);
        const place = (n, x, y, z) => { const prev = old.get(n.id); const node = { ...n, x, y, z, size: 2 + 6 * Math.sqrt(n.weight / maxW),
          born: prev ? prev.born : this.t, fresh: !prev && old.size > 0 }; nodes.push(node); byId.set(n.id, node); };
        if (anchor) place(anchor, c.x, c.y, c.z);
        group.forEach((n, i) => {
          const y = 1 - (i / Math.max(M - 1, 1)) * 2, rad = Math.sqrt(1 - y * y), th = golden * i;
          const r = 0.1 + 0.12 * (1 - Math.sqrt(n.weight / maxW)) + Math.min(0.08, M * 0.003);
          place(n, c.x + Math.cos(th) * rad * r, c.y + y * r, c.z + Math.sin(th) * rad * r);
        });
      }
      others.forEach((n, i) => {
        // esfera de Fibonacci: los nodos mas importantes, mas cerca del nucleo
        const y = 1 - (i / Math.max(N - 1, 1)) * 2, rad = Math.sqrt(1 - y * y), th = golden * i;
        const shell = (0.55 + 0.45 * (1 - Math.sqrt(n.weight / maxW)) + (i % 3) * 0.04) * (centers.size ? 0.62 : 1);   // con racimos: el general por dentro
        const prev = old.get(n.id);
        const node = { ...n, x: Math.cos(th) * rad * shell, y: y * shell, z: Math.sin(th) * rad * shell,
          size: 2 + 6 * Math.sqrt(n.weight / maxW), born: prev ? prev.born : this.t, fresh: !prev && old.size > 0 };
        nodes.push(node); byId.set(n.id, node);
      });
      const self = list.find((n) => n.type === "self");
      if (self) { const node = { ...self, x: 0, y: 0, z: 0, size: 0, born: 0, core: true }; nodes.push(node); byId.set(self.id, node); }
      this.nodes = nodes; this.byId = byId;
      this.links = (graph.links || []).filter((l) => byId.has(l.source) && byId.has(l.target));
      if (nodes.some((n) => n.fresh)) this._burst(12);
    }
    _burst(n) {
      for (let i = 0; i < n && this.links.length; i++) {
        const l = this.links[(Math.random() * this.links.length) | 0];
        this.pulses.push({ l, p: 0, v: 0.008 + Math.random() * 0.02, back: Math.random() < 0.5 });
      }
    }
    _project(n, cy, sy, cp, sp, R, cx0, cy0) {
      const x = n.x * cy - n.z * sy, z1 = n.x * sy + n.z * cy;
      const y = n.y * cp - z1 * sp, z = n.y * sp + z1 * cp;
      const persp = 2.6 / (2.6 + z);
      return { sx: cx0 + x * R * persp, sy: cy0 + y * R * persp, z, s: persp };
    }
    _loop() {
      requestAnimationFrame(this._loop);
      if (document.hidden || !this.w || this.paused) return;
      const ctx = this.ctx, st = STATE[this.state]; this.t += 1;
      this.levelS += (this.level - this.levelS) * 0.25;
      if (!this.drag) this.yaw += st.spin;
      if (Math.random() < st.pulse * 0.12) this._burst(1);
      ctx.clearRect(0, 0, this.w, this.h);
      const cx0 = this.w / 2, cy0 = this.h / 2.1, R = Math.min(this.w, this.h) * this.opts.radius * this.zoom;
      const cy = Math.cos(this.yaw), sy = Math.sin(this.yaw), cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
      const P = new Map();
      for (const n of this.nodes) P.set(n.id, this._project(n, cy, sy, cp, sp, R, cx0, cy0));

      // halo del nucleo
      const coreR = R * (0.13 + 0.07 * this.levelS + 0.012 * Math.sin(this.t * 0.05));
      const g = ctx.createRadialGradient(cx0, cy0, 0, cx0, cy0, coreR * 4);
      g.addColorStop(0, rgba(st.color, 0.35)); g.addColorStop(1, rgba(st.color, 0));
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(cx0, cy0, coreR * 4, 0, 7); ctx.fill();

      // aristas
      ctx.lineWidth = 1;
      for (const l of this.links) {
        const a = P.get(l.source), b = P.get(l.target); if (!a || !b) continue;
        const depth = (2 - (a.z + b.z) / 2) / 3, w = Math.min(1, 0.15 + l.weight * 0.08);
        ctx.strokeStyle = rgba(this.accent, 0.05 + 0.22 * depth * w);
        ctx.beginPath(); ctx.moveTo(a.sx, a.sy); ctx.lineTo(b.sx, b.sy); ctx.stroke();
      }
      // impulsos
      this.pulses = this.pulses.filter((p) => p.p <= 1);
      for (const p of this.pulses) {
        p.p += p.v * (this.state === "idle" ? 1 : 2.2);
        const a = P.get(p.back ? p.l.target : p.l.source), b = P.get(p.back ? p.l.source : p.l.target); if (!a || !b) continue;
        const x = a.sx + (b.sx - a.sx) * p.p, y = a.sy + (b.sy - a.sy) * p.p;
        ctx.fillStyle = rgba(st.color, 0.9); ctx.shadowColor = st.color; ctx.shadowBlur = 10;
        ctx.beginPath(); ctx.arc(x, y, 1.8, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      }
      // nodos (de atras hacia delante)
      const order = this.nodes.filter((n) => !n.core).sort((a, b) => P.get(b.id).z - P.get(a.id).z);
      let hover = null;
      for (const n of order) {
        const p = P.get(n.id), col = n.color || TYPE_COLORS[n.type] || TYPE_COLORS.concepto;
        const age = this.t - n.born, grow = n.fresh ? Math.min(1, age / 40) : 1;
        const r = n.size * p.s * grow * (this.opts.explore ? 1.3 : 1);
        const alpha = 0.35 + 0.65 * ((1 - p.z) / 2 + 0.25);
        ctx.fillStyle = rgba(col, Math.min(1, alpha)); ctx.shadowColor = col; ctx.shadowBlur = n.fresh && age < 120 ? 25 : 8;
        ctx.beginPath(); ctx.arc(p.sx, p.sy, Math.max(0.8, r), 0, 7); ctx.fill(); ctx.shadowBlur = 0;
        if (this.mouse && Math.hypot(this.mouse.x - p.sx, this.mouse.y - p.sy) < Math.max(6, r + 3)) hover = { n, p };
      }
      // anillos y nucleo
      ctx.save(); ctx.translate(cx0, cy0);
      for (let i = 0; i < 3; i++) {
        ctx.rotate(this.t * 0.004 * (i + 1) * (i % 2 ? -1 : 1));
        ctx.strokeStyle = rgba(st.color, 0.35 - i * 0.08); ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.arc(0, 0, coreR * (1.5 + i * 0.45), i, i + 1.6 + this.levelS * 2); ctx.stroke();
      }
      ctx.restore();
      const cg = ctx.createRadialGradient(cx0 - coreR * 0.3, cy0 - coreR * 0.3, coreR * 0.1, cx0, cy0, coreR);
      cg.addColorStop(0, "#ffffff"); cg.addColorStop(0.35, st.color); cg.addColorStop(1, rgba(st.color, 0.05));
      ctx.fillStyle = cg; ctx.shadowColor = st.color; ctx.shadowBlur = 30 + 50 * this.levelS;
      ctx.beginPath(); ctx.arc(cx0, cy0, coreR, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      // forma de onda al hablar / escuchar
      if (this.levelS > 0.02) {
        ctx.strokeStyle = rgba(st.color, 0.8); ctx.lineWidth = 2; ctx.beginPath();
        for (let a = 0; a <= 64; a++) {
          const ang = (a / 64) * Math.PI * 2, rr = coreR * 1.15 + Math.sin(ang * 6 + this.t * 0.3) * this.levelS * coreR * 0.5 * Math.random();
          const x = cx0 + Math.cos(ang) * rr, y = cy0 + Math.sin(ang) * rr; a ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
        }
        ctx.stroke();
      }
      // etiquetas
      ctx.font = `600 ${this.opts.explore ? 12 : 10.5}px "JetBrains Mono", monospace`; ctx.textAlign = "center";
      const labelled = order.filter((n) => P.get(n.id).z < 0.2).sort((a, b) => b.weight - a.weight).slice(0, this.opts.maxLabels);
      for (const n of labelled) {
        const p = P.get(n.id);
        ctx.fillStyle = rgba("#d8f6ff", 0.25 + 0.6 * (1 - (p.z + 1) / 2));
        ctx.fillText(n.name.slice(0, 22), p.sx, p.sy - n.size * p.s - 6);
      }
      if (hover !== this.hover) {
        this.hover = hover;
        if (hover) { this.tip.textContent = `${hover.n.name} · ${hover.n.type} · peso ${hover.n.weight.toFixed(1)}`; this.tip.classList.remove("hidden"); }
        else this.tip.classList.add("hidden");
      }
      if (hover) { this.tip.style.left = hover.p.sx + "px"; this.tip.style.top = hover.p.sy + "px"; }
    }
  }

  /* fondo de estrellas */
  function starfield(canvas) {
    const ctx = canvas.getContext("2d"); let stars = [], w, h;
    const resize = () => { w = canvas.width = innerWidth; h = canvas.height = innerHeight;
      stars = Array.from({ length: Math.min(260, (w * h) / 6000) }, () => ({ x: Math.random() * w, y: Math.random() * h, z: Math.random(), tw: Math.random() * 6 })); };
    resize(); addEventListener("resize", resize);
    const low = window.NexusDevice && NexusDevice.lowPower; let f = 0;
    (function loop() {
      requestAnimationFrame(loop); if (document.hidden || !canvas.offsetWidth) return;   // oculto: no dibuja
      if (low && f++ % 2) return;                                                     // equipos modestos: 30 fps
      ctx.clearRect(0, 0, w, h);
      for (const s of stars) {
        s.tw += 0.02; s.y += 0.05 + s.z * 0.15; if (s.y > h) { s.y = 0; s.x = Math.random() * w; }
        ctx.fillStyle = `rgba(160,230,255,${0.15 + 0.5 * s.z * (0.6 + 0.4 * Math.sin(s.tw))})`;
        ctx.fillRect(s.x, s.y, 1 + s.z, 1 + s.z);
      }
    })();
  }

  window.NeuralAvatar = NeuralAvatar; window.starfield = starfield; window.NEXUS_TYPE_COLORS = TYPE_COLORS;
})();
