// NexusLED - firmware para ESP32 y ESP8266 y tiras WS2812/WS2812B/SK6812, controlable desde NEXUS AI.
//
// Requisitos: Arduino IDE 2.x (o arduino-cli / PlatformIO) con el nucleo "esp32" de Espressif 3.x, o el nucleo
// "esp8266" 3.x para ESP-01/01S, ESP-12, NodeMCU, Wemos D1 mini y demas ESP8266.
// No necesita librerias externas: en el ESP32 los LED van por el periferico RMT; en el ESP8266, por un envio
// temporizado con el contador de ciclos (como hacen las librerias de NeoPixel).
//
// Primer arranque: crea la red Wi-Fi "NexusLED-XXXXXX" (clave "nexusled"). Conectate y abre
// http://192.168.4.1 para elegir tu Wi-Fi, los pines y el numero de LED. Despues, en NEXUS:
// Ajustes > Luces LED > Buscar.
//
// Protocolo (ver docs/LEDS.md de NEXUS): HTTP JSON en el puerto 80 (/api/...), UDP 4210
// (descubrimiento "NEXUSLED?" y pixeles en directo "NXL\x01") y una linea JSON por orden en el
// puerto serie USB a 115200 baudios.

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFiUdp.h>

#if defined(ESP8266)
// ESP8266 (ESP-01/01S, ESP-12, NodeMCU, Wemos D1 mini...): GPIO2 por defecto (el unico libre en el ESP-01;
// D4 en NodeMCU y Wemos) y como mucho 600 LED (80 KB de RAM y el envio para las interrupciones)
#define NL_DEFAULT_PIN 2
#define NL_MAX_LEDS 600
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <Updater.h>
using WebServer = ESP8266WebServer;
#else
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "NexusLED necesita el nucleo esp32 de Arduino 3.x (Herramientas > Placa > Gestor de placas > esp32 de Espressif)"
#endif
#endif

#include "nl_core.h"
#include "nl_ui.h"

using nl::Device;

#if defined(ESP8266)
// Preferences del ESP32 sobre ficheros de LittleFS (una clave = un fichero)
struct Preferences {
  bool begin(const char*, bool) { return LittleFS.begin() || (LittleFS.format() && LittleFS.begin()); }
  static String path(const char* k) { return String("/") + k; }
  size_t getBytesLength(const char* k) {
    File f = LittleFS.open(path(k), "r");
    size_t n = f ? f.size() : 0;
    if (f) f.close();
    return n;
  }
  size_t getBytes(const char* k, void* buf, size_t n) {
    File f = LittleFS.open(path(k), "r");
    if (!f) return 0;
    size_t r = f.read((uint8_t*)buf, n);
    f.close();
    return r;
  }
  size_t putBytes(const char* k, const void* buf, size_t n) {
    File f = LittleFS.open(path(k), "w");
    if (!f) return 0;
    size_t r = f.write((const uint8_t*)buf, n);
    f.close();
    return r;
  }
  bool clear() { return LittleFS.format(); }
};
#endif

static Device dev;
static WebServer server(80);
static DNSServer dns;
static WiFiUDP udp;
static Preferences prefs;

static bool apMode = false;
static uint32_t wifiStart = 0, lastFrame = 0, rebootAt = 0;
static bool mdnsUp = false;
static bool updateOk = false;

// ------------------------------------------------------------------ salida a los LED
#if defined(ESP8266)
// Envio temporizado con el contador de ciclos y las interrupciones paradas: WS2812 a 800 kHz
// (0 = 0,4 us alto + 0,85 us bajo; 1 = 0,8 us alto + 0,45 us bajo). Vale para GPIO 0-5 y 12-15.
struct HwOut {
  int pin = -1;
  bool ok = false;
  std::vector<uint8_t> bytes;
};
static HwOut hw[nl::MAX_OUTPUTS];

