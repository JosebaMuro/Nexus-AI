/* Nexus-Office — avatares 3D (Three.js, sin dependencias externas en tiempo de ejecucion).
 *
 *  · "geisha": modelo procedural inspirado en una geisha gotica bajo un eclipse: maquillaje blanco con trazos
 *    negros y rojos, mono negro con kanzashi y cadenas de cuentas que se balancean, tatuajes tribales,
 *    kimono negro y rojo con naipes, pagoda y eclipse negro cuya corona cambia con el estado.
 *  · "glb": cualquier modelo GLB/GLTF/VRM del usuario (reproduce su animacion y usa morphs de boca).
 *
 *  Ambos: respiracion, parpadeo, mirada al puntero, labios sincronizados con la voz (setLevel) y los nodos
 *  del grafo de memoria orbitando como particulas de conocimiento (update(graph)).
 */
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";

const STATE_COLORS = { idle: "#00f0ff", thinking: "#a66bff", tool: "#ffb020", speaking: "#ff2a4a", listening: "#7cff6b" };
const TYPE_COLORS = window.NEXUS_TYPE_COLORS || { concepto: "#5ea8ff" };
const SKIN = "#efe9e3";
const rand = (a, b) => a + Math.random() * (b - a);

function canvasTex(w, h, draw, srgb = true) {
  const c = document.createElement("canvas"); c.width = w; c.height = h;
  draw(c.getContext("2d"), w, h);
  const t = new THREE.CanvasTexture(c);
  if (srgb) t.colorSpace = THREE.SRGBColorSpace;
  t.anisotropy = 4;
  return t;
}

/* ------------------------------------------------------------------ texturas pintadas */
function heart(ctx, x, y, s, color) {
  ctx.fillStyle = color; ctx.beginPath(); ctx.moveTo(x, y + s * 0.9);
  ctx.bezierCurveTo(x - s * 1.4, y - s * 0.1, x - s * 0.6, y - s * 1.1, x, y - s * 0.35);
  ctx.bezierCurveTo(x + s * 0.6, y - s * 1.1, x + s * 1.4, y - s * 0.1, x, y + s * 0.9); ctx.fill();
}
function spade(ctx, x, y, s, color) {
  ctx.save(); ctx.translate(x, y); ctx.rotate(Math.PI); heart(ctx, 0, 0, s, color); ctx.restore();
  ctx.fillStyle = color; ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x - s * 0.35, y + s * 1.1); ctx.lineTo(x + s * 0.35, y + s * 1.1); ctx.fill();
}
function taper(ctx, pts, w0, color) { // trazo que se afila (pincel)
  ctx.fillStyle = color; ctx.beginPath();
  const left = [], right = [];
  for (let i = 0; i < pts.length; i++) {
    const [x, y] = pts[i], [nx, ny] = pts[Math.min(i + 1, pts.length - 1)], [px, py] = pts[Math.max(i - 1, 0)];
    let dx = nx - px, dy = ny - py; const l = Math.hypot(dx, dy) || 1; dx /= l; dy /= l;
    const w = w0 * (1 - i / (pts.length - 1)) ** 0.8;
    left.push([x - dy * w, y + dx * w]); right.push([x + dy * w, y - dx * w]);
  }
  ctx.moveTo(...left[0]); left.forEach((p) => ctx.lineTo(...p)); right.reverse().forEach((p) => ctx.lineTo(...p)); ctx.closePath(); ctx.fill();
}
function curve(x0, y0, cx, cy, x1, y1, n = 14) {
  const out = []; for (let i = 0; i <= n; i++) { const t = i / n, a = (1 - t) ** 2, b = 2 * (1 - t) * t, c = t * t; out.push([a * x0 + b * cx + c * x1, a * y0 + b * cy + c * y1]); } return out;
}

function faceTexture() {
  return canvasTex(1024, 1024, (ctx, W) => {
    ctx.fillStyle = SKIN; ctx.fillRect(0, 0, W, W);
    const g = ctx.createRadialGradient(512, 560, 120, 512, 560, 520); g.addColorStop(0, "rgba(255,255,255,0.10)"); g.addColorStop(1, "rgba(150,140,140,0.10)");
    ctx.fillStyle = g; ctx.fillRect(0, 0, W, W);
    const mirror = (fn) => { fn(1); ctx.save(); ctx.translate(W, 0); ctx.scale(-1, 1); fn(-1); ctx.restore(); };
    // sombras de pomulos
    mirror(() => { const cg = ctx.createRadialGradient(300, 690, 10, 300, 690, 150); cg.addColorStop(0, "rgba(90,80,85,0.22)"); cg.addColorStop(1, "rgba(90,80,85,0)"); ctx.fillStyle = cg; ctx.fillRect(140, 540, 330, 320); });
    // ornamento de la frente (filigrana negra con corazon rojo)
    ctx.save(); ctx.translate(512, 300);
    mirror(() => { taper(ctx, curve(0, 10, 70, -60, 150, -10), 9, "#0b0b0e"); taper(ctx, curve(10, 30, 90, 10, 120, 70), 6, "#0b0b0e"); taper(ctx, curve(0, -10, 30, -90, 90, -110), 5, "#0b0b0e"); });
    ctx.restore();
    ctx.save(); ctx.translate(512, 300);
    ctx.fillStyle = "#0b0b0e"; ctx.beginPath(); ctx.moveTo(0, -70); ctx.lineTo(34, 0); ctx.lineTo(0, 70); ctx.lineTo(-34, 0); ctx.fill();
    heart(ctx, 0, 6, 26, "#b3122a"); ctx.restore();
    [[455, 240], [569, 240], [512, 205]].forEach(([x, y]) => { ctx.fillStyle = "#b3122a"; ctx.beginPath(); ctx.arc(x, y, 9, 0, 7); ctx.fill(); });
    // sombra ahumada alrededor de los ojos
    mirror(() => { const sg = ctx.createRadialGradient(325, 505, 30, 325, 505, 150); sg.addColorStop(0, "rgba(25,22,28,0.6)"); sg.addColorStop(0.6, "rgba(60,20,30,0.25)"); sg.addColorStop(1, "rgba(60,20,30,0)");
      ctx.fillStyle = sg; ctx.beginPath(); ctx.ellipse(325, 500, 150, 95, 0, 0, 7); ctx.fill(); });
    // cejas afiladas
    mirror(() => taper(ctx, curve(445, 418, 360, 360, 238, 400), 11, "#0d0d10"));
    // ojos: esclerotica, iris, delineado con ala y trazos radiales
    mirror(() => {
      const ex = 325, ey = 512;
      ctx.fillStyle = "#e9e6e6"; ctx.beginPath(); ctx.ellipse(ex, ey, 68, 26, 0, 0, 7); ctx.fill();
      ctx.save(); ctx.beginPath(); ctx.ellipse(ex, ey, 68, 26, 0, 0, 7); ctx.clip();
      ctx.fillStyle = "#3a3c42"; ctx.beginPath(); ctx.arc(ex + 6, ey + 2, 25, 0, 7); ctx.fill();
      ctx.fillStyle = "#0a0a0c"; ctx.beginPath(); ctx.arc(ex + 6, ey + 2, 11, 0, 7); ctx.fill();
      ctx.fillStyle = "#fff"; ctx.beginPath(); ctx.arc(ex - 2, ey - 7, 5, 0, 7); ctx.fill();
      ctx.fillStyle = "rgba(0,0,0,0.35)"; ctx.fillRect(ex - 80, ey - 30, 160, 14); ctx.restore();
      ctx.strokeStyle = "#08080a"; ctx.lineWidth = 10; ctx.beginPath(); ctx.ellipse(ex, ey, 70, 28, 0, Math.PI * 1.02, Math.PI * 1.98); ctx.stroke();
      ctx.lineWidth = 5; ctx.beginPath(); ctx.ellipse(ex, ey, 70, 28, 0, 0.1, Math.PI - 0.1); ctx.stroke();
      taper(ctx, curve(ex - 66, ey - 6, ex - 120, ey - 30, ex - 170, ey - 70), 12, "#08080a");      // ala
      [[-40, 90, -70, 175], [-10, 100, -18, 190], [20, 95, 40, 180], [-62, 70, -120, 130], [45, 80, 80, 150]].forEach(([dx, dy, ex2, ey2]) =>
        taper(ctx, curve(ex + dx * 0.4, ey + 26, ex + dx, ey + dy * 0.6, ex + ex2, ey + ey2), 7, "#0a0a0d"));     // trazos bajo el ojo
      const rg = ctx.createRadialGradient(ex + 70, ey + 40, 4, ex + 70, ey + 40, 60); rg.addColorStop(0, "rgba(170,10,30,0.55)"); rg.addColorStop(1, "rgba(170,10,30,0)");
      ctx.fillStyle = rg; ctx.fillRect(ex, ey - 20, 140, 120);
    });
    heart(ctx, 360, 600, 15, "#b3122a");   // corazon rojo bajo el ojo
    // nariz
    ctx.strokeStyle = "rgba(80,70,75,0.45)"; ctx.lineWidth = 4;
    ctx.beginPath(); ctx.moveTo(492, 520); ctx.quadraticCurveTo(478, 640, 486, 668); ctx.stroke();
    ctx.fillStyle = "rgba(60,50,55,0.55)"; ctx.beginPath(); ctx.ellipse(490, 676, 9, 5, 0.3, 0, 7); ctx.fill(); ctx.beginPath(); ctx.ellipse(534, 676, 9, 5, -0.3, 0, 7); ctx.fill();
    heart(ctx, 512, 712, 11, "#b3122a");
    // sonrisa cosida negra que cruza las mejillas
    mirror(() => {
      const pts = curve(452, 800, 340, 790, 250, 700, 18); taper(ctx, pts, 7, "#0a0a0d");
      ctx.strokeStyle = "#0a0a0d"; ctx.lineWidth = 4;
      for (let i = 2; i < pts.length - 2; i += 2) { const [x, y] = pts[i]; ctx.beginPath(); ctx.moveTo(x - 4, y - 14); ctx.lineTo(x + 4, y + 14); ctx.stroke(); }
      taper(ctx, curve(470, 860, 430, 900, 380, 930), 8, "#0a0a0d");
    });
    // ensombrecido del menton
    ctx.fillStyle = "rgba(40,30,35,0.25)"; ctx.beginPath(); ctx.ellipse(512, 905, 70, 18, 0, 0, 7); ctx.fill();
  });
}

