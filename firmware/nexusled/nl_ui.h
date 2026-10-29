// NexusLED - interfaz web integrada (se sirve en http://<ip>/).
#pragma once
#ifndef PROGMEM
#define PROGMEM
#endif

static const char NL_UI_HTML[] PROGMEM = R"NLHTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NexusLED</title>
<style>
:root{--bg:#05070d;--panel:#0c1220;--line:#1c2a44;--txt:#e6f7ff;--mut:#7f93b0;--acc:#00f0ff;--acc2:#ff3df2;--ok:#7cff6b;--bad:#ff4d6d}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:15px system-ui,sans-serif}
header{display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid var(--line)}
header b{letter-spacing:.2em;color:var(--acc)}header span{color:var(--mut);font-size:13px}
main{max-width:760px;margin:0 auto;padding:16px;display:grid;gap:14px}
.p{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px;display:grid;gap:10px}
h3{margin:0;font-size:13px;letter-spacing:.15em;color:var(--mut);font-weight:600}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.row>*{flex:1;min-width:0}
label{display:grid;gap:4px;font-size:13px;color:var(--mut);min-width:0}
input,select,button{min-width:0;max-width:100%;font:inherit;color:var(--txt);background:#0a0f1b;border:1px solid var(--line);border-radius:8px;padding:8px}
input[type=range]{padding:0;accent-color:var(--acc)}input[type=color]{padding:2px;height:40px}
button{cursor:pointer;background:#10203a;border-color:#24406b}button.on{background:var(--acc);color:#001018;font-weight:700}
button.dng{border-color:#5a2030;background:#2a0f18}
canvas{width:100%;height:28px;border-radius:6px;background:#000}
.pre{display:flex;flex-wrap:wrap;gap:8px}.pre button{flex:0 0 auto}
.mut{color:var(--mut);font-size:13px}.out{display:grid;grid-template-columns:repeat(3,minmax(0,1fr)) auto auto;gap:6px;align-items:end}
details summary{cursor:pointer;color:var(--acc)}
</style></head><body>
<header><b>NEXUSLED</b><span id="hdr">…</span></header>
<main>
<div class="p"><canvas id="cv" height="28"></canvas>
<div class="row"><button id="pw">ENCENDIDO</button><label>Brillo<input type="range" id="bri" min="0" max="255"></label></div></div>
<div class="p"><h3>EFECTO</h3>
<div class="row"><label>Efecto<select id="fx"></select></label><label>Paleta<select id="pal"></select></label></div>
<div class="row"><label>Color 1<input type="color" id="c0"></label><label>Color 2<input type="color" id="c1"></label><label>Color 3<input type="color" id="c2"></label></div>
<div class="row"><label>Velocidad<input type="range" id="sx" min="0" max="255"></label><label>Intensidad<input type="range" id="ix" min="0" max="255"></label></div></div>
<div class="p"><h3>PRESETS</h3><div class="pre" id="presets"></div>
<div class="row"><input id="pname" placeholder="Nombre del preset"><button id="psave" style="flex:0 0 auto">GUARDAR ACTUAL</button></div></div>
<div class="p"><details><summary>Ajustes del dispositivo</summary><div style="display:grid;gap:10px;margin-top:10px">
<label>Nombre<input id="name"></label>
<div class="row"><label>Wi‑Fi<select id="ssid"></select></label><label>Clave Wi‑Fi<input id="wpass" type="password" placeholder="(sin cambios)"></label><button id="scan" style="flex:0 0 auto">BUSCAR</button></div>
<h3>SALIDAS (TIRAS)</h3><div class="mut">Pin GPIO, número de LED y orden de color. WS2812/WS2812B: GRB. Hasta 4 tiras.</div>
<div id="outs" style="display:grid;gap:6px"></div><button id="addout">AÑADIR TIRA</button>
<div class="row"><label>Límite de corriente (mA, 0 = sin límite)<input id="maxma" type="number" min="0"></label><label>Imágenes por segundo<input id="fps" type="number" min="5" max="120"></label></div>
<div class="row"><label>Clave del dispositivo (vacía = abierta)<input id="tok" type="password" placeholder="(sin cambios)"></label><label>Clave del punto de acceso<input id="appass" type="password" placeholder="(sin cambios)"></label></div>
<div class="row"><button id="savecfg" class="on">GUARDAR AJUSTES</button><button id="reboot">REINICIAR</button></div>
<form method="POST" action="/update" enctype="multipart/form-data" class="row"><input type="file" name="firmware" accept=".bin"><button>ACTUALIZAR FIRMWARE</button></form>
<div class="mut" id="info"></div></div></details></div>
</main>
<script>
const $=s=>document.querySelector(s);let st=null,busy=0,tmr=null;
const tok=()=>localStorage.getItem("nl_token")||"";
async function api(p,b,m){const h={"Content-Type":"application/json"};if(tok())h["X-Nexus-Token"]=tok();
 const r=await fetch(p,{method:m||(b?"POST":"GET"),headers:h,body:b?JSON.stringify(b):undefined});
 if(r.status==401){const t=prompt("Clave del dispositivo");if(t!=null){localStorage.setItem("nl_token",t);return api(p,b,m)}}
 return r.json()}
function send(b){clearTimeout(tmr);tmr=setTimeout(async()=>{st=await api("/api/state",b);paint()},120)}
function paint(){if(!st)return;const s=st.seg[0];$("#pw").className=st.on?"on":"";$("#pw").textContent=st.on?"ENCENDIDO":"APAGADO";
 $("#bri").value=st.bri;$("#fx").value=s.fx;$("#pal").value=s.pal;$("#sx").value=s.sx;$("#ix").value=s.ix;
 ["c0","c1","c2"].forEach((k,i)=>$("#"+k).value=s.col[i])}
async function presets(){const ps=await api("/api/presets");$("#presets").innerHTML="";ps.forEach(p=>{const b=document.createElement("button");
 b.textContent=p.name;b.onclick=()=>send({preset:p.id});b.oncontextmenu=e=>{e.preventDefault();if(confirm("¿Borrar "+p.name+"?"))api("/api/presets?id="+p.id,null,"DELETE").then(presets)};
 $("#presets").appendChild(b)});if(!ps.length)$("#presets").innerHTML='<span class="mut">Sin presets. Mantén pulsado (o clic derecho) para borrar.</span>'}
function outRow(o){const d=document.createElement("div");d.className="out";d.innerHTML=`<label>GPIO<input type="number" value="${o.pin}" class="o-pin"></label>
 <label>LED<input type="number" value="${o.count}" class="o-n"></label><label>Orden<select class="o-ord">${["GRB","RGB","BRG","RBG","GBR","BGR"].map(x=>`<option ${x==o.order?"selected":""}>${x}</option>`).join("")}</select></label>
 <label>RGBW<input type="checkbox" class="o-w" ${o.rgbw?"checked":""}></label><button type="button" class="dng">QUITAR</button>`;d.querySelector("button").onclick=()=>d.remove();$("#outs").appendChild(d)}
async function cfg(){const c=await api("/api/config");if(c.error)return;$("#name").value=c.name;$("#maxma").value=c.max_ma;$("#fps").value=c.fps;
 $("#ssid").innerHTML=`<option>${c.wifi.ssid||""}</option>`;$("#outs").innerHTML="";c.outputs.forEach(outRow)}
async function info(){const i=await api("/api/info");$("#hdr").textContent=`${i.name} · ${i.leds} LED · v${i.version}${i.ip?" · "+i.ip:""}`;
 $("#info").textContent=`ID ${i.id} · ${i.wifi||""} ${i.rssi?i.rssi+" dBm":""} · memoria libre ${i.heap||"?"} · consumo estimado ${i.power_ma} mA`}
async function preview(){try{const f=await api("/api/frame");const cv=$("#cv"),n=f.n||1,w=cv.width=Math.min(n,600);const x=cv.getContext("2d");
 for(let i=0;i<w;i++){const k=Math.floor(i*n/w)*6;x.fillStyle="#"+f.hex.substr(k,6);x.fillRect(i,0,1,28)}}catch(e){}setTimeout(preview,250)}
(async()=>{const [fx,pal]=await Promise.all([api("/api/effects"),api("/api/palettes")]);
 $("#fx").innerHTML=fx.map(e=>`<option value="${e.id}">${e.name}</option>`).join("");$("#pal").innerHTML=pal.map(e=>`<option value="${e.id}">${e.name}</option>`).join("");
 st=await api("/api/state");paint();presets();cfg();info();preview()})();
$("#pw").onclick=()=>send({on:"t"});$("#bri").oninput=e=>send({bri:+e.target.value});
$("#fx").onchange=e=>send({fx:+e.target.value});$("#pal").onchange=e=>send({pal:+e.target.value});
$("#sx").oninput=e=>send({sx:+e.target.value});$("#ix").oninput=e=>send({ix:+e.target.value});
["c0","c1","c2"].forEach(k=>$("#"+k).oninput=()=>send({col:["c0","c1","c2"].map(c=>$("#"+c).value)}));
$("#psave").onclick=async()=>{await api("/api/presets",{name:$("#pname").value});$("#pname").value="";presets()};
$("#addout").onclick=()=>outRow({pin:17,count:30,order:"GRB"});
$("#scan").onclick=async()=>{$("#scan").textContent="…";const r=await api("/api/wifi/scan");$("#scan").textContent="BUSCAR";
 $("#ssid").innerHTML=(r.networks||[]).map(n=>`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`).join("")};
$("#savecfg").onclick=async()=>{const outs=[...document.querySelectorAll(".out")].map(d=>({pin:+d.querySelector(".o-pin").value,count:+d.querySelector(".o-n").value,
 order:d.querySelector(".o-ord").value,rgbw:d.querySelector(".o-w").checked}));const b={name:$("#name").value,outputs:outs,max_ma:+$("#maxma").value,fps:+$("#fps").value};
 if($("#wpass").value||$("#ssid").value)b.wifi={ssid:$("#ssid").value};if($("#wpass").value)b.wifi.pass=$("#wpass").value;
 if($("#tok").value){b.token=$("#tok").value}if($("#appass").value)b.ap_pass=$("#appass").value;
 const r=await api("/api/config",b);if(b.token)localStorage.setItem("nl_token",b.token);
 alert(r.ok?(r.reboot?"Guardado. El dispositivo se reinicia para conectarse a la nueva red.":"Guardado."):("Error: "+r.error));if(r.reboot)api("/api/reboot",{});cfg();info()};
document.querySelector("form").onsubmit=e=>{e.target.action="/update?token="+encodeURIComponent(tok())};
$("#reboot").onclick=()=>confirm("¿Reiniciar?")&&api("/api/reboot",{});
</script></body></html>)NLHTML";