static void IRAM_ATTR ws2812Send(uint8_t pin, const uint8_t* p, size_t n) {
  const uint32_t mask = 1u << pin;
  const uint32_t mhz = ESP.getCpuFreqMHz();
  const uint32_t t0h = mhz * 400 / 1000, t1h = mhz * 800 / 1000, period = mhz * 1250 / 1000;
  uint32_t start = ESP.getCycleCount() - period;
  uint32_t ps = xt_rsil(15);
  for (size_t i = 0; i < n; i++) {
    uint8_t b = p[i];
    for (uint8_t m = 0x80; m; m >>= 1) {
      uint32_t high = (b & m) ? t1h : t0h;
      while (ESP.getCycleCount() - start < period) {
      }
      start = ESP.getCycleCount();
      GPOS = mask;
      while (ESP.getCycleCount() - start < high) {
      }
      GPOC = mask;
    }
  }
  while (ESP.getCycleCount() - start < period) {
  }
  xt_wsr_ps(ps);
}

static void releaseOutputs() {
  for (auto& h : hw) {
    h.ok = false;
    h.pin = -1;
  }
}

static void setupOutputs() {
  releaseOutputs();
  for (size_t i = 0; i < dev.outputs.size() && i < nl::MAX_OUTPUTS; i++) {
    int pin = dev.outputs[i].pin;
    hw[i].pin = pin;
    hw[i].ok = pin >= 0 && pin <= 15 && !(pin >= 6 && pin <= 11);   // 6-11 son la memoria flash; 16 no es rapido
    if (hw[i].ok) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
    } else {
      Serial.printf("[nexusled] el GPIO %d no vale para la tira %d en el ESP8266 (usa 0-5 o 12-15)\n", pin, (int)i + 1);
    }
  }
  if (dev.relayPin >= 0) pinMode(dev.relayPin, OUTPUT);
  if (dev.buttonPin >= 0) pinMode(dev.buttonPin, INPUT_PULLUP);
}

static void showOutputs() {
  for (size_t i = 0; i < dev.outputs.size() && i < nl::MAX_OUTPUTS; i++) {
    HwOut& h = hw[i];
    if (!h.ok) continue;
    dev.outputBytes((int)i, h.bytes);
    ws2812Send((uint8_t)h.pin, h.bytes.data(), h.bytes.size());
  }
  if (dev.relayPin >= 0) {
    bool lit = dev.powermA > dev.total() + 2;
    digitalWrite(dev.relayPin, (lit ^ dev.relayInvert) ? HIGH : LOW);
  }
}
#else
// ESP32: periferico RMT
struct HwOut {
  int pin = -1;
  bool ok = false;
  std::vector<rmt_data_t> sym;
  std::vector<uint8_t> bytes;
};
static HwOut hw[nl::MAX_OUTPUTS];

static void releaseOutputs() {
  for (auto& h : hw) {
    if (h.ok) rmtDeinit(h.pin);
    h.ok = false;
    h.pin = -1;
  }
}

static void setupOutputs() {
  releaseOutputs();
  for (size_t i = 0; i < dev.outputs.size() && i < nl::MAX_OUTPUTS; i++) {
    int pin = dev.outputs[i].pin;
    hw[i].pin = pin;
    // 10 MHz: cada tick son 100 ns
    hw[i].ok = rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
    if (!hw[i].ok) Serial.printf("[nexusled] no se pudo usar el GPIO %d para la tira %d\n", pin, (int)i + 1);
  }
  if (dev.relayPin >= 0) {
    pinMode(dev.relayPin, OUTPUT);
  }
  if (dev.buttonPin >= 0) pinMode(dev.buttonPin, INPUT_PULLUP);
}

static void showOutputs() {
  for (size_t i = 0; i < dev.outputs.size() && i < nl::MAX_OUTPUTS; i++) {
    HwOut& h = hw[i];
    if (!h.ok) continue;
    dev.outputBytes((int)i, h.bytes);
    h.sym.resize(h.bytes.size() * 8);
    size_t k = 0;
    for (uint8_t b : h.bytes) {
      for (int bit = 7; bit >= 0; bit--) {
        bool one = b & (1 << bit);
        rmt_data_t& s = h.sym[k++];
        // WS2812: 0 = 0,4 us alto + 0,85 us bajo; 1 = 0,8 us alto + 0,45 us bajo
        s.level0 = 1;
        s.duration0 = one ? 8 : 4;
        s.level1 = 0;
        s.duration1 = one ? 4 : 8;
      }
    }
    rmtWrite(h.pin, h.sym.data(), h.sym.size(), 100);
  }
  if (dev.relayPin >= 0) {
    bool lit = dev.powermA > dev.total() + 2;
    digitalWrite(dev.relayPin, (lit ^ dev.relayInvert) ? HIGH : LOW);
  }
}