function tattooTexture() {
  // patron tribal simetrico (se repite 2 veces alrededor del torso; el centro del lienzo queda en el frente)
  return canvasTex(1024, 1024, (ctx, W, H) => {
    ctx.fillStyle = SKIN; ctx.fillRect(0, 0, W, H);
    const half = () => {
      // llamas tribales que ascienden en diagonal desde el centro, con espirales y remates en gancho
      for (let i = 0; i < 7; i++) {
        const y = 140 + i * 128 + (i % 2) * 30, x = 480 - (i % 3) * 25;
        taper(ctx, curve(x, y + 70, x - 90, y - 10, x - 230 - (i % 2) * 40, y - 150), 30 - i * 1.5, "#121216");
        taper(ctx, curve(x - 40, y + 90, x - 150, y + 60, x - 250, y - 20), 14, "#121216");
        const sx = x - 230 - (i % 2) * 40, sy = y - 150;
        ctx.strokeStyle = "#121216"; ctx.lineWidth = 8; ctx.beginPath(); ctx.arc(sx - 18, sy + 22, 26, -0.6, 3.9); ctx.stroke();
        ctx.lineWidth = 5; ctx.beginPath(); ctx.arc(sx - 18, sy + 22, 11, -0.3, 3.6); ctx.stroke();
      }
      for (let k = 0; k < 18; k++) { ctx.fillStyle = "#121216"; ctx.beginPath(); ctx.arc(120 + (k * 53) % 330, 60 + k * 53, 4 + (k % 3), 0, 7); ctx.fill(); }
      heart(ctx, 410, 520, 16, "#a3101f");
    };
    half(); ctx.save(); ctx.translate(W, 0); ctx.scale(-1, 1); half(); ctx.restore();
  });
}

function kimonoTexture() {
  // u: alrededor del cuerpo (0 = frente), v: de abajo (0) a arriba (1). Canal alfa: escote en V abierto.
  return canvasTex(2048, 1024, (ctx, W, H) => {
    ctx.fillStyle = "#0b0a0d"; ctx.fillRect(0, 0, W, H);
    for (let i = 0; i < 7; i++) { // barridos rojos diagonales
      ctx.fillStyle = i % 2 ? "rgba(150,10,25,0.85)" : "rgba(110,5,18,0.75)";
      const x = i * 330 - 200; ctx.beginPath(); ctx.moveTo(x, H); ctx.lineTo(x + 140, H); ctx.lineTo(x + 520, 0); ctx.lineTo(x + 420, 0); ctx.fill();
    }
    ctx.strokeStyle = "rgba(90,90,100,0.25)"; ctx.lineWidth = 3; // olas seigaiha
    for (let y = 60; y < H; y += 70) for (let x = 0; x < W; x += 80) { ctx.beginPath(); ctx.arc(x + (y / 70 % 2) * 40, y, 32, Math.PI, 0); ctx.stroke(); }
    for (let i = 0; i < 46; i++) { // naipes
      const x = rand(40, W - 40), y = rand(40, H - 60), r = rand(-0.6, 0.6), s = rand(0.7, 1.2);
      ctx.save(); ctx.translate(x, y); ctx.rotate(r); ctx.scale(s, s);
      ctx.fillStyle = "#e9e4dc"; ctx.strokeStyle = "#111"; ctx.lineWidth = 2; ctx.beginPath(); ctx.roundRect(-26, -38, 52, 76, 6); ctx.fill(); ctx.stroke();
      (i % 2 ? heart : spade)(ctx, 0, 0, 13, i % 2 ? "#b3122a" : "#111");
      ctx.restore();
    }
    for (let i = 0; i < 20; i++) { // flores rojas
      const x = rand(0, W), y = rand(0, H), r = rand(20, 46); ctx.fillStyle = "#c01530";
      for (let p = 0; p < 5; p++) { ctx.beginPath(); ctx.ellipse(x + Math.cos(p * 1.256) * r * 0.6, y + Math.sin(p * 1.256) * r * 0.6, r * 0.45, r * 0.3, p * 1.256, 0, 7); ctx.fill(); }
      ctx.fillStyle = "#16090c"; ctx.beginPath(); ctx.arc(x, y, r * 0.2, 0, 7); ctx.fill();
    }
    // obi (franja inferior) y cordon
    ctx.fillStyle = "#7a0a16"; ctx.fillRect(0, H * 0.8, W, H * 0.2);
    ctx.fillStyle = "#0b0a0d"; for (let x = 0; x < W; x += 60) { ctx.beginPath(); ctx.moveTo(x, H * 0.8); ctx.lineTo(x + 30, H * 0.9); ctx.lineTo(x, H); ctx.fill(); }
    ctx.fillStyle = "#e0b04a"; ctx.fillRect(0, H * 0.845, W, 6);
    // escote en V: collar rojo y transparencia (frente en u=0 y u=1)
    const vEdge = (yy) => { const v = 1 - yy / H; return v < 0.38 ? 0 : 0.015 + 0.13 * Math.min(1, (v - 0.38) / 0.62); };
    for (let y = 0; y < H * 0.62; y += 2) {
      const w = vEdge(y) * W;
      if (!w) continue;
      ctx.fillStyle = "#c3142e"; ctx.fillRect(0, y, w + 46, 2); ctx.fillRect(W - w - 46, y, w + 46, 2);   // collar (eri)
      ctx.fillStyle = "#0b0a0d"; ctx.fillRect(w + 30, y, 8, 2); ctx.fillRect(W - w - 38, y, 8, 2);
      ctx.clearRect(0, y, w, 2); ctx.clearRect(W - w, y, w, 2);
    }
  });
}

function skyTexture() {
  return canvasTex(1024, 1024, (ctx, W, H) => {
    const g = ctx.createLinearGradient(0, 0, 0, H); g.addColorStop(0, "#0d0f14"); g.addColorStop(0.55, "#2a2d35"); g.addColorStop(1, "#14151a");
    ctx.fillStyle = g; ctx.fillRect(0, 0, W, H);
    for (let i = 0; i < 90; i++) {
      const x = rand(0, W), y = rand(0, H * 0.7), r = rand(40, 180), c = ctx.createRadialGradient(x, y, 0, x, y, r);
      c.addColorStop(0, `rgba(${rand(60, 100) | 0},${rand(62, 102) | 0},${rand(70, 110) | 0},0.16)`); c.addColorStop(1, "rgba(0,0,0,0)");
      ctx.fillStyle = c; ctx.fillRect(x - r, y - r, r * 2, r * 2);
    }
  });
}

function glowTexture() {
  return canvasTex(256, 256, (ctx) => {
    const g = ctx.createRadialGradient(128, 128, 40, 128, 128, 128);
    g.addColorStop(0, "rgba(255,255,255,1)"); g.addColorStop(0.42, "rgba(255,255,255,0.85)"); g.addColorStop(0.55, "rgba(255,255,255,0.25)"); g.addColorStop(1, "rgba(255,255,255,0)");
    ctx.fillStyle = g; ctx.fillRect(0, 0, 256, 256);
  });
}

// orienta un objeto segun una normal local (lookAt trabaja en coordenadas de mundo)
function face2(obj, normal) {
  obj.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), (normal || obj.position).clone().normalize());
}

