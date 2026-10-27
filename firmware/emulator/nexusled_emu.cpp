// Emulador de NexusLED para el PC (Linux/macOS/WSL): ejecuta el mismo nucleo que el ESP32
// (nl_core.h) con un servidor HTTP y UDP minimos. Sirve para probar NEXUS sin hardware.
//
//   make -C firmware/emulator && ./firmware/emulator/nexusled-emu --http 8080 --udp 4210 --leds 60 --ansi
//
// --ansi dibuja la tira en la terminal con color real.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "../nexusled/nl_core.h"
#include "../nexusled/nl_ui.h"

static uint32_t millis() {
  static struct timespec t0 = {0, 0};
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  if (!t0.tv_sec && !t0.tv_nsec) t0 = t;
  return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

static std::string dataDir;
static std::string readFile(const std::string& name) {
  std::ifstream f(dataDir + "/" + name, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static void writeFile(const std::string& name, const std::string& s) {
  std::ofstream f(dataDir + "/" + name, std::ios::binary | std::ios::trunc);
  f << s;
}

static std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) {
      int a = nl::hexv(s[i + 1]), b = nl::hexv(s[i + 2]);
      if (a >= 0 && b >= 0) { o += (char)(a * 16 + b); i += 2; }
      else o += s[i];
    } else o += s[i];
  }
  return o;
}

struct Client {
  int fd;
  std::string in;
  uint32_t since;
};

static const char* statusText(int c) {
  switch (c) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    default: return "Error";
  }
}

static void sendAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) { usleep(1000); continue; }
      return;
    }
    off += (size_t)n;
  }
}

// devuelve true si la peticion esta completa (y ya se ha respondido)
static bool serve(nl::Device& dev, Client& c) {
  size_t he = c.in.find("\r\n\r\n");
  if (he == std::string::npos) return false;
  std::istringstream hs(c.in.substr(0, he));
  std::string line;
  std::getline(hs, line);
  std::istringstream rl(line);
  std::string method, target;
  rl >> method >> target;
  size_t clen = 0;
  std::string token;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t col = line.find(':');
    if (col == std::string::npos) continue;
    std::string k = nl::lower(line.substr(0, col));
    std::string v = line.substr(col + 1);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    if (k == "content-length") clen = (size_t)atol(v.c_str());
    if (k == "x-nexus-token") token = v;
  }
  if (c.in.size() < he + 4 + clen) return false;
  nl::Request r;
  r.method = method;
  r.body = c.in.substr(he + 4, clen);
  r.token = token;
  size_t qm = target.find('?');
  r.path = target.substr(0, qm);
  if (qm != std::string::npos) {
    std::string q = target.substr(qm + 1);
    std::istringstream qs(q);
    std::string part;
    while (std::getline(qs, part, '&')) {
      size_t eq = part.find('=');
      r.query.push_back({urlDecode(part.substr(0, eq)), eq == std::string::npos ? "" : urlDecode(part.substr(eq + 1))});
    }
  }
  nl::Response res;
  if (r.path.rfind("/api/", 0) != 0) {
    res.type = "text/html; charset=utf-8";
    res.body = NL_UI_HTML;
  } else if (r.path == "/api/wifi/scan") {
    res.body = "{\"networks\":[{\"ssid\":\"emulador\",\"rssi\":-40}]}";
  } else {
    res = dev.handle(r);
  }
  std::ostringstream out;
  out << "HTTP/1.1 " << res.code << " " << statusText(res.code) << "\r\n";
  out << "Content-Type: " << res.type << "\r\nContent-Length: " << res.body.size() << "\r\n";
  out << "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, X-Nexus-Token\r\n";
  out << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\nConnection: close\r\n\r\n" << res.body;
  sendAll(c.fd, out.str());
  return true;
}

static volatile bool running = true;
static void onSig(int) { running = false; }

