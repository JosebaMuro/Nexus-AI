// ROM falsa de Espressif sobre un pseudoterminal, para probar nexus-flash sin placas.
//
//   rom_falsa CHIP RUTA_ENLACE VOLCADO   (CHIP: esp8266, esp32, esp32c3, esp32c5...)
//
// Crea un pty, enlaza su lado esclavo en RUTA_ENLACE y responde como el cargador de la ROM de ese chip: SYNC,
// READ_REG, GET_SECURITY_INFO, SPI_ATTACH, SPI_SET_PARAMS, CHANGE_BAUDRATE, FLASH_BEGIN/DATA/END y SPI_FLASH_MD5,
// con las diferencias de cada ROM (2 o 4 bytes de estado, el campo de cifrado de FLASH_BEGIN, sin MD5 ni cambio de
// velocidad en el ESP8266). Comprueba sumas y secuencia de cada bloque. Al recibir SIGTERM guarda la flash en
// VOLCADO (solo hasta lo escrito). Una linea que empiece por "{" fuera de SLIP se contesta como el firmware.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../nexus_flash.h"

static volatile sig_atomic_t g_fin = 0;
static void al_terminar(int) { g_fin = 1; }

struct Rom {
  std::string chip;
  int estado = 4;
  uint32_t magic = 0;
  int id = -1;
  bool cifrado = true, md5 = true, baud = true;
  std::vector<uint8_t> flash = std::vector<uint8_t>(4 << 20, 0xFF);
  uint32_t escrito_hasta = 0;
  uint32_t off = 0, bloques = 0, tam_bloque = 0, seq = 0;
  bool en_curso = false, adjunta = false;
  int fd = -1;
  int syncs = 0;

  void enviar(uint8_t op, const std::vector<uint8_t>& payload, uint32_t val = 0, uint8_t st = 0, uint8_t err = 0) {
    std::vector<uint8_t> d = payload;
    d.push_back(st);
    d.push_back(err);
    for (int i = 2; i < estado; i++) d.push_back(0);
    std::vector<uint8_t> p = {0x01, op, (uint8_t)(d.size() & 0xFF), (uint8_t)(d.size() >> 8)};
    nexusflash::detail::put32(p, val);
    p.insert(p.end(), d.begin(), d.end());
    std::vector<uint8_t> o = {0xC0};
    for (uint8_t b : p) {
      if (b == 0xC0) o.push_back(0xDB), o.push_back(0xDC);
      else if (b == 0xDB) o.push_back(0xDB), o.push_back(0xDD);
      else o.push_back(b);
    }
    o.push_back(0xC0);
    if (write(fd, o.data(), o.size()) < 0) perror("write");
  }
  void error(uint8_t op, uint8_t err = 0x05) { enviar(op, {}, 0, 1, err); }

