// Ubicacion en tiempo real (herramienta ubicacion_actual y /api/ubicacion). Todo dentro del equipo: la posicion no se
// envia a ningun servicio externo. Fuentes, de la mas directa a la de reserva:
//  - app:       la app Android escribe la ultima posicion del GPS del movil (LocationManager) en ubicacion.json
//  - gpsd:      receptor GPS servido por gpsd (Linux, Raspberry Pi, macOS) en 127.0.0.1:2947
//  - nmea:      receptor GPS por puerto serie o USB que habla NMEA 0183 (ubicacion.serie: /dev/ttyACM0, COM3...)
//  - windows:   servicio de ubicacion de Windows (Windows.Devices.Geolocation)
//  - geoclue:   servicio de ubicacion de Linux (GeoClue2 por D-Bus del sistema, cargado al ejecutar)
//  - navegador: la interfaz web manda la posicion del navegador si se activa (ubicacion.navegador)
//  - manual:    la posicion fija que pone el usuario en Ajustes (ubicacion.manual)
// Ademas da el lugar guardado o la ciudad de referencia mas cercana sin internet (tabla propia) y distancias.
#pragma once
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::ubicacion {

struct Fix {
  double lat = 0, lon = 0;
  double precision = -1;           // radio de error en metros (-1 = no se sabe)
  double altitud = NAN;            // metros sobre el nivel del mar
  double velocidad = NAN;          // m/s
  double rumbo = NAN;              // grados desde el norte
  int satelites = -1;
  std::string fuente, detalle;
  double t = 0;                    // segundos unix de la medida
};

// --- piezas puras (con pruebas)
std::optional<Fix> parse_nmea(const std::string& text);    // $GxGGA / $GxRMC con suma de control
std::optional<Fix> parse_gpsd(const std::string& lines);   // primera TPV con fijacion (mode >= 2)
std::optional<Fix> parse_fix_json(const json& j, const std::string& fuente);   // {lat, lon, precision, ..., t}
double distancia_km(double lat1, double lon1, double lat2, double lon2);
double rumbo_grados(double lat1, double lon1, double lat2, double lon2);
std::string rumbo_texto(double grados);   // "norte", "noreste"...
bool coordenadas_validas(double lat, double lon);
// "43.26, -2.93" o "43.26 -2.93" -> par; nullopt si no son coordenadas
std::optional<std::pair<double, double>> parse_coordenadas(const std::string& s);
// ciudad de referencia mas cercana de la tabla incluida: {nombre, region, pais, distancia_km, rumbo}
json ciudad_cercana(double lat, double lon);
// busca un lugar por nombre (lugares guardados primero, luego la tabla): {nombre, lat, lon} o null
json buscar_lugar(const std::string& nombre, const json& lugares);

json fix_json(const Fix& f, double ahora);

// --- consultas
// posicion mandada por la interfaz o por la app (POST /api/ubicacion); fuente "navegador" o "app"
void recibir(const json& body, const std::string& fuente);
// opciones: {fuente: "auto"|..., timeout_s, hasta: "lugar" o "lat,lon"}; devuelve la posicion o {"error": ...}
json consultar(const json& opciones = json::object());
// fuentes disponibles en este equipo, configuracion y ultima posicion conocida
json estado();
// ruta del fichero que escribe la app Android
std::string fichero_app();

}  // namespace nx::ubicacion
