// NexusWatch 1.0.0 - reloj para Nexus-Office en la placa ESP32-2424S012C (ESP32-C3, pantalla redonda GC9A01 de
// 1,28" 240x240 y tactil CST816S). Variantes -Y y (B): mismos pines.
//
// Pantalla de inicio: el avatar de Nexus animado segun su estado (escucha, piensa, usa herramientas, habla...).
// Deslizando a izquierda o derecha, las "apps": AGENTES (que hace cada uno, toca para ver su escena), MODOS (toca
// para cambiar el modo de Nexus en el escritorio o el movil) e INFO (Wi-Fi y conexion; manten pulsado para
// configurar la Wi-Fi desde el movil).
//
// Requisitos: Arduino IDE 2.x o arduino-cli con el nucleo "esp32" de Espressif 3.x; placa "ESP32C3 Dev Module"
// con "USB CDC On Boot: Enabled". Sin librerias externas.
//
// Configuracion: la graba el script de Nexus (firmware/flash.py) por el puerto serie, o desde el movil: si no hay
// Wi-Fi, el reloj crea la red "NexusWatch-XXXX" (clave "nexuswatch") con una pagina en http://192.168.4.1.
// Linea JSON por el puerto serie (115200): {"wifi":{"ssid":"...","pass":"..."},"nexus":{"host":"192.168.1.10:8775","key":"..."}}

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "NexusWatch necesita el nucleo esp32 de Arduino 3.x"
#endif

#define NW_STRIP 24
#include "nw_core.h"

// ------------------------------------------------------------------ pines de la ESP32-2424S012C
#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_CS 10
#define PIN_DC 2
#define PIN_RST -1
#define PIN_BL 3
#define PIN_TP_SDA 4
#define PIN_TP_SCL 5
#define PIN_TP_INT 0
#define PIN_TP_RST 1
#define CST816_ADDR 0x15

static nw::Watch watch;
static nw::Canvas canvas;
static Preferences prefs;
static WebServer portal(80);
static String ssid, pass, host, key, devId;
static bool apMode = false;
static SemaphoreHandle_t netLock;
static String netBody;
static bool netNew = false, netFail = false;
static String netErr;
static String modeToSend;

// ------------------------------------------------------------------ GC9A01
static SPISettings spiCfg(40000000, MSBFIRST, SPI_MODE0);

static void lcdCmd(uint8_t c, const uint8_t* d = nullptr, size_t n = 0) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  SPI.write(c);
  if (n) {
    digitalWrite(PIN_DC, HIGH);
    SPI.writeBytes(d, n);
  }
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

// secuencia de arranque habitual del GC9A01 (la misma de los ejemplos del fabricante)
static const uint8_t GC9A01_INIT[] PROGMEM = {
    0xEF, 0, 0xEB, 1, 0x14, 0xFE, 0, 0xEF, 0, 0xEB, 1, 0x14, 0x84, 1, 0x40, 0x85, 1, 0xFF, 0x86, 1, 0xFF, 0x87, 1,
    0xFF, 0x88, 1, 0x0A, 0x89, 1, 0x21, 0x8A, 1, 0x00, 0x8B, 1, 0x80, 0x8C, 1, 0x01, 0x8D, 1, 0x01, 0x8E, 1, 0xFF,
    0x8F, 1, 0xFF, 0xB6, 2, 0x00, 0x20, 0x36, 1, 0x08, 0x3A, 1, 0x05, 0x90, 4, 0x08, 0x08, 0x08, 0x08, 0xBD, 1, 0x06,
    0xBC, 1, 0x00, 0xFF, 3, 0x60, 0x01, 0x04, 0xC3, 1, 0x13, 0xC4, 1, 0x13, 0xC9, 1, 0x22, 0xBE, 1, 0x11, 0xE1, 2,
    0x10, 0x0E, 0xDF, 3, 0x21, 0x0C, 0x02, 0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, 0xF1, 6, 0x43, 0x70, 0x72,
    0x36, 0x37, 0x6F, 0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, 0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, 0xED,
    2, 0x1B, 0x0B, 0xAE, 1, 0x77, 0xCD, 1, 0x63, 0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03, 0xE8,
    1, 0x34, 0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70, 0x63, 12, 0x18, 0x11,
    0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70, 0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
    0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00, 0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00,
    0x01, 0x54, 0x10, 0x32, 0x98, 0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00, 0x98, 2, 0x3E, 0x07, 0x35, 0,
    0x21, 0};

