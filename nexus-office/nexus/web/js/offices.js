/* Nexus-Office — Despachos de los agentes: cada agente tiene su despacho y se le ve trabajar en lo que hace de verdad
 * (/api/agentes/actividad y los eventos «agent_activity»): piensa con la mano en la barbilla y se toma un café, escribe
 * en el portatil, busca en la web, lee documentos, dibuja en la tableta, compone con cascos, habla por telefono con la
 * IA online, se va a la reunion del salon y se manda avioncitos de papel con los demas cuando delega.
 * Reutiliza los personajes del salon (mismo aspecto, sexo elegido, parpadeo, respiracion...) y solo dibuja por tiempo
 * real con amortiguacion, sin bucles de estado bruscos. Coste bajo: el decorado fijo de cada despacho se pinta una vez. */
(function () {
  if (!window.LoungeRoom) return;
  const U = LoungeRoom.util;
  const { clamp, lerp, lerpP, damp, wob, hash, rng, mix, tone, rgba, FONT } = U;
  const CW = 300, CH = 232;                    // despacho en unidades de diseño
  const SEAT = { x: 124, y: 172 }, DESK = { x0: 62, x1: 206, top: 140, front: 151, bottom: 188 };
  const DOOR = { x: 24, y: 126 }, COFFEE = { x: 262, y: 206 }, SCREEN = { x0: 176, y0: 22, x1: 284, y1: 96 };
  const LABEL = { idle: "libre", thinking: "pensando", writing: "escribiendo", tool: "trabajando", meeting: "en el salón",
                  delegating: "delegando", browsing: "buscando en la web", reading: "leyendo documentos", drawing: "dibujando",
                  music: "componiendo", calculating: "calculando", coding: "programando", phone: "al teléfono", working: "trabajando",
                  coffee: "tomando un café" };

  class OfficeRoom extends LoungeRoom {
    constructor(canvas) {
      super(canvas);
      this.user.hidden = true;
      this.act = {};
      this.cells = new Map();
    }

    /* ------------------------------------------------------------ tamaño y decorado */
    _resize() {
      const parent = this.c.parentElement, r = parent.getBoundingClientRect(), d = Math.min(devicePixelRatio || 1, 2);
      const n = Math.max(1, (this.agents || []).length);
      const cols = r.width < 560 ? 1 : r.width < 980 ? 2 : 3, rows = Math.ceil(n / cols);
      this.cols = cols; this.sc = r.width / (cols * CW);
      const h = Math.round(rows * CH * this.sc);
      if (Math.abs(parent.offsetHeight - h) > 1) parent.style.height = h + "px";
      this.w = r.width; this.h = h; this.d = d; this.ox = 0; this.oy = 0;
      this.c.width = Math.round(r.width * d); this.c.height = Math.round(h * d);
      if (!this.w || !this.h) return;
      this._paintBg();
    }
    _paintBg() {
      const bg = this.bg || (this.bg = document.createElement("canvas")); bg.width = this.c.width; bg.height = this.c.height;
      const b = bg.getContext("2d");
      b.fillStyle = "#11161f"; b.fillRect(0, 0, bg.width, bg.height);
      this.bgHour = new Date().getHours();
      (this.agents || []).forEach((a, i) => {
        const cx = (i % this.cols) * CW, cy = Math.floor(i / this.cols) * CH;
        b.setTransform(this.d * this.sc, 0, 0, this.d * this.sc, this.d * this.sc * cx, this.d * this.sc * cy);
        paintOffice(b, a, this.bgHour);
      });
    }
    _layout() {
      if (!this.cols) return;
      this.agents.forEach((a, i) => {
        const ch = this.chars.get(a.id), cx = (i % this.cols) * CW, cy = Math.floor(i / this.cols) * CH;
        const seat = { x: cx + SEAT.x, y: cy + SEAT.y, ex: cx + SEAT.x, ey: cy + DESK.top };
        this.cells.set(a.id, { cx, cy, i });
        ch.s = 1.22; ch.seat = seat;
        if (ch.hidden && !ch.away) { ch.hidden = false; ch.path = null; ch.x = seat.x; ch.y = seat.y; ch.sit = 1; }
      });
      if (this.bg && (this.agents.length !== this._painted)) { this._painted = this.agents.length; this._resize(); }
    }
    setAgents(list) { super.setAgents(list); this._resize(); this._layout(); }

    /* ------------------------------------------------------------ actividad real de cada agente */
    setActivity(all) { for (const [id, a] of Object.entries(all || {})) this.onActivity(a); }
    onActivity(a) {
      if (!a || !a.agente) return;
      const prev = this.act[a.agente];
      this.act[a.agente] = { ...a, t: this.t };
      const ch = this.chars.get(a.agente);
      if (!ch) return;
      if (a.estado === "meeting" && !ch.away) this._goOut(ch);
      else if (a.estado !== "meeting" && ch.away) this._comeBack(ch);
      if (a.estado !== "idle" && ch.coffee) this._backToDesk(ch);
      if (a.estado === "thinking" && (!prev || prev.estado !== "thinking")) ch.nextSip = this.t + 2.5;
    }
    _cell(ch) { return this.cells.get(ch.a.id) || { cx: 0, cy: 0 }; }
    _goOut(ch) { const c = this._cell(ch); ch.away = true; ch.coffee = 0; this._walkTo(ch, { x: c.cx + DOOR.x, y: c.cy + DOOR.y }, 0); }
    _comeBack(ch) {
      const c = this._cell(ch); ch.away = false; ch.hidden = false;
      ch.x = c.cx + DOOR.x; ch.y = c.cy + DOOR.y; this._walkTo(ch, ch.seat, 0);
    }
    _toCoffee(ch) { const c = this._cell(ch); ch.coffee = 1; ch.coffeeEnd = 0; this._walkTo(ch, { x: c.cx + COFFEE.x, y: c.cy + COFFEE.y }, 0); }
    _backToDesk(ch) { ch.coffee = 0; this._walkTo(ch, ch.seat, 0); ch.holdMug = this.t + 6; }

    scene(ch) {
      const a = this.act[ch.a.id];
      if (ch.coffee) return "coffee";
      if (!a) return "idle";
      return a.estado === "tool" ? (a.escena || "working") : a.estado || "idle";
    }

    /* ------------------------------------------------------------ animacion */
    _update(dt) {
      this.t += dt; this.meetAmt = 0;
      if (new Date().getHours() !== this.bgHour && this.bg) this._paintBg();   // la luz de la ventana sigue la hora real
      let speaker = null, best = -1;
      for (const [id, b] of this.bubbles) if (this.t - b.t < b.type + 0.4 && b.t > best) { best = b.t; speaker = this.chars.get(id); }
      this.speaker = speaker;
      for (const ch of this.chars.values()) {
        const sc = this.scene(ch);
        ch.walking = false;
        if (ch.path) {
          const p = ch.path; p.t += dt;
          if (p.t >= 0) {
            const k = clamp(p.t / p.dur, 0, 1), e = U.smooth(k), u = 1 - e;
            const nx = u * u * p.from.x + 2 * u * e * p.ctl.x + e * e * p.to.x, ny = u * u * p.from.y + 2 * u * e * p.ctl.y + e * e * p.to.y;
            ch.walkPh += Math.hypot(nx - ch.x, ny - ch.y) * 0.17; ch.vx = (nx - ch.x) / dt; ch.x = nx; ch.y = ny;
            ch.walking = k < 1; ch.sit = damp(ch.sit, 0, 12, dt);
            if (k >= 1) { ch.path = null; if (ch.away) ch.hidden = true; if (ch.coffee) ch.coffeeEnd = this.t + 3.2; }
          }
        } else if (!ch.hidden) ch.sit = damp(ch.sit, ch.coffee ? 0 : 1, 5, dt);
        // cafe: espera a que salga, vuelve a la mesa con la taza
        if (ch.coffee && ch.coffeeEnd && this.t > ch.coffeeEnd) this._backToDesk(ch);
        const b = this.bubbles.get(ch.a.id), talking = (!!b && this.t - b.t < b.type + 0.4) || sc === "phone";
        ch.talk = damp(ch.talk, talking ? (sc === "phone" ? 0.55 + 0.45 * Math.max(0, wob(this.t * 0.9, ch.seed)) : 1) : 0, 7, dt);
        ch.gest = damp(ch.gest, b && this.t - b.t < b.type + 0.4 ? 1 : 0, 3.5, dt);
        // mirada: arriba pensando, a la pantalla de la pared buscando o leyendo, a la puerta al salir
        let lt = wob(this.t * 0.25, ch.seed) * 0.35;
        if (ch.walking) lt = clamp((ch.vx || 0) / 60, -1, 1);
        else if (sc === "thinking") lt = -0.55 + wob(this.t * 0.3, ch.seed) * 0.25;
        else if (["browsing", "reading", "coding", "calculating"].includes(sc)) lt = 0.75;
        else if (sc === "drawing" || sc === "writing") lt = 0.1;
        else if (ch.act === "look") lt = ch.actLook;
        ch.look = damp(ch.look, lt, 3.5, dt);
        // objetivos de cada gesto segun lo que hace
        const seated = !ch.walking && ch.sit > 0.85 && !ch.coffee;
        const typing = seated && ["writing", "coding", "calculating", "working", "browsing", "delegating"].includes(sc) ? (sc === "browsing" ? 0.55 : 1) : 0;
        ch.type = damp(ch.type, Math.max(typing, ch.act === "type" ? 1 : 0), 6, dt);
        ch.chin = damp(ch.chin || 0, seated && sc === "thinking" && ch.sip < 0.2 ? 1 : 0, 3, dt);
        ch.phone = damp(ch.phone || 0, seated && sc === "phone" ? 1 : 0, 4, dt);
        ch.draw = damp(ch.draw || 0, seated && sc === "drawing" ? 1 : 0, 4, dt);
        ch.read = damp(ch.read || 0, seated && sc === "reading" ? 1 : 0, 3, dt);
        ch.music = damp(ch.music || 0, sc === "music" ? 1 : 0, 3, dt);
        ch.nod = damp(ch.nod, sc === "music" || (sc === "phone" && wob(this.t * 0.4, ch.seed) > 0.2) ? 1 : 0, 3, dt);
        // cafe mientras piensa (y de vez en cuando si esta libre)
        if (seated && sc === "thinking" && this.t > (ch.nextSip || 0) && !ch.act) { ch.act = "sip"; ch.actEnd = this.t + 2.6; ch.nextSip = this.t + 7 + Math.random() * 6; }
        if (seated && sc === "idle" && !ch.act && this.t > ch.nextAct) {
          const r = Math.random();
          if (r < 0.12 && this.t > 20) this._toCoffee(ch);
          else if (r < 0.45) { ch.act = "sip"; ch.actEnd = this.t + 2.6; }
          else if (r < 0.7) { ch.act = "type"; ch.actEnd = this.t + 2 + Math.random() * 3; }
          else { ch.act = "look"; ch.actLook = Math.random() * 2 - 1; ch.actEnd = this.t + 1.5 + Math.random() * 2; }
        }
        if (ch.act && (this.t > ch.actEnd || ch.walking || (ch.act !== "sip" && sc !== "idle"))) { ch.act = null; ch.nextAct = this.t + 4 + Math.random() * 8; }
        ch.sip = damp(ch.sip, ch.act === "sip" ? 1 : 0, 4.5, dt);
        if (this.t > ch.nextBlink) { ch.blinkAt = this.t; ch.nextBlink = this.t + 1.8 + Math.random() * 4; }
        ch.alert = Math.max(0, ch.alert - dt);
        ch.P = this._pose(ch);
        if (ch.read > 0.01) ch.P.headY += 1.6 * ch.read;          // la cabeza baja hacia los papeles
        if (ch.music > 0.01) ch.P.headY += Math.max(0, Math.sin(this.t * 7.2 + ch.seed)) * 1.4 * ch.music;
      }
      for (const p of this.planes) p.p += dt / 1.6;
      this.planes = this.planes.filter((p) => p.p < 1);
      this.pops = this.pops.filter((p) => this.t - p.t < 2.8);
    }

    _arm(ch, sd) {
      const A = super._arm(ch, sd), P = ch.P, t = this.t, main = 1;
      const abs = (x, y) => ({ x, y });
      if (sd === main && ch.chin > 0.01) {           // mano en la barbilla, codo en la mesa
        A.e = lerpP(A.e, abs(A.sh.x + sd * 7, A.sh.y + 22), ch.chin);
        A.h = lerpP(A.h, abs(P.headX + sd * 2.5, P.headY + 12.5), ch.chin);
      }
      if (sd === -main && ch.phone > 0.01) {         // telefono a la oreja
        A.e = lerpP(A.e, abs(A.sh.x + sd * 9, A.sh.y + 13), ch.phone);
        A.h = lerpP(A.h, abs(P.headX + sd * 9.5, P.headY + 3), ch.phone);
      }
      if (sd === main && ch.draw > 0.01) {           // lapiz en la tableta
        A.h = lerpP(A.h, abs(A.sh.x - 9 + Math.cos(t * 3.1 + ch.seed) * 4.5, A.sh.y + 30 + Math.sin(t * 4.7) * 1.6), ch.draw);
      }
      if (ch.read > 0.01) A.h = lerpP(A.h, abs(A.sh.x - sd * 7, A.sh.y + 27 - (sd === main ? Math.max(0, Math.sin(t * 0.7)) * 3 : 0)), ch.read);
      if (ch.holdMug > t && sd === main && ch.walking) A.h = lerpP(A.h, abs(A.sh.x + 2, A.sh.y + 20), 1);
      return A;
    }

    _drawArms(ctx, ch) {
      super._drawArms(ctx, ch);
      const s = ch.s;
      const at = (sd) => { const A = this._arm(ch, sd); return { x: ch.x + A.h.x * s, y: ch.y + A.h.y * s }; };
      if (ch.phone > 0.3) {
        const h = at(-1); ctx.save(); ctx.translate(h.x, h.y); ctx.rotate(-0.35);
        ctx.fillStyle = "#1c1f26"; ctx.beginPath(); ctx.roundRect(-2.6 * s, -6 * s, 5.2 * s, 10 * s, 1.4 * s); ctx.fill();
        ctx.fillStyle = rgba(ch.a.color || "#00f0ff", 0.7); ctx.fillRect(-1.8 * s, -4.8 * s, 3.6 * s, 6.6 * s); ctx.restore();
      }
      if (ch.draw > 0.3) {
        const h = at(1); ctx.strokeStyle = "#2c2c34"; ctx.lineWidth = 1.3 * s; ctx.lineCap = "round";
        ctx.beginPath(); ctx.moveTo(h.x, h.y); ctx.lineTo(h.x + 4 * s, h.y + 6 * s); ctx.stroke();
      }
      if (ch.holdMug > this.t && ch.walking) { const h = at(1); this._mug(ctx, h.x + 2, h.y + 3 * s, s, true); }
    }

    _drawHead(ctx, ch, back, t) {
      super._drawHead(ctx, ch, back, t);
      if (ch.music > 0.3 && !back) {                  // cascos
        const s = ch.s, P = ch.P;
        ctx.save(); ctx.translate(ch.x + P.headX * s, ch.y + P.headY * s); ctx.rotate(P.tilt); ctx.scale(s, s);
        ctx.strokeStyle = "#23252c"; ctx.lineWidth = 2.2; ctx.beginPath(); ctx.arc(0, -1, 12.6, Math.PI * 1.08, Math.PI * 1.92); ctx.stroke();
        ctx.fillStyle = "#2c2f38"; for (const sd of [-1, 1]) { ctx.beginPath(); ctx.ellipse(sd * 11.2, 1.5, 3.1, 4.6, 0, 0, 7); ctx.fill(); }
        ctx.fillStyle = ch.a.color || "#00f0ff"; for (const sd of [-1, 1]) { ctx.beginPath(); ctx.ellipse(sd * 12.2, 1.5, 1, 2.4, 0, 0, 7); ctx.fill(); }
        ctx.restore();
      }
    }

    /* ------------------------------------------------------------ dibujo */
    _draw(dt) {
      const ctx = this.ctx, d = this.d, sc = this.sc;
      ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.drawImage(this.bg, 0, 0);
      ctx.setTransform(d * sc, 0, 0, d * sc, 0, 0);
      for (const ch of this.chars.values()) {
        const c = this._cell(ch), sceneName = this.scene(ch);
        ctx.save(); ctx.beginPath(); ctx.rect(c.cx, c.cy, CW, CH); ctx.clip();
        this._screen(ctx, c, ch, sceneName);
        this._coffeeMachine(ctx, c, ch);
        if (ch.away) this._doorSign(ctx, c);
        const layer = [{ y: c.cy + SEAT.y - 0.5, f: () => this._drawChair(ctx, c.cx + SEAT.x, c.cy + SEAT.y, ch.s, false) },
                       { y: c.cy + DESK.front, f: () => this._desk(ctx, c, ch, sceneName) }];
        if (!ch.hidden) layer.push({ y: ch.walking || ch.coffee ? ch.y : c.cy + SEAT.y, f: () => this._drawBody(ctx, ch) });
        layer.sort((p, q) => p.y - q.y).forEach((o) => o.f());
        if (!ch.hidden && ch.sit >= 0.5) this._drawArms(ctx, ch);
        if (!ch.hidden) this._extras(ctx, ch, sceneName);
        this._statusBar(ctx, c, ch, sceneName);
        ctx.restore();
      }
      for (const p of this.planes) this._drawPlane(ctx, p);
      for (const ch of this.chars.values()) {
        if (ch.hidden || !ch.P) continue;
        const col = ch.alert > 0 ? mix(U.GREEN, U.AMBER, Math.min(1, ch.alert)) : U.GREEN;
        U.drawPlumbob(ctx, ch.x + ch.P.headX * ch.s, U.headTop(ch) - 13 * ch.s + Math.sin(this.t * 2 + ch.seed) * 1.6, ch.s * 0.8, this.t * 1.4 + ch.seed, col, ch.talk);
      }
      for (const p of this.pops) this._drawBulb(ctx, p);
      ctx.setTransform(d, 0, 0, d, 0, 0);
      this._drawBubbles(ctx, dt);
    }

    _desk(ctx, c, ch, sceneName) {
      const x0 = c.cx + DESK.x0, x1 = c.cx + DESK.x1, top = c.cy + DESK.top, fr = c.cy + DESK.front, bot = c.cy + DESK.bottom;
      ctx.fillStyle = "rgba(0,0,0,.22)"; ctx.beginPath(); ctx.ellipse((x0 + x1) / 2, bot + 2, (x1 - x0) / 2 + 6, 5, 0, 0, 7); ctx.fill();
      const g = ctx.createLinearGradient(0, top, 0, fr); g.addColorStop(0, "#c89462"); g.addColorStop(1, "#9c6a3e");
      ctx.fillStyle = g; ctx.beginPath(); ctx.moveTo(x0 + 6, top); ctx.lineTo(x1 - 6, top); ctx.lineTo(x1, fr); ctx.lineTo(x0, fr); ctx.closePath(); ctx.fill();
      const fg = ctx.createLinearGradient(x0, 0, x1, 0); fg.addColorStop(0, "#7d5030"); fg.addColorStop(1, "#5b371f");
      ctx.fillStyle = fg; ctx.fillRect(x0, fr, x1 - x0, bot - fr);
      ctx.fillStyle = "rgba(255,230,190,.25)"; ctx.fillRect(x0, fr, x1 - x0, 1.4);
      ctx.strokeStyle = "rgba(0,0,0,.18)"; ctx.lineWidth = 1; ctx.strokeRect(x0 + 8, fr + 6, 30, bot - fr - 12); ctx.strokeRect(x1 - 38, fr + 6, 30, bot - fr - 12);
      // portatil (tapa hacia nosotros) con el logo que brilla mas al escribir
      const lx = c.cx + SEAT.x - 2, ly = top + 9, s = 1.15;
      ctx.fillStyle = "#9aa3ae"; ctx.beginPath(); ctx.moveTo(lx - 15 * s, ly + 2); ctx.lineTo(lx + 15 * s, ly + 2); ctx.lineTo(lx + 13 * s, ly - 1); ctx.lineTo(lx - 13 * s, ly - 1); ctx.closePath(); ctx.fill();
      ctx.fillStyle = "#cfd5dd"; ctx.beginPath(); ctx.roundRect(lx - 14 * s, ly - 17 * s, 28 * s, 17 * s, 2 * s); ctx.fill();
      ctx.fillStyle = rgba(ch.a.color || "#00f0ff", 0.9); ctx.shadowColor = ch.a.color || "#00f0ff"; ctx.shadowBlur = 5 + 7 * ch.type;
      ctx.beginPath(); ctx.arc(lx, ly - 8.5 * s, 2.4 * s, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      // papeles al leer, tableta al dibujar, taza si no la tiene en la mano
      if (sceneName === "reading" || ch.read > 0.2) {
        ctx.fillStyle = "#fffdf6"; ctx.save(); ctx.translate(lx - 36, top + 5); ctx.rotate(-0.12); ctx.fillRect(-11, -6, 22, 13); ctx.restore();
        ctx.fillStyle = "#f3eee2"; ctx.save(); ctx.translate(lx - 32, top + 3); ctx.rotate(0.08); ctx.fillRect(-11, -6, 22, 13); ctx.restore();
      }
      if (sceneName === "drawing" || ch.draw > 0.2) { ctx.fillStyle = "#2a2d36"; ctx.beginPath(); ctx.roundRect(lx + 18, top + 2, 26, 9, 2); ctx.fill(); ctx.fillStyle = rgba(ch.a.color || "#00f0ff", 0.35); ctx.fillRect(lx + 20, top + 3.5, 22, 6); }
      if (ch.sip < 0.35 && !(ch.holdMug > this.t && ch.walking) && !ch.coffee) this._mug(ctx, x1 - 18, top + 9, 1.1, this.scene(ch) === "thinking");
      // placa con el nombre
      const name = String(ch.a.name || ch.a.id).split(/\s+/)[0].slice(0, 14).toUpperCase();
      ctx.font = `800 8.5px ${FONT}`; const w = ctx.measureText(name).width + 12, px = (x0 + x1) / 2;
      ctx.fillStyle = "#fbf7ef"; ctx.beginPath(); ctx.roundRect(px - w / 2, fr + 9, w, 12, 2); ctx.fill();
      ctx.fillStyle = ch.a.color || "#00f0ff"; ctx.fillRect(px - w / 2, fr + 19, w, 2);
      ctx.fillStyle = tone(ch.a.color || "#00f0ff", -0.45); ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(name, px, fr + 15);
    }

    _screen(ctx, c, ch, sceneName) {
      const x0 = c.cx + SCREEN.x0 + 4, y0 = c.cy + SCREEN.y0 + 4, w = SCREEN.x1 - SCREEN.x0 - 8, h = SCREEN.y1 - SCREEN.y0 - 8, t = this.t;
      const col = ch.a.color || "#00f0ff", on = sceneName !== "idle" && sceneName !== "meeting" && sceneName !== "coffee";
      ctx.fillStyle = on ? "#0d1420" : "#06090f"; ctx.fillRect(x0, y0, w, h);
      if (!on) {
        ctx.fillStyle = rgba(col, 0.5); ctx.font = `700 14px ${FONT}`; ctx.textAlign = "center"; ctx.textBaseline = "middle";
        ctx.fillText(ch.a.icon || "◆", x0 + w / 2, y0 + h / 2 + Math.sin(t * 0.8) * 2); return;
      }
      ctx.save(); ctx.beginPath(); ctx.rect(x0, y0, w, h); ctx.clip();
      const r = rng(hash(ch.a.id + sceneName));
      if (sceneName === "browsing") {
        ctx.fillStyle = "#e9edf3"; ctx.fillRect(x0, y0, w, h); ctx.fillStyle = "#c9d1dc"; ctx.fillRect(x0, y0, w, 7);
        ctx.fillStyle = "#fff"; ctx.fillRect(x0 + 12, y0 + 1.5, w - 24, 4);
        const off = (t * 9) % 20;
        for (let i = 0; i < 9; i++) { const y = y0 + 11 + i * 10 - off; ctx.fillStyle = i % 3 ? "#9aa5b5" : rgba(col, 0.9); ctx.fillRect(x0 + 6, y, (w - 12) * (0.4 + r() * 0.55), i % 3 ? 2.5 : 4); if (i % 4 === 1) { ctx.fillStyle = "#b8c4d6"; ctx.fillRect(x0 + w - 30, y - 2, 22, 14); } }
      } else if (sceneName === "reading") {
        ctx.fillStyle = "#f6f2e8"; ctx.fillRect(x0 + 8, y0 + 4, w / 2 - 10, h - 8); ctx.fillRect(x0 + w / 2 + 2, y0 + 4, w / 2 - 10, h - 8);
        ctx.fillStyle = "#8c8578"; for (let i = 0; i < 9; i++) for (const k of [0, 1]) ctx.fillRect(x0 + 11 + k * (w / 2 - 6), y0 + 9 + i * 6.4, (w / 2 - 18) * (0.6 + r() * 0.4), 1.6);
        const flip = (t * 0.35) % 1; if (flip < 0.25) { ctx.fillStyle = "rgba(255,255,255,.75)"; ctx.fillRect(x0 + w / 2 + 2, y0 + 4, (w / 2 - 10) * (1 - flip * 4), h - 8); }
      } else if (sceneName === "drawing") {
        ctx.fillStyle = "#fbfaf6"; ctx.fillRect(x0, y0, w, h);
        const n = Math.floor((t * 6) % 90);
        for (let i = 0; i < n; i++) { const a = i * 0.37 + r() * 0.2; ctx.strokeStyle = [col, "#ff6b8b", "#ffd166", "#3fd65a", "#5b7cc9"][i % 5]; ctx.lineWidth = 1.5 + (i % 3);
          ctx.beginPath(); ctx.arc(x0 + w / 2 + Math.cos(a) * (8 + i * 0.35), y0 + h / 2 + Math.sin(a * 1.3) * (6 + i * 0.22), 2 + (i % 4), a, a + 1.6); ctx.stroke(); }
      } else if (sceneName === "music") {
        ctx.fillStyle = "#151b28"; ctx.fillRect(x0, y0, w, h);
        for (let i = 0; i < 18; i++) { const v = 0.25 + 0.75 * Math.abs(Math.sin(t * (2 + i * 0.37) + i)); ctx.fillStyle = mix(col, "#ff6b8b", i / 18); ctx.fillRect(x0 + 4 + i * (w - 8) / 18, y0 + h - 6 - v * (h - 16), (w - 8) / 18 - 1.5, v * (h - 16)); }
      } else if (sceneName === "phone") {
        ctx.fillStyle = "#121a26"; ctx.fillRect(x0, y0, w, h);
        ctx.fillStyle = rgba(col, 0.9); ctx.font = `800 9px ${FONT}`; ctx.textAlign = "center"; ctx.fillText("IA ONLINE", x0 + w / 2, y0 + 14);
        for (let i = 0; i < 3; i++) { const k = (t * 0.8 + i / 3) % 1; ctx.strokeStyle = rgba(col, 1 - k); ctx.lineWidth = 1.4; ctx.beginPath(); ctx.arc(x0 + w / 2, y0 + h / 2 + 8, 6 + k * 22, 0, 7); ctx.stroke(); }
      } else if (sceneName === "thinking") {
        ctx.fillStyle = "#101826"; ctx.fillRect(x0, y0, w, h);
        const nodes = Array.from({ length: 9 }, () => ({ x: x0 + 10 + r() * (w - 20), y: y0 + 8 + r() * (h - 16) }));
        ctx.strokeStyle = rgba(col, 0.35); ctx.lineWidth = 0.8; ctx.beginPath(); nodes.forEach((p, i) => { const q = nodes[(i * 4 + 3) % 9]; ctx.moveTo(p.x, p.y); ctx.lineTo(q.x, q.y); }); ctx.stroke();
        nodes.forEach((p, i) => { ctx.fillStyle = rgba(col, 0.4 + 0.6 * Math.max(0, Math.sin(t * 2 + i))); ctx.beginPath(); ctx.arc(p.x, p.y, 2.2, 0, 7); ctx.fill(); });
      } else if (sceneName === "delegating") {
        ctx.fillStyle = "#101826"; ctx.fillRect(x0, y0, w, h);
        const k = (t * 0.6) % 1; ctx.fillStyle = "#fff"; ctx.save(); ctx.translate(x0 + 10 + k * (w - 20), y0 + h - 12 - Math.sin(k * Math.PI) * (h - 24));
        ctx.beginPath(); ctx.moveTo(8, 0); ctx.lineTo(-6, -5); ctx.lineTo(-3, 0); ctx.lineTo(-6, 5); ctx.closePath(); ctx.fill(); ctx.restore();
      } else {   // escribir, programar, calcular, trabajar: texto o codigo que va saliendo
        ctx.fillStyle = sceneName === "coding" ? "#0b1220" : "#f7f8fb"; ctx.fillRect(x0, y0, w, h);
        const lines = Math.floor(t * 2.2) % 14;
        for (let i = 0; i < Math.min(lines + 1, 9); i++) {
          const y = y0 + 7 + i * 7.6, ln = (w - 14) * (0.35 + r() * 0.6) * (i === lines % 9 ? ((t * 2.2) % 1) : 1);
          ctx.fillStyle = sceneName === "coding" ? [col, "#c792ea", "#ffd166", "#7fdbca"][i % 4] : (i ? "#8892a3" : rgba(col, 0.95));
          ctx.fillRect(x0 + 6 + (sceneName === "coding" ? (i % 3) * 6 : 0), y, ln, i || sceneName === "coding" ? 2.4 : 3.6);
        }
        if (sceneName === "calculating") { ctx.strokeStyle = rgba(col, 0.9); ctx.lineWidth = 1.4; ctx.beginPath(); for (let i = 0; i <= 20; i++) { const x = x0 + 6 + i * (w - 12) / 20, y = y0 + h - 10 - (Math.sin(i * 0.7 + t) * 0.5 + 0.5) * 22; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); } ctx.stroke(); }
      }
      ctx.restore();
      ctx.fillStyle = "rgba(255,255,255,.05)"; ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x0 + w * 0.45, y0); ctx.lineTo(x0 + w * 0.2, y0 + h); ctx.lineTo(x0, y0 + h); ctx.fill();
    }

    _coffeeMachine(ctx, c, ch) {
      const x = c.cx + COFFEE.x + 10, y = c.cy + 160;
      ctx.fillStyle = "#3a3f4a"; ctx.beginPath(); ctx.roundRect(x - 13, y - 30, 26, 30, 3); ctx.fill();
      ctx.fillStyle = "#23262e"; ctx.fillRect(x - 9, y - 14, 18, 12);
      ctx.fillStyle = ch.coffee && ch.coffeeEnd > this.t ? "#ff9f43" : "#5fd27a"; ctx.beginPath(); ctx.arc(x + 8, y - 25, 1.6, 0, 7); ctx.fill();
      if (ch.coffee && ch.coffeeEnd > this.t) {
        this._mug(ctx, x, y - 2, 0.85, true);
        ctx.strokeStyle = "#6b3e26"; ctx.lineWidth = 1.2; ctx.beginPath(); ctx.moveTo(x, y - 14); ctx.lineTo(x, y - 9); ctx.stroke();
      }
    }

    _doorSign(ctx, c) {
      const x = c.cx + DOOR.x, y = c.cy + 70;
      ctx.fillStyle = "#fff4c2"; ctx.beginPath(); ctx.roundRect(x - 15, y - 7, 30, 13, 2); ctx.fill();
      ctx.fillStyle = "#6b5a1e"; ctx.font = `800 5.6px ${FONT}`; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText("EN EL SALÓN", x, y - 0.5);
    }

    _extras(ctx, ch, sceneName) {
      const t = this.t, s = ch.s, top = U.headTop(ch);
      if (sceneName === "thinking" && !ch.walking) {      // nube de pensamiento con puntos y engranaje
        const bx = ch.x + 26 * s, by = top - 14 * s;
        ctx.fillStyle = "rgba(255,255,255,.92)";
        ctx.beginPath(); ctx.arc(ch.x + 11 * s, top - 2 * s, 2 * s, 0, 7); ctx.fill();
        ctx.beginPath(); ctx.arc(ch.x + 17 * s, top - 7 * s, 3 * s, 0, 7); ctx.fill();
        ctx.beginPath(); ctx.ellipse(bx, by, 15 * s, 9.5 * s, 0, 0, 7); ctx.fill();
        ctx.fillStyle = tone(ch.a.color || "#00f0ff", -0.35);
        for (let i = 0; i < 3; i++) { const k = Math.max(0, Math.sin(t * 4 - i * 0.7)); ctx.beginPath(); ctx.arc(bx - 6 * s + i * 6 * s, by - k * 1.6, (1.4 + k * 0.6) * s, 0, 7); ctx.fill(); }
      }
      if (sceneName === "music") {                        // notas que suben
        for (let i = 0; i < 3; i++) {
          const k = (t * 0.45 + i / 3) % 1, x = ch.x + (18 + Math.sin(k * 6 + i) * 6) * s, y = top - k * 34 * s;
          ctx.globalAlpha = Math.sin(k * Math.PI); ctx.fillStyle = ch.a.color || "#00f0ff"; ctx.font = `700 ${10 * s}px ${FONT}`; ctx.fillText(i % 2 ? "♪" : "♫", x, y);
        }
        ctx.globalAlpha = 1;
      }
    }

    _statusBar(ctx, c, ch, sceneName) {
      const a = this.act[ch.a.id] || {}, y = c.cy + CH - 22, col = ch.a.color || "#00f0ff";
      ctx.fillStyle = "rgba(8,11,18,.72)"; ctx.fillRect(c.cx, y, CW, 22);
      const busy = sceneName !== "idle" && sceneName !== "coffee";
      ctx.fillStyle = busy ? col : "#7d8696"; ctx.beginPath(); ctx.arc(c.cx + 10, y + 11, busy ? 3 + Math.sin(this.t * 5) * 0.8 : 2.6, 0, 7); ctx.fill();
      ctx.font = `800 8.5px ${FONT}`; ctx.textAlign = "left"; ctx.textBaseline = "middle"; ctx.fillStyle = "#e9edf3";
      const lbl = (LABEL[sceneName] || sceneName).toUpperCase() + (a.herramienta && sceneName !== "idle" ? ` · ${a.herramienta}` : "");
      ctx.fillText(lbl, c.cx + 18, y + 11);
      const lw = ctx.measureText(lbl).width;
      if (a.tarea && busy) {
        ctx.font = `600 8px ${FONT}`; ctx.fillStyle = "#9aa3b2";
        let txt = a.tarea; const max = CW - lw - 30;
        while (txt.length > 4 && ctx.measureText(txt).width > max) txt = txt.slice(0, -2);
        ctx.fillText(txt === a.tarea ? txt : txt + "…", c.cx + 24 + lw, y + 11);
      } else if (a.tareas) { ctx.font = `600 8px ${FONT}`; ctx.fillStyle = "#9aa3b2"; ctx.fillText(`${a.tareas} tareas hechas`, c.cx + 24 + lw, y + 11); }
    }
  }

  /* ---------------------------------------------------------------- decorado fijo de un despacho */
  function paintOffice(b, a, hour) {
    const col = a.color || "#00f0ff", r = rng(hash(a.id + "despacho"));
    const wall = mix(mix(col, "#e8e2d6", 0.82), "#1b2130", 0.12), floor = mix("#8a5a36", col, 0.05);
    // pared y suelo
    const wg = b.createLinearGradient(0, 0, 0, 128); wg.addColorStop(0, tone(wall, -0.1)); wg.addColorStop(1, wall);
    b.fillStyle = wg; b.fillRect(0, 0, CW, 128);
    b.fillStyle = tone(wall, -0.25); b.fillRect(0, 122, CW, 6);
    const fg = b.createLinearGradient(0, 128, 0, CH); fg.addColorStop(0, tone(floor, 0.1)); fg.addColorStop(1, tone(floor, -0.25));
    b.fillStyle = fg; b.fillRect(0, 128, CW, CH - 128);
    b.strokeStyle = "rgba(40,20,8,.22)"; b.lineWidth = 0.8;
    for (let i = 0; i < 7; i++) { const y = 128 + i * 15 + i * i * 0.6; b.beginPath(); b.moveTo(0, y); b.lineTo(CW, y); b.stroke(); }
    for (let i = -6; i < 14; i++) { b.beginPath(); b.moveTo(CW / 2 + i * 26, 128); b.lineTo(CW / 2 + i * 52, CH); b.stroke(); }
    // alfombra bajo la mesa
    b.fillStyle = rgba(col, 0.18); b.beginPath(); b.ellipse(132, 196, 96, 20, 0, 0, 7); b.fill();
    // puerta
    b.fillStyle = "#6e4a2c"; b.fillRect(8, 40, 32, 84); b.fillStyle = "#86603c"; b.fillRect(11, 43, 26, 79);
    b.fillStyle = "#e8c45a"; b.beginPath(); b.arc(33, 86, 1.8, 0, 7); b.fill();
    // ventana con el cielo de la hora real
    const sky = hour >= 21 || hour < 6 ? ["#0b1430", "#1d2b55"] : hour < 8 ? ["#f6a76b", "#9cc4e8"] : hour >= 19 ? ["#f08a5d", "#5b6fa8"] : ["#7fc2f0", "#cfe9fb"];
    const sg = b.createLinearGradient(0, 24, 0, 84); sg.addColorStop(0, sky[0]); sg.addColorStop(1, sky[1]);
    b.fillStyle = "#f2f2f2"; b.fillRect(52, 20, 74, 66); b.fillStyle = sg; b.fillRect(56, 24, 66, 58);
    if (hour >= 21 || hour < 6) { b.fillStyle = "#fffbe0"; for (let i = 0; i < 9; i++) b.fillRect(58 + r() * 62, 26 + r() * 34, 1, 1); b.beginPath(); b.arc(108, 34, 5, 0, 7); b.fill(); }
    else { b.fillStyle = "rgba(255,255,255,.85)"; b.beginPath(); b.ellipse(76, 40, 10, 4, 0, 0, 7); b.ellipse(84, 38, 7, 4, 0, 0, 7); b.fill(); }
    b.fillStyle = "#5b6472"; b.fillRect(56, 66, 66, 16);   // edificios
    for (let i = 0; i < 6; i++) { b.fillStyle = tone("#5b6472", (r() - 0.5) * 0.4); const h = 10 + r() * 22; b.fillRect(56 + i * 11, 82 - h, 10, h); }
    b.strokeStyle = "#f2f2f2"; b.lineWidth = 2.4; b.beginPath(); b.moveTo(89, 24); b.lineTo(89, 82); b.moveTo(56, 53); b.lineTo(122, 53); b.stroke();
    b.fillStyle = "rgba(255,255,255,.14)"; b.beginPath(); b.moveTo(56, 24); b.lineTo(80, 24); b.lineTo(64, 82); b.lineTo(56, 82); b.fill();
    // estanteria con libros
    b.fillStyle = "#7a5232"; b.fillRect(136, 30, 34, 4); b.fillRect(136, 58, 34, 4);
    for (const sy of [30, 58]) { let x = 138; while (x < 166) { const w = 3 + r() * 3, h = 12 + r() * 9; b.fillStyle = mix(["#c0504d", "#4f81bd", "#9bbb59", "#f2c45a", col][Math.floor(r() * 5)], "#333", 0.15); b.fillRect(x, sy - h, w, h); x += w + 0.6; } }
    // pantalla de pared (el contenido se anima encima)
    b.fillStyle = "#1a1d24"; b.beginPath(); b.roundRect(SCREEN.x0, SCREEN.y0, SCREEN.x1 - SCREEN.x0, SCREEN.y1 - SCREEN.y0, 4); b.fill();
    b.fillStyle = "#2a2e38"; b.fillRect((SCREEN.x0 + SCREEN.x1) / 2 - 3, SCREEN.y1, 6, 6);
    // planta
    b.fillStyle = "#a0603a"; b.beginPath(); b.moveTo(146, 128); b.lineTo(162, 128); b.lineTo(160, 142); b.lineTo(148, 142); b.closePath(); b.fill();
    for (let i = 0; i < 9; i++) { const ang = -Math.PI / 2 + (r() - 0.5) * 2.2, len = 14 + r() * 14; b.save(); b.translate(154, 128); b.rotate(ang + Math.PI / 2); b.fillStyle = mix("#3f8f4e", "#7cc46a", r()); b.beginPath(); b.ellipse(0, -len / 2, 3.6, len / 2, 0, 0, 7); b.fill(); b.restore(); }
    // mueble del cafe
    b.fillStyle = "#6a4a30"; b.fillRect(COFFEE.x - 6, 160, 44, 44); b.fillStyle = "#7d593b"; b.fillRect(COFFEE.x - 6, 158, 44, 4);
    b.strokeStyle = "rgba(0,0,0,.2)"; b.strokeRect(COFFEE.x - 2, 166, 36, 32);
    b.fillStyle = "rgba(0,0,0,.25)"; b.fillRect(0, 0, CW, 1); b.fillRect(0, 0, 1, CH); b.fillRect(CW - 1, 0, 1, CH); b.fillRect(0, CH - 1, CW, 1);
  }

  window.OfficeRoom = OfficeRoom;
})();
