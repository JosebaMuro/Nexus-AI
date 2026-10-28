// NexusPico 2.0 - avatar de bolsillo de Nexus-Office en un Pico W con la pantalla de tinta del pwnagotchi
// (Waveshare 2,13" V3/V4, controlador SSD1680). C++ con el Pico SDK; la logica esta en np_core.h.
#include <stdio.h>

extern "C" {
#include "dhcpserver.h"
}
#include "hardware/flash.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "np_core.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------- flash: los ultimos 64 KB (16 sectores)
static const uint32_t ZONA = PICO_FLASH_SIZE_BYTES - 16 * FLASH_SECTOR_SIZE;

struct FlashSectores : np::Sectores {
  int cuantos() const override { return 16; }
  const uint8_t* leer(int s) override { return (const uint8_t*)(XIP_BASE + ZONA + (uint32_t)s * FLASH_SECTOR_SIZE); }
  struct Op {
    uint32_t off;
    const uint8_t* datos;
  };
  static void hacer(void* p) {
    Op* op = (Op*)p;
    flash_range_erase(op->off, FLASH_SECTOR_SIZE);
    flash_range_program(op->off, op->datos, FLASH_SECTOR_SIZE);
  }
  bool escribir(int s, const uint8_t* d) override {
    Op op{ZONA + (uint32_t)s * FLASH_SECTOR_SIZE, d};
    return flash_safe_execute(hacer, &op, UINT32_MAX) == PICO_OK;
  }
};

// ---------------------------------------------------------------- pantalla de tinta 2,13" (122x250)
class Epd {
 public:
  explicit Epd(const np::Pines& p) : p_(p) {
    spi_ = ((p.clk >> 3) & 1) ? spi1 : spi0;   // GP8-15 y GP24-29 son del SPI1
    spi_init(spi_, 4000000);
    spi_set_format(spi_, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(p.clk, GPIO_FUNC_SPI);
    gpio_set_function(p.din, GPIO_FUNC_SPI);
    for (int g : {p.rst, p.dc, p.cs}) {
      gpio_init(g);
      gpio_set_dir(g, GPIO_OUT);
    }
    gpio_put(p.cs, 1);
    gpio_init(p.busy);
    gpio_set_dir(p.busy, GPIO_IN);
    gpio_pull_up(p.busy);
    init();
  }

  void mostrar(const std::vector<uint8_t>& buf, int completo_cada) {
    if (parciales_ == 0 || parciales_ >= completo_cada) {
      completa(buf);
      parciales_ = 1;
    } else {
      parcial(buf);
    }
  }

 private:
  np::Pines p_;
  spi_inst_t* spi_;
  int parciales_ = 0;

  void cmd(uint8_t c) {
    gpio_put(p_.dc, 0);
    gpio_put(p_.cs, 0);
    spi_write_blocking(spi_, &c, 1);
    gpio_put(p_.cs, 1);
  }
  void data(const uint8_t* d, size_t n) {
    gpio_put(p_.dc, 1);
    gpio_put(p_.cs, 0);
    spi_write_blocking(spi_, d, n);
    gpio_put(p_.cs, 1);
  }
  void cmd(uint8_t c, std::initializer_list<uint8_t> d) {
    cmd(c);
    std::vector<uint8_t> v(d);
    data(v.data(), v.size());
  }
  void espera(uint32_t maximo_ms = 6000) {
    absolute_time_t fin = make_timeout_time_ms(maximo_ms);
    while (gpio_get(p_.busy) && absolute_time_diff_us(get_absolute_time(), fin) > 0) sleep_ms(10);
  }
  void reset() {
    gpio_put(p_.rst, 1);
    sleep_ms(20);
    gpio_put(p_.rst, 0);
    sleep_ms(2);
    gpio_put(p_.rst, 1);
    sleep_ms(20);
  }
  void ventana() {
    cmd(0x44, {0, (np::ANCHO - 1) >> 3});
    cmd(0x45, {0, 0, (np::ALTO - 1) & 0xFF, (np::ALTO - 1) >> 8});
    cmd(0x4E, {0});
    cmd(0x4F, {0, 0});
  }
  void init() {
    reset();
    espera();
    cmd(0x12);   // reinicio por software
    espera();
    cmd(0x01, {0xF9, 0x00, 0x00});   // 250 lineas
    cmd(0x11, {0x03});               // x+, y+
    ventana();
    cmd(0x3C, {0x05});   // borde
    cmd(0x21, {0x00, 0x80});
    cmd(0x18, {0x80});   // sensor de temperatura interno
    espera();
    parciales_ = 0;
  }
  void completa(const std::vector<uint8_t>& b) {   // parpadea y limpia restos; deja la base del parcial
    init();
    cmd(0x24);
    data(b.data(), b.size());
    cmd(0x26);
    data(b.data(), b.size());
    cmd(0x22, {0xF7});
    cmd(0x20);
    espera();
  }
  void parcial(const std::vector<uint8_t>& b) {   // rapido, sin parpadeo
    gpio_put(p_.rst, 0);
    sleep_ms(1);
    gpio_put(p_.rst, 1);
    cmd(0x3C, {0x80});
    cmd(0x01, {0xF9, 0x00, 0x00});
    cmd(0x11, {0x03});
    ventana();
    cmd(0x24);
    data(b.data(), b.size());
    cmd(0x22, {0xFF});
    cmd(0x20);
    espera();
    parciales_++;
  }
};

// ---------------------------------------------------------------- estado global
static FlashSectores g_flash;
static np::Almacen* g_alm;
static np::Config g_cfg;
static np::Consola g_con;
static std::string g_mac;

static void escribir_usb(const std::string& s) {
  fwrite(s.data(), 1, s.size(), stdout);
  fflush(stdout);
}

static void reiniciar(uint32_t ms = 300) {
  sleep_ms(ms);
  watchdog_reboot(0, 0, 10);
  while (true) tight_loop_contents();
}

// atiende la red y el USB durante `ms`
static void atender(uint32_t ms) {
  absolute_time_t fin = make_timeout_time_ms(ms);
  do {
    cyw43_arch_poll();
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) g_con.byte((uint8_t)c);
    if (g_con.reiniciar) reiniciar();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(10));
  } while (absolute_time_diff_us(get_absolute_time(), fin) > 0);
}