static void lcdInit() {
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  if (PIN_RST >= 0) {
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    delay(20);
    digitalWrite(PIN_RST, HIGH);
    delay(120);
  }
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  size_t i = 0;
  uint8_t tmp[16];
  while (i < sizeof(GC9A01_INIT)) {
    uint8_t c = pgm_read_byte(GC9A01_INIT + i++), n = pgm_read_byte(GC9A01_INIT + i++);
    for (uint8_t k = 0; k < n; k++) tmp[k] = pgm_read_byte(GC9A01_INIT + i++);
    lcdCmd(c, tmp, n);
  }
  lcdCmd(0x11);
  delay(120);
  lcdCmd(0x29);
  delay(20);
}

static uint8_t swapbuf[NW_W * NW_STRIP * 2];
static void lcdPushStrip(int y0, int h) {
  uint8_t ca[4] = {0, 0, 0, NW_W - 1}, ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y0 + h - 1) >> 8),
                                                (uint8_t)(y0 + h - 1)};
  lcdCmd(0x2A, ca, 4);
  lcdCmd(0x2B, ra, 4);
  for (int i = 0; i < NW_W * h; i++) {  // la pantalla espera RGB565 en big endian
    swapbuf[2 * i] = canvas.buf[i] >> 8;
    swapbuf[2 * i + 1] = canvas.buf[i] & 0xFF;
  }
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  SPI.write(0x2C);
  digitalWrite(PIN_DC, HIGH);
  SPI.writeBytes(swapbuf, NW_W * h * 2);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

static void drawFrame(uint32_t now) {
  for (int top = 0; top < NW_H; top += NW_STRIP) {
    canvas.begin(top);
    watch.render(canvas, now);
    lcdPushStrip(top, canvas.h);
  }
}

// ------------------------------------------------------------------ CST816S (tactil)
static bool tpDown = false;
static int tpX0, tpY0, tpX, tpY;
static uint32_t tpT0;

static void touchInit() {
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(50);
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  Wire.beginTransmission(CST816_ADDR);  // sin auto-reposo: si no, deja de contestar
  Wire.write(0xFE);
  Wire.write(0x01);
  Wire.endTransmission();
}

// devuelve un gesto propio a partir de pulsar y soltar (no depende de los gestos del chip)
static uint8_t touchPoll(int& gx, int& gy) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return nw::G_NONE;
  if (Wire.requestFrom(CST816_ADDR, 5) != 5) return nw::G_NONE;
  uint8_t fingers = Wire.read(), xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  int x = ((xh & 0x0F) << 8) | xl, y = ((yh & 0x0F) << 8) | yl;
  uint32_t now = millis();
  if (fingers) {
    if (!tpDown) {
      tpDown = true;
      tpX0 = x; tpY0 = y; tpT0 = now;
    }
    tpX = x; tpY = y;
    if (now - tpT0 > 900 && abs(tpX - tpX0) < 15 && abs(tpY - tpY0) < 15) {
      tpDown = false;  // pulsacion larga: no esperar a soltar
      gx = x; gy = y;
      while (true) {   // esperar a soltar
        Wire.beginTransmission(CST816_ADDR);
        Wire.write(0x02);
        Wire.endTransmission(false);
        Wire.requestFrom(CST816_ADDR, 1);
        if (!Wire.available() || !Wire.read()) break;
        delay(20);
      }
      return nw::G_LONG;
    }
    return nw::G_NONE;
  }
  if (!tpDown) return nw::G_NONE;
  tpDown = false;
  int dx = tpX - tpX0, dy = tpY - tpY0;
  gx = tpX0; gy = tpY0;
  if (abs(dx) > 40 && abs(dx) > abs(dy)) return dx < 0 ? nw::G_LEFT : nw::G_RIGHT;
  if (abs(dy) > 40) return dy < 0 ? nw::G_UP : nw::G_DOWN;
  static uint32_t lastTap = 0;
  uint8_t g = (now - lastTap < 350) ? nw::G_DOUBLE : nw::G_TAP;
  lastTap = now;
  return g;
}

// ------------------------------------------------------------------ configuracion
static void loadCfg() {
  prefs.begin("nexuswatch", false);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  host = prefs.getString("host", "");
  key = prefs.getString("key", "");
}