/* ------------------------------------------------------------------ base comun */
class Base3D {
  constructor(container) {
    this.container = container;
    // en moviles y tablets modestos: sin antialias y menos resolucion (mas fluido, menos bateria y calor)
    const low = !!(window.NexusDevice && NexusDevice.lowPower);
    this.renderer = new THREE.WebGLRenderer({ antialias: !low, alpha: true, powerPreference: low ? "low-power" : "high-performance" });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, low ? 1 : 1.5));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.05;
    this.canvas = this.renderer.domElement; this.canvas.className = "avatar3d";
    container.insertBefore(this.canvas, container.firstChild);  // bajo las capas HUD del contenedor
    this.scene = new THREE.Scene();
    this.camera = new THREE.PerspectiveCamera(30, 1, 0.05, 100);
    this.clock = new THREE.Clock();
    this.state = "idle"; this.level = 0; this.levelS = 0; this.mouse = new THREE.Vector2(); this.accent = "#00f0ff";
    this.stateColor = new THREE.Color(STATE_COLORS.idle); this.targetColor = this.stateColor.clone();
    this.motes = null; this.moteData = []; this.burst = 0;
    this.onMove = (e) => { const r = container.getBoundingClientRect(); this.mouse.set(((e.clientX - r.left) / r.width) * 2 - 1, ((e.clientY - r.top) / r.height) * 2 - 1); };
    window.addEventListener("pointermove", this.onMove);
    this.ro = new ResizeObserver(() => this.resize()); this.ro.observe(container);
    this.resize();
    this.loop = this.loop.bind(this); this.raf = requestAnimationFrame(this.loop);
  }
  resize() {
    const r = this.container.getBoundingClientRect(); if (!r.width || !r.height) return;
    this.renderer.setSize(r.width, r.height, false); this.camera.aspect = r.width / r.height; this.camera.updateProjectionMatrix();
    this.frame && this.frame(r.width / r.height);
  }
  /* ---- autoencuadre: el personaje cabe entero en el hueco del avatar, sea una columna alta, una franja ancha del
     movil o una ventana pequeña, y sin quedar tapado por los rotulos de arriba (HUD) y de abajo (estado) */
  safeInsets() {
    const r = this.container.getBoundingClientRect(); let top = 0, bottom = 0;
    for (const el of this.container.querySelectorAll(".avatar-hud, .avatar-state")) {
      const b = el.getBoundingClientRect(); if (!b.height || !b.width) continue;
      if (b.top + b.height / 2 - r.top < r.height / 2) top = Math.max(top, b.bottom - r.top + 4); else bottom = Math.max(bottom, r.bottom - b.top + 4);
    }
    return { w: r.width, h: r.height, top: Math.min(top, r.height * 0.25), bottom: Math.min(bottom, r.height * 0.25) };
  }
  fitBox(box, margin = 0.08) {
    const ins = this.safeInsets(); if (!ins.w || !ins.h || box.isEmpty()) return;
    const size = box.getSize(new THREE.Vector3()), c = box.getCenter(new THREE.Vector3());
    const aspect = ins.w / ins.h, usable = Math.max(0.4, 1 - (ins.top + ins.bottom) / ins.h);
    const tv = Math.tan(THREE.MathUtils.degToRad(this.camera.fov / 2));
    const dV = (size.y / 2) * (1 + margin) / (tv * usable), dH = (size.x / 2) * (1 + margin) / (tv * aspect);
    const d = Math.max(dV, dH) + size.z / 2;
    // centro del hueco libre (entre los rotulos) en vez del centro del lienzo
    const shift = ((ins.bottom - ins.top) / 2) * (2 * (d - size.z / 2) * tv / ins.h);
    this.camera.position.set(c.x, c.y - shift + d * 0.04, c.z + d);
    this.camera.lookAt(c.x, c.y - shift, c.z);
    this.camera.near = Math.max(0.01, d / 100); this.camera.far = Math.max(100, d * 20);
    this.camera.updateProjectionMatrix();
    this.fitted = { d, aspect, box: [size.x, size.y, size.z] };
  }
  // caja del sujeto segun el encuadre: «busto» (cabeza y hombros) o «cuerpo» (entero)
  subjectBox() {
    const body = this.body || this.model; if (!body) return null;
    body.updateMatrixWorld(true);
    const full = new THREE.Box3().setFromObject(body), H = full.max.y - full.min.y;
    if (this.framing === "cuerpo" || !this.head) {   // sin cabeza conocida no se adivina el busto
      if (this.ring && this.ringBase) {   // que el aro del suelo tambien quepa
        const r = 0.6 * this.ringBase;
        full.expandByPoint(new THREE.Vector3(-r, full.min.y, 0)).expandByPoint(new THREE.Vector3(r, full.min.y, 0));
      }
      return full;
    }
    let head = this.head && !this.head.isBone ? new THREE.Box3().setFromObject(this.head) : null;
    if (!head || head.isEmpty()) {   // modelo propio: el hueso de la cabeza o el 18 % superior
      const hy = this.head ? this.head.getWorldPosition(new THREE.Vector3()).y : full.max.y - H * 0.09;
      head = new THREE.Box3(new THREE.Vector3(-H * 0.06, hy - H * 0.06, -H * 0.06), new THREE.Vector3(H * 0.06, Math.min(full.max.y, hy + H * 0.1), H * 0.06));
    }
    const hh = head.max.y - head.min.y, hw = Math.max(head.max.x - head.min.x, hh * 0.7);
    const cx = (head.min.x + head.max.x) / 2, cz = (head.min.z + head.max.z) / 2;
    const halfW = Math.min(Math.max(hw * 1.5, hh * 1.15), (full.max.x - full.min.x) / 2);
    return new THREE.Box3(new THREE.Vector3(cx - halfW, Math.max(full.min.y, head.min.y - hh * 0.95), cz - hw / 2),
                          new THREE.Vector3(cx + halfW, head.max.y, cz + hw / 2));
  }
  frame() { const b = this.subjectBox(); if (b) this.fitBox(b); }
  toggleFraming() {
    this.framing = this.framing === "busto" ? "cuerpo" : "busto";
    try { localStorage.setItem(this.framingKey || "nexus_avatar_framing", this.framing); } catch {}
    this.frame();
  }
  setState(s) { this.state = STATE_COLORS[s] ? s : "idle"; this.targetColor.set(STATE_COLORS[this.state] === STATE_COLORS.idle ? this.accent : STATE_COLORS[this.state]); }
  setLevel(v) { this.level = Math.max(0, Math.min(1, v)); }
  setAccent(c) { this.accent = c; if (this.state === "idle") this.targetColor.set(c); }
  update(graph) {
    const nodes = (graph.nodes || []).filter((n) => n.type !== "self").slice(0, 90);
    if (this.motes) { this.scene.remove(this.motes); this.motes.geometry.dispose(); this.motes.material.dispose(); }
    const prev = this.moteData.length;
    this.motes = new THREE.InstancedMesh(new THREE.SphereGeometry(1, 10, 8), new THREE.MeshBasicMaterial({ transparent: true, opacity: 0.9 }), Math.max(1, nodes.length));
    this.motes.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    const maxW = Math.max(1, ...nodes.map((n) => n.weight));
    this.moteData = nodes.map((n, i) => ({ r: rand(0.42, 0.85), inc: rand(-0.7, 0.7), sp: rand(0.08, 0.25) * (i % 2 ? 1 : -1), ph: rand(0, 6.28),
                                            y: rand(-0.3, 0.4), s: 0.004 + 0.009 * Math.sqrt(n.weight / maxW) }));
    nodes.forEach((n, i) => this.motes.setColorAt(i, new THREE.Color(TYPE_COLORS[n.type] || TYPE_COLORS.concepto)));
    this.motes.count = nodes.length; this.motes.frustumCulled = false;
    this.scene.add(this.motes);
    if (prev && nodes.length > prev) this.burst = 1;
  }
  animateMotes(t, center) {
    if (!this.motes) return;
    const m = new THREE.Matrix4(), q = new THREE.Quaternion(), s = new THREE.Vector3(), p = new THREE.Vector3();
    const speed = this.state === "thinking" ? 3 : this.state === "tool" ? 2 : 1;
    this.moteData.forEach((d, i) => {
      d.ph += d.sp * 0.016 * speed;
      const r = d.r * (1 + this.burst * 0.4);
      p.set(Math.cos(d.ph) * r, d.y + Math.sin(d.ph) * r * d.inc * 0.4, Math.sin(d.ph) * r * 0.6).add(center);
      const k = d.s * (1 + 0.4 * Math.sin(t * 3 + i) + this.burst);
      s.set(k, k, k); m.compose(p, q, s); this.motes.setMatrixAt(i, m);
    });
    this.motes.instanceMatrix.needsUpdate = true; this.burst *= 0.96;
  }
  loop() {
    this.raf = requestAnimationFrame(this.loop);
    if (document.hidden || !this.renderer || !this.container.offsetWidth) return;   // franja recogida u otra vista: no renderiza
    const dt = Math.min(this.clock.getDelta(), 0.05), t = this.clock.elapsedTime;
    this.levelS += (this.level - this.levelS) * 0.35;
    this.stateColor.lerp(this.targetColor, 0.06);
    this.animate(t, dt);
    this.renderer.render(this.scene, this.camera);
  }
  destroy() {
    if (this.onDbl) this.container.removeEventListener("dblclick", this.onDbl);
    cancelAnimationFrame(this.raf); this.ro.disconnect(); window.removeEventListener("pointermove", this.onMove);
    this.scene.traverse((o) => { o.geometry?.dispose?.(); [].concat(o.material || []).forEach((mt) => { mt.map?.dispose?.(); mt.dispose?.(); }); });
    this.renderer.dispose(); this.canvas.remove(); this.renderer = null;
  }
}