static std::string titulo() { return std::string("NexusPico ") + np::VERSION; }

// ---------------------------------------------------------------- cliente HTTP minimo (lwIP en bruto)
struct Http {
  std::string pet, resp;
  tcp_pcb* pcb = nullptr;
  bool fin = false, dns_ok = false, dns_hecho = false;
  ip_addr_t ip;
};

static err_t http_recv(void* arg, tcp_pcb* pcb, pbuf* p, err_t) {
  Http* h = (Http*)arg;
  if (!p) {
    h->fin = true;
    return ERR_OK;
  }
  for (pbuf* q = p; q; q = q->next) h->resp.append((const char*)q->payload, q->len);
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}
static void http_err(void* arg, err_t) {
  Http* h = (Http*)arg;
  h->pcb = nullptr;   // lwIP ya lo ha liberado
  h->fin = true;
}
static err_t http_conectado(void* arg, tcp_pcb* pcb, err_t err) {
  Http* h = (Http*)arg;
  if (err != ERR_OK) {
    h->fin = true;
    return err;
  }
  tcp_write(pcb, h->pet.data(), (u16_t)h->pet.size(), TCP_WRITE_FLAG_COPY);
  tcp_output(pcb);
  return ERR_OK;
}
static void http_dns(const char*, const ip_addr_t* ip, void* arg) {
  Http* h = (Http*)arg;
  h->dns_hecho = true;
  if (ip) h->ip = *ip, h->dns_ok = true;
}

