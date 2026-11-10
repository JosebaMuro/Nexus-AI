/* Agente IA — ubicación en tiempo real (Ajustes > Ubicación). Muestra las fuentes que hay en este equipo, consulta la
   posición, guarda la posición fija y los lugares (Oficina, Casa, Almacén…) y, si se activa, manda al núcleo la
   posición del navegador mientras la página está abierta. En la app Android pide el permiso y arranca el GPS del
   móvil (puente NexusApp). La posición nunca sale del equipo. Usa window.NexusUI (app.js). */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const UI = () => window.NexusUI;
  const esc = (s) => UI().esc(s);
  const api = (...a) => UI().api(...a);
  const toast = (...a) => UI().toast(...a);
  const guard = (fn) => UI().guard(fn);
  const app = () => (window.NexusApp && typeof window.NexusApp.ubicacion === "function" ? window.NexusApp : null);

  let estado = null, vigilancia = null;
  const FUENTES = [["auto", "Automática (la mejor disponible)"], ["app", "GPS del móvil (app Android)"], ["gpsd", "Receptor GPS con gpsd"],
                   ["nmea", "GPS por USB o puerto serie"], ["windows", "Servicio de ubicación de Windows"],
                   ["geoclue", "Servicio de ubicación de Linux (GeoClue)"], ["navegador", "Navegador"], ["manual", "Posición fija"]];

  const guardar = async (cambios) => { await api("/api/config", { method: "PUT", body: cambios }); toast("Guardado"); await cargar(); };
  const fmt = (p) => {
    if (!p || p.error) return `<span class="muted">${esc(p ? p.error : "Sin posición todavía")}</span>`;
    const cerca = p.cerca_de ? (p.cerca_de.distancia_km < 1 ? `en ${esc(p.cerca_de.nombre)}` : `a ${p.cerca_de.distancia_km} km${p.cerca_de.direccion ? " al " + esc(p.cerca_de.direccion) : ""} de ${esc(p.cerca_de.nombre)}`) : "";
    return `<b>${p.lat}, ${p.lon}</b>${p.precision_m != null ? ` <span class="muted">±${p.precision_m} m</span>` : ""}
      ${p.lugar ? ` · 📍 <b>${esc(p.lugar)}</b>` : ""} · ${cerca}<br><small class="muted">${esc(p.fuente)}${p.detalle ? " (" + esc(p.detalle) + ")" : ""} · ${esc(p.hora)}
      ${p.velocidad_kmh != null ? ` · ${p.velocidad_kmh} km/h` : ""}${p.en_tiempo_real ? " · en tiempo real" : ""}${p.aviso ? "<br>" + esc(p.aviso) : ""}</small>`;
  };

  // posicion del navegador mientras la pagina esta abierta (solo si se activo en Ajustes)
  function vigilarNavegador(on) {
    if (vigilancia != null) { navigator.geolocation.clearWatch(vigilancia); vigilancia = null; }
    if (!on || !("geolocation" in navigator) || app()) return;   // en la app Android manda el GPS del movil
    let ultimo = 0;
    vigilancia = navigator.geolocation.watchPosition((p) => {
      const t = Date.now();
      if (t - ultimo < 4000) return;
      ultimo = t;
      const c = p.coords;
      api("/api/ubicacion", { body: { lat: c.latitude, lon: c.longitude, precision: c.accuracy, altitud: c.altitude, velocidad: c.speed, rumbo: c.heading, t: p.timestamp, proveedor: "navegador" } }).catch(() => {});
    }, (e) => { if (e.code === 1) toast("El navegador no dejó usar la ubicación", true); }, { enableHighAccuracy: true, maximumAge: 5000, timeout: 30000 });
  }

  async function cargar() {
    const box = $("#ubic-box");
    if (!box) return;
    try { estado = await api("/api/ubicacion"); } catch (e) { box.innerHTML = `<div class="muted">${esc(e.message)}</div>`; return; }
    const c = estado.config || {}, m = c.manual || {}, admin = UI().admin();
    const fuentes = (estado.fuentes || []).map((f) => `<div style="margin:3px 0">${f.disponible ? "✅" : "○"} <b>${esc(f.nombre)}</b> <small class="muted">· ${esc(f.nota)}</small></div>`).join("");
    const lugares = (c.lugares || []).map((l, i) => `<div class="row"><span>📍 <b>${esc(l.nombre)}</b> <span class="muted">${l.lat}, ${l.lon} · radio ${l.radio || 150} m</span></span><span style="flex:1"></span>
      <button class="btn ghost" data-borrar-lugar="${i}" ${admin ? "" : "disabled"}>QUITAR</button></div>`).join("") || `<div class="muted">Aún no hay lugares guardados.</div>`;
    const sw = (k, on) => `<label class="switch"><input type="checkbox" data-ubic="${k}" ${on ? "checked" : ""} ${admin ? "" : "disabled"}><span></span></label>`;
    box.innerHTML = `
      <div class="row"><div id="ubic-pos" style="flex:1">${fmt(estado.ultima)}</div><button class="btn primary" id="ubic-consultar">CONSULTAR AHORA</button></div>
      ${app() ? `<div class="row"><button class="btn" id="ubic-gps">USAR EL GPS DEL MÓVIL</button><small class="muted">Te pedirá el permiso de ubicación de Android.</small></div>` : ""}
      ${admin ? "" : `<div class="muted">Para cambiar estos ajustes entra en modo administrador.</div>`}
      <div class="setting"><div class="lbl"><b>Ubicación activada</b><small>Los agentes pueden usar la herramienta ubicacion_actual</small></div>${sw("activada", c.activada !== false)}</div>
      <div class="setting"><div class="lbl"><b>Fuente</b></div><select class="input" id="ubic-fuente" style="width:auto" ${admin ? "" : "disabled"}>${FUENTES.map(([v, n]) => `<option value="${v}" ${(c.fuente || "auto") === v ? "selected" : ""}>${n}</option>`).join("")}</select></div>
      <div class="setting"><div class="lbl"><b>Usar la ubicación del navegador</b><small>Mientras esta página esté abierta (el navegador pedirá permiso)</small></div>${sw("navegador", !!c.navegador)}</div>
      <div class="setting"><div class="lbl"><b>Compartir con los usuarios</b><small>Perfiles y dispositivos emparejados que no son el titular</small></div>${sw("usuarios", !!c.usuarios)}</div>
      <div class="setting"><div class="lbl"><b>Compartir con la red COOP</b><small>Otros nodos podrán preguntar dónde está este equipo</small></div>${sw("remota", !!c.remota)}</div>
      <h4 style="margin:12px 0 6px">Fuentes de este equipo</h4>${fuentes}
      <h4 style="margin:12px 0 6px">Receptor GPS</h4>
      <div class="row"><input class="input" id="ubic-serie" placeholder="${navigator.platform.startsWith("Win") ? "COM3" : "/dev/ttyACM0"}" value="${esc(c.serie || "")}" style="width:180px" ${admin ? "" : "disabled"}>
        <input class="input" id="ubic-baudios" type="number" value="${c.baudios || 9600}" style="width:100px" ${admin ? "" : "disabled"}>
        <input class="input" id="ubic-gpsd" placeholder="127.0.0.1:2947" value="${esc(c.gpsd ?? "127.0.0.1:2947")}" style="width:170px" ${admin ? "" : "disabled"}>
        <button class="btn" id="ubic-gps-guardar" ${admin ? "" : "disabled"}>GUARDAR</button></div>
      <small class="muted">Puerto serie del GPS (NMEA), velocidad y dirección de gpsd (vacía = no usar gpsd).</small>
      <h4 style="margin:12px 0 6px">Posición fija</h4>
      <div class="row"><input class="input" id="ubic-lat" placeholder="latitud" value="${m.lat ?? ""}" style="width:120px" ${admin ? "" : "disabled"}>
        <input class="input" id="ubic-lon" placeholder="longitud" value="${m.lon ?? ""}" style="width:120px" ${admin ? "" : "disabled"}>
        <input class="input" id="ubic-nombre" placeholder="nombre (p. ej. Oficina)" value="${esc(m.nombre || "")}" style="width:180px" ${admin ? "" : "disabled"}>
        <button class="btn" id="ubic-fija" ${admin ? "" : "disabled"}>GUARDAR</button></div>
      <small class="muted">Para equipos sin GPS: se usa cuando no hay una fuente en tiempo real.</small>
      <h4 style="margin:12px 0 6px">Lugares guardados</h4>${lugares}
      <div class="row"><input class="input" id="ubic-lugar" placeholder="nombre del lugar" style="width:180px" ${admin ? "" : "disabled"}>
        <input class="input" id="ubic-radio" type="number" value="150" style="width:90px" title="radio en metros" ${admin ? "" : "disabled"}>
        <button class="btn" id="ubic-aqui" ${admin ? "" : "disabled"}>GUARDAR DONDE ESTOY</button></div>`;

    $("#ubic-consultar").onclick = guard(async () => {
      $("#ubic-pos").innerHTML = `<span class="muted">Buscando la posición…</span>`;
      const p = await api("/api/ubicacion/consultar", { body: {} });
      $("#ubic-pos").innerHTML = fmt(p);
    });
    if ($("#ubic-gps")) $("#ubic-gps").onclick = () => { app().pedirUbicacion(); toast("Acepta el permiso y espera unos segundos a que el GPS fije la posición"); };
    box.querySelectorAll("[data-ubic]").forEach((el) => (el.onchange = guard(async () => {
      await guardar({ ["ubicacion." + el.dataset.ubic]: el.checked });
      if (el.dataset.ubic === "navegador") vigilarNavegador(el.checked);
      if (el.dataset.ubic === "activada" && app()) app().ubicacion(el.checked);
    })));
    $("#ubic-fuente").onchange = guard(() => guardar({ "ubicacion.fuente": $("#ubic-fuente").value }));
    $("#ubic-gps-guardar").onclick = guard(() => guardar({ "ubicacion.serie": $("#ubic-serie").value.trim(), "ubicacion.baudios": +$("#ubic-baudios").value || 9600, "ubicacion.gpsd": $("#ubic-gpsd").value.trim() }));
    $("#ubic-fija").onclick = guard(() => {
      const num = (s) => (s.trim() === "" ? null : +s.replace(",", "."));
      const lat = num($("#ubic-lat").value), lon = num($("#ubic-lon").value);
      if ((lat != null && (isNaN(lat) || Math.abs(lat) > 90)) || (lon != null && (isNaN(lon) || Math.abs(lon) > 180))) throw new Error("Coordenadas no válidas");
      return guardar({ "ubicacion.manual": { lat, lon, nombre: $("#ubic-nombre").value.trim() } });
    });
    $("#ubic-aqui").onclick = guard(async () => {
      const nombre = $("#ubic-lugar").value.trim();
      if (!nombre) throw new Error("Pon un nombre al lugar");
      const p = await api("/api/ubicacion/consultar", { body: {} });
      if (p.error) throw new Error(p.error);
      const lugares = [...(estado.config.lugares || []).filter((l) => l.nombre !== nombre), { nombre, lat: p.lat, lon: p.lon, radio: +$("#ubic-radio").value || 150 }];
      await guardar({ "ubicacion.lugares": lugares });
    });
    box.querySelectorAll("[data-borrar-lugar]").forEach((b) => (b.onclick = guard(() => {
      const lugares = (estado.config.lugares || []).filter((_, i) => i !== +b.dataset.borrarLugar);
      return guardar({ "ubicacion.lugares": lugares });
    })));
  }

  // al abrir la interfaz: GPS del movil en la app Android y navegador si estan activados
  async function arrancar() {
    for (let i = 0; i < 40 && !window.NexusUI; i++) await new Promise((r) => setTimeout(r, 250));
    if (!window.NexusUI) return;
    try {
      const e = await api("/api/ubicacion");
      const c = e.config || {};
      if (app()) app().ubicacion(c.activada !== false);
      vigilarNavegador(c.activada !== false && !!c.navegador);
    } catch {}
  }
  window.NexusUbicacion = { load: cargar };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", arrancar); else arrancar();
})();
