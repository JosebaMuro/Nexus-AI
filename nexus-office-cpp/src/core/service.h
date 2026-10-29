// Nexus-Office como servicio (service.py): arranca solo al encender el equipo, sin ventana, y se reinicia si falla.
// Linux: systemd (del sistema como root, si no de usuario) o cron @reboot; macOS: launchd; Windows: Programador de
// tareas; Termux: termux-services. La carpeta sigue siendo portable: el servicio apunta al ejecutable de esta carpeta.
#pragma once
#include <string>
#include <vector>

namespace nx::service {

std::string kind();                  // systemd | cron | launchd | windows | termux
bool is_admin();
std::string bundle();                // appimage | flatpak | ""
std::vector<std::string> command(const std::string& host, int port);
std::string install(const std::string& host, int port);   // lanza std::runtime_error
std::vector<std::string> uninstall();
std::string registered();            // donde esta registrado ("" si no lo esta)
bool running(int port, double timeout = 1.5);
int main(const std::vector<std::string>& args);   // nexus-office servicio instalar|quitar|estado [--red] [--host] [--puerto]

}  // namespace nx::service
