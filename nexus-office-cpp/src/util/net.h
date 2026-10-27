// Utilidades de red locales: puertos libres e IP de la LAN.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nx::net {

int free_port(int preferred);                 // el preferido si esta libre; si no, uno cualquiera
bool port_in_use(const std::string& host, int port);
std::vector<std::string> lan_ips();           // IPv4 de la red local (sin 127.x)
std::vector<std::string> resolve(const std::string& host);   // socket.getaddrinfo: IPs del nombre
// ipaddress: privada, loopback, enlace local o reservada (IPv4 e IPv6)
bool is_private_ip(const std::string& ip);
bool is_loopback_ip(const std::string& ip);

// psutil.net_if_addrs: cada IPv4 con el nombre de su adaptador y la MAC de este (AA:BB:...; vacia si no se sabe)
struct Iface {
  std::string name, ip, mac;
};
std::vector<Iface> interfaces();
std::string local_ip_towards(const std::string& host);   // IP de salida hacia ese destino (UDP connect); "" si no hay ruta
// UDP: envia los mensajes a host:port (multicast incluido) y recoge respuestas (datos, IP de origen) hasta timeout
std::vector<std::pair<std::string, std::string>> udp_query(const std::string& host, int port,
                                                           const std::vector<std::string>& messages, double timeout);
// UDP con difusion (SO_BROADCAST): envia msg a cada destino (host, puerto) `rounds` veces y recoge respuestas
// (datos, IP, puerto de origen) durante timeout segundos
struct UdpReply {
  std::string data, ip;
  int port = 0;
};
std::vector<UdpReply> udp_broadcast(const std::vector<std::pair<std::string, int>>& targets, const std::string& msg,
                                    double timeout, int rounds = 2);
// UDP sin respuesta (estado en directo para las luces); false si no se pudo enviar
bool udp_send(const std::string& host, int port, const std::string& data);
// TCP en claro: conecta, envia y lee hasta cerrar o max bytes. nullopt si no se pudo conectar
std::optional<std::string> tcp_exchange(const std::string& host, int port, const std::string& data, double timeout,
                                        size_t max_bytes = 65536);

// Todas las direcciones del nombre son publicas (ni privadas, ni loopback, ni enlace local, ni multicast); false si no
// se resuelve
bool public_host(const std::string& host);

// Proxy HTTP minimo en 127.0.0.1 (CONNECT para https y GET absoluto para http) que solo conecta con direcciones
// publicas: lo demas recibe 403 y queda en blocked(). Lo usa el navegador sin interfaz del RAG.
class GuardProxy {
 public:
  explicit GuardProxy(double timeout = 40);
  ~GuardProxy();
  GuardProxy(const GuardProxy&) = delete;
  GuardProxy& operator=(const GuardProxy&) = delete;
  int port() const;
  std::vector<std::string> blocked() const;
  void close();

  struct State;

 private:
  std::shared_ptr<State> st_;
  std::thread th_;
};

}  // namespace nx::net
