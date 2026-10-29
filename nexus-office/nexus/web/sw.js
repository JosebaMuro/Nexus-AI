/* Service worker minimo: permite instalar Nexus-Office como app (PWA) y mostrar la interfaz sin red.
 * Estrategia: red primero para todo; si falla, la ultima copia en cache del armazon de la app. */
const CACHE = "nexus-shell-v7";
const SHELL = ["/", "/static/css/nexus.css", "/static/js/app.js", "/static/js/device.js", "/static/js/themes.js", "/static/js/avatar.js", "/static/js/avatar3d.js", "/static/js/lounge.js", "/static/js/offices.js", "/static/js/kgadmin.js",
  "/static/vendor/three.module.min.js", "/static/vendor/addons/loaders/GLTFLoader.js", "/static/vendor/addons/utils/BufferGeometryUtils.js", "/static/icon.svg"];

self.addEventListener("install", (e) => { e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting())); });
self.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener("fetch", (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== "GET" || url.pathname.startsWith("/api/") || url.pathname.startsWith("/ws")) return;
  e.respondWith(fetch(e.request).then((res) => {
    if (res.ok && SHELL.includes(url.pathname)) { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(e.request, copy)); }
    return res;
  }).catch(() => caches.match(e.request).then((r) => r || (e.request.mode === "navigate" ? caches.match("/") : Response.error()))));   // la portada solo para navegar, nunca en lugar de un script
});
