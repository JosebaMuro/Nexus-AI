// Ubicacion en tiempo real (devices/ubicacion.h): NMEA, gpsd, fichero de la app Android, navegador, posicion fija,
// lugares guardados, ciudad cercana, distancias y la herramienta ubicacion_actual con sus limites de privacidad.
#include <cmath>
#include <thread>

#include "check.h"
#include "core/config.h"
#include "devices/ubicacion.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/fs.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace nx;

namespace {
bool cerca(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// configuracion de la ubicacion para una prueba (sin guardar) y vuelta a la de antes al terminar
struct Conf {
  json antes;
  explicit Conf(const json& cambios) {
    antes = get_config().get("ubicacion", json::object());
    json nueva = antes;
    for (auto it = cambios.begin(); it != cambios.end(); ++it) nueva[it.key()] = it.value();
    get_config().set("ubicacion", nueva, false);
  }
  ~Conf() { get_config().set("ubicacion", antes, false); }
};
}  // namespace

TEST("ubicacion: NMEA GGA y RMC con suma de control") {
  std::string s =
      "basura\r\n$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
      "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
  auto f = ubicacion::parse_nmea(s);
  CHECK(f.has_value());
  if (!f) return;
  CHECK(cerca(f->lat, 48.1173, 1e-4));
  CHECK(cerca(f->lon, 11.516667, 1e-4));
  CHECK_EQ(f->satelites, 8);
  CHECK(cerca(f->altitud, 545.4, 1e-6));
  CHECK(cerca(f->velocidad * 3.6, 22.4 * 1.852, 0.1));   // nudos -> km/h
  CHECK(cerca(f->rumbo, 84.4, 1e-6));
  CHECK_EQ(f->fuente, std::string("nmea"));
  // suma de control mala, sin fijacion o con aviso: no hay posicion
  CHECK(!ubicacion::parse_nmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48\n"));
  CHECK(!ubicacion::parse_nmea("$GPGGA,123519,,,,,0,00,,,M,,M,,*66\n"));
  CHECK(!ubicacion::parse_nmea("$GPRMC,123519,V,4807.038,N,01131.000,E,,,230394,,*3A\n"));
  // hemisferio sur y oeste (GNSS moderno: $GN)
  auto g = ubicacion::parse_nmea("$GNRMC,101010,A,3436.222,S,05822.896,W,0.0,,081126,,\n");
  CHECK(g && cerca(g->lat, -34.6037, 1e-3) && cerca(g->lon, -58.3816, 1e-3));
}

TEST("ubicacion: gpsd TPV y POLL") {
  std::string lineas =
      "{\"class\":\"VERSION\",\"release\":\"3.25\"}\n"
      "{\"class\":\"TPV\",\"mode\":1}\n"
      "{\"class\":\"TPV\",\"device\":\"/dev/ttyACM0\",\"mode\":3,\"lat\":43.263,\"lon\":-2.935,\"altMSL\":19.5,"
      "\"speed\":1.5,\"track\":90.0,\"eph\":4.2}\n";
  auto f = ubicacion::parse_gpsd(lineas);
  CHECK(f && cerca(f->lat, 43.263, 1e-9) && cerca(f->precision, 4.2, 1e-9) && f->detalle == "/dev/ttyACM0");
  auto p = ubicacion::parse_gpsd("{\"class\":\"POLL\",\"tpv\":[{\"class\":\"TPV\",\"mode\":2,\"lat\":40.4,\"lon\":-3.7,"
                                 "\"epx\":10,\"epy\":12}]}\n");
  CHECK(p && cerca(p->lon, -3.7, 1e-9) && cerca(p->precision, 12, 1e-9));
  CHECK(!ubicacion::parse_gpsd("{\"class\":\"TPV\",\"mode\":1,\"lat\":1,\"lon\":1}\n"));
}

TEST("ubicacion: coordenadas, distancias, rumbos y ciudad cercana") {
  CHECK(!ubicacion::coordenadas_validas(0, 0));
  CHECK(!ubicacion::coordenadas_validas(91, 0));
  auto c = ubicacion::parse_coordenadas("43.2630, -2.9350");
  CHECK(c && cerca(c->first, 43.263, 1e-9));
  CHECK(ubicacion::parse_coordenadas("43,263 -2,935").has_value());
  CHECK(!ubicacion::parse_coordenadas("Bilbao"));
  // Madrid - Barcelona: unos 505 km en linea recta; el rumbo inicial es de unos 77 grados (este)
  double km = ubicacion::distancia_km(40.4168, -3.7038, 41.3874, 2.1686);
  CHECK(km > 495 && km < 510);
  CHECK_EQ(ubicacion::rumbo_texto(ubicacion::rumbo_grados(40.4168, -3.7038, 41.3874, 2.1686)), std::string("este"));
  CHECK_EQ(ubicacion::rumbo_texto(0), std::string("norte"));
  CHECK_EQ(ubicacion::rumbo_texto(350), std::string("norte"));
  CHECK_EQ(ubicacion::rumbo_texto(180), std::string("sur"));
  json g = ubicacion::ciudad_cercana(43.27, -2.92);
  CHECK_EQ(g["nombre"].get<std::string>(), std::string("Bilbao"));
  CHECK(g["distancia_km"].get<double>() < 3);
  json l = ubicacion::buscar_lugar("san sebastian", json::array());
  CHECK(l.is_object() && cerca(l["lat"].get<double>(), 43.3183, 1e-4));
  json mio = ubicacion::buscar_lugar("Taller", json::array({{{"nombre", "taller"}, {"lat", 43.1}, {"lon", -2.5}}}));
  CHECK(mio.is_object() && cerca(mio["lon"].get<double>(), -2.5, 1e-9));
  CHECK(ubicacion::buscar_lugar("Ningunsitio", json::array()).is_null());
}

TEST("ubicacion: fuentes de la app, del navegador y fija, con lugares y destino") {
  auto dir = fs::temp_directory_path() / ("nexus-ubic-" + std::to_string((long long)(now() * 1000)));
  fs::create_directories(dir);
  set_env("NEXUS_UBICACION", S(dir / "ubicacion.json"));
  {
    // sin ninguna fuente: error con pista
    Conf conf({{"fuente", "manual"}, {"manual", {{"lat", nullptr}, {"lon", nullptr}}}});
    json r = ubicacion::consultar();
    CHECK(r.contains("error"));
  }
  {
    Conf conf({{"fuente", "manual"},
               {"manual", {{"lat", 43.2630}, {"lon", -2.9350}, {"nombre", "Oficina de Bilbao"}}},
               {"lugares", json::array({{{"nombre", "Oficina"}, {"lat", 43.2631}, {"lon", -2.9351}, {"radio", 100}}})}});
    json r = ubicacion::consultar({{"hasta", "Vitoria-Gasteiz"}});
    CHECK_EQ(r["fuente"].get<std::string>(), std::string("manual"));
    CHECK_EQ(r["en_tiempo_real"].get<bool>(), false);
    CHECK_EQ(r["lugar"].get<std::string>(), std::string("Oficina"));
    CHECK_EQ(r["cerca_de"]["nombre"].get<std::string>(), std::string("Bilbao"));
    CHECK(r["hasta"]["distancia_km"].get<double>() > 45 && r["hasta"]["distancia_km"].get<double>() < 55);
    CHECK(r.contains("aviso"));
  }
  {
    // la app Android escribe la posicion del GPS (t en milisegundos, como Java)
    write_file(dir / "ubicacion.json",
               json{{"lat", 40.4168}, {"lon", -3.7038}, {"precision", 6.5}, {"velocidad", 10.0}, {"rumbo", 45.0},
                    {"proveedor", "gps"}, {"t", (long long)(now() * 1000)}}
                   .dump());
    Conf conf({{"fuente", "auto"}, {"gpsd", ""}, {"serie", ""}});
    json r = ubicacion::consultar({{"fuente", "app"}});
    CHECK_EQ(r["fuente"].get<std::string>(), std::string("app"));
    CHECK_EQ(r["en_tiempo_real"].get<bool>(), true);
    CHECK(cerca(r["velocidad_kmh"].get<double>(), 36.0, 1e-6));
    CHECK_EQ(r["hacia"].get<std::string>(), std::string("noreste"));
    CHECK_EQ(r["cerca_de"]["nombre"].get<std::string>(), std::string("Madrid"));
    CHECK_EQ(r["detalle"].get<std::string>(), std::string("gps"));
    // vieja: se da avisando
    write_file(dir / "ubicacion.json", json{{"lat", 40.4168}, {"lon", -3.7038}, {"t", now() - 3600}}.dump());
    json v = ubicacion::consultar({{"fuente", "app"}});
    CHECK(v.contains("aviso") && v["en_tiempo_real"].get<bool>() == false);
  }
  {
    Conf conf({{"fuente", "navegador"}, {"navegador", true}});
    bool mal = false;
    try {
      ubicacion::recibir({{"lat", 200}, {"lon", 0}}, "navegador");
    } catch (const std::invalid_argument&) {
      mal = true;
    }
    CHECK(mal);
    ubicacion::recibir({{"latitude", 41.3874}, {"longitude", 2.1686}, {"accuracy", 25}}, "navegador");
    json r = ubicacion::consultar();
    CHECK_EQ(r["fuente"].get<std::string>(), std::string("navegador"));
    CHECK_EQ(r["cerca_de"]["nombre"].get<std::string>(), std::string("Barcelona"));
    json e = ubicacion::estado();
    CHECK(e["fuentes"].is_array() && e.contains("ultima"));
  }
  {
    Conf conf({{"activada", false}});
    CHECK(ubicacion::consultar().value("desactivada", false));
  }
  set_env("NEXUS_UBICACION", "");
  fs::remove_all(dir);
}

#ifndef _WIN32
TEST("ubicacion: gpsd de verdad por TCP (servidor de prueba)") {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int uno = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  CHECK(bind(srv, (sockaddr*)&a, sizeof a) == 0);
  socklen_t al = sizeof a;
  getsockname(srv, (sockaddr*)&a, &al);
  int port = ntohs(a.sin_port);
  listen(srv, 1);
  // como gpsd: no cierra la conexion y va mandando informes; la consulta debe volver en cuanto hay posicion
  std::thread th([srv] {
    int c = accept(srv, nullptr, nullptr);
    if (c < 0) return;
    char buf[256];
    (void)!read(c, buf, sizeof buf);
    std::string v = "{\"class\":\"VERSION\"}\n{\"class\":\"TPV\",\"mode\":1}\n";
    (void)!write(c, v.data(), v.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::string t = "{\"class\":\"TPV\",\"mode\":3,\"lat\":42.8467,\"lon\":-2.6716,\"eph\":3}\n";
    (void)!write(c, t.data(), t.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    close(c);
  });
  Conf conf({{"fuente", "gpsd"}, {"gpsd", "127.0.0.1:" + std::to_string(port)}});
  double t0 = now();
  json r = ubicacion::consultar();
  double dur = now() - t0;
  th.join();
  close(srv);
  CHECK_EQ(r.value("fuente", std::string()), std::string("gpsd"));
  CHECK(dur < 2.0);
  CHECK_EQ(r["cerca_de"]["nombre"].get<std::string>(), std::string("Vitoria-Gasteiz"));
}
#endif

TEST("ubicacion: herramienta ubicacion_actual y privacidad") {
  auto& fns = tools::builtin_functions();
  CHECK(fns.count("ubicacion_actual") == 1);
  if (!fns.count("ubicacion_actual")) return;
  Conf conf({{"fuente", "manual"}, {"manual", {{"lat", 39.4699}, {"lon", -0.3763}}}});
  tools::ToolContext ctx;
  json r = fns["ubicacion_actual"](json::object(), ctx);
  CHECK_EQ(r["cerca_de"]["nombre"].get<std::string>(), std::string("Valencia"));
  CHECK(r.contains("privacidad"));
  // otro nodo de la red cooperativa o un usuario que no es el titular: no
  ctx.remote = true;
  bool negado = false;
  try {
    fns["ubicacion_actual"](json::object(), ctx);
  } catch (const tools::ToolError&) {
    negado = true;
  }
  CHECK(negado);
  ctx.remote = false;
  ctx.user = "empleado";
  negado = false;
  try {
    fns["ubicacion_actual"](json::object(), ctx);
  } catch (const tools::ToolError&) {
    negado = true;
  }
  CHECK(negado);
}
