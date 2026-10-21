#include "util/net.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nx::net {

namespace {
#ifdef _WIN32
struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
};
using sock_t = SOCKET;
const sock_t BAD = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
#else
struct WsaInit {};
using sock_t = int;
const sock_t BAD = -1;
void close_sock(sock_t s) { ::close(s); }
#endif
WsaInit& wsa() {
  static WsaInit w;
  return w;
}

int try_bind(const char* host, int port) {
  wsa();
  sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == BAD) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  inet_pton(AF_INET, host, &a.sin_addr);
  int got = -1;
  if (bind(s, (sockaddr*)&a, sizeof a) == 0) {
    socklen_t len = sizeof a;
    if (getsockname(s, (sockaddr*)&a, &len) == 0) got = ntohs(a.sin_port);
  }
  close_sock(s);
  return got;
}
}  // namespace

int free_port(int preferred) {
  int p = try_bind("127.0.0.1", preferred);
  if (p > 0) return p;
  p = try_bind("127.0.0.1", 0);
  return p > 0 ? p : preferred;
}

bool port_in_use(const std::string& host, int port) { return try_bind(host.c_str(), port) < 0; }

std::vector<std::string> lan_ips() {
  std::vector<std::string> out;
#ifdef _WIN32
  wsa();
  ULONG size = 15000;
  std::string buf(size, '\0');
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                           (PIP_ADAPTER_ADDRESSES)buf.data(), &size) != NO_ERROR)
    return out;
  for (auto* a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      char ip[64];
      auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
      inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
      if (std::strncmp(ip, "127.", 4) && std::strncmp(ip, "169.254.", 8)) out.push_back(ip);
    }
  }
#else
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return out;
  for (auto* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    char ip[64];
    inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, ip, sizeof ip);
    if (std::strncmp(ip, "127.", 4) && std::strncmp(ip, "169.254.", 8)) out.push_back(ip);
  }
  freeifaddrs(ifs);
#endif
  return out;
}


std::vector<std::string> resolve(const std::string& host) {
  wsa();
  std::vector<std::string> out;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return out;
  for (auto* p = res; p; p = p->ai_next) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (p->ai_family == AF_INET)
      inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, buf, sizeof buf);
    else if (p->ai_family == AF_INET6)
      inet_ntop(AF_INET6, &((sockaddr_in6*)p->ai_addr)->sin6_addr, buf, sizeof buf);
    if (buf[0]) out.push_back(buf);
  }
  freeaddrinfo(res);
  return out;
}

static bool parse_v4(const std::string& ip, unsigned char b[4]) {
  in_addr a{};
  if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
  memcpy(b, &a, 4);
  return true;
}

bool is_loopback_ip(const std::string& ip) {
  unsigned char b[4];
  if (parse_v4(ip, b)) return b[0] == 127;
  return ip == "::1";
}

bool is_private_ip(const std::string& ip) {
  unsigned char b[4];
  if (parse_v4(ip, b)) {
    if (b[0] == 10 || b[0] == 127 || b[0] == 0) return true;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 169 && b[1] == 254) return true;               // enlace local
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return true;       // CGNAT (ipaddress lo marca como privada)
    if (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) return true;
    if (b[0] == 198 && (b[1] == 18 || b[1] == 19)) return true;
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return true;
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return true;
    if (b[0] >= 240) return true;                              // reservada y difusion
    return false;
  }
  in6_addr a{};
  if (inet_pton(AF_INET6, ip.c_str(), &a) != 1) return true;   // lo que no se entiende, no se permite
  const unsigned char* x = (const unsigned char*)&a;
  bool zero = true;
  for (int i = 0; i < 15; ++i) zero &= x[i] == 0;
  if (zero) return true;                                       // :: y ::1
  if ((x[0] & 0xFE) == 0xFC) return true;                      // fc00::/7
  if (x[0] == 0xFE && (x[1] & 0xC0) == 0x80) return true;      // fe80::/10
  if (x[0] == 0x20 && x[1] == 0x01 && x[2] == 0x0d && x[3] == 0xb8) return true;   // documentacion
  bool mapped = true;
  for (int i = 0; i < 10; ++i) mapped &= x[i] == 0;
  if (mapped && x[10] == 0xFF && x[11] == 0xFF) {
    char v4[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, x + 12, v4, sizeof v4);
    return is_private_ip(v4);
  }
  return false;
}

}  // namespace nx::net