static void saveCfg() {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("host", host);
  prefs.putString("key", key);
}

static String applyJson(const std::string& line) {
  nl::JVal v;
  if (!nl::parseJson(line, v) || !v.isObj()) return "{\"error\":\"JSON no valido\"}";
  if (const nl::JVal* c = v.get("cmd")) {
    if (c->isStr() && c->s == "info") {
      return String("{\"nexuswatch\":\"" NEXUSWATCH_VERSION "\",\"id\":\"") + devId + "\",\"ssid\":\"" + ssid +
             "\",\"host\":\"" + host + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    }
    if (c->isStr() && c->s == "reboot") { delay(100); ESP.restart(); }
  }
  bool changed = false;
  if (const nl::JVal* w = v.get("wifi")) {
    if (const nl::JVal* s = w->get("ssid")) if (s->isStr()) { ssid = s->s.c_str(); changed = true; }
    if (const nl::JVal* p = w->get("pass")) if (p->isStr()) { pass = p->s.c_str(); changed = true; }
  }
  if (const nl::JVal* n = v.get("nexus")) {
    if (const nl::JVal* h = n->get("host")) if (h->isStr()) { host = h->s.c_str(); changed = true; }
    if (const nl::JVal* k = n->get("key")) if (k->isStr()) { key = k->s.c_str(); changed = true; }
  }
  if (!changed) return "{\"error\":\"nada que guardar\"}";
  saveCfg();
  return "{\"ok\":true,\"reboot\":true}";
}

static void startPortal() {
  apMode = true;
  String ap = "NexusWatch-" + devId.substring(devId.length() - 4);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap.c_str(), "nexuswatch");
  watch.ssid = ap.c_str();
  watch.ip = "192.168.4.1";
  watch.offline("Conéctate a " + std::string(ap.c_str()));
  portal.on("/", HTTP_GET, []() {
    String page = F("<!doctype html><meta name=viewport content='width=device-width'><title>NexusWatch</title>"
                    "<body style='font-family:sans-serif;background:#04060c;color:#d8f6ff;padding:18px'>"
                    "<h2 style='color:#00f0ff'>NexusWatch</h2><form method=post action=/guardar>"
                    "<p>Wi-Fi<br><input name=ssid style='width:100%;font-size:17px'></p>"
                    "<p>Clave Wi-Fi<br><input name=pass type=password style='width:100%;font-size:17px'></p>"
                    "<p>Nexus (IP:puerto del PC o móvil)<br><input name=host placeholder='192.168.1.10:8775' style='width:100%;font-size:17px'></p>"
                    "<p>Clave de red de Nexus (Red COOP &gt; Conectar dispositivos)<br><input name=key style='width:100%;font-size:17px'></p>"
                    "<button style='font-size:18px;padding:10px 18px'>Guardar</button></form>");
    portal.send(200, "text/html; charset=utf-8", page);
  });
  portal.on("/guardar", HTTP_POST, []() {
    ssid = portal.arg("ssid");
    pass = portal.arg("pass");
    host = portal.arg("host");
    key = portal.arg("key");
    saveCfg();
    portal.send(200, "text/html; charset=utf-8", "<p style='font-family:sans-serif'>Guardado. El reloj se reinicia.</p>");
    delay(800);
    ESP.restart();
  });
  portal.begin();
}