int main(int argc, char** argv) {
  int httpPort = 8080, udpPort = 4210, leds = 60;
  bool ansi = false;
  std::string bind = "127.0.0.1";
  dataDir = ".";
  std::string name = "NexusLED emulado";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--http") httpPort = atoi(next().c_str());
    else if (a == "--udp") udpPort = atoi(next().c_str());
    else if (a == "--leds") leds = atoi(next().c_str());
    else if (a == "--data") dataDir = next();
    else if (a == "--bind") bind = next();
    else if (a == "--name") name = next();
    else if (a == "--ansi") ansi = true;
    else {
      fprintf(stderr, "uso: %s [--http 8080] [--udp 4210] [--leds 60] [--data DIR] [--bind 127.0.0.1] [--name N] [--ansi]\n", argv[0]);
      return 2;
    }
  }
  signal(SIGINT, onSig);
  signal(SIGTERM, onSig);
  mkdir(dataDir.c_str(), 0755);

  nl::Device dev;
  dev.deviceId = "E1E2E3";
  dev.clock = millis;
  dev.rng.s = (uint32_t)time(nullptr) | 1;
  dev.name = name;
  dev.outputs[0].count = leds;
  dev.segs.clear();
  dev.ensureSegments();
  dev.udpPort = udpPort;
  dev.httpPort = httpPort;
  dev.extraInfo = [&](nl::JWriter& w) {
    w.kv("ip", bind).kv("wifi", "emulador").kv("rssi", -40).kv("heap", 200000).kv("uptime", (unsigned long)(millis() / 1000));
    w.kv("chip", "emulador");
  };
  std::string s;
  if (!(s = readFile("cfg.json")).empty()) dev.loadConfig(s);
  if (!(s = readFile("presets.json")).empty()) dev.loadPresets(s);
  if (!(s = readFile("state.json")).empty()) dev.loadState(s);
  dev.ensureSegments();

  int hs = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(hs, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in ha{};
  ha.sin_family = AF_INET;
  ha.sin_port = htons(httpPort);
  inet_pton(AF_INET, bind.c_str(), &ha.sin_addr);
  if (::bind(hs, (sockaddr*)&ha, sizeof ha) < 0 || listen(hs, 16) < 0) {
    perror("http");
    return 1;
  }
  fcntl(hs, F_SETFL, O_NONBLOCK);
  int us = socket(AF_INET, SOCK_DGRAM, 0);
  setsockopt(us, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(us, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  sockaddr_in ua{};
  ua.sin_family = AF_INET;
  ua.sin_port = htons(udpPort);
  ua.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(us, (sockaddr*)&ua, sizeof ua) < 0) {
    perror("udp");
    return 1;
  }
  fprintf(stderr, "[nexusled-emu] http://%s:%d  udp %d  %d LED\n", bind.c_str(), httpPort, udpPort, dev.total());
  fflush(stderr);

  std::vector<Client> clients;
  uint32_t lastFrame = 0, lastAnsi = 0;
  while (running) {
    std::vector<pollfd> fds;
    fds.push_back({hs, POLLIN, 0});
    fds.push_back({us, POLLIN, 0});
    for (auto& c : clients) fds.push_back({c.fd, POLLIN, 0});
    poll(fds.data(), fds.size(), 5);
    uint32_t now = millis();
    if (fds[0].revents & POLLIN) {
      int fd;
      while ((fd = accept(hs, nullptr, nullptr)) >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        clients.push_back({fd, "", now});
      }
    }
    if (fds[1].revents & POLLIN) {
      uint8_t buf[2048];
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t n = recvfrom(us, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
      if (n > 0) {
        std::string reply = dev.handleUdp(buf, (size_t)n);
        if (!reply.empty()) {
          reply.pop_back();
          reply += ",\"ip\":\"" + bind + "\"}";
          sendto(us, reply.data(), reply.size(), 0, (sockaddr*)&from, fl);
        }
      }
    }
    for (size_t i = 2; i < fds.size(); i++) {
      Client& c = clients[i - 2];
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[4096];
        ssize_t n;
        while ((n = recv(c.fd, buf, sizeof buf, 0)) > 0) c.in.append(buf, (size_t)n);
        if (n == 0 && c.in.empty()) c.since = 0;
      }
    }
    for (size_t i = 0; i < clients.size();) {
      Client& c = clients[i];
      bool done = c.since == 0 || serve(dev, c) || now - c.since > 5000;
      if (done) {
        close(c.fd);
        clients.erase(clients.begin() + i);
      } else {
        i++;
      }
    }
    if (dev.outputsChanged) dev.outputsChanged = false;
    if (dev.wifiChanged) dev.wifiChanged = false;
    if (now - lastFrame >= (uint32_t)(1000 / dev.fps)) {
      lastFrame = now;
      dev.render();
      if (ansi && now - lastAnsi > 50) {
        lastAnsi = now;
        std::string o = "\r";
        int n = dev.total() > 120 ? 120 : dev.total();
        for (int k = 0; k < n; k++) {
          auto& p = dev.frame[(size_t)k * dev.total() / n];
          char b[40];
          snprintf(b, sizeof b, "\x1b[38;2;%d;%d;%dm\xe2\x96\x88", p.r, p.g, p.b);
          o += b;
        }
        o += "\x1b[0m";
        fputs(o.c_str(), stdout);
        fflush(stdout);
      }
    }
    if (dev.configDirty) { writeFile("cfg.json", dev.configJson(true)); dev.configDirty = false; }
    if (dev.presetsDirty) { writeFile("presets.json", dev.presetsJson(true)); dev.presetsDirty = false; }
    if (dev.stateNeedsSave(now)) { writeFile("state.json", dev.stateJson(false)); dev.stateDirtyAt = 0; }
    if (dev.rebootRequested || dev.factoryRequested) {
      if (dev.factoryRequested) {
        unlink((dataDir + "/cfg.json").c_str());
        unlink((dataDir + "/presets.json").c_str());
        unlink((dataDir + "/state.json").c_str());
      }
      fprintf(stderr, "[nexusled-emu] reinicio solicitado: sale\n");
      break;
    }
  }
  close(hs);
  close(us);
  return 0;
}
