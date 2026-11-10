// Prueba de humo de la interfaz web servida por la edicion C++: abre cada vista, envia un mensaje por el chat y
// falla si hay errores de JavaScript o respuestas 5xx. Guarda capturas en la carpeta indicada.
//
//   node tests/ui_smoke.js http://127.0.0.1:8790 /tmp/capturas
const { chromium } = require("playwright");

(async () => {
  const base = process.argv[2] || "http://127.0.0.1:8790";
  const out = process.argv[3] || ".";
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
  const problems = [];
  page.on("pageerror", (e) => problems.push("JS: " + e.message));
  page.on("response", (r) => { if (r.status() >= 500) problems.push(`HTTP ${r.status()} ${r.url()}`); });
  await page.goto(base, { waitUntil: "networkidle" });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: `${out}/01-inicio.png` });
  // el selector de inicio (CPU/GPU/NPU) puede aparecer la primera vez: se acepta tal cual
  const accept = page.locator("#accel-go, [data-accel-go]");
  if (await accept.count()) await accept.first().click().catch(() => {});
  await page.fill("#input", "Cuanto es 12*7?");
  await page.keyboard.press("Enter");
  await page.waitForFunction(() => document.body.innerText.includes("84"), null, { timeout: 60000 });
  await page.screenshot({ path: `${out}/02-chat.png` });
  let n = 3;
  for (const view of ["agents", "lounge", "memory", "studio", "tools", "models", "system", "settings"]) {
    const tab = page.locator(`[data-view="${view}"]`).first();
    if (!(await tab.isVisible().catch(() => false))) continue;
    await tab.click();
    await page.waitForTimeout(1200);
    await page.screenshot({ path: `${out}/${String(n++).padStart(2, "0")}-${view}.png` });
  }
  // laboratorio BETA: sus tres pestañas, una medición rápida (sin IA) y el HUD de rendimiento
  const beta = page.locator('[data-view="beta"]').first();
  if (await beta.isVisible().catch(() => false)) {
    await beta.click();
    await page.waitForSelector("#beta-info:not(:empty)", { timeout: 15000 });
    await page.selectOption("#beta-opt-rondas", "1");
    await page.uncheck("#beta-opt-llm");
    await page.click("#beta-run");
    await page.waitForFunction(() => document.querySelector("#beta-run").textContent.includes("MEDIR AHORA") &&
      document.querySelector("#beta-hist table"), null, { timeout: 120000 });
    await page.click("#beta-hud-btn");
    await page.screenshot({ path: `${out}/${String(n++).padStart(2, "0")}-beta-rendimiento.png` });
    for (const tab of ["aspecto", "opinion"]) {
      await page.click(`[data-btab="${tab}"]`);
      await page.waitForTimeout(800);
      await page.screenshot({ path: `${out}/${String(n++).padStart(2, "0")}-beta-${tab}.png`, fullPage: true });
    }
    await page.click('[data-star="4"]');
    await page.fill("#beta-op-text", "Prueba automática de la interfaz");
    await page.click("#beta-op-send");
    await page.waitForFunction(() => document.body.innerText.includes("Opinión guardada"), null, { timeout: 15000 });
    await page.click("#beta-hud-btn");
    await page.click('[data-btab="rend"]');
  }
  await browser.close();
  if (problems.length) {
    console.log("PROBLEMAS:\n" + problems.join("\n"));
    process.exit(1);
  }
  console.log("Interfaz OK: chat respondido y vistas sin errores");
})().catch((e) => { console.error(e); process.exit(1); });