// ------------------------------------------------------------------ red (tarea aparte para no parar la animacion)
static void netTask(void*) {
  uint32_t backoff = 1500;
  for (;;) {
    if (apMode || host.isEmpty()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (WiFi.status() != WL_CONNECTED) {
      xSemaphoreTake(netLock, portMAX_DELAY);
      netFail = true; netErr = "Sin Wi-Fi";
      xSemaphoreGive(netLock);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    String mode;
    xSemaphoreTake(netLock, portMAX_DELAY);
    mode = modeToSend; modeToSend = "";
    xSemaphoreGive(netLock);
    HTTPClient http;
    if (mode.length()) {
      http.begin("http://" + host + "/api/dispositivos/modo");
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Nexus-Key", key);
      http.setTimeout(4000);
      http.POST("{\"modo\":\"" + mode + "\",\"origen\":\"reloj\"}");
      http.end();
    }
    http.begin("http://" + host + "/api/dispositivos/reloj?cliente=" + devId);
    http.addHeader("X-Nexus-Key", key);
    http.setTimeout(3000);
    int code = http.GET();
    xSemaphoreTake(netLock, portMAX_DELAY);
    if (code == 200) {
      netBody = http.getString();
      netNew = true; netFail = false; backoff = 1500;
    } else {
      netFail = true;
      netErr = code == 401 ? "Clave de red incorrecta" : code < 0 ? "Nexus no responde" : ("Error " + String(code));
      backoff = backoff < 8000 ? backoff * 2 : 8000;
    }
    xSemaphoreGive(netLock);
    http.end();
    vTaskDelay(pdMS_TO_TICKS(code == 200 ? 1500 : backoff));
  }
}

// ------------------------------------------------------------------ arranque y bucle
static uint32_t lastFrame = 0, lastActivity = 0;
static std::string serialLine;
static uint8_t blLevel = 255;

static void backlight(uint8_t v) {
  if (v != blLevel) { analogWrite(PIN_BL, v); blLevel = v; }
}

void setup() {
  Serial.begin(115200);
  uint64_t mac = ESP.getEfuseMac();
  char id[20];
  snprintf(id, sizeof id, "reloj-%06lx", (unsigned long)(mac & 0xFFFFFF));
  devId = id;
  pinMode(PIN_BL, OUTPUT);
  backlight(200);
  lcdInit();
  touchInit();
  loadCfg();
  netLock = xSemaphoreCreateMutex();
  watch.host = host.c_str();
  watch.offline(host.isEmpty() ? "Sin configurar" : "Conectando...");
  drawFrame(millis());
  if (ssid.isEmpty()) startPortal();
  else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    watch.ssid = ssid.c_str();
  }
  xTaskCreate(netTask, "nexus-red", 8192, nullptr, 1, nullptr);
  lastActivity = millis();
  Serial.printf("[nexuswatch] v%s %s\n", NEXUSWATCH_VERSION, devId.c_str());
}

void loop() {
  uint32_t now = millis();
  if (apMode) portal.handleClient();
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (!serialLine.empty()) {
        String r = applyJson(serialLine);
        Serial.println(r);
        serialLine.clear();
        if (r.indexOf("reboot") > 0) { delay(200); ESP.restart(); }
      }
    } else if (serialLine.size() < 1024) serialLine += ch;
  }
  if (!apMode && WiFi.status() == WL_CONNECTED) watch.ip = WiFi.localIP().toString().c_str();
  // datos nuevos de Nexus
  xSemaphoreTake(netLock, portMAX_DELAY);
  if (netNew) {
    std::string body = netBody.c_str();
    netNew = false;
    xSemaphoreGive(netLock);
    std::string prev = watch.state;
    watch.load(body, now);
    if (watch.state != prev && watch.state != "idle") lastActivity = now;  // se enciende al cambiar de estado
  } else {
    if (netFail && now - watch.lastData > 6000) watch.offline(netErr.c_str());
    xSemaphoreGive(netLock);
  }
  // tactil
  int gx = 120, gy = 120;
  uint8_t g = touchPoll(gx, gy);
  if (g != nw::G_NONE) {
    lastActivity = now;
    if (blLevel < 40) g = nw::G_NONE;  // el primer toque solo enciende la pantalla
    else if (g == nw::G_LONG && watch.screen == nw::S_INFO && !apMode) startPortal();
    else watch.gesture(g, gx, gy, now);
  }
  if (!watch.pendingMode.empty()) {
    xSemaphoreTake(netLock, portMAX_DELAY);
    modeToSend = watch.pendingMode.c_str();
    xSemaphoreGive(netLock);
    watch.pendingMode.clear();
  }
  // brillo: atenua a los 30 s y apaga a los 2 min sin uso (el avatar se ve mientras Nexus trabaja)
  uint32_t idle = now - lastActivity;
  bool working = watch.online && watch.state != "idle";
  backlight(working || idle < 30000 ? 200 : idle < 120000 ? 40 : 0);
  // fotograma: ~12 fps con animacion, 2 fps en las listas
  uint32_t period = (watch.screen == nw::S_AVATAR || watch.screen == nw::S_AGENT) ? 80 : 400;
  if (blLevel && now - lastFrame >= period) {
    lastFrame = now;
    drawFrame(now);
  }
  delay(5);
}