  void orden(const std::vector<uint8_t>& p) {
    if (p.size() < 8 || p[0] != 0) return;
    uint8_t op = p[1];
    size_t n = p[2] | p[3] << 8;
    uint32_t suma = nexusflash::detail::get32(&p[4]);
    if (p.size() != 8 + n) return error(op, 0x05);
    const uint8_t* d = p.data() + 8;
    auto u32 = [&](size_t i) { return nexusflash::detail::get32(d + 4 * i); };
    switch (op) {
      case 0x08:   // SYNC: la ROM responde 8 veces
        if (n != 36 || d[0] != 0x07 || d[1] != 0x07 || d[2] != 0x12 || d[3] != 0x20) return;
        if (++syncs < 2) return;   // como una ROM real, el primer SYNC se suele perder
        for (int i = 0; i < 8; i++) enviar(op, {}, 0x20120707);
        return;
      case 0x0A:   // READ_REG
        if (n != 4) return error(op);
        return enviar(op, {}, u32(0) == 0x40001000 ? magic : 0);
      case 0x14:   // GET_SECURITY_INFO
        if (id < 0) return error(op);
        {
          std::vector<uint8_t> r(12, 0);
          nexusflash::detail::put32(r, (uint32_t)id);
          nexusflash::detail::put32(r, 0);
          return enviar(op, r);
        }
      case 0x0D:   // SPI_ATTACH
        if (chip == "esp8266" || n != 8) return error(op);
        adjunta = true;
        return enviar(op, {});
      case 0x0B:   // SPI_SET_PARAMS
        if (n != 24) return error(op);
        return enviar(op, {});
      case 0x0F:   // CHANGE_BAUDRATE
        if (!baud || n != 8) return error(op);
        return enviar(op, {});
      case 0x02: {   // FLASH_BEGIN
        size_t esperado = cifrado ? 20 : 16;
        if (n != esperado) return error(op);
        if (chip != "esp8266" && !adjunta) return error(op, 0x06);
        uint32_t borrar = u32(0);
        bloques = u32(1), tam_bloque = u32(2), off = u32(3), seq = 0;
        if (off + bloques * tam_bloque > flash.size()) return error(op, 0x06);
        if (chip == "esp8266" && bloques && borrar == bloques * tam_bloque && bloques * tam_bloque > 0x10000) {
          // sin la correccion de get_erase_size la ROM del ESP8266 borraria de mas: aqui se toma como fallo
          std::fprintf(stderr, "rom_falsa: FLASH_BEGIN sin corregir el tamano de borrado del ESP8266\n");
          return error(op, 0x06);
        }
        std::fill(flash.begin() + off, flash.begin() + off + bloques * tam_bloque, 0xFF);
        en_curso = true;
        return enviar(op, {});
      }
      case 0x03: {   // FLASH_DATA
        if (!en_curso || n < 16) return error(op);
        uint32_t len = u32(0), s = u32(1);
        if (len != n - 16 || len != tam_bloque) return error(op, 0x05);
        uint8_t ck = 0xEF;
        for (size_t i = 0; i < len; i++) ck ^= d[16 + i];
        if (ck != suma) return error(op, 0x07);
        if (s != seq || s >= bloques) return error(op, 0x05);
        std::memcpy(&flash[off + s * tam_bloque], d + 16, len);
        escrito_hasta = std::max(escrito_hasta, off + (s + 1) * tam_bloque);
        seq++;
        return enviar(op, {});
      }
      case 0x04:   // FLASH_END
        en_curso = false;
        return enviar(op, {});
      case 0x13: {   // SPI_FLASH_MD5
        if (!md5 || n != 16) return error(op);
        uint32_t a = u32(0), t = u32(1);
        if (a + t > flash.size()) return error(op, 0x06);
        std::string h = nexusflash::detail::md5_hex(&flash[a], t);
        return enviar(op, std::vector<uint8_t>(h.begin(), h.end()));
      }
      default:
        return error(op);
    }
  }
};

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "uso: rom_falsa CHIP RUTA_ENLACE VOLCADO\n");
    return 2;
  }
  Rom r;
  r.chip = argv[1];
  struct Def {
    const char* n;
    uint32_t magic;
    int id;
  };
  const Def defs[] = {{"esp8266", 0xFFF0C101, -1}, {"esp32", 0x00F01D83, -1}, {"esp32s2", 0x7C6, 2},
                      {"esp32s3", 0x9, 9},          {"esp32c3", 0x1B31506F, 5}, {"esp32c2", 0x6F51306F, 12},
                      {"esp32c6", 0x2CE0806F, 13},  {"esp32c5", 0x1101406F, 23}, {"esp32p4", 0x0ADDBAD0, 18}};
  bool ok = false;
  for (auto& d : defs)
    if (r.chip == d.n) r.magic = d.magic, r.id = d.id, ok = true;
  if (!ok) {
    std::fprintf(stderr, "chip desconocido: %s\n", argv[1]);
    return 2;
  }
  if (r.chip == "esp8266") r.estado = 2, r.cifrado = false, r.md5 = false, r.baud = false;
  if (r.chip == "esp32") r.cifrado = false;

  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0 || grantpt(m) || unlockpt(m)) {
    perror("pty");
    return 1;
  }
  std::string esclavo = ptsname(m);
  int s = open(esclavo.c_str(), O_RDWR | O_NOCTTY);   // mantiene el pty vivo entre aperturas del grabador
  termios t{};
  tcgetattr(s, &t);
  cfmakeraw(&t);
  tcsetattr(s, TCSANOW, &t);
  unlink(argv[2]);
  if (symlink(esclavo.c_str(), argv[2])) {
    perror("symlink");
    return 1;
  }
  r.fd = m;
  signal(SIGTERM, al_terminar);
  signal(SIGINT, al_terminar);
  std::fprintf(stderr, "rom_falsa: %s en %s\n", r.chip.c_str(), esclavo.c_str());

  // el texto de arranque llega antes que nada, como en una placa de verdad
  const char* arranque = "ets Jan  8 2013,rst cause:2, boot mode:(1,7)\r\nwaiting for download\r\n";
  if (write(m, arranque, std::strlen(arranque)) < 0) perror("write");

  std::vector<uint8_t> pkt;
  std::string linea;
  bool dentro = false, esc = false;
  while (!g_fin) {
    pollfd pf{m, POLLIN, 0};
    if (poll(&pf, 1, 100) <= 0) continue;
    uint8_t buf[4096];
    ssize_t n = read(m, buf, sizeof buf);
    if (n <= 0) continue;
    for (ssize_t i = 0; i < n; i++) {
      uint8_t b = buf[i];
      if (!dentro) {
        if (b == 0xC0) {
          dentro = true;
          pkt.clear();
        } else if (b == '{' || !linea.empty()) {
          linea.push_back((char)b);
          if (b == '\n') {
            std::string resp = "{\"ok\":true,\"chip\":\"" + r.chip + "\",\"recibido\":" + std::to_string(linea.size() - 1) + "}\r\n";
            if (write(m, resp.data(), resp.size()) < 0) perror("write");
            std::fprintf(stderr, "rom_falsa: linea %s", linea.c_str());
            linea.clear();
          }
        }
        continue;
      }
      if (b == 0xC0) {
        if (!pkt.empty()) {
          r.orden(pkt);
          dentro = false;
        }
        continue;
      }
      if (esc) {
        pkt.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
        esc = false;
      } else if (b == 0xDB) {
        esc = true;
      } else {
        pkt.push_back(b);
      }
    }
  }
  FILE* f = std::fopen(argv[3], "wb");
  if (f) {
    std::fwrite(r.flash.data(), 1, r.escrito_hasta, f);
    std::fclose(f);
  }
  unlink(argv[2]);
  close(s);
  close(m);
  return 0;
}
