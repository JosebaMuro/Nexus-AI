/* Nexus-Office — deteccion del cliente y del dispositivo. Se ejecuta en <head>, antes del primer pintado, y marca <html> con:
 *   data-client   apk (app Android) · pwa (instalada) · web
 *   data-layout   phone · tablet · desktop   (diseno de la interfaz)
 *   data-orient   portrait · landscape
 *   data-input    touch · mouse
 *   data-lowpower presente en moviles/tablets modestos: efectos visuales ligeros
 * El usuario puede forzar el diseno en Ajustes; se guarda solo en este dispositivo. */
(function () {
  "use strict";
  const root = document.documentElement, ua = navigator.userAgent, KEY = "nexus_layout";
  const mq = (q) => window.matchMedia && matchMedia(q).matches;
  const apk = /NexusAndroid\//.test(ua) || !!window.NexusApp;
  const pwa = !apk && (mq("(display-mode: standalone)") || navigator.standalone === true);
  const mobileUA = /Android|iPhone|iPad|iPod|Mobile|Silk|Kindle|PlayStation Vita|Nintendo/i.test(ua) ||
                   (navigator.maxTouchPoints > 1 && /Macintosh/.test(ua));   // iPadOS se presenta como Mac
  const touch = apk || (mq("(pointer: coarse)") && !mq("(any-pointer: fine)")) || (mobileUA && navigator.maxTouchPoints > 0);
  const handheld = apk || mobileUA || touch;
  // la app Android 1.2+ informa de la clase de pantalla oficial (smallestScreenWidthDp >= 600 -> tablet)
  let apkForm = (ua.match(/NexusAndroid\/[\d.]+ \((phone|tablet)/) || [])[1] || "";
  const lowPower = handheld && ((navigator.deviceMemory || 8) <= 4 || (navigator.hardwareConcurrency || 8) <= 4);

  const read = () => { try { return localStorage.getItem(KEY) || "auto"; } catch { return "auto"; } };
  const qp = new URLSearchParams(location.search).get("layout");   // ?layout=phone (pruebas / enlaces directos)
  if (qp && /^(auto|phone|tablet|desktop)$/.test(qp)) { try { localStorage.setItem(KEY, qp); } catch {} }

  function detect() {
    if (apkForm) return apkForm;
    const short = Math.min(screen.width || innerWidth, screen.height || innerHeight);
    if (handheld) return short < 600 ? "phone" : "tablet";
    // ventana de escritorio estrecha -> diseno compacto; sin medida aun (pestana oculta) -> ancho de pantalla
    const w = innerWidth || document.documentElement.clientWidth || screen.width || 1280;
    return w < 700 ? "phone" : w < 1100 ? "tablet" : "desktop";
  }
  let last = "";
  function apply() {
    const pref = read(), layout = pref !== "auto" ? pref : detect();
    const vw = innerWidth || screen.width, vh = innerHeight || screen.height;
    const orient = vw > vh ? "landscape" : "portrait";
    root.dataset.client = apk ? "apk" : pwa ? "pwa" : "web";
    root.dataset.layout = layout; root.dataset.orient = orient; root.dataset.input = touch ? "touch" : "mouse";
    root.toggleAttribute("data-lowpower", lowPower);
    if (!("avatar" in root.dataset)) {
      let av = "on"; try { av = localStorage.getItem("nexus_avatar_band") || "on"; } catch {}
      root.dataset.avatar = av;
    }
    const sig = layout + orient;
    if (sig !== last) { last = sig; window.dispatchEvent(new CustomEvent("nexus-layout", { detail: api.info() })); }
  }
  const api = {
    apk, pwa, touch, lowPower,
    get layout() { return root.dataset.layout; }, get orient() { return root.dataset.orient; },
    get preference() { return read(); }, detected: detect,
    compact() { return root.dataset.layout !== "desktop"; },
    setPreference(v) { try { localStorage.setItem(KEY, v); } catch {} apply(); },
    setAvatarBand(on) { root.dataset.avatar = on ? "on" : "off"; try { localStorage.setItem("nexus_avatar_band", root.dataset.avatar); } catch {} },
    info() { return { client: root.dataset.client, layout: root.dataset.layout, orient: root.dataset.orient, input: root.dataset.input, lowPower }; },
    haptic() { try { window.NexusApp && NexusApp.haptic ? NexusApp.haptic() : navigator.vibrate && navigator.vibrate(8); } catch {} },
  };
  window.NexusDevice = api;
  // la app avisa si cambia la pantalla (plegables, multiventana)
  window.__nexusConfig = (d) => { if (d && d.form) { apkForm = d.form; apply(); } };
  let t = 0; const later = () => { clearTimeout(t); t = setTimeout(apply, 120); };
  addEventListener("resize", later); addEventListener("orientationchange", later); addEventListener("load", apply);
  document.addEventListener("visibilitychange", () => document.hidden || apply());
  if (window.ResizeObserver) new ResizeObserver(later).observe(root);   // tambien cuando la vista pasa de 0 px a su tamano real
  apply();
})();