/* ------------------------------------------------------------------ geisha procedural (respaldo sin recursos) */
class ProceduralGeisha extends Base3D {
  constructor(container) {
    super(container);
    this.swing = []; this.blinkAt = 1.5; this.blink = 0;
    this.framing = (() => { try { return localStorage.getItem("nexus_avatar_framing") || "busto"; } catch { return "busto"; } })();
    container.addEventListener("dblclick", this.onDbl = () => this.toggleFraming());
    this.build();
    this.frame();
  }
  // encuadre automatico (Base3D.frame): busto o cuerpo entero, siempre dentro del hueco del avatar
  mat(color, o = {}) { return new THREE.MeshStandardMaterial({ color, roughness: 0.6, metalness: 0.0, ...o }); }
  build() {
    const S = this.scene;
    S.fog = new THREE.Fog(0x15161b, 4, 11);
    // fondo: cielo, eclipse, pagoda, ceniza
    const sky = new THREE.Mesh(new THREE.PlaneGeometry(16, 10), new THREE.MeshBasicMaterial({ map: skyTexture(), fog: false }));
    sky.position.set(0, 3, -7); S.add(sky);
    this.corona = new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTexture(), color: this.stateColor, transparent: true, blending: THREE.AdditiveBlending, depthWrite: false, fog: false }));
    this.corona.position.set(1.15, 2.55, -5.5); this.corona.scale.set(2.1, 2.1, 1); S.add(this.corona);
    const moon = new THREE.Mesh(new THREE.CircleGeometry(0.62, 64), new THREE.MeshBasicMaterial({ color: 0x030304, fog: false }));
    moon.position.set(1.15, 2.55, -5.45); S.add(moon);
    const pag = new THREE.Group(), dark = new THREE.MeshStandardMaterial({ color: 0x17181d, roughness: 0.9 });
    let y = 0;
    for (let i = 0; i < 5; i++) {
      const w = 1.0 - i * 0.15;
      const body = new THREE.Mesh(new THREE.BoxGeometry(w * 0.7, 0.42, w * 0.7), dark); body.position.y = y + 0.21; pag.add(body);
      const roof = new THREE.Mesh(new THREE.ConeGeometry(w * 0.82, 0.24, 4, 1), dark); roof.rotation.y = Math.PI / 4; roof.position.y = y + 0.52; roof.scale.set(1, 1, 1); pag.add(roof);
      y += 0.55;
    }
    const spire = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.035, 0.9, 8), dark); spire.position.y = y + 0.4; pag.add(spire);
    for (let i = 0; i < 6; i++) { const ring = new THREE.Mesh(new THREE.TorusGeometry(0.06, 0.012, 6, 16), dark); ring.rotation.x = Math.PI / 2; ring.position.y = y + 0.1 + i * 0.1; pag.add(ring); }
    pag.position.set(1.7, -0.1, -3.6); pag.scale.setScalar(1.25); S.add(pag);
    const ash = new THREE.BufferGeometry(), N = 260, pos = new Float32Array(N * 3), col = new Float32Array(N * 3);
    for (let i = 0; i < N; i++) { pos.set([rand(-3, 3), rand(0, 4), rand(-4, 1.5)], i * 3); const red = Math.random() < 0.25; col.set(red ? [0.75, 0.08, 0.15] : [0.55, 0.56, 0.6], i * 3); }
    ash.setAttribute("position", new THREE.BufferAttribute(pos, 3)); ash.setAttribute("color", new THREE.BufferAttribute(col, 3));
    this.ash = new THREE.Points(ash, new THREE.PointsMaterial({ size: 0.025, vertexColors: true, transparent: true, opacity: 0.8, depthWrite: false }));
    S.add(this.ash);
    // luces
    S.add(new THREE.HemisphereLight(0x8a96a8, 0x2a1014, 0.75));
    const key = new THREE.DirectionalLight(0xfff4ee, 2.1); key.position.set(-1.6, 2.6, 2.6); S.add(key);
    this.redFill = new THREE.PointLight(0xff1e3c, 1.4, 6); this.redFill.position.set(1.4, 1.3, 1.2); S.add(this.redFill);
    this.rim = new THREE.PointLight(this.stateColor, 6, 5); this.rim.position.set(-0.3, 2.1, -1.2); S.add(this.rim);

    // cuerpo
    this.body = new THREE.Group(); S.add(this.body);
    const tattoo = tattooTexture(); tattoo.wrapS = THREE.RepeatWrapping; tattoo.repeat.x = 2; tattoo.offset.x = 0.5;  // las UV giratorias empiezan en el frente
    const torsoProfile = [[0.001, 0.25], [0.29, 0.25], [0.31, 0.55], [0.33, 0.85], [0.37, 1.02], [0.36, 1.1], [0.26, 1.17], [0.14, 1.22], [0.11, 1.25]].map(([r, y]) => new THREE.Vector2(r, y));
    const torso = new THREE.Mesh(new THREE.LatheGeometry(torsoProfile, 64), this.mat(SKIN, { map: tattoo, roughness: 0.55 }));
    torso.scale.set(1, 1, 0.62); this.body.add(torso);
    const neck = new THREE.Mesh(new THREE.CylinderGeometry(0.082, 0.1, 0.24, 32, 1, true), this.mat(SKIN, { map: tattoo, roughness: 0.55 }));
    neck.position.y = 1.3; neck.scale.z = 0.92; this.body.add(neck);
    const kimonoProfile = [[0.36, 0.0], [0.36, 0.3], [0.345, 0.6], [0.36, 0.86], [0.405, 1.03], [0.4, 1.11], [0.29, 1.18], [0.18, 1.225]].map(([r, y]) => new THREE.Vector2(r, y));
    const kimono = new THREE.Mesh(new THREE.LatheGeometry(kimonoProfile, 96),
      this.mat(0xffffff, { map: kimonoTexture(), transparent: false, alphaTest: 0.5, side: THREE.DoubleSide, roughness: 0.75 }));
    kimono.scale.set(1.04, 1, 0.66); this.body.add(kimono); this.kimono = kimono;
    const obiRope = new THREE.Mesh(new THREE.TorusGeometry(0.37, 0.012, 8, 96), this.mat(0xc01530, { roughness: 0.4 }));
    obiRope.rotation.x = Math.PI / 2; obiRope.scale.set(1.04, 0.66, 1); obiRope.position.y = 0.2; this.body.add(obiRope);

    // cabeza
    this.head = new THREE.Group(); this.head.position.set(0, 1.43, 0.025); this.head.scale.setScalar(0.86); this.body.add(this.head);
    const scaled = new THREE.Group(); scaled.scale.set(0.84, 0.97, 0.9); this.head.add(scaled); this.headScale = scaled.scale;
    const R = 0.17;
    scaled.add(new THREE.Mesh(new THREE.SphereGeometry(R, 64, 48), this.mat(SKIN, { roughness: 0.5 })));
    const P0 = Math.PI / 2 - 0.5, PL = 1.0, T0 = 0.8, TL = 1.32;
    this.uv = (u, v, lift = 1.012) => { const phi = P0 + u * PL, th = T0 + v * TL; return new THREE.Vector3(-R * lift * Math.cos(phi) * Math.sin(th), R * lift * Math.cos(th), R * lift * Math.sin(phi) * Math.sin(th)); };
    const face = new THREE.Mesh(new THREE.SphereGeometry(R * 1.004, 64, 64, P0, PL, T0, TL),
      this.mat(0xffffff, { map: faceTexture(), roughness: 0.5, polygonOffset: true, polygonOffsetFactor: -1 }));
    scaled.add(face);
    // nariz y menton sutiles
    const nose = new THREE.Mesh(new THREE.SphereGeometry(0.018, 16, 12), this.mat(SKIN, { roughness: 0.5 }));
    nose.position.copy(this.uv(0.5, 0.635, 0.985)); nose.scale.set(0.8, 1.4, 1.1); scaled.add(nose);
    // parpados (parpadeo)
    this.lids = [];
    for (const u of [0.318, 0.682]) {
      const g = new THREE.CircleGeometry(0.0165, 24, 0, Math.PI); g.rotateZ(Math.PI); g.scale(1.3, 1.1, 1);
      const lid = new THREE.Mesh(g, this.mat(SKIN, { roughness: 0.5, side: THREE.DoubleSide }));
      const line = new THREE.Mesh(new THREE.TorusGeometry(0.0215, 0.0022, 6, 24, Math.PI), this.mat(0x070708)); line.rotation.z = Math.PI; line.scale.set(1, 0.84, 1);
      const holder = new THREE.Group(); holder.add(lid); holder.add(line);
      holder.position.copy(this.uv(u, 0.475, 1.03)); face2(holder); holder.scale.y = 0.001;
      scaled.add(holder); this.lids.push(holder);
    }
    // boca animada: labios negros brillantes e interior
    this.mouth = new THREE.Group(); this.mouth.position.copy(this.uv(0.5, 0.775, 1.03)); face2(this.mouth);
    const lipMat = new THREE.MeshPhysicalMaterial({ color: 0x0b0b0e, roughness: 0.25, clearcoat: 1, clearcoatRoughness: 0.15 });
    const lipShape = (upper) => { const s = new THREE.Shape(); s.moveTo(-0.034, 0);
      if (upper) { s.bezierCurveTo(-0.02, 0.004, -0.012, 0.016, 0, 0.009); s.bezierCurveTo(0.012, 0.016, 0.02, 0.004, 0.034, 0); s.bezierCurveTo(0.012, 0.002, -0.012, 0.002, -0.034, 0); }
      else { s.bezierCurveTo(-0.02, -0.002, 0.02, -0.002, 0.034, 0); s.bezierCurveTo(0.02, -0.02, -0.02, -0.02, -0.034, 0); } return new THREE.ShapeGeometry(s, 16); };
    this.inner = new THREE.Mesh(new THREE.CircleGeometry(0.026, 24), new THREE.MeshBasicMaterial({ color: 0x2a0306 })); this.inner.scale.set(1.2, 0.05, 1); this.inner.position.z = -0.002;
    this.upperLip = new THREE.Mesh(lipShape(true), lipMat); this.lowerLip = new THREE.Mesh(lipShape(false), lipMat);
    this.mouth.add(this.inner, this.upperLip, this.lowerLip); scaled.add(this.mouth);
    // cabello: casco, mono superior, alas laterales y nudo trasero
    const hair = new THREE.MeshPhysicalMaterial({ color: 0x08080b, roughness: 0.32, metalness: 0.1, clearcoat: 0.6, clearcoatRoughness: 0.3, sheen: 0.4, sheenColor: new THREE.Color(0x30304a) });
    const cap = new THREE.Mesh(new THREE.SphereGeometry(R * 1.035, 48, 24, 0, Math.PI * 2, 0, 0.62), hair); scaled.add(cap);
    const back = new THREE.Mesh(new THREE.SphereGeometry(R * 1.03, 48, 32, Math.PI * 1.05, Math.PI * 0.9 + 0.0, 0.3, 2.3), hair); back.rotation.y = 0; scaled.add(back);
    const lobe = (x, y, z, sx, sy, sz, r) => { const m = new THREE.Mesh(new THREE.SphereGeometry(r, 32, 24), hair); m.position.set(x, y, z); m.scale.set(sx, sy, sz); this.head.add(m); return m; };
    lobe(0, 0.205, -0.015, 1.35, 0.72, 1.0, 0.12); lobe(0, 0.16, -0.12, 1.1, 0.8, 0.9, 0.11);
    lobe(-0.15, 0.07, -0.02, 0.62, 1.05, 1.05, 0.1); lobe(0.15, 0.07, -0.02, 0.62, 1.05, 1.05, 0.1); lobe(0, 0.0, -0.17, 1.0, 0.9, 0.8, 0.1);
    // kanzashi: peineta, adornos plateados, varillas rojas y cadenas de cuentas
    const lacquer = this.mat(0x0d0d10, { roughness: 0.2, metalness: 0.3 }), red = this.mat(0xc0102a, { roughness: 0.35 }), silver = this.mat(0xc9ccd4, { roughness: 0.25, metalness: 0.95 });
    const comb = new THREE.Mesh(new THREE.TorusGeometry(0.11, 0.012, 8, 32, Math.PI), lacquer); comb.position.set(0, 0.19, 0.075); comb.scale.set(1, 0.5, 1); this.head.add(comb);
    const combEdge = new THREE.Mesh(new THREE.TorusGeometry(0.11, 0.004, 6, 32, Math.PI), red); combEdge.position.copy(comb.position).add(new THREE.Vector3(0, 0.002, 0.008)); combEdge.scale.copy(comb.scale); this.head.add(combEdge);
    for (const sx of [-1, 1]) {
      const pin = new THREE.Mesh(new THREE.CylinderGeometry(0.004, 0.004, 0.32, 8), red); pin.position.set(sx * 0.13, 0.24, 0.02); pin.rotation.z = sx * -1.05; this.head.add(pin);
      const ball = new THREE.Mesh(new THREE.SphereGeometry(0.014, 12, 10), red); ball.position.set(sx * 0.27, 0.32, 0.02); this.head.add(ball);
      const flower = new THREE.Group();
      for (let p = 0; p < 6; p++) { const pet = new THREE.Mesh(new THREE.SphereGeometry(0.013, 10, 8), silver); pet.position.set(Math.cos(p) * 0.016, Math.sin(p) * 0.016, 0); pet.scale.z = 0.4; flower.add(pet); }
      flower.position.set(sx * 0.115, 0.21, 0.085); face2(flower, new THREE.Vector3(sx * 0.4, 0.2, 1)); this.head.add(flower);
      const plate = new THREE.Mesh(new THREE.BoxGeometry(0.05, 0.012, 0.004), silver); plate.position.set(sx * 0.17, 0.15, 0.07); this.head.add(plate);
      // flecos plateados y cadenas rojas
      for (const [ax, ay, az, n, mat] of [[sx * 0.17, 0.145, 0.07, 5, silver], [sx * 0.165, 0.18, 0.05, 15, red], [sx * 0.135, 0.13, -0.06, 11, red]]) {
        const chain = new THREE.Group(); chain.position.set(ax, ay, az);
        for (let i = 0; i < n; i++) {
          const bead = new THREE.Mesh(mat === silver ? new THREE.BoxGeometry(0.003, 0.012, 0.003) : new THREE.SphereGeometry(0.0085, 10, 8), mat);
          bead.position.y = -0.011 - i * (mat === silver ? 0.013 : 0.02); chain.add(bead);
        }
        if (mat === red) { const tassel = new THREE.Mesh(new THREE.ConeGeometry(0.01, 0.05, 10), red); tassel.position.y = -0.03 - n * 0.02; chain.add(tassel); }
        this.head.add(chain); this.swing.push({ g: chain, a: 0, v: 0, b: 0, w: 0, len: n });
      }
    }
  }
  animate(t, dt) {
    const L = this.levelS, st = this.state;
    // respiracion
    const br = Math.sin(t * 1.5);
    this.body.position.y = br * 0.004; this.body.scale.set(1 + br * 0.004, 1, 1 + br * 0.006);
    // mirada y gestos segun estado
    let yaw = this.mouse.x * 0.45 + Math.sin(t * 0.37) * 0.06, pitch = this.mouse.y * 0.22 + Math.sin(t * 0.53) * 0.03, roll = Math.sin(t * 0.29) * 0.03;
    if (st === "thinking") { yaw += 0.22 + Math.sin(t * 0.8) * 0.05; pitch -= 0.12; }
    if (st === "listening") roll += 0.13;
    if (st === "speaking") { pitch += Math.sin(t * 6.3) * 0.02 * L; yaw += Math.sin(t * 2.1) * 0.04; }
    const prevY = this.head.rotation.y, prevZ = this.head.rotation.z;
    this.head.rotation.y += (yaw - this.head.rotation.y) * 0.06;
    this.head.rotation.x += (pitch - this.head.rotation.x) * 0.06;
    this.head.rotation.z += (roll - this.head.rotation.z) * 0.06;
    const angVelY = (this.head.rotation.y - prevY) / Math.max(dt, 1e-3), angVelZ = (this.head.rotation.z - prevZ) / Math.max(dt, 1e-3);
    // cadenas: pendulos amortiguados que contrarrestan la rotacion de la cabeza
    for (const c of this.swing) {
      const k = 30 / (1 + c.len * 0.06), d = 3.2;
      c.v += (-k * c.a - d * c.v - angVelZ * 6 - angVelY * 2.5) * dt; c.a += c.v * dt;
      c.w += (-k * c.b - d * c.w + angVelY * 3) * dt; c.b += c.w * dt;
      c.g.rotation.z = -this.head.rotation.z + c.a; c.g.rotation.x = -this.head.rotation.x + c.b * 0.6;
    }
    // parpadeo
    this.blinkAt -= dt;
    if (this.blinkAt <= 0) { this.blink = 1; this.blinkAt = rand(2.2, 5.5) * (st === "thinking" ? 0.6 : 1); }
    if (this.blink > 0) this.blink = Math.max(0, this.blink - dt * 7);
    const lid = this.blink > 0 ? Math.sin(this.blink * Math.PI) : (st === "listening" ? 0.18 : 0.04);
    this.lids.forEach((l) => (l.scale.y = Math.max(0.001, lid)));
    // labios: apertura con el nivel de voz
    const open = st === "speaking" || L > 0.05 ? Math.min(1, L * 1.3) * (0.75 + 0.25 * Math.sin(t * 22)) : 0;
    this.lowerLip.position.y = -open * 0.014; this.inner.scale.set(1.15 - open * 0.15, 0.05 + open * 0.55, 1); this.inner.position.y = -open * 0.006;
    this.upperLip.position.y = open * 0.002;
    // eclipse, luces y ceniza
    const pulse = 1 + Math.sin(t * (st === "thinking" ? 3 : 1)) * 0.04 + L * 0.25;
    this.corona.scale.set(2.1 * pulse, 2.1 * pulse, 1); this.corona.material.color.copy(this.stateColor);
    this.rim.color.copy(this.stateColor); this.rim.intensity = 5 + L * 8 + (st === "thinking" ? Math.sin(t * 4) * 2 : 0);
    this.redFill.intensity = 1.2 + (st === "speaking" ? L * 2 : 0);
    const p = this.ash.geometry.attributes.position;
    for (let i = 0; i < p.count; i++) { let y = p.getY(i) - dt * (0.05 + (i % 7) * 0.012); if (y < -0.2) y = 4; p.setY(i, y); p.setX(i, p.getX(i) + Math.sin(t * 0.3 + i) * 0.0008); }
    p.needsUpdate = true;
    this.animateMotes(t, new THREE.Vector3(0, 1.35, 0));
  }
}

