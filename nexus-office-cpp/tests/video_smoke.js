// Prueba del generador de video (recursos/web/video.html) en un Chrome real: sirve la pagina y unas imagenes de prueba,
// genera el video con varias combinaciones de opciones (transicion, proporcion, ajuste, titulo, textos y audio), lo
// recoge como lo hace la app (POST /api/video/guardar) y lo comprueba con ffprobe y ffmpeg: formato, dimensiones,
// duracion y que los fotogramas tienen los colores de las imagenes (no negro).
//
//   node tests/video_smoke.js [carpeta-de-salida]        (CHROMIUM=/ruta/al/chrome para elegir el navegador)
const { chromium } = require("playwright");
const http = require("http"), fs = require("fs"), os = require("os"), path = require("path"), zlib = require("zlib");
const { execFileSync, spawnSync } = require("child_process");

const WEB = path.join(__dirname, "..", "recursos", "web");
const OUT = path.resolve(process.argv[2] || fs.mkdtempSync(path.join(os.tmpdir(), "video-smoke-")));
fs.mkdirSync(OUT, { recursive: true });

// ---- PNG sin dependencias: degradado de dos colores
const CRC = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b) => { let c = 0xffffffff; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data) { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td)); return Buffer.concat([len, td, crc]); }
function png(w, h, [r, g, b]) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; for (let x = 0; x < w; x++) { const o = y * (w * 3 + 1) + 1 + x * 3, k = 0.75 + 0.25 * (x / w); raw[o] = r * k; raw[o + 1] = g * k; raw[o + 2] = b * k; } }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}
const COLORS = [[230, 30, 30], [30, 200, 60], [40, 70, 230], [230, 200, 20]];   // rojo, verde, azul, amarillo

// ---- audio de prueba: un tono WAV de 6 s
function wav(seconds) {
  const sr = 22050, n = sr * seconds, buf = Buffer.alloc(44 + n * 2);
  buf.write("RIFF", 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write("WAVEfmt ", 8); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(sr, 24); buf.writeUInt32LE(sr * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34); buf.write("data", 36); buf.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) buf.writeInt16LE(Math.round(9000 * Math.sin(2 * Math.PI * 440 * i / sr)), 44 + i * 2);
  return buf;
}

const MANIFESTS = {};
const SAVED = {};
const server = http.createServer((req, res) => {
  const u = new URL(req.url, "http://x");
  if (req.method === "POST" && u.pathname === "/api/video/guardar") {
    const parts = []; req.on("data", (c) => parts.push(c));
    req.on("end", () => {
      const body = Buffer.concat(parts), head = body.indexOf("\r\n\r\n"), name = /filename="([^"]+)"/.exec(body.slice(0, head).toString("latin1"));
      const boundary = body.slice(0, body.indexOf("\r\n")), end = body.lastIndexOf(Buffer.concat([Buffer.from("\r\n"), boundary]));
      const file = body.slice(head + 4, end), id = u.searchParams.get("id") || "sin-id", ext = path.extname(name ? name[1] : ".webm").slice(1);
      const dest = path.join(OUT, `${id}.${ext}`); fs.writeFileSync(dest, file); SAVED[id] = dest;
      res.setHeader("Content-Type", "application/json");
      res.end(JSON.stringify({ url: `/outputs/videos/${id}/video.${ext}`, size: file.length, markdown: `[video](/outputs/videos/${id}/video.${ext})` }));
    });
    return;
  }
  let m;
  if ((m = /^\/static\/(.+)$/.exec(u.pathname))) {
    const f = path.join(WEB, path.normalize(m[1]).replace(/^(\.\.[\\/])+/, ""));
    if (!fs.existsSync(f) || !fs.statSync(f).isFile()) { res.statusCode = 404; return res.end(); }
    res.setHeader("Content-Type", { ".html": "text/html", ".svg": "image/svg+xml", ".js": "text/javascript" }[path.extname(f)] || "application/octet-stream");
    return res.end(fs.readFileSync(f));
  }
  if ((m = /^\/outputs\/videos\/([^/]+)\/(.+)$/.exec(u.pathname))) {
    const [, id, file] = m;
    if (file === "manifest.json" && MANIFESTS[id]) { res.setHeader("Content-Type", "application/json"); return res.end(JSON.stringify(MANIFESTS[id])); }
    if (/^img(\d)\.png$/.test(file)) { const i = +file[3]; res.setHeader("Content-Type", "image/png"); return res.end(i % 2 ? png(600, 900, COLORS[i]) : png(900, 600, COLORS[i])); }
    if (file === "audio.wav") { res.setHeader("Content-Type", "audio/wav"); return res.end(wav(6)); }
  }
  res.statusCode = 404; res.end();
});

