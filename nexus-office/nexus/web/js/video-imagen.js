// Generador de video (video.html): mejora de imagenes y separacion de hojas con varias diapositivas.
// Funciones puras sobre {data, width, height} (RGBA, como ImageData): se prueban tambien fuera del navegador.
(function (root) {
  "use strict";

  const lum = (d, i) => 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];

  // ---------------------------------------------------------------- separar diapositivas
  // Una hoja con varias diapositivas (captura de una presentacion en cuadricula, hoja de contactos, PDF con 4 o 6
  // por pagina) tiene «calles»: filas o columnas enteras casi del mismo color que el fondo. Se buscan primero las
  // filas (bandas) y, dentro de cada banda, las columnas; cada celda se recorta quitando su margen y se ordenan en
  // orden de lectura (de arriba abajo y de izquierda a derecha).

  function fondo(img) {
    // color del fondo: la mediana del borde
    const { data: d, width: w, height: h } = img, v = [];
    const take = (x, y) => v.push(lum(d, (y * w + x) * 4));
    for (let x = 0; x < w; x += Math.max(1, (w / 200) | 0)) { take(x, 0); take(x, h - 1); }
    for (let y = 0; y < h; y += Math.max(1, (h / 200) | 0)) { take(0, y); take(w - 1, y); }
    v.sort((a, b) => a - b);
    return v[(v.length / 2) | 0];
  }

  // una linea es «calle» si casi todos sus pixeles estan cerca del fondo y apenas varian
  function lineaLisa(img, bg, horiz, k, a, b, tol) {
    const { data: d, width: w } = img;
    let n = 0, fuera = 0, s = 0, s2 = 0;
    const paso = Math.max(1, ((b - a) / 400) | 0);
    for (let t = a; t < b; t += paso) {
      const i = horiz ? (k * w + t) * 4 : (t * w + k) * 4, l = lum(d, i);
      n++; s += l; s2 += l * l;
      if (Math.abs(l - bg) > tol) fuera++;
    }
    const m = s / n, sd = Math.sqrt(Math.max(0, s2 / n - m * m));
    return fuera / n < 0.01 && sd < tol * 0.6;
  }

  // tramos de contenido a lo largo de un eje: [inicio, fin) separados por calles de al menos `minCalle`
  function tramos(img, bg, horiz, a0, a1, b0, b1, tol, minCalle) {
    const out = [];
    let ini = -1, lisas = 0;
    for (let k = a0; k < a1; k++) {
      const lisa = lineaLisa(img, bg, horiz, k, b0, b1, tol);
      if (!lisa) {
        if (ini < 0) ini = k;
        else if (lisas >= minCalle) { out.push([ini, k - lisas]); ini = k; }
        lisas = 0;
      } else if (ini >= 0) lisas++;
    }
    if (ini >= 0) out.push([ini, a1 - lisas]);
    return out;
  }

  // celdas en orden de lectura: [{x, y, w, h}] (coordenadas de la imagen analizada)
  function separar(img, o = {}) {
    const { width: w, height: h } = img;
    const tol = o.tol || 14, bg = fondo(img);
    const minCalleY = Math.max(2, Math.round(h * (o.calle || 0.012)));
    const minCalleX = Math.max(2, Math.round(w * (o.calle || 0.012)));
    const minW = w * (o.min || 0.12), minH = h * (o.min || 0.12);
    const celdas = [];
    for (const [y0, y1] of tramos(img, bg, true, 0, h, 0, w, tol, minCalleY)) {
      if (y1 - y0 < minH) continue;
      for (const [x0, x1] of tramos(img, bg, false, 0, w, y0, y1, tol, minCalleX)) {
        if (x1 - x0 < minW) continue;
        // dentro de la celda puede quedar margen arriba o abajo (diapositivas de alturas distintas en la banda)
        const ys = tramos(img, bg, true, y0, y1, x0, x1, tol, Math.max(minCalleY, (y1 - y0) >> 1));
        const cy0 = ys.length ? ys[0][0] : y0, cy1 = ys.length ? ys[ys.length - 1][1] : y1;
        if (cy1 - cy0 < minH) continue;
        celdas.push({ x: x0, y: cy0, w: x1 - x0, h: cy1 - cy0 });
      }
    }
    // una sola celda que ocupa casi todo: no es una hoja de diapositivas
    if (celdas.length < 2) return [];
    // las celdas deben parecerse en tamano (las diapositivas de una hoja son iguales): fuera las muy distintas
    const areas = celdas.map((c) => c.w * c.h).sort((a, b) => a - b), med = areas[(areas.length / 2) | 0];
    const buenas = celdas.filter((c) => c.w * c.h > med * 0.35);
    return buenas.length >= 2 ? ordenar(buenas) : [];
  }

  function ordenar(celdas) {
    // por filas (con tolerancia: centros a menos de media altura) y en cada fila de izquierda a derecha
    const cs = celdas.slice().sort((a, b) => a.y - b.y), filas = [];
    for (const c of cs) {
      const f = filas.find((r) => Math.abs(r.cy - (c.y + c.h / 2)) < Math.min(r.h, c.h) / 2);
      if (f) f.items.push(c); else filas.push({ cy: c.y + c.h / 2, h: c.h, items: [c] });
    }
    filas.sort((a, b) => a.cy - b.cy);
    return filas.flatMap((f) => f.items.sort((a, b) => a.x - b.x));
  }

  // cuadricula fija (cuando la deteccion no ve las calles): filas x columnas iguales
  function cuadricula(w, h, filas, cols) {
    const out = [];
    for (let r = 0; r < filas; r++)
      for (let c = 0; c < cols; c++)
        out.push({ x: Math.round((c * w) / cols), y: Math.round((r * h) / filas), w: Math.round(w / cols), h: Math.round(h / filas) });
    return out;
  }

  // ---------------------------------------------------------------- mejorar
  // niveles automaticos (percentiles por canal mezclados con los de la luminancia: corrige dominantes sin
  // inventar colores), un poco de saturacion, gamma hacia un brillo medio y enfoque (mascara de desenfoque)
  function percentil(hist, total, p) {
    let acc = 0, lim = total * p;
    for (let i = 0; i < 256; i++) { acc += hist[i]; if (acc >= lim) return i; }
    return 255;
  }

  function mejorar(img, o = {}) {
    const { data: d, width: w, height: h } = img, n = w * h;
    const hs = [new Uint32Array(256), new Uint32Array(256), new Uint32Array(256)], hl = new Uint32Array(256);
    let sumL = 0;
    for (let i = 0; i < n * 4; i += 4) {
      hs[0][d[i]]++; hs[1][d[i + 1]]++; hs[2][d[i + 2]]++;
      const l = lum(d, i) | 0; hl[l]++; sumL += l;
    }
    const lo = percentil(hl, n, 0.005), hi = percentil(hl, n, 0.995);
    const lut = [0, 1, 2].map((c) => {
      const clo = percentil(hs[c], n, 0.005), chi = percentil(hs[c], n, 0.995);
      const a = 0.5 * clo + 0.5 * lo, b = Math.max(a + 32, 0.5 * chi + 0.5 * hi);
      const t = new Uint8ClampedArray(256);
      for (let v = 0; v < 256; v++) t[v] = ((v - a) * 255) / (b - a);
      return t;
    });
    // gamma suave para acercar el brillo medio a 0.5 (sin pasar de 0.75..1.35)
    const media = Math.min(0.95, Math.max(0.05, (sumL / n - lo) / Math.max(1, hi - lo)));
    const g = Math.min(1.35, Math.max(0.75, Math.log(0.5) / Math.log(media)));
    const gl = new Float32Array(256);
    for (let v = 0; v < 256; v++) gl[v] = 255 * Math.pow(v / 255, 1 / (1 + (g - 1) * 0.5));
    const sat = o.saturacion ?? 1.12;
    const out = new Uint8ClampedArray(d.length);
    for (let i = 0; i < n * 4; i += 4) {
      let r = gl[lut[0][d[i]]], gg = gl[lut[1][d[i + 1]]], b = gl[lut[2][d[i + 2]]];
      const l = 0.299 * r + 0.587 * gg + 0.114 * b;
      out[i] = l + (r - l) * sat; out[i + 1] = l + (gg - l) * sat; out[i + 2] = l + (b - l) * sat; out[i + 3] = d[i + 3];
    }
    const amt = o.enfoque ?? 0.55;
    if (amt > 0) enfocar(out, w, h, amt);
    return { data: out, width: w, height: h };
  }

  // mascara de desenfoque con caja 3x3 (sin ctx.filter, que Safari no siempre tiene)
  function enfocar(d, w, h, amt) {
    const src = new Uint8ClampedArray(d);
    for (let y = 1; y < h - 1; y++)
      for (let x = 1; x < w - 1; x++) {
        const i = (y * w + x) * 4;
        for (let c = 0; c < 3; c++) {
          const k = i + c, row = w * 4;
          const m = (src[k - row - 4] + src[k - row] + src[k - row + 4] + src[k - 4] + src[k] + src[k + 4] +
                     src[k + row - 4] + src[k + row] + src[k + row + 4]) / 9;
          d[k] = src[k] + amt * (src[k] - m);
        }
      }
  }

  const api = { separar, ordenar, cuadricula, mejorar, fondo };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  else root.VideoImagen = api;
})(typeof window !== "undefined" ? window : globalThis);
