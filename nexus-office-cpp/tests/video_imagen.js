// Pruebas del generador de video (recursos/web/js/video-imagen.js): separar hojas con varias diapositivas y mejorar.
// node nexus-office-cpp/tests/video_imagen.js
const VI = require(process.argv[2] || require("path").join(__dirname, "..", "recursos", "web", "js", "video-imagen.js"));
function hoja(W, H, filas, cols, gut, bg, opts = {}) {
  const d = new Uint8ClampedArray(W * H * 4).fill(bg);
  const sw = (W - gut * (cols + 1)) / cols, sh = (H - gut * (filas + 1)) / filas;
  let n = 0;
  for (let r = 0; r < filas; r++) for (let c = 0; c < cols; c++) {
    const x0 = Math.round(gut + c * (sw + gut)), y0 = Math.round(gut + r * (sh + gut)), col = 40 + 30 * (n++);
    for (let y = y0; y < y0 + sh; y++) for (let x = x0; x < x0 + sw; x++) {
      const i = (y * W + x) * 4;
      // fondo blanco de la diapositiva con borde gris, titulo y texto a la misma altura en todas
      let v = 250;
      if (x - x0 < 2 || x0 + sw - x < 3 || y - y0 < 2 || y0 + sh - y < 3) v = 150;
      if (y - y0 > sh * 0.1 && y - y0 < sh * 0.2 && x - x0 > sw * 0.1 && x - x0 < sw * 0.8) v = 20;
      if (y - y0 > sh * 0.4 && y - y0 < sh * 0.9 && (x * 7 + y * 3) % 11 < 4) v = col;
      d[i] = v; d[i + 1] = v; d[i + 2] = opts.color ? (v + col) % 256 : v; d[i + 3] = 255;
    }
  }
  return { data: d, width: W, height: H };
}
let fails = 0;
const chk = (c, m) => { if (!c) { fails++; console.log("FALLO", m); } else console.log("ok", m); };
let r = VI.separar(hoja(900, 700, 2, 3, 20, 255));
chk(r.length === 6, "2x3 sobre blanco: " + r.length);
chk(r[0].x < r[1].x && r[1].x < r[2].x && r[0].y < r[3].y, "orden de lectura");
r = VI.separar(hoja(1000, 1300, 3, 2, 30, 40, { color: 1 }));
chk(r.length === 6, "3x2 sobre fondo oscuro: " + r.length);
// una foto normal (ruido) no se separa
const W = 640, H = 480, d = new Uint8ClampedArray(W * H * 4);
for (let i = 0; i < d.length; i++) d[i] = (i * 2654435761) >>> 24;
chk(VI.separar({ data: d, width: W, height: H }).length === 0, "foto con ruido: sin separar");
// foto con cielo liso arriba: sin separar
const d2 = new Uint8ClampedArray(W * H * 4);
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const i = (y * W + x) * 4; const v = y < H / 3 ? 200 : ((x * 31 + y * 17) ^ (x * y)) & 255; d2[i] = d2[i + 1] = d2[i + 2] = v; d2[i + 3] = 255; }
chk(VI.separar({ data: d2, width: W, height: H }).length === 0, "foto con cielo liso: sin separar");
chk(VI.cuadricula(100, 100, 2, 2).length === 4, "cuadricula");
// mejorar: una imagen apagada (60..160) se estira
const d3 = new Uint8ClampedArray(100 * 100 * 4);
for (let i = 0; i < d3.length; i += 4) { const v = 60 + ((i / 4) % 100); d3[i] = d3[i + 1] = d3[i + 2] = v; d3[i + 3] = 255; }
const m = VI.mejorar({ data: d3, width: 100, height: 100 });
let mn = 255, mx = 0; for (let i = 0; i < m.data.length; i += 4) { mn = Math.min(mn, m.data[i]); mx = Math.max(mx, m.data[i]); }
chk(mn < 15 && mx > 240, `mejorar estira niveles ${mn}..${mx}`);
process.exit(fails ? 1 : 0);