// ---- casos
const imgs = (id, n, textos) => Array.from({ length: n }, (_, i) => ({ url: `/outputs/videos/${id}/img${i}.png`, nombre: `img${i}.png`, ...(textos ? { texto: textos[i] } : {}) }));
const CASES = [
  { id: "fundido-16x9", m: { segundos: 1, transicion: "fundido", transicion_segundos: 0.4, proporcion: "16:9", ajuste: "cubrir", fps: 30, imagenes: imgs("fundido-16x9", 4) }, w: 1280, h: 720, dur: 4 * 1 - 3 * 0.4 },
  { id: "deslizar-9x16", m: { segundos: 1, transicion: "deslizar", transicion_segundos: 0.3, proporcion: "9:16", ajuste: "ajustar", fps: 30, kenburns: true, titulo: "Prueba", imagenes: imgs("deslizar-9x16", 3, ["Uno", "Dos", "Tres"]) }, w: 720, h: 1280, dur: 3 - 2 * 0.3 },
  { id: "corte-1x1", m: { segundos: 1.5, transicion: "corte", proporcion: "1:1", fps: 24, imagenes: imgs("corte-1x1", 2) }, w: 1080, h: 1080, dur: 3 },
  { id: "zoom-audio", m: { segundos: 1.5, transicion: "zoom", transicion_segundos: 0.5, proporcion: "4:3", fps: 30, audio: "/outputs/videos/zoom-audio/audio.wav", imagenes: imgs("zoom-audio", 2) }, w: 1024, h: 768, dur: 3 - 0.5, audio: true, gesto: true },
];

// indice del color de prueba mas parecido en tono (similitud del coseno: da igual que el fotograma este mas oscuro)
function nearest(a) {
  const cos = (x, y) => (x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / (Math.hypot(...x) * Math.hypot(...y) || 1);
  return COLORS.map((c) => cos(a, c)).reduce((best, v, i, all) => (v > all[best] ? i : best), 0);
}
const probe = (f) => JSON.parse(execFileSync("ffprobe", ["-v", "error", "-show_streams", "-show_format", "-of", "json", f]).toString());
function avgColor(f, t) {   // color medio del fotograma en el segundo t (3 bytes RGB)
  const r = spawnSync("ffmpeg", ["-v", "error", "-ss", String(t), "-i", f, "-frames:v", "1", "-vf", "scale=1:1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]);
  return r.status === 0 && r.stdout.length >= 3 ? [...r.stdout.slice(0, 3)] : null;
}

(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const base = `http://127.0.0.1:${server.address().port}`;
  const haveFF = spawnSync("ffprobe", ["-version"]).status === 0 && spawnSync("ffmpeg", ["-version"]).status === 0;
  if (!haveFF) console.log("AVISO: sin ffprobe/ffmpeg solo se comprueba que el navegador genere y guarde el video");
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined, args: ["--autoplay-policy=no-user-gesture-required"] });
  const fails = [];
  const check = (ok, msg) => { console.log((ok ? "  ok     " : "  FALLO  ") + msg); if (!ok) fails.push(msg); };
  for (const c of CASES) {
    console.log(`\n${c.id}`);
    MANIFESTS[c.id] = c.m;
    const page = await browser.newPage();
    const errs = []; page.on("pageerror", (e) => errs.push(e.message)); page.on("console", (m) => m.type() === "error" && errs.push(m.text()));
    await page.goto(`${base}/static/video.html?m=/outputs/videos/${c.id}/manifest.json&id=${c.id}${c.gesto ? "" : "&auto=1"}`);
    await page.waitForFunction((n) => window.NexusVideo && window.NexusVideo.imagenes() === n, c.m.imagenes.length, { timeout: 15000 });
    if (c.gesto) await page.click("#gen");   // con audio el navegador pide un gesto: el boton lo da
    try { await page.waitForFunction(() => ["guardado", "error"].includes(document.body.dataset.estado), null, { timeout: 60000 }); } catch { /* se informa abajo */ }
    const estado = await page.evaluate(() => document.body.dataset.estado), msg = await page.evaluate(() => document.querySelector("#msg").textContent);
    check(estado === "guardado", `estado final «${estado}»${estado === "guardado" ? "" : " · " + msg}`);
    check(!errs.length, "sin errores de JavaScript" + (errs.length ? ": " + errs.join(" | ") : ""));
    if (estado === "guardado" && SAVED[c.id] && haveFF) {
      const f = SAVED[c.id], p = probe(f), v = p.streams.find((s) => s.codec_type === "video"), d = parseFloat(p.format.duration);
      check(+v.width === c.w && +v.height === c.h, `dimensiones ${v.width}x${v.height} (esperadas ${c.w}x${c.h})`);
      check(Number.isFinite(d) && Math.abs(d - c.dur) < 1.2, `duracion ${Number.isFinite(d) ? d.toFixed(2) : "N/D"} s (esperada ~${c.dur.toFixed(1)} s)`);
      if (c.audio) check(p.streams.some((s) => s.codec_type === "audio"), "lleva pista de audio");
      const first = avgColor(f, 0.35), last = avgColor(f, Math.max(0.4, c.dur - 0.35)), lastIdx = c.m.imagenes.length - 1;
      check(first && nearest(first) === 0 && Math.max(...first) > 80, `primer fotograma = imagen 1, roja (${first})`);
      check(last && nearest(last) === lastIdx && Math.max(...last) > 80, `ultimo fotograma = imagen ${lastIdx + 1} (${last})`);
    }
    await page.close();
  }
  await browser.close(); server.close();
  console.log(`\nSalida: ${OUT}`);
  if (fails.length) { console.log(`\n${fails.length} comprobaciones fallidas`); process.exit(1); }
  console.log("\nGenerador de video OK");
})().catch((e) => { console.error(e); process.exit(1); });