/* ------------------------------------------------------------------ geisha a partir de la hoja de personaje */
// Casco visual: cada anillo (y, x0, x1, z0, z1) sale de las siluetas frontal y de perfil; el shader proyecta las
// fotos desde frente, perfiles y espalda y las mezcla segun la normal. Iluminacion suave sobre las fotos (que ya
// traen su sombreado) + borde del color del estado.
const ASSETS = "/static/assets/geisha/";

function hullGeometry(rows, seg = 72, power = 2.6) {
  const pos = [], idx = [];
  const ex = 2 / power;
  rows.forEach(([y, x0, x1, z0, z1]) => {
    const cx = (x0 + x1) / 2, a = Math.max(0.004, (x1 - x0) / 2), cz = (z0 + z1) / 2, b = Math.max(0.004, (z1 - z0) / 2);
    for (let i = 0; i < seg; i++) {
      const t = (i / seg) * Math.PI * 2, c = Math.cos(t), s = Math.sin(t);
      pos.push(cx + a * Math.sign(c) * Math.abs(c) ** ex, y, cz + b * Math.sign(s) * Math.abs(s) ** ex);
    }
  });
  for (let r = 0; r < rows.length - 1; r++) for (let i = 0; i < seg; i++) {
    const a = r * seg + i, b = r * seg + (i + 1) % seg, c = (r + 1) * seg + i, d = (r + 1) * seg + (i + 1) % seg;
    idx.push(a, c, b, b, c, d);
  }
  // tapas superior e inferior
  const capTop = pos.length / 3, top = rows[0], bot = rows[rows.length - 1];
  pos.push((top[1] + top[2]) / 2, top[0] + 0.003, (top[3] + top[4]) / 2);
  const capBot = pos.length / 3; pos.push((bot[1] + bot[2]) / 2, bot[0], (bot[3] + bot[4]) / 2);
  const last = (rows.length - 1) * seg;
  for (let i = 0; i < seg; i++) { idx.push(capTop, i, (i + 1) % seg); idx.push(capBot, last + (i + 1) % seg, last + i); }
  const g = new THREE.BufferGeometry();
  g.setAttribute("position", new THREE.Float32BufferAttribute(pos, 3)); g.setIndex(idx); g.computeVertexNormals();
  return g;
}

