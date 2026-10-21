// Utilidades de red locales: puertos libres e IP de la LAN.
#pragma once
#include <string>
#include <vector>

namespace nx::net {

int free_port(int preferred);                 // el preferido si esta libre; si no, uno cualquiera
bool port_in_use(const std::string& host, int port);
std::vector<std::string> lan_ips();           // IPv4 de la red local (sin 127.x)

}  // namespace nx::net
