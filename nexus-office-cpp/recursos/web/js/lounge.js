/* Nexus-Office — Salon virtual con estetica «Los Sims»: sala isometrica con suelo de madera, mesa de reuniones y
 * personajes con su rombo verde encima. Llegan andando desde la puerta, respiran, parpadean, miran a quien
 * habla, asienten, gesticulan, teclean y beben cafe. Todo se anima por tiempo real con amortiguacion
 * (sin saltos de un estado a otro), asi que los movimientos son suaves aunque baje la tasa de fotogramas. */
(function () {
  const VW = 760, VH = 470;                                             // escena de diseño; se escala para encajar
  const TABLE = { x: 380, y: 300, rx: 228, ry: 68, th: 13, h: 40 };    // tablero proyectado y su altura sobre el suelo
  const ROOM = { T: { x: 380, y: 18 }, L: { x: -190, y: 303 }, R: { x: 950, y: 303 }, B: { x: 380, y: 588 }, wallH: 215 };
  const WALL_LEN = Math.hypot(ROOM.T.x - ROOM.L.x, ROOM.T.y - ROOM.L.y);
  const DOOR = { x: 205, y: 140 };
  const USER_SEAT = { x: 380, y: 486 };
  const FONT = '"Nunito", "Segoe UI", system-ui, -apple-system, sans-serif';
  const SKIN = ["#f6d3b5", "#eebe96", "#dba57c", "#c08458", "#93603f", "#f3c9a6", "#704a30"];
  const HAIR = ["#2a1c14", "#5b3620", "#9a5b2a", "#d8b060", "#1d1d24", "#8c2f1e", "#bdb7af", "#4a3466", "#d9773a"];
  const PANTS = ["#3b4a6b", "#2f3542", "#5c4a3a", "#44544a", "#5d4f6e", "#283a52"];
  const GREEN = "#3fd65a", AMBER = "#ffb020";
  // peinados (ver _drawHead): 0 corto · 1 media melena · 2 mono/flequillo · 3 de punta · 4 melena larga · 5 rizado
  const STYLES = { mujer: [1, 4, 4, 5, 2, 1], hombre: [0, 3, 0, 2, 5, 3] };
  function sexOf(a) {
    const sx = String(a.sexo || "").toLowerCase();
    if (sx === "mujer" || sx === "hombre") return sx;
    return hash(a.id + "#sexo") % 2 ? "mujer" : "hombre";   // sin elegir: estable para cada id
  }

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const lerp = (a, b, t) => a + (b - a) * t;
  const lerpP = (p, q, t) => ({ x: lerp(p.x, q.x, t), y: lerp(p.y, q.y, t) });
  const smooth = (t) => t * t * (3 - 2 * t);
  const damp = (cur, target, k, dt) => cur + (target - cur) * (1 - Math.exp(-k * dt));
  const wob = (t, s) => Math.sin(t * 1.31 + s) * 0.5 + Math.sin(t * 2.17 + s * 1.7) * 0.3 + Math.sin(t * 0.53 + s * 2.9) * 0.2;
  const backOut = (t) => { const c = 1.7; t -= 1; return 1 + t * t * ((c + 1) * t + c); };
  function hexRgb(h) { const m = /^#?([0-9a-f]{6})$/i.exec(h || ""); const n = m ? parseInt(m[1], 16) : 0x00f0ff; return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }
  const toHex = (c) => "#" + c.map((v) => clamp(Math.round(v), 0, 255).toString(16).padStart(2, "0")).join("");
  const rgba = (h, a) => { const [r, g, b] = hexRgb(h); return `rgba(${r},${g},${b},${a})`; };
  const mix = (a, b, t) => { const x = hexRgb(a), y = hexRgb(b); return toHex(x.map((v, i) => lerp(v, y[i], t))); };
  const tone = (h, f) => mix(h, f >= 0 ? "#ffffff" : "#000000", Math.abs(f));
  function hash(s) { let h = 2166136261; for (const ch of String(s)) { h ^= ch.codePointAt(0); h = Math.imul(h, 16777619); } return h >>> 0; }
  function rng(seed) {
    return () => { seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
  }
  const pick = (r, arr) => arr[Math.floor(r() * arr.length) % arr.length];

  function wrap(ctx, text, maxW, maxLines) {
    const words = text.split(/\s+/), lines = []; let cur = "";
    for (const w of words) {
      const t = cur ? cur + " " + w : w;
      if (ctx.measureText(t).width > maxW && cur) { lines.push(cur); cur = w; if (lines.length === maxLines) break; } else cur = t;
    }
    if (lines.length < maxLines && cur) lines.push(cur);
    if (lines.length === maxLines && words.join(" ").length > lines.join(" ").length) lines[maxLines - 1] = lines[maxLines - 1].replace(/.{0,3}$/, "…");
    return lines;
  }

  /* ---------------------------------------------------------------- sala (se pinta una vez por tamaño) */
  // dibuja en coordenadas de pared: x a lo largo de la pared, y negativa hacia arriba
  function onWall(ctx, side, fn) {
    const o = side === "L" ? ROOM.L : ROOM.T, e = side === "L" ? ROOM.T : ROOM.R;
    ctx.save(); ctx.transform((e.x - o.x) / WALL_LEN, (e.y - o.y) / WALL_LEN, 0, 1, o.x, o.y); fn(ctx); ctx.restore();
  }
  function plant(ctx, x, y, s, seed) {
    const r = rng(seed);
    ctx.fillStyle = "rgba(0,0,0,.18)"; ctx.beginPath(); ctx.ellipse(x, y, 16 * s, 6 * s, 0, 0, 7); ctx.fill();
    for (let i = 0; i < 13; i++) {
      const a = -Math.PI / 2 + (r() - 0.5) * 2.4, len = (26 + r() * 30) * s;
      ctx.save(); ctx.translate(x + (r() - 0.5) * 8 * s, y - 26 * s); ctx.rotate(a + Math.PI / 2);
      ctx.fillStyle = mix("#3f8f4e", "#7cc46a", r()); ctx.beginPath(); ctx.ellipse(0, -len / 2, 6 * s, len / 2, 0, 0, 7); ctx.fill();
      ctx.strokeStyle = "rgba(255,255,255,.18)"; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(0, -2); ctx.lineTo(0, -len + 3); ctx.stroke();
      ctx.restore();
    }
    const g = ctx.createLinearGradient(x - 13 * s, 0, x + 13 * s, 0); g.addColorStop(0, "#d9805a"); g.addColorStop(1, "#a9573a");
    ctx.fillStyle = g; ctx.beginPath(); ctx.moveTo(x - 13 * s, y - 28 * s); ctx.lineTo(x + 13 * s, y - 28 * s); ctx.lineTo(x + 10 * s, y); ctx.lineTo(x - 10 * s, y); ctx.closePath(); ctx.fill();
    ctx.fillStyle = "#c2694a"; ctx.fillRect(x - 14 * s, y - 31 * s, 28 * s, 5 * s);
  }
  function cooler(ctx, x, y) {
    ctx.fillStyle = "rgba(0,0,0,.2)"; ctx.beginPath(); ctx.ellipse(x, y, 18, 7, 0, 0, 7); ctx.fill();
    const g = ctx.createLinearGradient(x - 14, 0, x + 14, 0); g.addColorStop(0, "#f4f6f8"); g.addColorStop(1, "#c3cad3");
    ctx.fillStyle = g; ctx.beginPath(); ctx.roundRect(x - 14, y - 52, 28, 52, 4); ctx.fill();
    ctx.fillStyle = "#8a96a5"; ctx.fillRect(x - 6, y - 40, 12, 6);
    ctx.fillStyle = "#e14b4b"; ctx.fillRect(x - 5, y - 38, 3, 3); ctx.fillStyle = "#3d8be0"; ctx.fillRect(x + 2, y - 38, 3, 3);
    const b = ctx.createLinearGradient(x - 12, 0, x + 12, 0); b.addColorStop(0, "rgba(140,205,255,.95)"); b.addColorStop(1, "rgba(70,140,220,.95)");
    ctx.fillStyle = b; ctx.beginPath(); ctx.roundRect(x - 12, y - 84, 24, 32, 8); ctx.fill();
    ctx.fillStyle = "rgba(255,255,255,.45)"; ctx.fillRect(x - 7, y - 80, 3, 22);
  }
  function paintRoom(ctx) {
    const { T, L, R, B, wallH: H } = ROOM;
    for (const side of ["L", "R"]) onWall(ctx, side, (c) => {
      const dk = side === "R" ? 0.08 : 0;
      c.fillStyle = tone("#f1e2c3", -dk); c.fillRect(0, -H, WALL_LEN, H);
      c.fillStyle = tone("#e6d1a8", -dk); for (let x = 8; x < WALL_LEN; x += 26) c.fillRect(x, -H, 9, H - 64);
      c.fillStyle = tone("#7fa797", -dk); c.fillRect(0, -64, WALL_LEN, 64);
      c.strokeStyle = "rgba(0,0,0,.09)"; c.lineWidth = 1.2; for (let x = 12; x < WALL_LEN - 30; x += 44) c.strokeRect(x, -54, 34, 38);
      c.fillStyle = tone("#fff7e6", -dk); c.fillRect(0, -68, WALL_LEN, 6);
      c.fillStyle = tone("#7a4e2e", -dk); c.fillRect(0, -9, WALL_LEN, 9);
      const g = c.createLinearGradient(0, -H, 0, -H + 90); g.addColorStop(0, "rgba(0,0,0,.2)"); g.addColorStop(1, "rgba(0,0,0,0)");
      c.fillStyle = g; c.fillRect(0, -H, WALL_LEN, 90);
    });
    ctx.strokeStyle = "rgba(0,0,0,.16)"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(T.x, T.y); ctx.lineTo(T.x, T.y - H); ctx.stroke();
    // puerta y cuadro (pared izquierda)
    onWall(ctx, "L", (c) => {
      const x0 = 392, w = 60, h = 104;
      c.fillStyle = "#faf4e8"; c.fillRect(x0 - 6, -h - 7, w + 12, h + 7);
      const d = c.createLinearGradient(x0, 0, x0 + w, 0); d.addColorStop(0, "#a8754c"); d.addColorStop(1, "#8a5b37");
      c.fillStyle = d; c.fillRect(x0, -h, w, h);
      c.strokeStyle = "rgba(0,0,0,.18)"; c.strokeRect(x0 + 8, -h + 9, w - 16, 36); c.strokeRect(x0 + 8, -h + 53, w - 16, 40);
      c.fillStyle = "#f0cc5c"; c.beginPath(); c.arc(x0 + w - 9, -50, 3, 0, 7); c.fill();
      const px = 222, pw = 92;
      c.fillStyle = "#5b3a22"; c.fillRect(px - 6, -166, pw + 12, 76);
      const sky = c.createLinearGradient(0, -160, 0, -96); sky.addColorStop(0, "#ffb37a"); sky.addColorStop(1, "#ffe6b0");
      c.fillStyle = sky; c.fillRect(px, -160, pw, 64);
      c.fillStyle = "#f46f4f"; c.beginPath(); c.arc(px + 62, -132, 10, 0, 7); c.fill();
      c.fillStyle = "#5d9a6e"; c.beginPath(); c.moveTo(px, -96); c.quadraticCurveTo(px + 30, -130, px + 55, -104);
      c.quadraticCurveTo(px + 75, -122, px + pw, -104); c.lineTo(px + pw, -96); c.closePath(); c.fill();
    });
    // ventana con cortinas (pared derecha)
    onWall(ctx, "R", (c) => {
      const x0 = 300, w = 100, y0 = -146, h = 82;
      c.fillStyle = "#fbf7ee"; c.fillRect(x0 - 7, y0 - 7, w + 14, h + 14);
      const g = c.createLinearGradient(0, y0, 0, y0 + h); g.addColorStop(0, "#74c0f4"); g.addColorStop(1, "#dcf1ff");
      c.fillStyle = g; c.fillRect(x0, y0, w, h);
      c.fillStyle = "rgba(255,255,255,.9)";
      for (const [cx, cy, r] of [[x0 + 22, y0 + 20, 8], [x0 + 32, y0 + 17, 10], [x0 + 43, y0 + 21, 7], [x0 + 72, y0 + 38, 6], [x0 + 80, y0 + 35, 8]]) { c.beginPath(); c.arc(cx, cy, r, 0, 7); c.fill(); }
      c.fillStyle = "#79b96d"; c.beginPath(); c.moveTo(x0, y0 + h); c.quadraticCurveTo(x0 + 30, y0 + h - 26, x0 + 60, y0 + h - 10); c.quadraticCurveTo(x0 + 82, y0 + h - 24, x0 + w, y0 + h - 14); c.lineTo(x0 + w, y0 + h); c.closePath(); c.fill();
      c.fillStyle = "#fbf7ee"; c.fillRect(x0 + w / 2 - 2, y0, 4, h); c.fillRect(x0, y0 + h / 2 - 2, w, 4);
      c.fillStyle = "#e9e1d0"; c.fillRect(x0 - 12, y0 + h + 7, w + 24, 5);
      c.fillStyle = "#d65f55";
      for (const sx of [x0 - 16, x0 + w + 4]) {
        c.beginPath(); c.moveTo(sx, y0 - 12); c.lineTo(sx + 12, y0 - 12); c.quadraticCurveTo(sx + 6, y0 + h / 2, sx + 13, y0 + h + 14); c.lineTo(sx - 2, y0 + h + 14); c.closePath(); c.fill();
      }
      c.fillStyle = "#8b5a3a"; c.fillRect(x0 - 22, y0 - 15, w + 44, 4);
    });
    // suelo de tablas
    ctx.save();
    ctx.beginPath(); ctx.moveTo(T.x, T.y); ctx.lineTo(R.x, R.y); ctx.lineTo(B.x, B.y); ctx.lineTo(L.x, L.y); ctx.closePath(); ctx.clip();
    ctx.fillStyle = "#c48d5a"; ctx.fillRect(L.x, T.y, R.x - L.x, B.y - T.y);
    const r = rng(7), N = 34, ox = R.x - T.x, oy = R.y - T.y;
    for (let k = 0; k < N; k++) {
      const a0 = k / N, a1 = (k + 1) / N;
      ctx.fillStyle = r() < 0.5 ? `rgba(255,235,200,${r() * 0.09})` : `rgba(60,30,10,${r() * 0.09})`;
      ctx.beginPath(); ctx.moveTo(L.x + a0 * ox, L.y + a0 * oy); ctx.lineTo(T.x + a0 * ox, T.y + a0 * oy); ctx.lineTo(T.x + a1 * ox, T.y + a1 * oy); ctx.lineTo(L.x + a1 * ox, L.y + a1 * oy); ctx.closePath(); ctx.fill();
      ctx.strokeStyle = "rgba(80,40,15,.32)"; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(L.x + a0 * ox, L.y + a0 * oy); ctx.lineTo(T.x + a0 * ox, T.y + a0 * oy); ctx.stroke();
      for (let j = 0; j < 3; j++) {
        const s = (j + r()) / 3, px = L.x + a0 * ox + s * (T.x - L.x), py = L.y + a0 * oy + s * (T.y - L.y);
        ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(px + ox / N, py + oy / N); ctx.stroke();
      }
    }
    // luz de la ventana sobre el suelo
    const ur = { x: (R.x - T.x) / WALL_LEN, y: (R.y - T.y) / WALL_LEN }, inw = { x: -ur.x, y: ur.y };
    const w0 = { x: T.x + ur.x * 300, y: T.y + ur.y * 300 }, w1 = { x: T.x + ur.x * 400, y: T.y + ur.y * 400 };
    ctx.fillStyle = "rgba(255,244,200,.13)"; ctx.beginPath(); ctx.moveTo(w0.x, w0.y); ctx.lineTo(w1.x, w1.y);
    ctx.lineTo(w1.x + inw.x * 190, w1.y + inw.y * 190); ctx.lineTo(w0.x + inw.x * 150, w0.y + inw.y * 150); ctx.closePath(); ctx.fill();
    // sombra de contacto con las paredes
    ctx.shadowColor = "rgba(0,0,0,.45)"; ctx.shadowBlur = 28; ctx.strokeStyle = "rgba(0,0,0,.28)"; ctx.lineWidth = 8;
    ctx.beginPath(); ctx.moveTo(L.x, L.y); ctx.lineTo(T.x, T.y); ctx.lineTo(R.x, R.y); ctx.stroke(); ctx.shadowBlur = 0;
    ctx.restore();
    // alfombra bajo la mesa
    const fy = TABLE.y + TABLE.h;
    ctx.fillStyle = "#2f5f7e"; ctx.beginPath(); ctx.ellipse(TABLE.x, fy + 4, 318, 122, 0, 0, 7); ctx.fill();
    ctx.strokeStyle = "#ecd9a5"; ctx.lineWidth = 5; ctx.beginPath(); ctx.ellipse(TABLE.x, fy + 4, 302, 113, 0, 0, 7); ctx.stroke();
    ctx.strokeStyle = "rgba(236,217,165,.55)"; ctx.lineWidth = 2; ctx.setLineDash([3, 7]); ctx.beginPath(); ctx.ellipse(TABLE.x, fy + 4, 284, 104, 0, 0, 7); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = "#3c7397"; ctx.beginPath(); ctx.ellipse(TABLE.x, fy + 4, 250, 88, 0, 0, 7); ctx.fill();
    // sombra de la mesa
    ctx.save(); ctx.translate(TABLE.x, fy + 6); ctx.scale(1, 0.3);
    const sg = ctx.createRadialGradient(0, 0, 40, 0, 0, 240); sg.addColorStop(0, "rgba(0,0,0,.32)"); sg.addColorStop(1, "rgba(0,0,0,0)");
    ctx.fillStyle = sg; ctx.beginPath(); ctx.arc(0, 0, 240, 0, 7); ctx.fill(); ctx.restore();
    // mobiliario estatico
    plant(ctx, 382, 82, 1.15, 3); cooler(ctx, 690, 212); plant(ctx, 36, 432, 1.5, 11);
  }

  /* ---------------------------------------------------------------- personajes */
  function headTop(ch) { return ch.y + (ch.P.headY - 12) * ch.s; }

  function drawPlumbob(ctx, x, y, s, rot, color, glow) {
    const r = 5.6 * s, hh = 11 * s, eq = [0, 1, 2, 3].map((i) => { const a = rot + i * Math.PI / 2; return { x: Math.cos(a) * r, y: Math.sin(a) * r * 0.3, z: Math.sin(a), a }; });
    ctx.save(); ctx.translate(x, y);
    if (glow > 0) { ctx.shadowColor = color; ctx.shadowBlur = 8 + glow * 14; }
    for (let i = 0; i < 4; i++) {
      const p = eq[i], q = eq[(i + 1) % 4], z = (p.z + q.z) / 2; if (z < -0.05) continue;
      const lit = 0.5 + 0.5 * Math.cos(p.a + Math.PI / 4 - 2.2);
      for (const top of [true, false]) {
        ctx.fillStyle = tone(color, top ? -0.05 + lit * 0.35 : -0.35 + lit * 0.25);
        ctx.beginPath(); ctx.moveTo(0, top ? -hh : hh); ctx.lineTo(p.x, p.y); ctx.lineTo(q.x, q.y); ctx.closePath(); ctx.fill();
      }
    }
    ctx.restore();
  }

  class LoungeRoom {
    constructor(canvas) {
      this.c = canvas; this.ctx = canvas.getContext("2d");
      this.agents = []; this.chars = new Map(); this.bubbles = new Map(); this.planes = []; this.pops = [];
      this.t = 0; this.last = 0; this.meeting = 0; this.meetAmt = 0; this.speaker = null;
      this.user = this._makeChar({ id: "usuario", name: "Tú", color: "#5b7cc9", icon: "◉" });
      Object.assign(this.user, { isUser: true, x: USER_SEAT.x, y: USER_SEAT.y, seat: USER_SEAT, sit: 1, hidden: false, s: 1.32 });
      new ResizeObserver(() => this._resize()).observe(canvas.parentElement); this._resize();
      this._loop = this._loop.bind(this); requestAnimationFrame(this._loop);
    }
    _resize() {
      const r = this.c.parentElement.getBoundingClientRect(), d = Math.min(devicePixelRatio || 1, 2);
      this.w = r.width; this.h = r.height; this.d = d; this.c.width = r.width * d; this.c.height = r.height * d;
      this.sc = Math.min(this.w / 560, this.h / VH); this.ox = (this.w - VW * this.sc) / 2; this.oy = (this.h - VH * this.sc) / 2;
      // en pantallas estrechas se recortan las paredes, no la mesa
      if (!this.w || !this.h) return;
      // fondo cacheado: la sala completa no cambia entre fotogramas
      const bg = this.bg || (this.bg = document.createElement("canvas")); bg.width = this.c.width; bg.height = this.c.height;
      const b = bg.getContext("2d");
      const g = b.createLinearGradient(0, 0, 0, bg.height); g.addColorStop(0, "#2a3344"); g.addColorStop(1, "#141a24");
      b.fillStyle = g; b.fillRect(0, 0, bg.width, bg.height);
      b.setTransform(d * this.sc, 0, 0, d * this.sc, d * this.ox, d * this.oy); paintRoom(b);
    }
    _makeChar(a) {
      const r = rng(hash(a.id)), sexo = sexOf(a);
      const ch = { a, sexo, seed: r() * 100, skin: pick(r, SKIN), hair: pick(r, HAIR), style: Math.floor(r() * 6), pants: pick(r, PANTS), laptop: r() < 0.5,
               x: DOOR.x, y: DOOR.y, seat: null, path: null, hidden: true, walking: false, sit: 0, walkPh: 0, s: 1.08,
               look: 0, talk: 0, gest: 0, sip: 0, type: 0, nod: 0, alert: 0, act: null, actEnd: 0, actLook: 0, nextAct: 3 + r() * 6,
               blinkAt: -1, nextBlink: 1 + r() * 3, P: null };
      ch.style = pick(rng(hash(a.id) ^ 0x2f9), STYLES[sexo]);   // peinado segun el sexo elegido
      return ch;
    }
    /* sexo de un personaje (agente o usuario): si cambia, se rehace su aspecto */
    _resex(ch) {
      const sx = sexOf(ch.a);
      if (sx === ch.sexo) return;
      const fresh = this._makeChar(ch.a);
      Object.assign(ch, { sexo: fresh.sexo, style: fresh.style, L: null });
    }
    setUser(u) {
      Object.assign(this.user.a, u || {});
      this._resex(this.user);
    }
    setAgents(list) {
      const first = this.chars.size === 0, ids = new Set(list.map((a) => a.id));
      for (const id of [...this.chars.keys()]) if (!ids.has(id)) { this.chars.delete(id); this.bubbles.delete(id); }
      let k = 0;
      for (const a of list) {
        const ch = this.chars.get(a.id);
        if (ch) { ch.a = a; this._resex(ch); } else { const n = this._makeChar(a); n.enterDelay = first ? 0.2 + k++ * 0.45 : 0; this.chars.set(a.id, n); }
      }
      this.agents = list; this._layout();
    }
    _layout() {
      const n = this.agents.length; if (!n) return;
      const s = 1.2 * (n > 7 ? Math.max(0.68, 7 / n) : 1);
      this.agents.forEach((a, i) => {
        const ang = n > 1 ? Math.PI * (1.1 + 0.8 * (i / (n - 1))) : Math.PI * 1.5;
        const ex = TABLE.x + Math.cos(ang) * (TABLE.rx - 6), ey = TABLE.y + Math.sin(ang) * (TABLE.ry - 2);
        const seat = { x: ex, y: ey + 26, ex, ey }, ch = this.chars.get(a.id); ch.s = s;
        if (!ch.seat || Math.hypot(ch.seat.x - seat.x, ch.seat.y - seat.y) > 2) {
          ch.seat = seat; this._walkTo(ch, seat, ch.enterDelay || 0); ch.enterDelay = 0;
        } else ch.seat = seat;
      });
    }
    _walkTo(ch, to, delay) {
      const from = { x: ch.x, y: ch.y }, ctl = { x: lerp(from.x, to.x, 0.75), y: Math.min(from.y, to.y - 30) };
      const len = Math.hypot(ctl.x - from.x, ctl.y - from.y) + Math.hypot(to.x - ctl.x, to.y - ctl.y);
      ch.path = { from, ctl, to, t: -delay, dur: Math.max(0.6, len / 110) };
    }
    _char(id) { return id === "usuario" || id === "admin" ? this.user : this.chars.get(id) || this.chars.get("nexus") || null; }

    say(msg) {
      const ch = this._char(msg.agent); if (!ch) return;
      const meta = msg.meta || {};
      if (msg.kind === "delegation" && meta.from && meta.to) {
        const a = this._char(meta.from), b = this._char(meta.to);
        if (a && b && a !== b) this.planes.push({ a, b, p: 0, color: a.a.color || "#00f0ff" });
      }
      if (msg.kind === "insight") this.pops.push({ ch, t: this.t });
      if (msg.kind === "proposal") ch.alert = 6;
      if (msg.kind === "system") this.meeting = 10;
      if (msg.kind === "summary") this.meeting = Math.max(this.meeting, 2);
      const text = String(msg.content || "").replace(/\s+/g, " ").trim(); if (!text) return;
      const type = Math.min(text.length / 42, 5.5);
      this.bubbles.set(ch.a.id, { text, t: this.t, type, life: type + 4.5, kind: msg.kind, x: null, y: null, lines: null });
      ch.act = null;
    }

    _loop(now) {
      requestAnimationFrame(this._loop);
      if (document.hidden || !this.c.offsetParent || !this.w || !this.bg) { this.last = 0; return; }
      const dt = this.last ? Math.min(0.05, (now - this.last) / 1000) : 1 / 60; this.last = now;
      const t0 = performance.now(); this._update(dt); this._draw(dt);
      this.ms = this.ms ? this.ms * 0.95 + (performance.now() - t0) * 0.05 : performance.now() - t0;   // coste medio por fotograma
    }

    _update(dt) {
      this.t += dt; this.meeting = Math.max(0, this.meeting - dt);
      this.meetAmt = damp(this.meetAmt, this.meeting > 0 ? 1 : 0, 2, dt);
      let speaker = null, best = -1;
      for (const [id, b] of this.bubbles) if (this.t - b.t < b.type + 0.4 && b.t > best) { best = b.t; speaker = this._char(id); }
      this.speaker = speaker;
      for (const ch of [...this.chars.values(), this.user]) {
        // desplazamiento por una curva suave, con arranque y frenada
        ch.walking = false;
        if (ch.path) {
          const p = ch.path; p.t += dt;
          if (p.t >= 0) {
            if (ch.hidden) { ch.hidden = false; ch.x = p.from.x; ch.y = p.from.y; }
            const k = clamp(p.t / p.dur, 0, 1), e = smooth(k), u = 1 - e;
            const nx = u * u * p.from.x + 2 * u * e * p.ctl.x + e * e * p.to.x, ny = u * u * p.from.y + 2 * u * e * p.ctl.y + e * e * p.to.y;
            ch.walkPh += Math.hypot(nx - ch.x, ny - ch.y) * 0.17; ch.vx = (nx - ch.x) / dt; ch.x = nx; ch.y = ny;
            ch.walking = k < 1; ch.sit = damp(ch.sit, 0, 12, dt);
            if (k >= 1) ch.path = null;
          }
        } else if (!ch.hidden) ch.sit = damp(ch.sit, 1, 5, dt);
        const b = this.bubbles.get(ch.a.id), talking = !!b && this.t - b.t < b.type + 0.4;
        ch.talk = damp(ch.talk, talking ? 1 : 0, 7, dt);
        ch.gest = damp(ch.gest, talking ? 1 : 0, 3.5, dt);
        // a quien mira: al que habla, al centro en reunion, o deja vagar la mirada
        let lt = wob(this.t * 0.25, ch.seed) * 0.35;
        if (ch.walking) lt = clamp((ch.vx || 0) / 60, -1, 1);
        else if (ch.act === "look") lt = ch.actLook;
        else if (speaker && speaker !== ch) lt = clamp((speaker.x - ch.x) / 110, -1, 1);
        else if (this.meetAmt > 0.3 && ch !== this.user) lt = clamp((TABLE.x - ch.x) / 220, -1, 1);
        ch.look = damp(ch.look, lt, 3.5, dt);
        ch.nod = damp(ch.nod, speaker && speaker !== ch && wob(this.t * 0.35, ch.seed) > 0.05 ? 1 : 0, 2.5, dt);
        // pequeñas acciones cuando esta sentado y callado
        if (!ch.isUser && !ch.walking && !talking && ch.sit > 0.9 && !ch.act && this.t > ch.nextAct) {
          const r = Math.random();
          if (r < 0.32) { ch.act = "sip"; ch.actEnd = this.t + 2.6; }
          else if (r < 0.65) { ch.act = "type"; ch.actEnd = this.t + 2.5 + Math.random() * 3; }
          else { ch.act = "look"; ch.actLook = Math.random() * 2 - 1; ch.actEnd = this.t + 1.5 + Math.random() * 2; }
        }
        if (ch.act && (this.t > ch.actEnd || talking || ch.walking)) { ch.act = null; ch.nextAct = this.t + 3 + Math.random() * 7; }
        ch.sip = damp(ch.sip, ch.act === "sip" ? 1 : 0, 4.5, dt);
        ch.type = damp(ch.type, ch.act === "type" ? 1 : 0, 6, dt);
        if (this.t > ch.nextBlink) { ch.blinkAt = this.t; ch.nextBlink = this.t + 1.8 + Math.random() * 4 + (Math.random() < 0.15 ? -1.5 : 0); }
        ch.alert = Math.max(0, ch.alert - dt);
        ch.P = this._pose(ch);
      }
      for (const p of this.planes) p.p += dt / 1.6;
      this.planes = this.planes.filter((p) => p.p < 1);
      this.pops = this.pops.filter((p) => this.t - p.t < 2.8);
    }

    _pose(ch) {
      const t = this.t, sd = ch.seed;
      const breathe = Math.sin(t * 1.7 + sd) * 0.7;
      const bounce = ch.walking ? Math.abs(Math.sin(ch.walkPh)) * 2.2 : 0;
      const sway = ch.walking ? Math.sin(ch.walkPh) * 1.4 : wob(t * 0.45, sd) * 1.5;
      const lean = ch.talk * 2.2 + ch.type * 1.5 + this.meetAmt * 1.2;
      const hipY = -lerp(42, 27, ch.sit);
      const shY = hipY - 33 + breathe * 0.5 - bounce + lean;
      const nod = ch.nod * Math.max(0, Math.sin(t * 4 + sd)) * 1.8;
      return { hipY, shY, sway, headX: sway * 0.9 + ch.look * 1.5, headY: shY - 14.5 + nod + ch.talk * Math.sin(t * 6.3 + sd) * 0.7,
               tilt: wob(t * 0.7, sd + 3) * 0.07 + ch.look * 0.05 + ch.talk * Math.sin(t * 4.1 + sd) * 0.05 };
    }
    /* rasgos fijos de cada personaje y sus degradados, creados una sola vez en coordenadas locales (pies en 0,0):
     * en cada fotograma solo se trasladan, asi el aspecto mas realista no cuesta tiempo de dibujo */
    _look(ch) {
      const col = ch.a.color || "#00f0ff";
      if (ch.L && ch.L.of === col) return ch.L;
      const r = rng(hash(ch.a.id) ^ 0x51ed27), ctx = this.ctx, low = document.documentElement.hasAttribute("data-lowpower");
      const fem = ch.sexo === "mujer";
      const outfit = ch.isUser ? 1 : Math.floor(r() * 6), build = fem ? Math.floor(r() * 2) : 1 + Math.floor(r() * 2);
      const main = mix(col, "#8d93a0", 0.28), blazer = mix(col, "#2b2f3a", 0.62);
      const top = outfit === 2 ? blazer : main;
      const short = outfit === 0 || outfit === 5;
      const L = { of: col, low, outfit, top, inner: outfit === 2 ? (r() < 0.5 ? "#eef1f5" : tone(col, 0.72)) : null, tie: outfit === 2 && r() < 0.55,
                  W: [12.4, 13.8, 15.2][build] * (fem ? 0.93 : 1), w: [9.2, 10.4, 11.8][build] * (fem ? 0.9 : 1), short, fem,
                  eye: pick(r, ["#5a3a22", "#3d6d9a", "#4f7a46", "#7a5a2e", "#6b7680", "#2e2018"]),
                  lip: mix(ch.skin, fem ? "#c23a58" : "#b5485a", fem ? 0.62 : 0.36), glasses: r() < 0.2, beard: !fem && r() < 0.32,
                  earring: fem ? r() < 0.7 : r() < 0.08, strands: [] };
      const tg = ctx.createLinearGradient(-L.W - 1, 0, L.W + 1, 0);
      tg.addColorStop(0, tone(top, 0.2)); tg.addColorStop(0.45, top); tg.addColorStop(1, tone(top, -0.3));
      L.torso = tg;
      const fg = ctx.createRadialGradient(-3.5, -4.5, 1.5, 0, 0, 13.5);
      fg.addColorStop(0, tone(ch.skin, 0.14)); fg.addColorStop(0.7, ch.skin); fg.addColorStop(1, tone(ch.skin, -0.16));
      L.face = fg;
      const hg = ctx.createLinearGradient(0, -16, 0, 14);
      hg.addColorStop(0, tone(ch.hair, 0.22)); hg.addColorStop(0.35, ch.hair); hg.addColorStop(1, tone(ch.hair, -0.25));
      L.hair = hg;
      const pg = ctx.createLinearGradient(-9, 0, 9, 0);
      pg.addColorStop(0, tone(ch.pants, 0.12)); pg.addColorStop(1, tone(ch.pants, -0.22));
      L.pants = pg;
      for (let i = 0; i < 7; i++) L.strands.push([-8 + i * 2.7 + r() * 1.2, -10 - r() * 3, (r() - 0.5) * 5]);
      return (ch.L = L);
    }

    _arm(ch, sd) {
      const t = this.t, P = ch.P, L = this._look(ch), sh = { x: P.sway + sd * (L.W - 2), y: P.shY + 3 };
      const swing = ch.walking ? Math.sin(ch.walkPh + (sd > 0 ? Math.PI : 0)) : 0;
      let e = { x: sd * 3, y: 15 }, h = { x: sd * 4, y: 30 - swing * 3 };
      e = lerpP(e, { x: sd * 3, y: 20 }, ch.sit); h = lerpP(h, { x: -sd * 4, y: 34 }, ch.sit);
      if (ch.type > 0.01) h = lerpP(h, { x: -sd * 6, y: 32 - Math.max(0, Math.sin(t * 15 + sd * 2 + ch.seed)) * 2.2 }, ch.type);
      const main = ch.seed % 2 > 1 ? 1 : -1;
      const g = ch.gest * (sd === main ? 1 : 0.5 * Math.max(0, wob(t * 0.8, ch.seed + 5)));
      if (g > 0.01) {
        const w1 = wob(t * 1.9, ch.seed + sd), w2 = wob(t * 2.6, ch.seed + sd * 3);
        e = lerpP(e, { x: sd * 7, y: 15 + w2 * 2 }, g); h = lerpP(h, { x: sd * (4 + w1 * 6), y: 5 + w2 * 7 }, g);
      }
      if (sd === 1 && ch.sip > 0.01) { e = lerpP(e, { x: 5, y: 17 }, ch.sip); h = lerpP(h, { x: -10 + P.headX - P.sway, y: -9 + Math.sin(t * 1.3) }, ch.sip); }
      return { sh, e: { x: sh.x + e.x, y: sh.y + e.y }, h: { x: sh.x + h.x, y: sh.y + h.y } };
    }
    _drawArms(ctx, ch) {
      const s = ch.s, L = this._look(ch);
      ctx.save(); ctx.translate(ch.x, ch.y); ctx.scale(s, s);
      ctx.lineCap = "round"; ctx.lineJoin = "round";
      for (const sd of [-1, 1]) {
        const A = this._arm(ch, sd), m = L.short ? lerpP(A.sh, A.e, 0.62) : lerpP(A.e, A.h, 0.86);
        // manga (corta o larga hasta el puño) y antebrazo
        ctx.strokeStyle = tone(L.top, -0.06); ctx.lineWidth = 7.6;
        ctx.beginPath(); ctx.moveTo(A.sh.x, A.sh.y); if (L.short) ctx.lineTo(m.x, m.y); else { ctx.lineTo(A.e.x, A.e.y); ctx.lineTo(m.x, m.y); } ctx.stroke();
        if (!L.low) { ctx.strokeStyle = "rgba(255,255,255,.13)"; ctx.lineWidth = 2.2; ctx.beginPath(); ctx.moveTo(A.sh.x - 1.2, A.sh.y); ctx.lineTo((L.short ? m.x : A.e.x) - 1.2, (L.short ? m.y : A.e.y) - 0.5); ctx.stroke(); }
        ctx.strokeStyle = ch.skin; ctx.lineWidth = 5;
        ctx.beginPath(); ctx.moveTo(m.x, m.y); if (L.short) ctx.lineTo(A.e.x, A.e.y); ctx.lineTo(A.h.x, A.h.y); ctx.stroke();
        if (!L.short) { ctx.strokeStyle = L.inner || tone(L.top, -0.2); ctx.lineWidth = 6.4; ctx.beginPath(); const c0 = lerpP(m, A.h, -0.05), c1 = lerpP(m, A.h, 0.12); ctx.moveTo(c0.x, c0.y); ctx.lineTo(c1.x, c1.y); ctx.stroke(); }
        // mano: palma orientada al antebrazo y pulgar
        const ang = Math.atan2(A.h.y - A.e.y, A.h.x - A.e.x);
        ctx.save(); ctx.translate(A.h.x, A.h.y); ctx.rotate(ang - Math.PI / 2);
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.ellipse(0, 1.2, 2.9, 3.6, 0, 0, 7); ctx.fill();
        ctx.fillStyle = tone(ch.skin, -0.1); ctx.beginPath(); ctx.ellipse(sd * 2.4, -0.2, 1.1, 1.9, sd * 0.5, 0, 7); ctx.fill();
        if (!L.low) { ctx.strokeStyle = tone(ch.skin, -0.2); ctx.lineWidth = 0.5; ctx.beginPath(); ctx.moveTo(-1.6, 3.6); ctx.lineTo(-1.6, 4.4); ctx.moveTo(0, 4); ctx.lineTo(0, 4.8); ctx.moveTo(1.6, 3.6); ctx.lineTo(1.6, 4.4); ctx.stroke(); }
        ctx.restore();
        if (sd === 1 && ch.sip > 0.35) { ctx.restore(); this._mug(ctx, ch.x + (A.h.x + 3) * s, ch.y + (A.h.y + 3) * s, s, false); ctx.save(); ctx.translate(ch.x, ch.y); ctx.scale(s, s); ctx.lineCap = "round"; }
      }
      ctx.restore();
    }
    _drawChair(ctx, x, y, s, front) {
      const top = y - (front ? 50 : 70) * s, bot = y - 22 * s, w = 31 * s;
      ctx.fillStyle = "#5e3b22"; ctx.beginPath(); ctx.roundRect(x - w / 2 - 2 * s, top - 2 * s, w + 4 * s, bot - top + 4 * s, 9 * s); ctx.fill();
      const g = ctx.createLinearGradient(x - w / 2, 0, x + w / 2, 0); g.addColorStop(0, "#d86a4e"); g.addColorStop(1, "#a9463a");
      ctx.fillStyle = g; ctx.beginPath(); ctx.roundRect(x - w / 2 + 2 * s, top + 2 * s, w - 4 * s, bot - top - 4 * s, 7 * s); ctx.fill();
      ctx.strokeStyle = "rgba(255,255,255,.18)"; ctx.lineWidth = 1.2 * s; ctx.beginPath(); ctx.moveTo(x - w / 2 + 6 * s, top + 7 * s); ctx.lineTo(x - w / 2 + 6 * s, bot - 8 * s); ctx.stroke();
    }

    _drawBody(ctx, ch) {
      const s = ch.s, P = ch.P, L = this._look(ch), back = !!ch.isUser, sw = P.sway, sy = P.shY, W = L.W, w = L.w;
      // sombra de contacto: suave por fuera y mas oscura bajo los pies
      ctx.fillStyle = "rgba(0,0,0,.13)"; ctx.beginPath(); ctx.ellipse(ch.x, ch.y, 17 * s, 5.6 * s, 0, 0, 7); ctx.fill();
      ctx.fillStyle = "rgba(0,0,0,.14)"; ctx.beginPath(); ctx.ellipse(ch.x, ch.y, 10 * s, 3 * s, 0, 0, 7); ctx.fill();
      ctx.save(); ctx.translate(ch.x, ch.y); ctx.scale(s, s);
      // piernas (sentado: rodillas hacia la camara; andando: alternan)
      if (!back) {
        ctx.lineCap = "round"; ctx.lineJoin = "round";
        for (const sd of [-1, 1]) {
          const lift = ch.walking ? Math.max(0, Math.sin(ch.walkPh + (sd > 0 ? Math.PI : 0))) * 5 : 0;
          const hip = { x: sw * 0.4 + sd * (w - 4.5), y: P.hipY }, knee = { x: sd * 6, y: lerp(P.hipY + 21, P.hipY + 11, ch.sit) - lift * 0.6 }, foot = { x: sd * 6.5, y: lerp(-3, 9, ch.sit) - lift };
          ctx.strokeStyle = L.pants; ctx.lineWidth = 8;
          ctx.beginPath(); ctx.moveTo(hip.x, hip.y); ctx.lineTo(knee.x, knee.y); ctx.lineTo(foot.x, foot.y); ctx.stroke();
          if (!L.low) { ctx.strokeStyle = "rgba(255,255,255,.1)"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(knee.x - 2, knee.y - 1); ctx.lineTo(foot.x - 2, foot.y - 3); ctx.stroke(); }
          ctx.fillStyle = "#26201e"; ctx.beginPath(); ctx.ellipse(foot.x + sd * 0.6, foot.y + 2.6, 4.9, 2.9, 0, 0, 7); ctx.fill();
          ctx.fillStyle = "rgba(255,255,255,.18)"; ctx.beginPath(); ctx.ellipse(foot.x + sd * 0.6 - 1.4, foot.y + 1.6, 2, 0.8, 0, 0, 7); ctx.fill();
        }
      }
      if (!back && [1, 4, 5].includes(ch.style)) {   // melenas detras del cuerpo
        ctx.save(); ctx.translate(P.headX, P.headY); ctx.rotate(P.tilt); this._backHair(ctx, ch, L); ctx.restore();
      }
      // capucha por detras del cuello
      if (L.outfit === 3) { ctx.fillStyle = tone(L.top, -0.2); ctx.beginPath(); ctx.ellipse(sw, sy - 3.5, 9, 4.2, 0, 0, 7); ctx.fill(); }
      // cuello con sombra de la barbilla
      ctx.fillStyle = tone(ch.skin, -0.1); ctx.fillRect(P.headX - 3.3, P.headY + 6, 6.6, sy - P.headY - 4);
      ctx.fillStyle = "rgba(60,30,20,.22)"; ctx.beginPath(); ctx.ellipse(P.headX, P.headY + 9.5, 4, 1.8, 0, 0, 7); ctx.fill();
      // torso: hombros redondeados que se estrechan hacia la cintura
      ctx.fillStyle = L.torso; ctx.beginPath();
      ctx.moveTo(sw - W, sy + 3.5); ctx.quadraticCurveTo(sw - W - 0.4, sy - 2.6, sw - W + 5.8, sy - 3.6);
      ctx.lineTo(sw + W - 5.8, sy - 3.6); ctx.quadraticCurveTo(sw + W + 0.4, sy - 2.6, sw + W, sy + 3.5);
      ctx.bezierCurveTo(sw + W - 0.5, sy + 14, w + sw * 0.4 + 0.6, P.hipY - 8, w + sw * 0.4, P.hipY + 2);
      ctx.quadraticCurveTo(sw * 0.4, P.hipY + 5, -w + sw * 0.4, P.hipY + 2);
      ctx.bezierCurveTo(-w + sw * 0.4 - 0.6, P.hipY - 8, sw - W + 0.5, sy + 14, sw - W, sy + 3.5); ctx.closePath(); ctx.fill();
      ctx.fillStyle = L.pants; ctx.fillRect(-w + sw * 0.4, P.hipY, w * 2, 4);
      ctx.fillStyle = "#2a2422"; ctx.fillRect(-w + sw * 0.4, P.hipY - 0.2, w * 2, 1.4);           // cinturon
      if (!back) {
        if (!L.low) {   // pliegues de la tela
          ctx.strokeStyle = "rgba(0,0,0,.1)"; ctx.lineWidth = 0.9; ctx.beginPath();
          ctx.moveTo(sw - W + 3, sy + 10); ctx.quadraticCurveTo(sw - 3, sy + 15, sw - w + 2, P.hipY - 2);
          ctx.moveTo(sw + W - 3, sy + 12); ctx.quadraticCurveTo(sw + 4, sy + 17, sw + w - 3, P.hipY - 3); ctx.stroke();
        }
        this._outfit(ctx, ch, L, sw, sy, P);
        // insignia con el icono del agente
        const bx = sw + (L.outfit === 2 ? -6.8 : 6.6), by = sy + 9;
        ctx.fillStyle = "#fffaf0"; ctx.beginPath(); ctx.arc(bx, by, 3.7, 0, 7); ctx.fill();
        ctx.fillStyle = tone(ch.a.color || "#00f0ff", -0.3); ctx.font = `700 5.2px "Segoe UI Symbol", sans-serif`; ctx.textAlign = "center"; ctx.textBaseline = "middle";
        ctx.fillText(ch.a.icon || "◆", bx, by + 0.3);
      } else {   // tu, de espaldas: cuello de la camisa
        ctx.fillStyle = tone(L.top, 0.25); ctx.beginPath(); ctx.ellipse(sw, sy - 3, 5.5, 2.4, 0, Math.PI, 0); ctx.fill();
        ctx.strokeStyle = "rgba(0,0,0,.12)"; ctx.lineWidth = 0.8; ctx.beginPath(); ctx.moveTo(sw, sy); ctx.lineTo(sw * 0.5, P.hipY); ctx.stroke();
      }
      ctx.restore();
      if (ch.sit < 0.5 && !back) this._drawArms(ctx, ch);
      this._drawHead(ctx, ch, back, this.t);
    }

    _outfit(ctx, ch, L, sw, sy, P) {
      const o = L.outfit, top = L.top, light = tone(top, 0.35);
      if (o === 0) {          // camiseta: cuello redondo
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.ellipse(sw, sy - 3.4, 4.4, 2.8, 0, 0, Math.PI); ctx.fill();
        ctx.strokeStyle = tone(top, -0.25); ctx.lineWidth = 1.1; ctx.beginPath(); ctx.ellipse(sw, sy - 3.4, 4.6, 3, 0, 0.1, Math.PI - 0.1); ctx.stroke();
      } else if (o === 1 || o === 5) {   // camisa (cuello y botonadura) o polo
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.moveTo(sw - 3, sy - 3.6); ctx.lineTo(sw + 3, sy - 3.6); ctx.lineTo(sw, sy + (o === 5 ? 2.5 : 3.2)); ctx.closePath(); ctx.fill();
        ctx.fillStyle = light;
        for (const sd of [-1, 1]) { ctx.beginPath(); ctx.moveTo(sw + sd * 0.6, sy + 1.5); ctx.lineTo(sw + sd * 4.6, sy - 3.9); ctx.lineTo(sw + sd * 6, sy - 0.4); ctx.closePath(); ctx.fill(); }
        ctx.strokeStyle = tone(top, -0.18); ctx.lineWidth = 0.7; ctx.beginPath(); ctx.moveTo(sw, sy + 2); ctx.lineTo(sw * 0.6, o === 5 ? sy + 8 : P.hipY); ctx.stroke();
        ctx.fillStyle = tone(top, 0.5); for (let i = 0; i < (o === 5 ? 2 : 4); i++) { ctx.beginPath(); ctx.arc(lerp(sw, sw * 0.6, i / 4) + 0.9, sy + 4.5 + i * 5, 0.55, 0, 7); ctx.fill(); }
      } else if (o === 2) {   // americana con camisa (y corbata)
        ctx.fillStyle = L.inner; ctx.beginPath(); ctx.moveTo(sw - 4.8, sy - 3.6); ctx.lineTo(sw + 4.8, sy - 3.6); ctx.lineTo(sw + 0.4, sy + 13); ctx.closePath(); ctx.fill();
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.moveTo(sw - 2.4, sy - 3.6); ctx.lineTo(sw + 2.4, sy - 3.6); ctx.lineTo(sw, sy - 0.6); ctx.closePath(); ctx.fill();
        if (L.tie) { ctx.fillStyle = tone(ch.a.color || "#00f0ff", -0.25); ctx.beginPath(); ctx.moveTo(sw - 1.1, sy - 0.8); ctx.lineTo(sw + 1.1, sy - 0.8); ctx.lineTo(sw + 1.6, sy + 9); ctx.lineTo(sw + 0.2, sy + 11); ctx.lineTo(sw - 1.2, sy + 9); ctx.closePath(); ctx.fill(); }
        ctx.fillStyle = tone(top, -0.18);
        for (const sd of [-1, 1]) { ctx.beginPath(); ctx.moveTo(sw + sd * 4.8, sy - 3.6); ctx.lineTo(sw + sd * 7.2, sy + 3); ctx.lineTo(sw + sd * 4.2, sy + 3.6); ctx.lineTo(sw + sd * 0.6, sy + 13); ctx.closePath(); ctx.fill(); }
        ctx.fillStyle = tone(top, -0.45); ctx.beginPath(); ctx.arc(sw + 1.2, sy + 16, 0.7, 0, 7); ctx.fill();
      } else if (o === 3) {   // sudadera: cordones y bolsillo
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.ellipse(sw, sy - 3.4, 3.8, 2.4, 0, 0, Math.PI); ctx.fill();
        ctx.strokeStyle = "#f2f2f2"; ctx.lineWidth = 0.7; ctx.beginPath(); ctx.moveTo(sw - 2, sy - 1); ctx.lineTo(sw - 2.3, sy + 7); ctx.moveTo(sw + 2, sy - 1); ctx.lineTo(sw + 2.4, sy + 6); ctx.stroke();
        ctx.strokeStyle = tone(top, -0.22); ctx.lineWidth = 0.9; ctx.beginPath(); ctx.moveTo(sw * 0.4 - 6, P.hipY - 1); ctx.lineTo(sw * 0.5 - 4.5, P.hipY - 8); ctx.lineTo(sw * 0.5 + 4.5, P.hipY - 8); ctx.lineTo(sw * 0.4 + 6, P.hipY - 1); ctx.stroke();
      } else {                // jersey de pico con remate de punto
        ctx.fillStyle = tone(top, -0.2); ctx.beginPath(); ctx.moveTo(sw - 4.6, sy - 3.6); ctx.lineTo(sw + 4.6, sy - 3.6); ctx.lineTo(sw, sy + 5.5); ctx.closePath(); ctx.fill();
        ctx.fillStyle = ch.skin; ctx.beginPath(); ctx.moveTo(sw - 3.6, sy - 3.6); ctx.lineTo(sw + 3.6, sy - 3.6); ctx.lineTo(sw, sy + 4); ctx.closePath(); ctx.fill();
        ctx.strokeStyle = tone(top, -0.16); ctx.lineWidth = 0.6; ctx.beginPath();
        for (let i = -3; i <= 3; i++) { ctx.moveTo(sw * 0.4 + i * 2.6, P.hipY - 3.5); ctx.lineTo(sw * 0.4 + i * 2.6, P.hipY); } ctx.stroke();
      }
    }

    _backHair(ctx, ch, L) {   // melenas: van por detras de la cabeza y del torso
      const st = ch.style; if (st !== 1 && st !== 4 && st !== 5) return;
      ctx.fillStyle = L.hair; ctx.beginPath();
      if (st === 1) ctx.roundRect(-12, -9, 24, 19.5, 6);
      else if (st === 4) { ctx.moveTo(-12, -4); ctx.quadraticCurveTo(-13.4, 14, -10, 21); ctx.lineTo(10, 21); ctx.quadraticCurveTo(13.4, 14, 12, -4); ctx.closePath(); }
      else ctx.arc(0, -3, 14.4, 0, 7);
      ctx.fill();
    }

    _drawHead(ctx, ch, back, t) {
      const s = ch.s, P = ch.P, L = this._look(ch), st = ch.style;
      ctx.save(); ctx.translate(ch.x + P.headX * s, ch.y + P.headY * s); ctx.rotate(P.tilt); ctx.scale(s, s);
      if (back) this._backHair(ctx, ch, L);
      // orejas
      for (const sd of [-1, 1]) {
        const ex = sd * 9.9 - ch.look * 0.8;
        ctx.fillStyle = tone(ch.skin, -0.07); ctx.beginPath(); ctx.ellipse(ex, 1.2, 2.2, 3.2, 0, 0, 7); ctx.fill();
        ctx.fillStyle = tone(ch.skin, -0.22); ctx.beginPath(); ctx.ellipse(ex + sd * 0.3, 1.2, 0.9, 1.8, 0, 0, 7); ctx.fill();
        if (L.earring && !back) { ctx.fillStyle = "#e8c45a"; ctx.beginPath(); ctx.arc(ex, 4.6, 0.9, 0, 7); ctx.fill(); }
      }
      // cara: ovalo con mandibula y barbilla, luz desde arriba a la izquierda
      ctx.fillStyle = L.face; ctx.beginPath();
      ctx.moveTo(0, -11.4); ctx.bezierCurveTo(6.5, -11.4, 10.2, -7, 10.1, -1); ctx.bezierCurveTo(10, 4.6, 7.4, 8.7, 4, 10.7);
      ctx.quadraticCurveTo(0, 12.7, -4, 10.7); ctx.bezierCurveTo(-7.4, 8.7, -10, 4.6, -10.1, -1); ctx.bezierCurveTo(-10.2, -7, -6.5, -11.4, 0, -11.4); ctx.closePath(); ctx.fill();
      if (back) {
        ctx.fillStyle = L.hair; ctx.beginPath(); ctx.ellipse(-ch.look * 1.2, -1.5, 10.8, 11.4, 0, 0, 7); ctx.fill();
        if (!L.low) { ctx.strokeStyle = tone(ch.hair, -0.25); ctx.lineWidth = 0.6; ctx.beginPath(); for (const [x, , c] of L.strands) { ctx.moveTo(x * 0.9, -9); ctx.quadraticCurveTo(x + c * 0.4, 0, x * 0.8, 8); } ctx.stroke(); }
        ctx.fillStyle = "rgba(255,255,255,.12)"; ctx.beginPath(); ctx.ellipse(-4, -6, 4, 2.5, -0.5, 0, 7); ctx.fill();
        ctx.restore(); return;
      }
      const fx = ch.look * 3, talk = ch.talk;
      // sombra suave en el lado contrario a la luz
      ctx.fillStyle = "rgba(90,40,30,.08)"; ctx.beginPath(); ctx.ellipse(5.5 + fx * 0.3, 2, 4.2, 8, 0, 0, 7); ctx.fill();
      // ojos: esclerotica, iris del color de cada uno, pupila, brillo y parpado
      const bl = t - ch.blinkAt, closed = bl >= 0 && bl < 0.13, gaze = clamp(ch.look * 0.7, -0.7, 0.7);
      for (const sd of [-1, 1]) {
        const ex = fx + sd * 3.9, ey = 0.5;
        if (closed) {
          ctx.strokeStyle = "#3a2626"; ctx.lineWidth = 0.9; ctx.beginPath(); ctx.moveTo(ex - 2.1, ey); ctx.quadraticCurveTo(ex, ey + 1, ex + 2.1, ey); ctx.stroke();
        } else {
          ctx.fillStyle = "#f7f3ee"; ctx.beginPath(); ctx.ellipse(ex, ey, 2.15, 1.5, 0, 0, 7); ctx.fill();
          ctx.fillStyle = L.eye; ctx.beginPath(); ctx.arc(ex + gaze, ey + 0.05, 1.22, 0, 7); ctx.fill();
          ctx.fillStyle = "#140e10"; ctx.beginPath(); ctx.arc(ex + gaze, ey + 0.05, 0.58, 0, 7); ctx.fill();
          ctx.fillStyle = "#fff"; ctx.beginPath(); ctx.arc(ex + gaze + 0.45, ey - 0.45, 0.36, 0, 7); ctx.fill();
          ctx.strokeStyle = "#2a1c1c"; ctx.lineWidth = 0.85; ctx.beginPath(); ctx.moveTo(ex - 2.3, ey + 0.2); ctx.quadraticCurveTo(ex, ey - 2, ex + 2.3, ey + 0.1); ctx.stroke();
          if (L.fem) { ctx.lineWidth = 0.5; ctx.beginPath(); for (const k of [0.25, 0.6, 0.95]) { const lx = ex + sd * (2.3 * k), ly = ey - 1.5 * (1 - Math.abs(k - 0.5)); ctx.moveTo(lx, ly); ctx.lineTo(lx + sd * 0.8, ly - 0.9); } ctx.stroke(); }
          if (!L.low) { ctx.strokeStyle = "rgba(80,40,30,.22)"; ctx.lineWidth = 0.45; ctx.beginPath(); ctx.moveTo(ex - 1.6, ey + 1.4); ctx.quadraticCurveTo(ex, ey + 2, ex + 1.6, ey + 1.4); ctx.stroke(); }
        }
        // ceja con grosor variable que sube al hablar
        const by = -3.6 - talk * 1 - Math.max(0, wob(t * 0.9, ch.seed + sd)) * 0.6;
        ctx.fillStyle = tone(ch.hair, -0.12); ctx.beginPath();
        ctx.moveTo(ex - sd * 2.5, by + 0.9); ctx.quadraticCurveTo(ex, by - 1.1, ex + sd * 2.6, by + 0.3);
        ctx.lineTo(ex + sd * 2.5, by + 0.85); ctx.quadraticCurveTo(ex, by - 0.2, ex - sd * 2.4, by + 1.5); ctx.closePath(); ctx.fill();
      }
      // nariz con luz en el puente y sombra lateral
      ctx.strokeStyle = tone(ch.skin, -0.26); ctx.lineWidth = 0.7; ctx.beginPath(); ctx.moveTo(fx * 1.1 + 0.8, 0); ctx.quadraticCurveTo(fx * 1.1 + 1.5, 3.2, fx * 1.1 + 0.2, 3.9); ctx.stroke();
      ctx.fillStyle = tone(ch.skin, -0.3); for (const sd of [-1, 1]) { ctx.beginPath(); ctx.ellipse(fx * 1.1 + sd * 1.1, 4, 0.55, 0.35, 0, 0, 7); ctx.fill(); }
      if (!L.low) { ctx.fillStyle = "rgba(255,255,255,.22)"; ctx.beginPath(); ctx.ellipse(fx * 1.1 - 0.2, 1.8, 0.45, 1.4, 0, 0, 7); ctx.fill(); }
      ctx.fillStyle = "rgba(230,90,90,.14)"; for (const sd of [-1, 1]) { ctx.beginPath(); ctx.ellipse(fx + sd * 6.2, 4.2, 2.4, 1.5, 0, 0, 7); ctx.fill(); }
      // barba y bigote
      if (L.beard) {
        ctx.fillStyle = rgba(ch.hair, 0.88); ctx.beginPath();
        ctx.moveTo(-9.6, 1.5); ctx.bezierCurveTo(-9, 6, -7, 9.4, -4, 10.9); ctx.quadraticCurveTo(0, 13, 4, 10.9); ctx.bezierCurveTo(7, 9.4, 9, 6, 9.6, 1.5);
        ctx.quadraticCurveTo(8, 6.5, fx + 3.6, 7.6); ctx.quadraticCurveTo(fx, 6, fx - 3.6, 7.6); ctx.quadraticCurveTo(-8, 6.5, -9.6, 1.5); ctx.closePath(); ctx.fill();
        ctx.beginPath(); ctx.moveTo(fx - 3.6, 6.2); ctx.quadraticCurveTo(fx, 4.4, fx + 3.6, 6.2); ctx.quadraticCurveTo(fx, 5.6, fx - 3.6, 6.2); ctx.fill();
      }
      // boca: labios; al hablar se abre de forma irregular y asoman los dientes
      const open = talk * (0.3 + 0.7 * Math.abs(Math.sin(t * 10.5 + ch.seed) * Math.sin(t * 6.7 + ch.seed * 2)));
      if (open > 0.18) {
        ctx.fillStyle = "#5a1f24"; ctx.beginPath(); ctx.ellipse(fx, 7, 2.3, 0.5 + open * 1.9, 0, 0, 7); ctx.fill();
        ctx.fillStyle = "#f4efe6"; ctx.fillRect(fx - 1.5, 6.6 - open * 1.5, 3, Math.min(0.9, open));
        ctx.strokeStyle = L.lip; ctx.lineWidth = 0.8; ctx.beginPath(); ctx.ellipse(fx, 7, 2.5, 0.6 + open * 1.9, 0, 0, 7); ctx.stroke();
      } else {
        ctx.fillStyle = tone(L.lip, -0.12); ctx.beginPath(); ctx.moveTo(fx - 2.6, 6.4); ctx.quadraticCurveTo(fx - 1, 5.5, fx, 6); ctx.quadraticCurveTo(fx + 1, 5.5, fx + 2.6, 6.4); ctx.quadraticCurveTo(fx, 6.9, fx - 2.6, 6.4); ctx.fill();
        ctx.fillStyle = L.lip; ctx.beginPath(); ctx.moveTo(fx - 2.4, 6.5); ctx.quadraticCurveTo(fx, 8.4, fx + 2.4, 6.5); ctx.quadraticCurveTo(fx, 7, fx - 2.4, 6.5); ctx.fill();
        ctx.strokeStyle = "rgba(90,30,35,.5)"; ctx.lineWidth = 0.4; ctx.beginPath(); ctx.moveTo(fx - 2.6, 6.4); ctx.quadraticCurveTo(fx, 7.1, fx + 2.6, 6.4); ctx.stroke();
      }
      // gafas
      if (L.glasses) {
        ctx.strokeStyle = "#1f1b1d"; ctx.lineWidth = 0.75; ctx.fillStyle = "rgba(200,225,255,.12)";
        for (const sd of [-1, 1]) { ctx.beginPath(); ctx.roundRect(fx + sd * 3.9 - 2.9, -1.6, 5.8, 4, 1.4); ctx.fill(); ctx.stroke(); }
        ctx.beginPath(); ctx.moveTo(fx - 1, -0.2); ctx.quadraticCurveTo(fx, -0.9, fx + 1, -0.2); ctx.moveTo(fx - 6.8, -0.6); ctx.lineTo(-9.8, -0.9); ctx.moveTo(fx + 6.8, -0.6); ctx.lineTo(9.8, -0.9); ctx.stroke();
      }
      // pelo delantero con mechones y brillo
      ctx.fillStyle = L.hair;
      const cap = (part) => {
        ctx.beginPath(); ctx.moveTo(-10.8, 0.5); ctx.bezierCurveTo(-12, -15.2, 12, -15.2, 10.8, 0.5);
        ctx.quadraticCurveTo(8, -6.4, part, -6.2); ctx.quadraticCurveTo(-7, -6.6, -10.8, 0.5); ctx.closePath(); ctx.fill();
      };
      if (st === 0) cap(1);
      else if (st === 1) { cap(0); ctx.fillRect(-12, -4, 3.2, 14); ctx.fillRect(8.8, -4, 3.2, 14); }
      else if (st === 2) { cap(-2); ctx.beginPath(); ctx.arc(0, -13.6, 4.8, 0, 7); ctx.fill(); }
      else if (st === 3) { cap(2); for (let i = -2; i <= 2; i++) { ctx.beginPath(); ctx.moveTo(i * 4.1 - 2.6, -9); ctx.lineTo(i * 4.6, -15.4 - (i % 2 ? 0 : 2)); ctx.lineTo(i * 4.1 + 2.6, -9); ctx.fill(); } }
      else if (st === 4) cap(-4);
      else { ctx.beginPath(); ctx.ellipse(0, -6.5, 12.2, 8, 0, Math.PI, 0); ctx.quadraticCurveTo(6, -5, 0, -5.5); ctx.quadraticCurveTo(-6, -5, -12.2, -6.5); ctx.fill(); }
      if (!L.low) {
        ctx.strokeStyle = tone(ch.hair, -0.3); ctx.lineWidth = 0.55; ctx.beginPath();
        for (const [x, y, c] of L.strands) { ctx.moveTo(x * 0.5, y); ctx.quadraticCurveTo(x + c * 0.3, y + 2.5, x * 1.05, -5.5); }
        ctx.stroke();
      }
      ctx.fillStyle = "rgba(255,255,255,.17)"; ctx.beginPath(); ctx.ellipse(-4, -8.6, 3.8, 1.4, -0.35, 0, 7); ctx.fill();
      ctx.restore();
    }
    _mug(ctx, x, y, s, steam) {
      ctx.fillStyle = "#f7f3ea"; ctx.beginPath(); ctx.roundRect(x - 3.5 * s, y - 9 * s, 7 * s, 9 * s, 1.5 * s); ctx.fill();
      ctx.strokeStyle = "#f7f3ea"; ctx.lineWidth = 1.4 * s; ctx.beginPath(); ctx.arc(x + 4 * s, y - 5 * s, 2.2 * s, -1.3, 1.3); ctx.stroke();
      ctx.fillStyle = "#6b3e26"; ctx.beginPath(); ctx.ellipse(x, y - 9 * s, 3.2 * s, 1.1 * s, 0, 0, 7); ctx.fill();
      if (steam) {
        ctx.strokeStyle = "rgba(255,255,255,.35)"; ctx.lineWidth = 1;
        for (let i = 0; i < 2; i++) {
          const ph = (this.t * 0.6 + i * 0.5 + x * 0.01) % 1; ctx.globalAlpha = Math.sin(ph * Math.PI);
          ctx.beginPath(); ctx.moveTo(x - 1 + i * 2, y - 11 * s - ph * 12);
          ctx.quadraticCurveTo(x + 2 + i * 2 + Math.sin(this.t * 3 + i) * 2, y - 15 * s - ph * 12, x - 1 + i * 2, y - 19 * s - ph * 12); ctx.stroke();
        }
        ctx.globalAlpha = 1;
      }
    }

    _drawTable(ctx) {
      const { x, y, rx, ry, th } = TABLE;
      ctx.fillStyle = "#4a2c18";
      for (const a of [0.24, 0.76]) { const px = x + Math.cos(a * Math.PI) * rx * 0.85, py = y + Math.sin(a * Math.PI) * ry * 0.85; ctx.fillRect(px - 5.5, py, 11, TABLE.h); }
      const eg = ctx.createLinearGradient(x - rx, 0, x + rx, 0); eg.addColorStop(0, "#7a4828"); eg.addColorStop(1, "#55301a");
      ctx.fillStyle = eg; ctx.beginPath(); ctx.ellipse(x, y + th, rx, ry, 0, 0, Math.PI); ctx.lineTo(x - rx, y); ctx.ellipse(x, y, rx, ry, 0, Math.PI, 0, true); ctx.closePath(); ctx.fill();
      const g = ctx.createLinearGradient(x - rx, y - ry, x + rx, y + ry); g.addColorStop(0, "#bb8050"); g.addColorStop(1, "#8e5532");
      ctx.fillStyle = g; ctx.beginPath(); ctx.ellipse(x, y, rx, ry, 0, 0, 7); ctx.fill();
      ctx.save(); ctx.clip();
      ctx.strokeStyle = "rgba(70,35,12,.13)"; ctx.lineWidth = 1;
      for (let i = 0; i < 9; i++) { ctx.beginPath(); ctx.ellipse(x - 60 + i * 17, y - 6 + (i % 3) * 5, rx * (0.35 + i * 0.07), ry * (0.25 + i * 0.07), 0, 0, 7); ctx.stroke(); }
      ctx.translate(x, y - 6); ctx.scale(1, ry / rx);
      const lg = ctx.createRadialGradient(0, 0, 10, 0, 0, rx); lg.addColorStop(0, `rgba(255,236,190,${0.2 + 0.28 * this.meetAmt})`); lg.addColorStop(1, "rgba(255,236,190,0)");
      ctx.fillStyle = lg; ctx.fillRect(-rx, -rx, rx * 2, rx * 2);
      ctx.restore();
      ctx.strokeStyle = "rgba(255,220,180,.35)"; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.ellipse(x, y, rx - 1, ry - 1, 0, Math.PI * 1.05, Math.PI * 1.95); ctx.stroke();
      // jarron con flores que se mecen
      ctx.fillStyle = "rgba(0,0,0,.2)"; ctx.beginPath(); ctx.ellipse(x, y + 2, 15, 5, 0, 0, 7); ctx.fill();
      const flowers = ["#ff6b8b", "#ffd166", "#ff9f43", "#c792ea", "#ff6b8b"];
      flowers.forEach((c, i) => {
        const a = (i - 2) * 0.32 + wob(this.t * 0.6, i * 3) * 0.06, len = 30 + (i % 2) * 7;
        const fx = x + Math.sin(a) * len, fy = y - 20 - Math.cos(a) * len;
        ctx.strokeStyle = "#4f8f45"; ctx.lineWidth = 1.4; ctx.beginPath(); ctx.moveTo(x, y - 18); ctx.quadraticCurveTo(x + Math.sin(a) * len * 0.3, fy + 14, fx, fy); ctx.stroke();
        ctx.fillStyle = c; for (let k = 0; k < 5; k++) { const pa = k * 1.2566 + i; ctx.beginPath(); ctx.arc(fx + Math.cos(pa) * 3, fy + Math.sin(pa) * 3, 2.6, 0, 7); ctx.fill(); }
        ctx.fillStyle = "#fff1a8"; ctx.beginPath(); ctx.arc(fx, fy, 1.8, 0, 7); ctx.fill();
      });
      const vg = ctx.createLinearGradient(x - 10, 0, x + 10, 0); vg.addColorStop(0, "#7cc4dd"); vg.addColorStop(1, "#2f7193");
      ctx.fillStyle = vg; ctx.beginPath(); ctx.moveTo(x - 6, y - 22); ctx.bezierCurveTo(x - 6, y - 14, x - 13, y - 12, x - 9, y);
      ctx.lineTo(x + 9, y); ctx.bezierCurveTo(x + 13, y - 12, x + 6, y - 14, x + 6, y - 22); ctx.closePath(); ctx.fill();
      ctx.fillStyle = "#3d8bb0"; ctx.beginPath(); ctx.ellipse(x, y - 22, 6, 1.8, 0, 0, 7); ctx.fill();
    }

    _drawTableItems(ctx, ch) {
      const st = ch.seat, s = ch.s, dx = TABLE.x - st.ex, dy = TABLE.y - st.ey, n = Math.hypot(dx, dy) || 1, ux = dx / n, uy = dy / n;
      // portatil (vemos la tapa) o libreta
      const lx = st.ex + ux * 16 - 4 * s, ly = st.ey + uy * 16 + 8;
      if (ch.laptop) {
        ctx.fillStyle = "#9aa3ae"; ctx.beginPath(); ctx.moveTo(lx - 13 * s, ly + 2); ctx.lineTo(lx + 13 * s, ly + 2); ctx.lineTo(lx + 11 * s, ly - 1); ctx.lineTo(lx - 11 * s, ly - 1); ctx.closePath(); ctx.fill();
        ctx.fillStyle = "#cfd5dd"; ctx.beginPath(); ctx.roundRect(lx - 12 * s, ly - 15 * s, 24 * s, 15 * s, 2 * s); ctx.fill();
        ctx.fillStyle = rgba(ch.a.color || "#00f0ff", 0.85); ctx.shadowColor = ch.a.color || "#00f0ff"; ctx.shadowBlur = 6 + 4 * ch.type;
        ctx.beginPath(); ctx.arc(lx, ly - 7.5 * s, 2.2 * s, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      } else {
        ctx.fillStyle = "#fffdf6"; ctx.beginPath(); ctx.moveTo(lx - 11 * s, ly + 3); ctx.lineTo(lx + 9 * s, ly + 3); ctx.lineTo(lx + 12 * s, ly - 6); ctx.lineTo(lx - 8 * s, ly - 6); ctx.closePath(); ctx.fill();
        ctx.strokeStyle = "rgba(80,110,170,.45)"; ctx.lineWidth = 0.8;
        for (let i = 0; i < 3; i++) { ctx.beginPath(); ctx.moveTo(lx - 8 * s + i, ly - 3.5 + i * 2.2); ctx.lineTo(lx + 8 * s + i, ly - 3.5 + i * 2.2); ctx.stroke(); }
      }
      if (ch.sip < 0.35) this._mug(ctx, st.ex + ux * 10 + 17 * s, st.ey + uy * 10 + 10, s, true);
      // cartelito con el nombre
      const name = String(ch.a.name || ch.a.id).split(/\s+/)[0].slice(0, 12).toUpperCase();
      const px = st.ex + ux * 34, py = st.ey + uy * 34 + 14;
      ctx.font = `800 ${8.5 * Math.min(1, s)}px ${FONT}`; const w = ctx.measureText(name).width + 10;
      ctx.fillStyle = "rgba(0,0,0,.18)"; ctx.fillRect(px - w / 2 + 2, py + 1, w, 3);
      ctx.fillStyle = "#e9e2d2"; ctx.fillRect(px - w / 2, py - 14, w, 3);
      ctx.fillStyle = "#fbf7ef"; ctx.beginPath(); ctx.roundRect(px - w / 2, py - 11, w, 12, 2); ctx.fill();
      ctx.fillStyle = ch.a.color || "#00f0ff"; ctx.fillRect(px - w / 2, py - 1, w, 2);
      ctx.fillStyle = tone(ch.a.color || "#00f0ff", -0.45); ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(name, px, py - 5);
    }

    _drawPlane(ctx, p) {
      const a = p.a, b = p.b, P0 = { x: a.x, y: headTop(a) + 6 }, P2 = { x: b.x, y: headTop(b) + 6 };
      const C = { x: (P0.x + P2.x) / 2, y: Math.min(P0.y, P2.y) - 70 };
      const at = (k) => { const u = 1 - k; return { x: u * u * P0.x + 2 * u * k * C.x + k * k * P2.x, y: u * u * P0.y + 2 * u * k * C.y + k * k * P2.y }; };
      const k = smooth(p.p), pos = at(k), nx = at(Math.min(1, k + 0.01)), ang = Math.atan2(nx.y - pos.y, nx.x - pos.x);
      ctx.strokeStyle = rgba(p.color, 0.55); ctx.lineWidth = 1.6; ctx.setLineDash([2, 5]); ctx.beginPath();
      for (let i = 0; i <= 12; i++) { const q = at(Math.max(0, k - 0.3 + i * 0.025)); i ? ctx.lineTo(q.x, q.y) : ctx.moveTo(q.x, q.y); }
      ctx.stroke(); ctx.setLineDash([]);
      ctx.save(); ctx.translate(pos.x, pos.y); ctx.rotate(ang + Math.sin(this.t * 9) * 0.08);
      ctx.fillStyle = "#ffffff"; ctx.strokeStyle = "rgba(0,0,0,.25)"; ctx.lineWidth = 0.8;
      ctx.beginPath(); ctx.moveTo(10, 0); ctx.lineTo(-7, -6); ctx.lineTo(-4, 0); ctx.lineTo(-7, 6); ctx.closePath(); ctx.fill(); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(-4, 0); ctx.lineTo(10, 0); ctx.stroke(); ctx.restore();
    }

    _drawBulb(ctx, p) {
      const age = this.t - p.t, ch = p.ch, k = age < 0.35 ? backOut(age / 0.35) : 1, alpha = age > 2.1 ? 1 - (age - 2.1) / 0.7 : 1;
      const x = ch.x + 20 * ch.s, y = headTop(ch) - 6 - age * 5;
      ctx.save(); ctx.globalAlpha = clamp(alpha, 0, 1); ctx.translate(x, y); ctx.scale(k, k);
      ctx.strokeStyle = "rgba(255,214,74,.8)"; ctx.lineWidth = 1.5;
      for (let i = 0; i < 8; i++) { const a = i * Math.PI / 4 + this.t; ctx.beginPath(); ctx.moveTo(Math.cos(a) * 11, Math.sin(a) * 11); ctx.lineTo(Math.cos(a) * 15, Math.sin(a) * 15); ctx.stroke(); }
      ctx.fillStyle = "#ffd84a"; ctx.shadowColor = "#ffd84a"; ctx.shadowBlur = 14; ctx.beginPath(); ctx.arc(0, -1, 7, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      ctx.fillStyle = "#9aa0a8"; ctx.fillRect(-3.5, 5, 7, 5); ctx.fillStyle = "#fff8d0"; ctx.beginPath(); ctx.arc(-2.4, -3.4, 2, 0, 7); ctx.fill();
      ctx.restore();
    }

    _drawBubbles(ctx, dt) {
      // en pixeles de pantalla para que el texto se lea igual en el movil
      const narrow = this.w < 520, sc = this.sc, ox = this.ox, oy = this.oy, fs = narrow ? 11 : 12.5, lh = narrow ? 13.5 : 15.5, placed = [];
      const entries = [...this.bubbles].sort((p, q) => q[1].t - p[1].t), max = this.w < 520 ? 2 : 3;
      let shown = 0;
      for (const [id, b] of entries) {
        const ch = this._char(id), age = this.t - b.t;
        const alpha = age < b.life ? 1 : 1 - (age - b.life) / 0.9;
        if (alpha <= 0 || !ch) { this.bubbles.delete(id); continue; }
        if (ch.hidden || !ch.P || shown++ >= max) continue;
        ctx.font = `600 ${fs}px ${FONT}`;
        if (!b.lines || b.narrow !== narrow) { b.narrow = narrow; b.lines = wrap(ctx, b.text, narrow ? 146 : 196, 3); b.bw = Math.max(90, Math.min(narrow ? 166 : 216, Math.max(...b.lines.map((l) => ctx.measureText(l).width)) + 22)); }
        const bw = b.bw, bh = 26 + b.lines.length * lh;
        const ax = ox + ch.x * sc, ay = oy + (headTop(ch) - (ch.isUser ? 4 : 26) * ch.s) * sc;
        // busca un hueco libre: encima, mas arriba o desplazada a un lado
        const hits = (x, y) => placed.reduce((acc, r) => acc + Math.max(0, Math.min(x + bw, r.x + r.w) - Math.max(x, r.x) + 5) * Math.max(0, Math.min(y + bh, r.y + r.h) - Math.max(y, r.y) + 5), 0);
        let bx = 0, by = 0, worst = Infinity;
        for (const dy of [0, 1, 2]) for (const dx of [0, -0.55, 0.55, -1, 1]) {
          const x = clamp(ax - bw / 2 + dx * bw, 6, this.w - bw - 6), y = Math.max(4, ay - bh - 10 - dy * (bh + 8));
          const o = hits(x, y) + (dy * 40 + Math.abs(dx) * 30);
          if (o < worst) { worst = o; bx = x; by = y; }
        }
        b.x = b.x == null ? bx : damp(b.x, bx, 9, dt); b.y = b.y == null ? by : damp(b.y, by, 9, dt);
        placed.push({ x: bx, y: by, w: bw, h: bh });
        const pop = age < 0.3 ? backOut(age / 0.3) : 1, tipX = clamp(ax, b.x + 14, b.x + bw - 14);
        ctx.save(); ctx.globalAlpha = clamp(alpha, 0, 1);
        ctx.translate(tipX, b.y + bh); ctx.scale(pop, pop); ctx.translate(-tipX, -(b.y + bh));
        ctx.shadowColor = "rgba(0,0,0,.28)"; ctx.shadowBlur = 10; ctx.shadowOffsetY = 3;
        ctx.fillStyle = "#ffffff"; ctx.beginPath(); ctx.roundRect(b.x, b.y, bw, bh, 13);
        ctx.moveTo(tipX - 8, b.y + bh - 1); ctx.lineTo(clamp(ax, b.x - 24, b.x + bw + 24), Math.min(ay - 2, b.y + bh + 14)); ctx.lineTo(tipX + 8, b.y + bh - 1); ctx.fill();
        ctx.shadowBlur = 0; ctx.shadowOffsetY = 0;
        const col = ch.isUser ? "#4b67b0" : tone(ch.a.color || "#00f0ff", -0.42);
        ctx.fillStyle = col; ctx.font = `800 10px ${FONT}`; ctx.textAlign = "left"; ctx.textBaseline = "top";
        ctx.fillText(String(ch.a.name || ch.a.id).toUpperCase().slice(0, 18), b.x + 11, b.y + 7);
        const lbl = { delegation: "delega", insight: "idea", proposal: "propuesta", summary: "resumen", system: "reunión", feedback: "feedback", intro: "hola" }[b.kind];
        if (lbl && !narrow) { ctx.fillStyle = "#9097a3"; ctx.font = `700 9px ${FONT}`; ctx.textAlign = "right"; ctx.fillText(lbl.toUpperCase(), b.x + bw - 11, b.y + 8); }
        // texto que se va escribiendo
        let left = Math.floor(age * 42) + 1;
        ctx.fillStyle = "#2a2f3a"; ctx.font = `600 ${fs}px ${FONT}`; ctx.textAlign = "left";
        b.lines.forEach((ln, i) => { if (left <= 0) return; ctx.fillText(ln.slice(0, left), b.x + 11, b.y + 21 + i * lh); left -= ln.length + 1; });
        ctx.restore();
      }
    }

    _draw(dt) {
      const ctx = this.ctx, d = this.d, sc = this.sc;
      ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.drawImage(this.bg, 0, 0);
      ctx.setTransform(d * sc, 0, 0, d * sc, d * this.ox, d * this.oy);
      const chars = [...this.chars.values()];
      // sillas y personajes del fondo, de atras hacia delante
      const layer = [];
      for (const ch of chars) { if (ch.seat) layer.push({ y: ch.seat.y - 0.5, f: () => this._drawChair(ctx, ch.seat.x, ch.seat.y, ch.s, false) }); if (!ch.hidden) layer.push({ y: ch.seat && Math.hypot(ch.x - ch.seat.x, ch.y - ch.seat.y) < 45 ? Math.max(ch.y, ch.seat.y) : ch.y, f: () => this._drawBody(ctx, ch) }); }
      layer.sort((p, q) => p.y - q.y).forEach((o) => o.f());
      this._drawTable(ctx);
      for (const ch of chars) if (ch.seat) this._drawTableItems(ctx, ch);
      for (const ch of chars) if (!ch.hidden && ch.sit >= 0.5) this._drawArms(ctx, ch);
      // tu, de espaldas, en primer plano
      this._drawBody(ctx, this.user); this._drawChair(ctx, USER_SEAT.x, USER_SEAT.y, this.user.s, true);
      for (const p of this.planes) this._drawPlane(ctx, p);
      for (const ch of [...chars, this.user]) {
        if (ch.hidden || !ch.P) continue;
        const col = ch.alert > 0 ? mix(GREEN, AMBER, Math.min(1, ch.alert)) : GREEN;
        drawPlumbob(ctx, ch.x + ch.P.headX * ch.s, headTop(ch) - 13 * ch.s + Math.sin(this.t * 2 + ch.seed) * 1.6, ch.s * (ch.isUser ? 0.8 : 1), this.t * 1.4 + ch.seed, col, ch.talk + (ch.alert > 0 ? 0.6 : 0));
      }
      for (const p of this.pops) this._drawBulb(ctx, p);
      ctx.setTransform(d, 0, 0, d, 0, 0);
      this._drawBubbles(ctx, dt);
    }
  }
  LoungeRoom.util = { clamp, lerp, lerpP, smooth, damp, wob, hash, rng, pick, mix, tone, rgba, wrap, backOut, FONT, GREEN, AMBER, drawPlumbob, headTop, sexOf };
  window.LoungeRoom = LoungeRoom;
})();