function projectedMaterial(views, fallback, accentColor) {
  // views: {front: [tex, a,b,c,d], back:[...], left:[...], right:[...]} — u = h*a+b, v = y*c+d
  const u = { uRim: { value: accentColor }, uRimK: { value: 0.6 }, uFallback: { value: new THREE.Color(fallback) },
              uKey: { value: new THREE.Vector3(-0.4, 0.6, 0.7).normalize() }, uSides: { value: views.left ? 1 : 0 } };
  for (const k of ["front", "back", "left", "right"]) {
    const v = views[k] || views.front;
    u["t_" + k] = { value: v[0] }; u["p_" + k] = { value: new THREE.Vector4(v[1], v[2], v[3], v[4]) };
  }
  return new THREE.ShaderMaterial({
    uniforms: u, toneMapped: false, transparent: false,
    vertexShader: `varying vec3 vP; varying vec3 vN; varying vec3 vVN; varying vec3 vView;
      void main(){ vP = position; vN = normal; vVN = normalize(normalMatrix*normal);
        vec4 mv = modelViewMatrix*vec4(position,1.0); vView = normalize(-mv.xyz); gl_Position = projectionMatrix*mv; }`,
    fragmentShader: `uniform sampler2D t_front, t_back, t_left, t_right; uniform vec4 p_front, p_back, p_left, p_right;
      uniform vec3 uRim, uFallback, uKey; uniform float uRimK, uSides; varying vec3 vP; varying vec3 vN; varying vec3 vVN; varying vec3 vView;
      vec4 tap(sampler2D t, vec4 p, float h){ vec2 uv = vec2(h*p.x + p.y, vP.y*p.z + p.w);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec4(0.0); return texture2D(t, uv); }
      void main(){
        vec3 n = normalize(vN);
        float wf, wb, wl, wr;
        if (uSides > 0.5) { wf = pow(max(n.z,0.0),2.0); wb = pow(max(-n.z,0.0),2.0); wl = pow(max(n.x,0.0),2.0); wr = pow(max(-n.x,0.0),2.0); }
        else { wf = smoothstep(-0.35, 0.35, n.z); wb = 1.0 - wf; wl = 0.0; wr = 0.0; }
        vec4 cf = tap(t_front, p_front, vP.x), cb = tap(t_back, p_back, vP.x), cl = tap(t_left, p_left, vP.z), cr = tap(t_right, p_right, vP.z);
        float W = wf*cf.a + wb*cb.a + wl*cl.a + wr*cr.a;
        vec3 col = (wf*cf.a*cf.rgb + wb*cb.a*cb.rgb + wl*cl.a*cl.rgb + wr*cr.a*cr.rgb) / max(W, 1e-4);
        col = mix(uFallback, col, smoothstep(0.02, 0.25, W));
        float lit = 0.82 + 0.28*max(dot(normalize(vVN), uKey), 0.0);
        float rim = pow(1.0 - max(dot(normalize(vVN), normalize(vView)), 0.0), 4.0);
        gl_FragColor = vec4(col*lit + uRim*rim*uRimK, 1.0);
        #include <colorspace_fragment>
      }`,
  });
}

