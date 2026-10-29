/* Nexus-Office — temas: plantillas y aplicacion de un tema completo (colores, tipografias, forma, efectos). */
(function () {
  const F = {
    rajdhani: '"Rajdhani", "Segoe UI", system-ui, sans-serif', orbitron: '"Orbitron", "Rajdhani", "Segoe UI", sans-serif',
    jet: '"JetBrains Mono", "Cascadia Code", Consolas, monospace', system: '"Segoe UI", system-ui, -apple-system, Roboto, sans-serif',
    serif: '"Yu Mincho", "Hiragino Mincho ProN", "Noto Serif JP", Georgia, "Times New Roman", serif',
    mono: '"Cascadia Code", "JetBrains Mono", Consolas, "Courier New", monospace', rounded: '"Nunito", "Segoe UI", system-ui, sans-serif',
  };
  const FONTS = { "Rajdhani (futurista)": F.rajdhani, "Orbitron (HUD)": F.orbitron, "Sistema": F.system, "Serif japonesa": F.serif,
                  "Monoespaciada": F.mono, "JetBrains Mono": F.jet, "Redondeada": F.rounded };
  const base = { font_ui: F.rajdhani, font_hud: F.orbitron, font_mono: F.jet, radius: 14, glow: 45, blur: 14, panel_alpha: 0.62,
                 chrome_alpha: 0.7, grid: true, stars: true, scanlines: true, vignette: true, corners: true, light: false };
  const PRESETS = {
    neon: { name: "Neón cian", ...base, bg: "#04060c", panel: "#0a1222", text: "#d8f6ff", muted: "#6f8aa3", accent: "#00f0ff", accent2: "#ff3df2",
            ok: "#7cff6b", warn: "#ffb020", bad: "#ff4d6d", chrome: "#040812", field: "#000812", code_bg: "#020409", on_accent: "#00121a" },
    geisha: { name: "Geisha carmesí", ...base, bg: "#0b0809", panel: "#171011", text: "#f3ebe6", muted: "#a38d88", accent: "#d4142f", accent2: "#e8e1d8",
              ok: "#c9b27a", warn: "#e0a03c", bad: "#ff3355", chrome: "#0d0909", field: "#120c0c", code_bg: "#080505", on_accent: "#fff6f0",
              font_hud: F.serif, font_ui: F.system, radius: 6, glow: 30, panel_alpha: 0.72, grid: false, scanlines: false },
    sakura: { name: "Sakura (claro)", ...base, bg: "#fbf3f6", panel: "#ffffff", text: "#3a2430", muted: "#8d6f7d", accent: "#e0558a", accent2: "#7a5af5",
              ok: "#2e9e5b", warn: "#c98a12", bad: "#d33a4f", chrome: "#fff5f8", field: "#ffffff", code_bg: "#f7eef2", on_accent: "#ffffff",
              font_ui: F.rounded, font_hud: F.rounded, radius: 18, glow: 25, blur: 10, panel_alpha: 0.85, chrome_alpha: 0.85,
              grid: false, stars: false, scanlines: false, vignette: false, light: true },
    matrix: { name: "Matrix", ...base, bg: "#000000", panel: "#001a08", text: "#b6ffbf", muted: "#3f8f52", accent: "#00ff66", accent2: "#00c8ff",
              ok: "#00ff66", warn: "#d4ff00", bad: "#ff3b3b", chrome: "#000a03", field: "#000f05", code_bg: "#000500", on_accent: "#001a08",
              font_ui: F.mono, font_hud: F.mono, font_mono: F.mono, radius: 2, glow: 55 },
    ambar: { name: "Ámbar retro", ...base, bg: "#120b02", panel: "#1f1405", text: "#ffd9a0", muted: "#a57d43", accent: "#ffb000", accent2: "#ff5e00",
             ok: "#c6ff6b", warn: "#ffd000", bad: "#ff4a1c", chrome: "#140c02", field: "#170e03", code_bg: "#0c0701", on_accent: "#1a0f00",
             font_ui: F.mono, font_hud: F.mono, radius: 4, glow: 40, stars: false },
    minimal: { name: "Minimal oscuro", ...base, bg: "#111317", panel: "#1a1d23", text: "#e7e9ee", muted: "#8b92a0", accent: "#6aa1ff", accent2: "#b48cff",
               ok: "#4cc38a", warn: "#e2a336", bad: "#ef5b6b", chrome: "#14161b", field: "#0f1115", code_bg: "#0c0e11", on_accent: "#0b1220",
               font_ui: F.system, font_hud: F.system, radius: 10, glow: 0, blur: 0, panel_alpha: 0.97, chrome_alpha: 1,
               grid: false, stars: false, scanlines: false, vignette: false, corners: false },
    claro: { name: "Claro profesional", ...base, bg: "#f4f6f9", panel: "#ffffff", text: "#1d2330", muted: "#6b7385", accent: "#2563eb", accent2: "#7c3aed",
             ok: "#16a34a", warn: "#d97706", bad: "#dc2626", chrome: "#ffffff", field: "#ffffff", code_bg: "#f1f3f6", on_accent: "#ffffff",
             font_ui: F.system, font_hud: F.system, radius: 10, glow: 10, blur: 0, panel_alpha: 1, chrome_alpha: 0.95,
             grid: false, stars: false, scanlines: false, vignette: false, corners: false, light: true },
  };
  const rgba = (hex, a) => { const n = parseInt((hex || "#000000").slice(1), 16); return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`; };

  function resolve(theme) {
    const t = theme && Object.keys(theme).length ? theme : {};
    const p = PRESETS[t.preset] || PRESETS.neon;
    return { ...p, ...t };
  }
  function apply(theme) {
    const t = resolve(theme), r = document.documentElement.style;
    const set = (k, v) => r.setProperty(k, v);
    set("--bg", t.bg); set("--panel", rgba(t.panel, t.panel_alpha)); set("--panel-solid", t.panel); set("--text", t.text); set("--muted", t.muted);
    set("--accent", t.accent); set("--accent2", t.accent2); set("--ok", t.ok); set("--warn", t.warn); set("--bad", t.bad);
    set("--chrome", rgba(t.chrome, t.chrome_alpha)); set("--field", rgba(t.field, t.light ? 0.9 : 0.6)); set("--code-bg", t.code_bg);
    set("--on-accent", t.on_accent); set("--shade", t.light ? "rgba(0,0,0,0.04)" : "rgba(0,0,0,0.25)");
    set("--font-ui", t.font_ui); set("--font-hud", t.font_hud); set("--font-mono", t.font_mono);
    set("--radius", t.radius + "px"); set("--blur", t.blur + "px"); set("--glow-k", t.glow + "%");
    const b = document.body.classList;
    b.toggle("no-grid", !t.grid); b.toggle("no-stars", !t.stars); b.toggle("no-scanlines", !t.scanlines); b.toggle("no-vignette", !t.vignette);
    b.toggle("flat", !t.corners); b.toggle("light", !!t.light);
    document.documentElement.style.colorScheme = t.light ? "light" : "dark";
    document.querySelector('meta[name="theme-color"]')?.setAttribute("content", t.bg);
    try { window.NexusApp && NexusApp.setBars && NexusApp.setBars(t.chrome || t.bg, !!t.light); } catch (e) {}   // barras de Android con el color del tema
    return t;
  }
  window.NexusThemes = { PRESETS, FONTS, apply, resolve };
})();
