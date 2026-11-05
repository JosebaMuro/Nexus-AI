// Prueba del asistente de agentes en un Chrome real: crea un agente por los 7 pasos, comprueba que se guarda con lo
// elegido (plantilla, icono, color, herramientas, delegacion) y lo borra.
//   node tests/wizard_smoke.js http://127.0.0.1:8790 [carpeta-de-capturas]
const { chromium } = require("playwright");
(async () => {
  const base = process.argv[2] || "http://127.0.0.1:8790", out = process.argv[3];
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
  const page = await browser.newPage({ viewport: { width: 1300, height: 900 } });
  const problems = [];
  page.on("pageerror", (e) => problems.push("JS: " + e.message));
  const ok = (c, m) => { console.log((c ? "  ok     " : "  FALLO  ") + m); if (!c) problems.push(m); };
  // sesion de administrador: el asistente solo se ve en ese modo (PIN de prueba; si ya hay otro PIN, ADMIN_PIN=...)
  const pin = process.env.ADMIN_PIN || "4321";
  let r = await page.request.post(base + "/api/admin/login", { data: { pin } });
  if (!r.ok()) r = await page.request.post(base + "/api/admin/setup", { data: { pin } });
  const token = (await r.json()).token;
  await page.addInitScript((t) => sessionStorage.setItem("nexus_admin", JSON.stringify(t)), token);
  await page.goto(base, { waitUntil: "networkidle" });
  const accept = page.locator("#accel-go, [data-accel-go]");
  if (await accept.count()) await accept.first().click().catch(() => {});
  await page.locator('[data-view="agents"]').first().click();
  await page.waitForSelector("#agent-wizard");
  await page.click("#agent-wizard");
  await page.click('[data-tpl="analista"]');
  await page.fill("#wz-name", "Marta Finanzas");
  ok((await page.inputValue("#wz-id")) === "marta-finanzas", "el ID se deduce del nombre");
  await page.click('[data-icon="⚖"]');
  await page.click('[data-color="#ff9f45"]');
  ok((await page.textContent(".wz-preview .t")).includes("Marta Finanzas"), "la vista previa sigue al nombre");
  if (out) await page.screenshot({ path: `${out}/wz-identidad.png` });
  await page.click("#wz-next");
  await page.fill("#wz-kw", "iva, factura, balance");
  await page.click('[data-temp="0.4"]');
  await page.click("#wz-next"); await page.click("#wz-next");     // modelo -> herramientas
  const first = page.locator("[data-tool]").first(); const toolName = await first.getAttribute("data-tool");
  await first.check();
  if (out) await page.screenshot({ path: `${out}/wz-herramientas.png` });
  await page.click("#wz-next");
  await page.click('[data-deleg="nexus"]');
  await page.click("#wz-next");
  if (out) await page.screenshot({ path: `${out}/wz-resumen.png` });
  await page.click("#wz-save");
  await page.waitForFunction(() => !document.querySelector("#agent-wizard") || document.querySelector("#agent-editor").classList.contains("hidden"));
  const agents = await (await page.request.get(base + "/api/agents", { headers: { "X-Admin-Token": token } })).json();
  const a = agents.find((x) => x.id === "marta-finanzas");
  ok(!!a, "el agente existe");
  if (a) {
    ok(a.icon === "⚖" && a.color === "#ff9f45", "icono y color");
    ok(a.temperature === 0.4, "tono 0,4");
    ok(a.keywords.join() === "iva,factura,balance", "palabras clave");
    ok(a.tools.join() === toolName, "herramienta elegida: " + toolName);
    ok(a.can_delegate_to.join() === "nexus", "delegacion");
    ok(/analista/i.test(a.system_prompt), "prompt de la plantilla");
    await page.request.delete(base + "/api/agents/marta-finanzas", { headers: { "X-Admin-Token": token } });
  }
  await browser.close();
  if (problems.length) { console.log("\n" + problems.join("\n")); process.exit(1); }
  console.log("\nAsistente OK");
})().catch((e) => { console.error(e); process.exit(1); });
