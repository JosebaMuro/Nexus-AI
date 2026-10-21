// Utilidades de red locales: puertos libres e IP de la LAN.
#pragma once
#include <string>
#include <vector>

namespace nx::net {

int free_port(int preferred);                 // el preferido si esta libre; si no, uno cualquiera
bool port_in_use(const std::string& host, int port);
std::vector<std::string> lan_ips();           // IPv4 de la red local (sin 127.x)
std::vector<std::string> resolve(const std::string& host);   // socket.getaddrinfo: IPs del nombre
// ipaddress: privada, loopback, enlace local o reservada (IPv4 e IPv6)
bool is_private_ip(const std::string& ip);
bool is_loopback_ip(const std::string& ip);

}  // namespace nx::net