class GeishaAvatar extends Base3D {
  constructor(container) {
    super(container);
    this.framing = (() => { try { return localStorage.getItem("nexus_avatar_framing") || "busto"; } catch { return "busto"; } })();
    this.blinkAt = 2; this.blink = 0; this.ready = false;
    container.addEventListener("dblclick", this.onDbl = () => this.toggleFraming());
    this.load().catch((e) => { console.warn("geisha: recursos no disponibles, modelo procedural", e); this.fallback = new ProceduralGeisha(container); this.destroy(); });
  }
  frame() { if (this.ready) super.frame(); }
  async load() {
    const rig = await fetch(ASSETS + "rig.json").then((r) => { if (!r.ok) throw new Error("rig"); return r.json(); });
    const loader = new THREE.TextureLoader();
    const tex = (n) => new Promise((res, rej) => loader.load(ASSETS + n, (t) => { t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 4; res(t); }, undefined, rej));
    const [tF, tL, tR, tB, tBF, tBB, tBg] = await Promise.all(["face_front.png", "profile_left.png", "profile_right.png", "back_head.png",
      "body_front.png", "body_back.png", "background.jpg"].map(tex));
    const S = this.scene;
    S.fog = new THREE.Fog(0x15161b, 5, 14);
    // fondo de la referencia (pagoda y eclipse) con corona animada encima del eclipse
    const bgAspect = tBg.image.width / tBg.image.height;
    const BGH = 10, bg = new THREE.Mesh(new THREE.PlaneGeometry(BGH * bgAspect * 1.6, BGH), new THREE.MeshBasicMaterial({ map: tBg, fog: false, toneMapped: false }));
    tBg.wrapS = THREE.MirroredRepeatWrapping; tBg.repeat.x = 1.6; tBg.offset.x = -0.3;
    bg.position.set(0.4, 1.9, -6); S.add(bg);
    this.corona = new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTexture(), color: this.stateColor, transparent: true, opacity: 0.55,
      blending: THREE.AdditiveBlending, depthWrite: false, fog: false }));
    this.corona.position.set(0.4 + BGH * bgAspect * 1.6 * ((0.62 + 0.3) / 1.6 - 0.5), 1.9 + BGH * (0.5 - 0.1), -5.95); this.corona.scale.set(2.6, 2.6, 1); S.add(this.corona);
    const ash = new THREE.BufferGeometry(), N = 220, pos = new Float32Array(N * 3), col = new Float32Array(N * 3);
    for (let i = 0; i < N; i++) { pos.set([rand(-2.5, 2.5), rand(0, 3.2), rand(-3, 1.2)], i * 3); const red = Math.random() < 0.3; col.set(red ? [0.75, 0.08, 0.15] : [0.55, 0.56, 0.6], i * 3); }
    ash.setAttribute("position", new THREE.BufferAttribute(pos, 3)); ash.setAttribute("color", new THREE.BufferAttribute(col, 3));
    this.ash = new THREE.Points(ash, new THREE.PointsMaterial({ size: 0.018, vertexColors: true, transparent: true, opacity: 0.75, depthWrite: false }));
    S.add(this.ash);
    const floor = new THREE.Mesh(new THREE.CircleGeometry(0.75, 64), new THREE.MeshBasicMaterial({ color: this.stateColor, transparent: true, opacity: 0.18 }));
    floor.rotation.x = -Math.PI / 2; floor.position.y = 0.002; S.add(floor); this.floor = floor;
    // cuerpo
    this.rimColor = this.stateColor.clone();
    this.body = new THREE.Group(); S.add(this.body);
    const H = rig.head, B = rig.body;
    this.bodyMat = projectedMaterial({ front: [tBF, ...B.front], back: [tBB, ...B.back] }, 0x0c0a0d, this.rimColor);
    this.body.add(new THREE.Mesh(hullGeometry(B.rows, 72, 2.3), this.bodyMat));
    // cabeza articulada en el cuello
    const neckY = 1.43;
    this.head = new THREE.Group(); this.head.position.set(0, neckY, 0); this.body.add(this.head);
    const headRows = H.rows.map(([y, ...r]) => [y - neckY, ...r]);
    // puente de cuello: prolonga el ultimo anillo hacia abajo (la proyeccion frontal pinta el tatuaje del cuello)
    const lastRow = headRows[headRows.length - 1];
    for (const dy of [0.02, 0.045, 0.07]) headRows.push([lastRow[0] - dy, lastRow[1] * (1 + dy), lastRow[2] * (1 + dy), lastRow[3], lastRow[4]]);
    const shift = (p) => [p[0], p[1], p[2], p[3] + p[2] * neckY];   // la cabeza se modela relativa al cuello
    this.headMat = projectedMaterial({ front: [tF, ...shift(H.front)], back: [tB, ...shift(H.back)], left: [tL, ...shift(H.left)],
                                       right: [tR, ...shift(H.right)] }, 0x09090c, this.rimColor);
    const headMesh = new THREE.Mesh(hullGeometry(headRows, 96, 2.4), this.headMat);
    this.head.add(headMesh);
    // superficie frontal de la cara a cierta altura (para parpados y boca)
    const frontZ = (y) => { const r = H.rows.reduce((a, b) => (Math.abs(b[0] - y) < Math.abs(a[0] - y) ? b : a)); return r[4]; };
    // parpados (parpadeo) y boca (sincronia labial) como capas sobre la cara
    const lidMat = new THREE.MeshBasicMaterial({ color: 0x1b1517, toneMapped: false });
    this.lids = H.eyes.map(([x, y]) => {
      const g = new THREE.CircleGeometry(0.0115, 20, 0, Math.PI); g.rotateZ(Math.PI); g.scale(1.35, 1.0, 1);
      const m = new THREE.Mesh(g, lidMat); m.position.set(x, y - neckY + 0.006, frontZ(y) * 0.985 + 0.004); m.scale.y = 0.001; this.head.add(m); return m;
    });
    const mouthTex = canvasTex(64, 64, (ctx) => { const g = ctx.createRadialGradient(32, 32, 2, 32, 32, 31); g.addColorStop(0, "rgba(40,2,6,1)"); g.addColorStop(0.7, "rgba(25,2,4,0.95)"); g.addColorStop(1, "rgba(0,0,0,0)"); ctx.fillStyle = g; ctx.fillRect(0, 0, 64, 64); });
    this.mouth = new THREE.Mesh(new THREE.PlaneGeometry(0.03, 0.02), new THREE.MeshBasicMaterial({ map: mouthTex, transparent: true, toneMapped: false, depthWrite: false }));
    this.mouth.position.set(H.mouth[0], H.mouth[1] - neckY - 0.001, frontZ(H.mouth[1]) * 0.99 + 0.005); this.mouth.scale.y = 0.05; this.head.add(this.mouth);
    // kanzashi: varillas finas con bolas rojas que la silueta suavizada no conserva
    const silver = new THREE.MeshStandardMaterial({ color: 0xb9bcc6, metalness: 0.9, roughness: 0.3 }), red = new THREE.MeshStandardMaterial({ color: 0xb0101f, roughness: 0.35 });
    for (const [xa, ya, xb, yb] of H.pins) {
      const a = new THREE.Vector3(xa, ya - neckY, 0.01), b = new THREE.Vector3(xb, yb - neckY, 0.01), len = a.distanceTo(b);
      const rod = new THREE.Mesh(new THREE.CylinderGeometry(0.0018, 0.0018, len, 6), silver);
      rod.position.copy(a.clone().add(b).multiplyScalar(0.5)); rod.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), b.clone().sub(a).normalize()); this.head.add(rod);
      const ball = new THREE.Mesh(new THREE.SphereGeometry(0.0085, 14, 10), red); ball.position.copy(a); this.head.add(ball);
    }
    S.add(new THREE.HemisphereLight(0xc8d0e0, 0x301018, 1.2));
    const key = new THREE.DirectionalLight(0xffffff, 1.5); key.position.set(-1, 2.5, 2); S.add(key);
    this.ready = true;
    this.frame();
  }
  animate(t, dt) {
    if (!this.ready) return;
    const L = this.levelS, st = this.state;
    const br = Math.sin(t * 1.4);
    this.body.scale.set(1 + br * 0.003, 1 + br * 0.002, 1 + br * 0.004);
    this.body.rotation.y += ((this.mouse.x * 0.12 + Math.sin(t * 0.21) * 0.05) - this.body.rotation.y) * 0.03;
    let yaw = this.mouse.x * 0.4 + Math.sin(t * 0.37) * 0.05, pitch = this.mouse.y * 0.18 + Math.sin(t * 0.53) * 0.025, roll = Math.sin(t * 0.29) * 0.025;
    if (st === "thinking") { yaw += 0.2 + Math.sin(t * 0.8) * 0.04; pitch -= 0.1; }
    if (st === "listening") roll += 0.11;
    if (st === "speaking") { pitch += Math.sin(t * 6.3) * 0.015 * L; yaw += Math.sin(t * 2.1) * 0.03; }
    this.head.rotation.y += (yaw - this.body.rotation.y * 0.5 - this.head.rotation.y) * 0.06;
    this.head.rotation.x += (pitch - this.head.rotation.x) * 0.06;
    this.head.rotation.z += (roll - this.head.rotation.z) * 0.06;
    this.blinkAt -= dt;
    if (this.blinkAt <= 0) { this.blink = 1; this.blinkAt = rand(2.4, 5.8) * (st === "thinking" ? 0.6 : 1); }
    if (this.blink > 0) this.blink = Math.max(0, this.blink - dt * 7);
    const lid = this.blink > 0 ? Math.sin(this.blink * Math.PI) : (st === "listening" ? 0.15 : 0.001);
    this.lids.forEach((l) => (l.scale.y = Math.max(0.001, lid)));
    const open = st === "speaking" || L > 0.05 ? Math.min(1, L * 1.3) * (0.75 + 0.25 * Math.sin(t * 22)) : 0;
    this.mouth.scale.set(1 - open * 0.15, 0.05 + open * 0.95, 1);
    this.rimColor.copy(this.stateColor);
    this.headMat.uniforms.uRimK.value = this.bodyMat.uniforms.uRimK.value = 0.015 + L * 0.2 + (st === "thinking" ? 0.04 + Math.sin(t * 4) * 0.03 : 0) + (st === "tool" ? 0.05 : 0);
    const pulse = 1 + Math.sin(t * (st === "thinking" ? 3 : 1)) * 0.05 + L * 0.3;
    this.corona.scale.set(2.6 * pulse, 2.6 * pulse, 1); this.corona.material.color.copy(this.stateColor);
    this.floor.material.color.copy(this.stateColor); this.floor.scale.setScalar(1 + L * 0.15);
    const p = this.ash.geometry.attributes.position;
    for (let i = 0; i < p.count; i++) { let y = p.getY(i) - dt * (0.05 + (i % 7) * 0.012); if (y < 0) y = 3.2; p.setY(i, y); }
    p.needsUpdate = true;
    this.animateMotes(t, new THREE.Vector3(0, this.framing === "cuerpo" ? 1.0 : 1.45, 0));
  }
  destroy() { this.container.removeEventListener("dblclick", this.onDbl); super.destroy(); }
}