#endif

// ------------------------------------------------------------------ guardar y cargar
static String readBlob(const char* key) {
  size_t n = prefs.getBytesLength(key);
  if (!n) return String();
  std::vector<char> buf(n + 1, 0);
  prefs.getBytes(key, buf.data(), n);
  return String(buf.data());
}
static void writeBlob(const char* key, const std::string& s) { prefs.putBytes(key, s.data(), s.size()); }

static void loadAll() {
  prefs.begin("nexusled", false);
  String c = readBlob("cfg");
  if (c.length()) dev.loadConfig(c.c_str());
  String p = readBlob("presets");
  if (p.length()) dev.loadPresets(p.c_str());
  String s = readBlob("state");
  if (s.length()) dev.loadState(s.c_str());
}

static void saveDirty(uint32_t now) {
  if (dev.configDirty) {
    writeBlob("cfg", dev.configJson(true));
    dev.configDirty = false;
  }
  if (dev.presetsDirty) {
    writeBlob("presets", dev.presetsJson(true));
    dev.presetsDirty = false;
  }
  if (dev.stateNeedsSave(now)) {
    writeBlob("state", dev.stateJson(false));
    dev.stateDirtyAt = 0;
  }
}

// ------------------------------------------------------------------ Wi-Fi
static String hostName() {
  String h;
  for (char ch : dev.name) {
    if (isalnum((unsigned char)ch)) h += (char)tolower((unsigned char)ch);
    else if (ch == ' ' || ch == '-' || ch == '_') h += '-';
  }
  if (!h.length()) h = "nexusled";
  return h + "-" + String(dev.deviceId.c_str()).substring(2);
}

static void startAP() {
  if (apMode) return;
  String ap = "NexusLED-" + String(dev.deviceId.c_str());
  WiFi.mode(WIFI_AP_STA);  // AP + STA: permite buscar redes desde la pagina de configuracion
  WiFi.softAP(ap.c_str(), dev.apPass.size() >= 8 ? dev.apPass.c_str() : nullptr);
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  Serial.printf("[nexusled] punto de acceso %s en http://%s\n", ap.c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopAP() {
  if (!apMode) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
}

static void startWifi() {
  WiFi.persistent(false);
#if defined(ESP8266)
  WiFi.hostname(hostName().c_str());
#else
  WiFi.setHostname(hostName().c_str());
#endif
  if (dev.ssid.empty()) {
    startAP();
    return;
  }
  WiFi.mode(WIFI_STA);
#if defined(ESP8266)
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // menos latencia y menos parpadeos
#else
  WiFi.setSleep(false);  // menos latencia y menos parpadeos
#endif
  WiFi.begin(dev.ssid.c_str(), dev.pass.c_str());
  wifiStart = millis();
}

static void wifiLoop(uint32_t now) {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !mdnsUp) {
    if (MDNS.begin(hostName().c_str())) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("nexusled", "tcp", 80);
      MDNS.addServiceTxt("nexusled", "tcp", "id", dev.deviceId.c_str());
      MDNS.addServiceTxt("nexusled", "tcp", "version", NEXUSLED_VERSION);
    }
    mdnsUp = true;
    Serial.printf("[nexusled] conectado a %s: http://%s (%s.local)\n", dev.ssid.c_str(), WiFi.localIP().toString().c_str(),
                  hostName().c_str());
    if (apMode && now > 60000) stopAP();
  }
  if (!up && mdnsUp) {
    MDNS.end();
    mdnsUp = false;
  }
  // sin conexion 20 s: se abre el punto de acceso para poder configurarlo (y se sigue intentando)
  if (!up && !apMode && now - wifiStart > 20000) startAP();
  // con conexion estable, el punto de acceso se cierra a los 2 minutos
  if (up && apMode && now - wifiStart > 120000) stopAP();
  if (apMode) dns.processNextRequest();
#if defined(ESP8266)
  if (mdnsUp) MDNS.update();
#endif
}