static int http_get(const std::string& ruta, std::string& cuerpo) {
  std::string nombre;
  int puerto;
  if (!np::partir_host(g_cfg.host, nombre, puerto)) return -1;
  Http h;
  h.pet = "GET " + ruta + " HTTP/1.0\r\nHost: " + nombre + "\r\nX-Nexus-Key: " + g_cfg.key +
          "\r\nUser-Agent: NexusPico/" + np::VERSION + "\r\nConnection: close\r\n\r\n";
  absolute_time_t fin = make_timeout_time_ms(8000);
  err_t e = dns_gethostbyname(nombre.c_str(), &h.ip, http_dns, &h);
  if (e == ERR_INPROGRESS) {
    while (!h.dns_hecho && absolute_time_diff_us(get_absolute_time(), fin) > 0) atender(10);
    if (!h.dns_ok) return -1;
  } else if (e != ERR_OK) {
    return -1;
  }
  h.pcb = tcp_new_ip_type(IP_GET_TYPE(&h.ip));
  if (!h.pcb) return -1;
  tcp_arg(h.pcb, &h);
  tcp_recv(h.pcb, http_recv);
  tcp_err(h.pcb, http_err);
  if (tcp_connect(h.pcb, &h.ip, (u16_t)puerto, http_conectado) != ERR_OK) {
    tcp_abort(h.pcb);
    return -1;
  }
  while (!h.fin && absolute_time_diff_us(get_absolute_time(), fin) > 0) atender(5);
  if (h.pcb) {
    tcp_arg(h.pcb, nullptr);
    tcp_recv(h.pcb, nullptr);
    tcp_err(h.pcb, nullptr);
    if (tcp_close(h.pcb) != ERR_OK) tcp_abort(h.pcb);
  }
  if (!h.fin) return -1;
  return np::respuesta_http(h.resp, cuerpo);
}

// ---------------------------------------------------------------- portal de configuracion
struct Cliente {
  std::string pet;
  bool respondido = false;
};
static bool g_portal_guardado = false;

static err_t portal_enviado(void* arg, tcp_pcb* pcb, u16_t) {
  Cliente* c = (Cliente*)arg;
  if (c && c->respondido && tcp_sndbuf(pcb) == TCP_SND_BUF) {
    tcp_arg(pcb, nullptr);
    delete c;
    tcp_close(pcb);
  }
  return ERR_OK;
}
static err_t portal_recv(void* arg, tcp_pcb* pcb, pbuf* p, err_t) {
  Cliente* c = (Cliente*)arg;
  if (!p) {
    tcp_arg(pcb, nullptr);
    delete c;
    tcp_close(pcb);
    return ERR_OK;
  }
  for (pbuf* q = p; q; q = q->next) c->pet.append((const char*)q->payload, q->len);
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  if (!c->respondido && c->pet.find("\r\n\r\n") != std::string::npos) {
    bool guardado = false;
    np::Config n = g_cfg;
    std::string r = np::portal(c->pet, n, guardado);
    if (guardado && g_alm->escribir("config", np::config_a_json(n))) {
      g_cfg = n;
      g_portal_guardado = true;
    }
    tcp_write(pcb, r.data(), (u16_t)r.size(), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    c->respondido = true;
  }
  return ERR_OK;
}
static void portal_err(void* arg, err_t) { delete (Cliente*)arg; }
static err_t portal_aceptar(void*, tcp_pcb* pcb, err_t err) {
  if (err != ERR_OK || !pcb) return ERR_VAL;
  Cliente* c = new Cliente;
  tcp_arg(pcb, c);
  tcp_recv(pcb, portal_recv);
  tcp_sent(pcb, portal_enviado);
  tcp_err(pcb, portal_err);
  return ERR_OK;
}

[[noreturn]] static void portal(Epd& epd, const std::string& aviso) {
  std::string red = "NexusPico-" + g_mac.substr(g_mac.size() - 4);
  epd.mostrar(np::pantalla_texto({titulo(), aviso, "Configúrame:", "Wi-Fi " + red, "clave: nexuspico", "http://192.168.4.1"}), 1);
  cyw43_arch_disable_sta_mode();
  cyw43_arch_enable_ap_mode(red.c_str(), "nexuspico", CYW43_AUTH_WPA2_AES_PSK);
  ip_addr_t gw, mask;
  IP4_ADDR(ip_2_ip4(&gw), 192, 168, 4, 1);
  IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);
  static dhcp_server_t dhcp;
  dhcp_server_init(&dhcp, &gw, &mask);
  tcp_pcb* srv = tcp_new_ip_type(IPADDR_TYPE_ANY);
  tcp_bind(srv, IP_ANY_TYPE, 80);
  srv = tcp_listen_with_backlog(srv, 2);
  tcp_accept(srv, portal_aceptar);
  printf("{\"nexuspico\":\"portal\",\"red\":\"%s\"}\n", red.c_str());
  while (true) {
    atender(100);
    if (g_portal_guardado) {
      atender(1500);   // que llegue la pagina de "Guardado"
      reiniciar();
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (to_ms_since_boot(get_absolute_time()) / 500) & 1);
  }
}

