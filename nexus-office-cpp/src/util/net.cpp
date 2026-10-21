#include "util/net.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
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

}  // namespace nx::net