// ------------------------------------------------------------------ HTTP
static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Nexus-Token");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
}

static void handleAny() {
  String uri = server.uri();
  if (!uri.startsWith("/api/")) {
    // portal cautivo: cualquier pagina lleva a la interfaz
    server.send_P(200, "text/html", NL_UI_HTML);
    return;
  }
  cors();
  if (uri == "/api/wifi/scan" && server.method() == HTTP_GET) {
    int n = WiFi.scanNetworks();
    nl::JWriter w;
    w.obj().key("networks").arr();
    for (int i = 0; i < n; i++) w.obj().kv("ssid", WiFi.SSID(i).c_str()).kv("rssi", (int)WiFi.RSSI(i)).end();
    w.end().end();
    WiFi.scanDelete();
    server.send(200, "application/json", w.out.c_str());
    return;
  }
  nl::Request r;
  switch (server.method()) {
    case HTTP_GET: r.method = "GET"; break;
    case HTTP_POST: r.method = "POST"; break;
    case HTTP_PUT: r.method = "POST"; break;
    case HTTP_DELETE: r.method = "DELETE"; break;
    case HTTP_OPTIONS: r.method = "OPTIONS"; break;
    default: r.method = "GET";
  }
  r.path = uri.c_str();
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "plain") continue;
    r.query.push_back({server.argName(i).c_str(), server.arg(i).c_str()});
  }
  if (server.hasArg("plain")) r.body = server.arg("plain").c_str();
  r.token = server.header("X-Nexus-Token").c_str();
  nl::Response res = dev.handle(r);
  if (res.code == 204) {
    server.send(204);
    return;
  }
  server.send(res.code, res.type.c_str(), res.body.c_str());
}

static void setupHttp() {
#if defined(ESP8266)
  server.collectHeaders("X-Nexus-Token");
#else
  static const char* headers[] = {"X-Nexus-Token"};
  server.collectHeaders(headers, 1);
#endif
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", NL_UI_HTML); });
  // actualizacion de firmware desde el navegador (.bin de Arduino: Programa > Exportar binario compilado)
  server.on(
      "/update", HTTP_POST,
      []() {
        bool ok = updateOk;
        server.send(200, "text/html", ok ? "<meta http-equiv='refresh' content='8;url=/'>Actualizado. Reiniciando..."
                                         : "Error al actualizar el firmware");
        if (ok) rebootAt = millis() + 500;
      },
      []() {
        HTTPUpload& up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
          if (!dev.token.empty() && server.arg("token") != dev.token.c_str() &&
              server.header("X-Nexus-Token") != dev.token.c_str()) {
            Serial.println("[nexusled] actualizacion rechazada: falta la clave");
            return;
          }
          updateOk = false;
#if defined(ESP8266)
          Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
#else
          Update.begin(UPDATE_SIZE_UNKNOWN);
#endif
        } else if (up.status == UPLOAD_FILE_WRITE) {
          if (Update.isRunning()) Update.write(up.buf, up.currentSize);
        } else if (up.status == UPLOAD_FILE_END) {
          if (Update.isRunning()) updateOk = Update.end(true);
        }
      });
  server.onNotFound(handleAny);
  server.begin();
}

// ------------------------------------------------------------------ UDP y serie
static void udpLoop() {
  int n = udp.parsePacket();
  if (n <= 0) return;
  static uint8_t buf[1472];
  int len = udp.read(buf, sizeof buf);
  if (len <= 0) return;
  std::string reply = dev.handleUdp(buf, (size_t)len);
  if (!reply.empty()) {
    // la IP va en la respuesta para quien escuche por difusion
    if (reply.back() == '}') {
      reply.pop_back();
      reply += ",\"ip\":\"";
      reply += (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str();
      reply += "\"}";
    }
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write((const uint8_t*)reply.data(), reply.size());
    udp.endPacket();
  }
}

static std::string serialLine;
static void serialLoop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!serialLine.empty() && serialLine[0] == '{') Serial.println(dev.handleSerial(serialLine).c_str());
      serialLine.clear();
    } else if (serialLine.size() < 8192) {
      serialLine += c;
    }
  }
}