/* ------------------------------------------------------------------ modelo propio GLB/GLTF/VRM */
/* ---- modelos propios: caja con la pose real (piel) y brazos relajados si vienen en T ---- */
function skinnedBox(model) {
  model.updateMatrixWorld(true);
  model.traverse((o) => { if (o.isSkinnedMesh && o.skeleton) o.skeleton.update(); });
  return new THREE.Box3().setFromObject(model);
}
function relaxArms(model, gltf, deg = 68) {
  const json = gltf.parser?.json || {}, assoc = gltf.parser?.associations;
  const human = json.extensions?.VRMC_vrm?.humanoid?.humanBones || {};
  const vrm0 = Object.fromEntries((json.extensions?.VRM?.humanoid?.humanBones || []).map((b) => [b.bone, { node: b.node }]));
  const byNode = (idx) => { let f = null; if (idx == null || !assoc) return null; model.traverse((o) => { if (!f && assoc.get(o)?.nodes === idx) f = o; }); return f; };
  const find = (side) => byNode((human[side + "UpperArm"] || vrm0[side + "UpperArm"] || {}).node) || (() => {
    const re = side === "left" ? /^(mixamorig\d*:?)?leftarm$|left.?upper.?arm|upper.?arm.?l$|^l.?upperarm$|^arm\.l$/i : /^(mixamorig\d*:?)?rightarm$|right.?upper.?arm|upper.?arm.?r$|^r.?upperarm$|^arm\.r$/i;
    let f = null; model.traverse((o) => { if (!f && o.isBone && re.test(o.name.replace(/[\s_]/g, ""))) f = o; }); return f; })();
  for (const side of ["left", "right"]) {
    const bone = find(side);
    if (!bone) continue;
    // el antebrazo: el del humanoide VRM o, si no, el hijo con mas huesos debajo (no una manga ni un hueso de fisica)
    const depth = (o) => { let n = 0; o.traverse(() => n++); return n; };
    const lower = byNode((human[side + "LowerArm"] || vrm0[side + "LowerArm"] || {}).node);
    const child = lower && lower.parent === bone ? lower
      : bone.children.filter((c) => c.isBone || c.isObject3D).sort((x, y) => depth(y) - depth(x))[0];
    if (!child) continue;
    const a = bone.getWorldPosition(new THREE.Vector3()), b = child.getWorldPosition(new THREE.Vector3());
    const d = b.sub(a); const len = d.length(); if (!len || Math.abs(d.y) > len * 0.35) continue;   // ya esta bajado
    const horiz = new THREE.Vector3(d.x, 0, d.z).normalize(), r = THREE.MathUtils.degToRad(deg);
    const target = horiz.multiplyScalar(Math.cos(r)).add(new THREE.Vector3(0, -Math.sin(r), 0)).normalize();
    const qw = new THREE.Quaternion().setFromUnitVectors(d.normalize(), target);
    const parentQ = bone.parent.getWorldQuaternion(new THREE.Quaternion()), boneQ = bone.getWorldQuaternion(new THREE.Quaternion());
    bone.quaternion.copy(parentQ.invert().multiply(qw).multiply(boneQ));
    model.updateMatrixWorld(true);
  }
}

class GLBAvatar extends Base3D {
  constructor(container, url, onError) {
    super(container);
    const S = this.scene;
    S.add(new THREE.HemisphereLight(0xb0c4de, 0x201018, 1.0));
    const key = new THREE.DirectionalLight(0xffffff, 2.2); key.position.set(-1.5, 3, 3); S.add(key);
    this.rim = new THREE.PointLight(this.stateColor, 6, 6); this.rim.position.set(0, 2.2, -1.4); S.add(this.rim);
    const ring = new THREE.Mesh(new THREE.RingGeometry(0.55, 0.6, 64), new THREE.MeshBasicMaterial({ color: this.stateColor, transparent: true, opacity: 0.6, side: THREE.DoubleSide }));
    ring.rotation.x = -Math.PI / 2; S.add(ring); this.ring = ring;
    this.camera.position.set(0, 1.4, 2.6); this.camera.lookAt(0, 1.2, 0);
    this.morphs = []; this.headBone = null; this.mixer = null;
    // modelo propio: por defecto entero (cabe siempre en el hueco, sea del tamano que sea); doble clic = busto
    this.framingKey = "nexus_avatar_framing_glb";
    this.framing = (() => { try { return localStorage.getItem(this.framingKey) || "cuerpo"; } catch { return "cuerpo"; } })();
    container.addEventListener("dblclick", this.onDbl = () => this.toggleFraming());
    new GLTFLoader().load(url, (gltf) => {
      const model = gltf.scene || gltf.scenes?.[0];
      if (!model) return onError && onError(new Error("el fichero no tiene escena"));
      model.updateMatrixWorld(true);
      // VRM 0.x mira hacia -Z: se gira para que mire a la camara
      const vrm0 = gltf.parser?.json?.extensions?.VRM; if (vrm0) model.rotation.y = Math.PI;
      model.updateMatrixWorld(true);
      if (!gltf.animations.length) relaxArms(model, gltf);   // en T: brazos bajados (mas natural y cabe mejor)
      const box = skinnedBox(model), size = box.getSize(new THREE.Vector3());
      // cualquier unidad (cm, mm, m): se normaliza a 1,75 de alto; los modelos tumbados o anchos (no humanos) por su lado mayor
      const k = 1.75 / (Math.max(size.y, size.x * 0.75, size.z * 0.75) || 1); model.scale.multiplyScalar(k);
      const b2 = skinnedBox(model), c = b2.getCenter(new THREE.Vector3());
      model.position.x -= c.x; model.position.z -= c.z; model.position.y -= b2.min.y; this.baseY = model.position.y;
      S.add(model); this.model = model;
      model.traverse((o) => {
        if (o.isBone && !this.headBone && /head/i.test(o.name)) this.headBone = o;
        if (o.morphTargetDictionary) for (const [name, idx] of Object.entries(o.morphTargetDictionary))
          if (/^(aa|a|ah|jawopen|mouthopen|mouth_open|vrc\.v_aa|viseme_aa|open)$/i.test(name) || /mouth.?open|jaw.?open/i.test(name)) this.morphs.push([o, idx]);
      });
      if (gltf.animations.length) {
        this.mixer = new THREE.AnimationMixer(model);
        const clip = gltf.animations.find((a) => /idle|breath|stand/i.test(a.name)) || gltf.animations[0];
        this.mixer.clipAction(clip).play();
      }
      this.body = model; if (this.headBone) this.head = this.headBone;
      this.ringAt(); this.frame();
    }, undefined, (err) => onError && onError(err));
  }
  ringAt() {   // el aro del estado y las particulas a la medida del modelo
    const b = skinnedBox(this.model), sz = b.getSize(new THREE.Vector3());
    this.ring.scale.setScalar(Math.max(0.6, Math.max(sz.x, sz.z) / 1.1)); this.ringBase = this.ring.scale.x;
    this.moteCenter = new THREE.Vector3(0, b.min.y + sz.y * 0.74, 0);
  }
  destroy() { this.container.removeEventListener("dblclick", this.onDbl); super.destroy(); }
  animate(t, dt) {
    this.mixer && this.mixer.update(dt);
    const L = this.levelS;
    for (const [o, idx] of this.morphs) o.morphTargetInfluences[idx] = Math.min(1, L * 1.4);
    if (this.headBone) {
      this.headBone.rotation.y += ((this.mouse.x * 0.4) - this.headBone.rotation.y) * 0.05;
      this.headBone.rotation.x += ((this.mouse.y * 0.2) - this.headBone.rotation.x) * 0.05;
    } else if (this.model) this.model.rotation.y += ((this.mouse.x * 0.3) - this.model.rotation.y) * 0.04;
    // respiracion sobre su altura de reposo (antes se pisaba el desplazamiento que lo apoya en el suelo y el modelo
    // quedaba fuera del encuadre si su origen no estaba en los pies)
    if (this.model && !this.mixer) this.model.position.y = this.baseY + Math.sin(t * 1.5) * 0.005;
    this.rim.color.copy(this.stateColor); this.rim.intensity = 5 + L * 8;
    this.ring.material.color.copy(this.stateColor); this.ring.scale.setScalar((this.ringBase || 1) * (1 + L * 0.2 + Math.sin(t * 2) * 0.02));
    this.animateMotes(t, this.moteCenter || new THREE.Vector3(0, 1.3, 0));
  }
}

window.NexusAvatar3D = {
  create(kind, container, opts = {}) {
    if (kind === "glb") return new GLBAvatar(container, opts.url, opts.onError);
    if (kind === "geisha-proc") return new ProceduralGeisha(container);
    const g = new GeishaAvatar(container);
    // si faltan los recursos de la hoja, se sustituye por el modelo procedural manteniendo la interfaz
    return new Proxy(g, { get: (o, k) => { const tgt = o.fallback || o; const v = tgt[k]; return typeof v === "function" ? v.bind(tgt) : v; } });
  },
};
window.dispatchEvent(new Event("nexus-avatar3d-ready"));