// ---------------------------------------------------------------- arranque
int main() {
  stdio_init_all();
  static np::Almacen alm(g_flash);
  g_alm = &alm;
  std::string guardada;
  if (alm.leer("config", guardada)) np::mezclar_config(guardada, g_cfg);

  g_con.cfg = &g_cfg;
  g_con.responder = escribir_usb;
  g_con.guardar = [](const np::Config& c) { return g_alm->escribir("config", np::config_a_json(c)); };
  g_con.info = [] {
    nl::JWriter w;
    w.obj().kvb("ok", true).kv("dispositivo", "nexuspico").kv("version", np::VERSION).kv("mac", g_mac);
    w.kvb("configurado", g_cfg.valida()).kv("wifi", g_cfg.ssid).kv("nexus", g_cfg.host).end();
    return w.out;
  };

  if (cyw43_arch_init()) {
    while (true) {   // sin chip Wi-Fi (no es un Pico W): solo la consola
      int c = getchar_timeout_us(100000);
      if (c != PICO_ERROR_TIMEOUT) g_con.byte((uint8_t)c);
      if (g_con.reiniciar) reiniciar();
    }
  }
  uint8_t mac[6];
  cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
  char hex[13];
  snprintf(hex, sizeof hex, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  g_mac = hex;
  for (auto& ch : g_mac) ch = (char)toupper(ch);
  std::string cliente = "pico-" + std::string(hex + 6);

  Epd epd(g_cfg.pines);
  atender(1500);   // da tiempo a que Nexus mande la configuracion nada mas conectar el USB
  if (!g_cfg.valida()) portal(epd, "");

  epd.mostrar(np::pantalla_texto({titulo(), "", "Conectando a", g_cfg.ssid}), 1);
  cyw43_arch_enable_sta_mode();
  uint32_t auth = g_cfg.pass.empty() ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK;
  auto conectar = [&](uint32_t ms) {
    cyw43_arch_wifi_connect_async(g_cfg.ssid.c_str(), g_cfg.pass.empty() ? nullptr : g_cfg.pass.c_str(), auth);
    absolute_time_t fin = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), fin) > 0) {
      int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
      if (st == CYW43_LINK_UP) return true;
      if (st == CYW43_LINK_BADAUTH) return false;
      cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (to_ms_since_boot(get_absolute_time()) / 250) & 1);
      atender(50);
    }
    return false;
  };
  if (!conectar(20000)) portal(epd, "No conecta a " + g_cfg.ssid.substr(0, 18));

  np::Nucleo nucleo(http_get, [&](const std::vector<uint8_t>& b, const std::string&) { epd.mostrar(b, g_cfg.completo_cada); },
                    alm, cliente);
  nucleo.host = g_cfg.host;
  while (true) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    nucleo.paso();
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    atender((uint32_t)g_cfg.cada_s * 1000);
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) conectar(15000);
  }
}