// ------------------------------------------------------------------ boton: pulsacion corta = encender/apagar,
// larga (1 s) = siguiente preset, muy larga (10 s) = olvidar el Wi-Fi y abrir el punto de acceso
static void buttonLoop(uint32_t now) {
  static uint32_t downAt = 0;
  static bool wasDown = false;
  if (dev.buttonPin < 0) return;
  bool down = digitalRead(dev.buttonPin) == LOW;
  if (down && !wasDown) downAt = now;
  if (!down && wasDown) {
    uint32_t held = now - downAt;
    std::string err;
    nl::JVal v;
    if (held > 10000) {
      nl::parseJson("{\"wifi\":{\"ssid\":\"\",\"pass\":\"\"}}", v);
      dev.applyConfig(v, err);
      rebootAt = now + 300;
    } else if (held > 1000) {
      if (!dev.presets.empty()) {
        size_t i = 0;
        for (; i < dev.presets.size(); i++)
          if (dev.presets[i].id == dev.preset) break;
        int next = dev.presets[(i + 1) % dev.presets.size()].id;
        nl::parseJson(("{\"preset\":" + std::to_string(next) + "}").c_str(), v);
      } else {
        int fx = (dev.segs.empty() ? 0 : dev.segs[0].fx) + 1;
        nl::parseJson(("{\"fx\":" + std::to_string(fx % nl::EFFECT_COUNT) + "}").c_str(), v);
      }
      dev.applyState(v, err);
    } else if (held > 40) {
      nl::parseJson("{\"on\":\"t\"}", v);
      dev.applyState(v, err);
    }
  }
  wasDown = down;
}

// ------------------------------------------------------------------ arranque y bucle
void setup() {
  Serial.begin(115200);
  uint8_t mac[6];
#if defined(ESP8266)
  WiFi.macAddress(mac);
#else
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
  char id[8];
  snprintf(id, sizeof id, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  dev.deviceId = id;
#if defined(ESP8266)
  dev.rng.s = ESP.random() | 1;
#else
  dev.rng.s = esp_random() | 1;
#endif
  dev.clock = []() -> uint32_t { return millis(); };
  dev.extraInfo = [](nl::JWriter& w) {
    bool up = WiFi.status() == WL_CONNECTED;
    w.kv("ip", (up ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
    w.kv("wifi", up ? dev.ssid : std::string("punto de acceso"));
    w.kv("rssi", up ? (int)WiFi.RSSI() : 0);
    w.kv("heap", (unsigned long)ESP.getFreeHeap());
    w.kv("uptime", (unsigned long)(millis() / 1000));
#if defined(ESP8266)
    w.kv("chip", "ESP8266");
    w.kv("flash", (unsigned long)ESP.getFlashChipRealSize());
#else
    w.kv("chip", ESP.getChipModel());
#endif
    w.kv("mac", WiFi.macAddress().c_str());
  };
  loadAll();
  dev.ensureSegments();
  setupOutputs();
  dev.outputsChanged = false;
  startWifi();
  setupHttp();
  udp.begin(dev.udpPort);
  Serial.printf("[nexusled] %s v%s, %d LED en %d tira(s)\n", dev.name.c_str(), NEXUSLED_VERSION, dev.total(),
                (int)dev.outputs.size());
}

void loop() {
  uint32_t now = millis();
  server.handleClient();
  wifiLoop(now);
  udpLoop();
  serialLoop();
  buttonLoop(now);
  if (dev.outputsChanged) {
    dev.outputsChanged = false;
    setupOutputs();
  }
  if (dev.wifiChanged) {
    dev.wifiChanged = false;
    saveDirty(now);
    rebootAt = now + 1500;
  }
  if (now - lastFrame >= (uint32_t)(1000 / dev.fps)) {
    lastFrame = now;
    dev.render();
    showOutputs();
  }
  saveDirty(now);
  if (dev.factoryRequested) {
    // valores de fabrica: se borra todo y se reinicia sin volver a guardar
    prefs.clear();
    delay(300);
    ESP.restart();
  }
  if (dev.rebootRequested) {
    dev.rebootRequested = false;
    rebootAt = now + 500;
  }
  if (rebootAt && (int32_t)(now - rebootAt) >= 0) {
    saveDirty(now);
    ESP.restart();
  }
  delay(1);
}
