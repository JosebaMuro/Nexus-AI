// Ubicacion en tiempo real: ver ubicacion.h. Nada de esto llama a servicios de internet.
#include "devices/ubicacion.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "core/config.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/net.h"
#include "util/sysinfo.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#include <roapi.h>
#include <windows.devices.geolocation.h>
#include <windows.foundation.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace nx::ubicacion {

namespace {

const auto L = log::get("ubicacion");
constexpr double PI = 3.14159265358979323846;
double rad(double g) { return g * PI / 180.0; }

std::mutex mu_;
std::map<std::string, Fix> recibidas_;   // "navegador" / "app" -> ultima posicion recibida por la API
std::optional<Fix> ultima_;               // ultima posicion entregada

json cfg(const std::string& k, const json& def) { return get_config().get("ubicacion." + k, def); }

// ------------------------------------------------------------------ tabla de ciudades de referencia (sin internet)
struct Ciudad {
  const char* nombre;
  const char* region;
  const char* pais;
  double lat, lon;
};

const Ciudad CIUDADES[] = {
    // capitales de provincia de Espana, Ceuta y Melilla
    {"A Coruña", "A Coruña", "España", 43.3623, -8.4115}, {"Albacete", "Albacete", "España", 38.9943, -1.8585},
    {"Alicante", "Alicante", "España", 38.3452, -0.4810}, {"Almería", "Almería", "España", 36.8340, -2.4637},
    {"Ávila", "Ávila", "España", 40.6565, -4.6818}, {"Badajoz", "Badajoz", "España", 38.8794, -6.9707},
    {"Barcelona", "Barcelona", "España", 41.3874, 2.1686}, {"Bilbao", "Bizkaia", "España", 43.2630, -2.9350},
    {"Burgos", "Burgos", "España", 42.3439, -3.6969}, {"Cáceres", "Cáceres", "España", 39.4753, -6.3724},
    {"Cádiz", "Cádiz", "España", 36.5271, -6.2886}, {"Castellón de la Plana", "Castellón", "España", 39.9864, -0.0513},
    {"Ciudad Real", "Ciudad Real", "España", 38.9848, -3.9274}, {"Córdoba", "Córdoba", "España", 37.8882, -4.7794},
    {"Cuenca", "Cuenca", "España", 40.0704, -2.1374}, {"Donostia / San Sebastián", "Gipuzkoa", "España", 43.3183, -1.9812},
    {"Girona", "Girona", "España", 41.9794, 2.8214}, {"Granada", "Granada", "España", 37.1773, -3.5986},
    {"Guadalajara", "Guadalajara", "España", 40.6329, -3.1672}, {"Huelva", "Huelva", "España", 37.2614, -6.9447},
    {"Huesca", "Huesca", "España", 42.1401, -0.4089}, {"Jaén", "Jaén", "España", 37.7796, -3.7849},
    {"León", "León", "España", 42.5987, -5.5671}, {"Lleida", "Lleida", "España", 41.6176, 0.6200},
    {"Logroño", "La Rioja", "España", 42.4627, -2.4450}, {"Lugo", "Lugo", "España", 43.0097, -7.5568},
    {"Madrid", "Madrid", "España", 40.4168, -3.7038}, {"Málaga", "Málaga", "España", 36.7213, -4.4214},
    {"Murcia", "Murcia", "España", 37.9922, -1.1307}, {"Pamplona / Iruña", "Navarra", "España", 42.8125, -1.6458},
    {"Ourense", "Ourense", "España", 42.3358, -7.8639}, {"Oviedo", "Asturias", "España", 43.3603, -5.8448},
    {"Palencia", "Palencia", "España", 42.0095, -4.5288}, {"Palma", "Illes Balears", "España", 39.5696, 2.6502},
    {"Las Palmas de Gran Canaria", "Las Palmas", "España", 28.1235, -15.4363},
    {"Pontevedra", "Pontevedra", "España", 42.4310, -8.6444}, {"Salamanca", "Salamanca", "España", 40.9701, -5.6635},
    {"Santa Cruz de Tenerife", "Santa Cruz de Tenerife", "España", 28.4636, -16.2518},
    {"Santander", "Cantabria", "España", 43.4623, -3.8100}, {"Segovia", "Segovia", "España", 40.9429, -4.1088},
    {"Sevilla", "Sevilla", "España", 37.3891, -5.9845}, {"Soria", "Soria", "España", 41.7666, -2.4790},
    {"Tarragona", "Tarragona", "España", 41.1189, 1.2445}, {"Teruel", "Teruel", "España", 40.3457, -1.1065},
    {"Toledo", "Toledo", "España", 39.8628, -4.0273}, {"Valencia", "Valencia", "España", 39.4699, -0.3763},
    {"Valladolid", "Valladolid", "España", 41.6523, -4.7245}, {"Vitoria-Gasteiz", "Araba / Álava", "España", 42.8467, -2.6716},
    {"Zamora", "Zamora", "España", 41.5035, -5.7446}, {"Zaragoza", "Zaragoza", "España", 41.6488, -0.8891},
    {"Ceuta", "Ceuta", "España", 35.8894, -5.3213}, {"Melilla", "Melilla", "España", 35.2923, -2.9381},
    // otras ciudades grandes de Espana
    {"Vigo", "Pontevedra", "España", 42.2406, -8.7207}, {"Gijón", "Asturias", "España", 43.5322, -5.6611},
    {"Santiago de Compostela", "A Coruña", "España", 42.8782, -8.5448}, {"Getxo", "Bizkaia", "España", 43.3569, -3.0116},
    {"Barakaldo", "Bizkaia", "España", 43.2956, -2.9973}, {"Durango", "Bizkaia", "España", 43.1700, -2.6300},
    {"Irun", "Gipuzkoa", "España", 43.3390, -1.7896}, {"Eibar", "Gipuzkoa", "España", 43.1843, -2.4718},
    {"Elche", "Alicante", "España", 38.2699, -0.7126}, {"Benidorm", "Alicante", "España", 38.5411, -0.1225},
    {"Torrevieja", "Alicante", "España", 37.9787, -0.6822}, {"Cartagena", "Murcia", "España", 37.6257, -0.9966},
    {"Lorca", "Murcia", "España", 37.6772, -1.7008}, {"Jerez de la Frontera", "Cádiz", "España", 36.6850, -6.1261},
    {"Algeciras", "Cádiz", "España", 36.1408, -5.4562}, {"Marbella", "Málaga", "España", 36.5101, -4.8825},
    {"Mérida", "Badajoz", "España", 38.9161, -6.3437}, {"Talavera de la Reina", "Toledo", "España", 39.9635, -4.8308},
    {"Ponferrada", "León", "España", 42.5499, -6.5983}, {"Móstoles", "Madrid", "España", 40.3223, -3.8650},
    {"Alcalá de Henares", "Madrid", "España", 40.4818, -3.3643}, {"Badalona", "Barcelona", "España", 41.4500, 2.2474},
    {"L'Hospitalet de Llobregat", "Barcelona", "España", 41.3596, 2.0997},
    {"Terrassa", "Barcelona", "España", 41.5610, 2.0089}, {"Sabadell", "Barcelona", "España", 41.5463, 2.1086},
    {"Reus", "Tarragona", "España", 41.1561, 1.1069}, {"Ibiza", "Illes Balears", "España", 38.9067, 1.4206},
    {"Arrecife", "Las Palmas", "España", 28.9630, -13.5477},
    {"Puerto del Rosario", "Las Palmas", "España", 28.5004, -13.8627},
    {"Andorra la Vella", "Andorra", "Andorra", 42.5063, 1.5218},
    // Europa
    {"Lisboa", "", "Portugal", 38.7223, -9.1393}, {"Oporto", "", "Portugal", 41.1579, -8.6291},
    {"París", "", "Francia", 48.8566, 2.3522}, {"Burdeos", "", "Francia", 44.8378, -0.5792},
    {"Biarritz", "", "Francia", 43.4832, -1.5586}, {"Toulouse", "", "Francia", 43.6047, 1.4442},
    {"Marsella", "", "Francia", 43.2965, 5.3698}, {"Lyon", "", "Francia", 45.7640, 4.8357},
    {"Londres", "", "Reino Unido", 51.5074, -0.1278}, {"Dublín", "", "Irlanda", 53.3498, -6.2603},
    {"Bruselas", "", "Bélgica", 50.8503, 4.3517}, {"Ámsterdam", "", "Países Bajos", 52.3676, 4.9041},
    {"Berlín", "", "Alemania", 52.5200, 13.4050}, {"Múnich", "", "Alemania", 48.1351, 11.5820},
    {"Fráncfort", "", "Alemania", 50.1109, 8.6821}, {"Zúrich", "", "Suiza", 47.3769, 8.5417},
    {"Ginebra", "", "Suiza", 46.2044, 6.1432}, {"Viena", "", "Austria", 48.2082, 16.3738},
    {"Roma", "", "Italia", 41.9028, 12.4964}, {"Milán", "", "Italia", 45.4642, 9.1900},
    {"Atenas", "", "Grecia", 37.9838, 23.7275}, {"Varsovia", "", "Polonia", 52.2297, 21.0122},
    {"Praga", "", "Chequia", 50.0755, 14.4378}, {"Estocolmo", "", "Suecia", 59.3293, 18.0686},
    {"Oslo", "", "Noruega", 59.9139, 10.7522}, {"Copenhague", "", "Dinamarca", 55.6761, 12.5683},
    {"Helsinki", "", "Finlandia", 60.1699, 24.9384}, {"Moscú", "", "Rusia", 55.7558, 37.6173},
    {"Estambul", "", "Turquía", 41.0082, 28.9784},
    // Africa
    {"Rabat", "", "Marruecos", 34.0209, -6.8416}, {"Casablanca", "", "Marruecos", 33.5731, -7.5898},
    {"Tánger", "", "Marruecos", 35.7595, -5.8340}, {"Argel", "", "Argelia", 36.7538, 3.0588},
    {"El Cairo", "", "Egipto", 30.0444, 31.2357}, {"Lagos", "", "Nigeria", 6.5244, 3.3792},
    {"Nairobi", "", "Kenia", -1.2921, 36.8219}, {"Johannesburgo", "", "Sudáfrica", -26.2041, 28.0473},
    // America
    {"Nueva York", "", "Estados Unidos", 40.7128, -74.0060}, {"Washington", "", "Estados Unidos", 38.9072, -77.0369},
    {"Los Ángeles", "", "Estados Unidos", 34.0522, -118.2437}, {"Miami", "", "Estados Unidos", 25.7617, -80.1918},
    {"Chicago", "", "Estados Unidos", 41.8781, -87.6298}, {"Toronto", "", "Canadá", 43.6532, -79.3832},
    {"Ciudad de México", "", "México", 19.4326, -99.1332}, {"La Habana", "", "Cuba", 23.1136, -82.3666},
    {"Caracas", "", "Venezuela", 10.4806, -66.9036}, {"Bogotá", "", "Colombia", 4.7110, -74.0721},
    {"Quito", "", "Ecuador", -0.1807, -78.4678}, {"Lima", "", "Perú", -12.0464, -77.0428},
    {"Santiago de Chile", "", "Chile", -33.4489, -70.6693}, {"Buenos Aires", "", "Argentina", -34.6037, -58.3816},
    {"Montevideo", "", "Uruguay", -34.9011, -56.1645}, {"São Paulo", "", "Brasil", -23.5505, -46.6333},
    {"Río de Janeiro", "", "Brasil", -22.9068, -43.1729},
    // Asia y Oceania
    {"Dubái", "", "Emiratos Árabes Unidos", 25.2048, 55.2708}, {"Bombay", "", "India", 19.0760, 72.8777},
    {"Nueva Delhi", "", "India", 28.6139, 77.2090}, {"Pekín", "", "China", 39.9042, 116.4074},
    {"Shanghái", "", "China", 31.2304, 121.4737}, {"Tokio", "", "Japón", 35.6762, 139.6503},
    {"Seúl", "", "Corea del Sur", 37.5665, 126.9780}, {"Singapur", "", "Singapur", 1.3521, 103.8198},
    {"Sídney", "", "Australia", -33.8688, 151.2093},
};

// sin tildes ni mayusculas para comparar nombres
std::string clave(const std::string& s) {
  static const std::vector<std::pair<std::string, std::string>> T = {
      {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"ü", "u"}, {"ñ", "n"}, {"Á", "a"}, {"É", "e"},
      {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"}, {"Ñ", "n"}, {"ã", "a"}, {"ç", "c"}, {"à", "a"}, {"è", "e"}, {"ò", "o"}};
  std::string r = s;
  for (auto& [a, b] : T) {
    size_t p = 0;
    while ((p = r.find(a, p)) != std::string::npos) r.replace(p, a.size(), b);
  }
  return lower(trim(r));
}

// ------------------------------------------------------------------ NMEA 0183
double nmea_grados(const std::string& v, const std::string& hemi) {
  if (v.empty()) return NAN;
  size_t punto = v.find('.');
  size_t gl = (punto == std::string::npos ? v.size() : punto);
  if (gl < 3) return NAN;
  double grados = std::atof(v.substr(0, gl - 2).c_str());
  double minutos = std::atof(v.substr(gl - 2).c_str());
  double r = grados + minutos / 60.0;
  if (hemi == "S" || hemi == "W") r = -r;
  return r;
}

bool nmea_suma_ok(const std::string& linea) {
  size_t ast = linea.rfind('*');
  if (linea.empty() || linea[0] != '$') return false;
  if (ast == std::string::npos) return true;   // sin suma: se acepta (algunos receptores la omiten)
  if (ast + 3 > linea.size()) return false;
  unsigned suma = 0;
  for (size_t i = 1; i < ast; ++i) suma ^= (unsigned char)linea[i];
  unsigned dada = (unsigned)std::strtoul(linea.substr(ast + 1, 2).c_str(), nullptr, 16);
  return suma == dada;
}

std::vector<std::string> campos(const std::string& linea) {
  std::string cuerpo = linea.substr(0, linea.rfind('*') == std::string::npos ? linea.size() : linea.rfind('*'));
  std::vector<std::string> out;
  std::string cur;
  for (char c : cuerpo) {
    if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r' && c != '\n') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

double num_o(const json& j, const char* k) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_number()) return NAN;
  return it->get<double>();
}

// ------------------------------------------------------------------ fuentes
std::optional<Fix> leer_app() {
  std::string txt = read_text(P(fichero_app()));
  if (txt.empty()) return std::nullopt;
  json j = json::parse(txt, nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  return parse_fix_json(j, "app");
}

std::optional<Fix> leer_recibida(const std::string& fuente) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = recibidas_.find(fuente);
  if (it == recibidas_.end()) return std::nullopt;
  return it->second;
}

std::optional<Fix> leer_gpsd(double timeout, std::string& err) {
  std::string dir = cfg("gpsd", "127.0.0.1:2947").get<std::string>();
  if (dir.empty()) return std::nullopt;
  std::string host = dir;
  int port = 2947;
  if (auto p = dir.rfind(':'); p != std::string::npos) {
    host = dir.substr(0, p);
    port = std::atoi(dir.substr(p + 1).c_str());
  }
  // solo un gpsd del propio equipo o de la red local: la posicion no se pide fuera
  if (!net::is_loopback_ip(host) && host != "localhost" && !net::is_private_ip(host)) {
    err = "gpsd solo se consulta en este equipo o en la red local";
    return std::nullopt;
  }
  auto r = net::tcp_exchange(host == "localhost" ? "127.0.0.1" : host, port,
                             "?WATCH={\"enable\":true,\"json\":true};?POLL;\n", std::min(timeout, 4.0), 262144,
                             [](const std::string& out) { return parse_gpsd(out).has_value(); });
  if (!r) return std::nullopt;   // no hay gpsd
  auto f = parse_gpsd(*r);
  if (!f) err = "gpsd responde pero el receptor aun no tiene posicion (busca satelites a cielo abierto)";
  return f;
}

// puertos serie que se pueden abrir sin preguntar: solo los que se identifican como GPS (abrir un ESP32 o un
// Arduino lo reinicia, asi que nunca se prueban puertos al azar)
std::vector<std::string> series_gps() {
  std::vector<std::string> out;
  std::string conf = cfg("serie", "").get<std::string>();
  if (!conf.empty()) {
    out.push_back(conf);
    return out;
  }
#if !defined(_WIN32)
  fs::path d = "/dev/serial/by-id";
  std::error_code ec;
  if (fs::is_directory(d, ec))
    for (auto& e : fs::directory_iterator(d, ec)) {
      std::string n = lower(e.path().filename().string());
      if (contains(n, "gps") || contains(n, "gnss") || contains(n, "u-blox") || contains(n, "ublox"))
        out.push_back(e.path().string());
    }
#endif
  return out;
}

std::optional<Fix> leer_serie(const std::string& puerto, int baudios, double timeout, std::string& err) {
  std::string buf;
  auto fin = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(timeout * 1000));
#ifdef _WIN32
  std::string nombre = puerto.rfind("\\\\.\\", 0) == 0 ? puerto : "\\\\.\\" + puerto;
  HANDLE h = CreateFileW(P(nombre).wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    err = "no se pudo abrir " + puerto;
    return std::nullopt;
  }
  DCB dcb{};
  dcb.DCBlength = sizeof dcb;
  if (GetCommState(h, &dcb)) {
    dcb.BaudRate = (DWORD)baudios;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);
  }
  COMMTIMEOUTS to{};
  to.ReadIntervalTimeout = 50;
  to.ReadTotalTimeoutConstant = 200;
  SetCommTimeouts(h, &to);
  char tmp[512];
  while (std::chrono::steady_clock::now() < fin) {
    DWORD n = 0;
    if (!ReadFile(h, tmp, sizeof tmp, &n, nullptr)) break;
    buf.append(tmp, n);
    if (auto f = parse_nmea(buf)) {
      CloseHandle(h);
      f->detalle = puerto;
      return f;
    }
  }
  CloseHandle(h);
#else
  int fd = ::open(puerto.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    err = "no se pudo abrir " + puerto + " (¿permiso del grupo dialout?)";
    return std::nullopt;
  }
  termios t{};
  if (tcgetattr(fd, &t) == 0) {
    cfmakeraw(&t);
    speed_t v = baudios >= 115200 ? B115200 : baudios >= 57600 ? B57600 : baudios >= 38400 ? B38400
              : baudios >= 19200 ? B19200 : baudios >= 9600 ? B9600 : B4800;
    cfsetispeed(&t, v);
    cfsetospeed(&t, v);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
  }
  char tmp[512];
  while (std::chrono::steady_clock::now() < fin) {
    ssize_t n = ::read(fd, tmp, sizeof tmp);
    if (n > 0) {
      buf.append(tmp, (size_t)n);
      if (auto f = parse_nmea(buf)) {
        ::close(fd);
        f->detalle = puerto;
        return f;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    if (buf.size() > 65536) buf.erase(0, buf.size() - 4096);
  }
  ::close(fd);
#endif
  err = buf.empty() ? puerto + " no envia datos NMEA" : "el GPS de " + puerto + " aun no tiene posicion";
  return std::nullopt;
}

#ifdef _WIN32
// ------------------------------------------------------------------ Windows.Devices.Geolocation (Windows 10/11)
namespace geo = ABI::Windows::Devices::Geolocation;
namespace wf = ABI::Windows::Foundation;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

// espera una operacion asincrona sin manejador de finalizacion (0 en curso, 1 completada)
template <class Op>
bool esperar(Op* op, double timeout) {
  ComPtr<wf::IAsyncInfo> info;
  if (!op || FAILED(op->QueryInterface(IID_PPV_ARGS(&info)))) return false;
  auto fin = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(timeout * 1000));
  wf::AsyncStatus st{};
  while (true) {
    if (FAILED(info->get_Status(&st))) return false;
    if ((int)st != 0 || std::chrono::steady_clock::now() >= fin) break;
    Sleep(50);
  }
  if ((int)st == 0) info->Cancel();
  return (int)st == 1;
}

double ref_valor(wf::IReference<double>* r) {
  double v = NAN;
  if (r) r->get_Value(&v);
  return v;
}

std::optional<Fix> leer_windows(double timeout, std::string& err) {
  HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
  struct Fin {
    HRESULT h;
    ~Fin() {
      if (SUCCEEDED(h)) RoUninitialize();
    }
  } fin{init};
  HStringReference clase(RuntimeClass_Windows_Devices_Geolocation_Geolocator);
  ComPtr<geo::IGeolocatorStatics> estaticos;
  if (SUCCEEDED(RoGetActivationFactory(clase.Get(), IID_PPV_ARGS(&estaticos))) && estaticos) {
    ComPtr<wf::IAsyncOperation<geo::GeolocationAccessStatus>> op;
    if (SUCCEEDED(estaticos->RequestAccessAsync(&op)) && esperar(op.Get(), 10)) {
      geo::GeolocationAccessStatus a{};
      if (SUCCEEDED(op->GetResults(&a)) && (int)a == 2) {
        err = "Windows no deja usar la ubicacion: Configuracion > Privacidad > Ubicacion y activa «Permitir que las "
              "aplicaciones de escritorio accedan a tu ubicacion»";
        return std::nullopt;
      }
    }
  }
  ComPtr<IInspectable> insp;
  if (FAILED(RoActivateInstance(clase.Get(), &insp))) {
    err = "este Windows no tiene servicio de ubicacion";
    return std::nullopt;
  }
  ComPtr<geo::IGeolocator> g;
  if (FAILED(insp.As(&g))) return std::nullopt;
  g->put_DesiredAccuracy((geo::PositionAccuracy)1);   // alta: GPS si lo hay
  ComPtr<wf::IAsyncOperation<geo::Geoposition*>> op;
  if (FAILED(g->GetGeopositionAsync(&op)) || !esperar(op.Get(), timeout)) {
    err = "Windows no dio la posicion a tiempo (¿ubicacion desactivada?)";
    return std::nullopt;
  }
  ComPtr<geo::IGeoposition> pos;
  if (FAILED(op->GetResults(&pos)) || !pos) return std::nullopt;
  ComPtr<geo::IGeocoordinate> c;
  if (FAILED(pos->get_Coordinate(&c)) || !c) return std::nullopt;
  Fix f;
  f.fuente = "windows";
  ComPtr<geo::IGeocoordinateWithPoint> cp;
  ComPtr<geo::IGeopoint> pt;
  geo::BasicGeoposition b{};
  if (SUCCEEDED(c.As(&cp)) && SUCCEEDED(cp->get_Point(&pt)) && pt && SUCCEEDED(pt->get_Position(&b))) {
    f.lat = b.Latitude;
    f.lon = b.Longitude;
    f.altitud = b.Altitude != 0 ? b.Altitude : NAN;
  } else {
    c->get_Latitude(&f.lat);
    c->get_Longitude(&f.lon);
  }
  c->get_Accuracy(&f.precision);
  ComPtr<wf::IReference<double>> v, r;
  if (SUCCEEDED(c->get_Speed(&v))) f.velocidad = ref_valor(v.Get());
  if (SUCCEEDED(c->get_Heading(&r))) f.rumbo = ref_valor(r.Get());
  ComPtr<geo::IGeocoordinateWithPositionData> pd;
  geo::PositionSource src{};
  static const char* ORIGEN[] = {"red movil", "satelite (GPS)", "wifi", "direccion IP", "desconocido", "por defecto",
                                 "ubicacion aproximada"};
  if (SUCCEEDED(c.As(&pd)) && SUCCEEDED(pd->get_PositionSource(&src)) && (int)src >= 0 && (int)src < 7)
    f.detalle = ORIGEN[(int)src];
  f.t = now();
  if (!coordenadas_validas(f.lat, f.lon)) return std::nullopt;
  return f;
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
// ------------------------------------------------------------------ GeoClue2 por D-Bus (libdbus-1 cargada al ejecutar)
struct DBus {
  struct Err {
    const char* name;
    const char* message;
    unsigned d1 : 1, d2 : 1, d3 : 1, d4 : 1, d5 : 1;
    void* padding1;
  };
  using Conn = void;
  using Msg = void;
  void* h = nullptr;
  void (*error_init)(Err*) = nullptr;
  void (*error_free)(Err*) = nullptr;
  Conn* (*bus_get_private)(int, Err*) = nullptr;
  void (*connection_close)(Conn*) = nullptr;
  void (*connection_unref)(Conn*) = nullptr;
  Msg* (*new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
  Msg* (*send_block)(Conn*, Msg*, int, Err*) = nullptr;
  void (*message_unref)(Msg*) = nullptr;
  int (*append_args)(Msg*, int, ...) = nullptr;
  int (*get_args)(Msg*, Err*, int, ...) = nullptr;
  void (*iter_init_append)(Msg*, void*) = nullptr;
  int (*iter_append_basic)(void*, int, const void*) = nullptr;
  int (*iter_open)(void*, int, const char*, void*) = nullptr;
  int (*iter_close)(void*, void*) = nullptr;
  int (*iter_init)(Msg*, void*) = nullptr;
  int (*iter_type)(void*) = nullptr;
  void (*iter_recurse)(void*, void*) = nullptr;
  void (*iter_get_basic)(void*, void*) = nullptr;

  bool cargar() {
    if (h) return true;
    h = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    auto s = [this](const char* n) { return dlsym(h, n); };
#define NX_SYM(campo, nombre) campo = reinterpret_cast<decltype(campo)>(s(nombre))
    NX_SYM(error_init, "dbus_error_init");
    NX_SYM(error_free, "dbus_error_free");
    NX_SYM(bus_get_private, "dbus_bus_get_private");
    NX_SYM(connection_close, "dbus_connection_close");
    NX_SYM(connection_unref, "dbus_connection_unref");
    NX_SYM(new_method_call, "dbus_message_new_method_call");
    NX_SYM(send_block, "dbus_connection_send_with_reply_and_block");
    NX_SYM(message_unref, "dbus_message_unref");
    NX_SYM(append_args, "dbus_message_append_args");
    NX_SYM(get_args, "dbus_message_get_args");
    NX_SYM(iter_init_append, "dbus_message_iter_init_append");
    NX_SYM(iter_append_basic, "dbus_message_iter_append_basic");
    NX_SYM(iter_open, "dbus_message_iter_open_container");
    NX_SYM(iter_close, "dbus_message_iter_close_container");
    NX_SYM(iter_init, "dbus_message_iter_init");
    NX_SYM(iter_type, "dbus_message_iter_get_arg_type");
    NX_SYM(iter_recurse, "dbus_message_iter_recurse");
    NX_SYM(iter_get_basic, "dbus_message_iter_get_basic");
#undef NX_SYM
    bool ok = error_init && error_free && bus_get_private && connection_close && connection_unref && new_method_call &&
              send_block && message_unref && append_args && get_args && iter_init_append && iter_append_basic &&
              iter_open && iter_close && iter_init && iter_type && iter_recurse && iter_get_basic;
    if (!ok) h = nullptr;
    return ok;
  }
};

constexpr int T_INVALID = 0, T_STRING = 's', T_OBJPATH = 'o', T_UINT32 = 'u', T_DOUBLE = 'd', T_VARIANT = 'v';
const char* GC = "org.freedesktop.GeoClue2";

// iterador de libdbus (estructura opaca del tamano de unos 10 punteros): sitio de sobra y bien alineado
struct Iter {
  alignas(16) unsigned char b[256];
};

std::optional<Fix> leer_geoclue(double timeout, std::string& err) {
  static DBus d;
  static std::mutex carga;
  {
    std::lock_guard<std::mutex> lk(carga);
    if (!d.cargar()) return std::nullopt;
  }
  DBus::Err e{};
  d.error_init(&e);
  void* conn = d.bus_get_private(1 /* DBUS_BUS_SYSTEM */, &e);
  if (!conn) {
    d.error_free(&e);
    return std::nullopt;
  }
  struct Cierre {
    DBus& d;
    void* c;
    ~Cierre() {
      d.connection_close(c);
      d.connection_unref(c);
    }
  } cierre{d, conn};

  auto llamar = [&](void* msg, int ms = 3000) -> void* {
    void* r = d.send_block(conn, msg, ms, &e);
    d.message_unref(msg);
    if (!r) {
      std::string n = e.name ? e.name : "", m = e.message ? e.message : "";
      d.error_free(&e);
      d.error_init(&e);
      if (contains(n, "ServiceUnknown") || contains(n, "NameHasNoOwner")) err = "";
      else if (!m.empty()) err = "GeoClue: " + m;
    }
    return r;
  };
  // propiedad de una interfaz: double en *salida, o la ruta de objeto copiada en *ruta_obj
  auto propiedad = [&](const char* ruta, const char* iface, const char* nombre, int tipo, double* salida,
                       std::string* ruta_obj = nullptr) -> bool {
    void* m = d.new_method_call(GC, ruta, "org.freedesktop.DBus.Properties", "Get");
    d.append_args(m, T_STRING, &iface, T_STRING, &nombre, T_INVALID);
    void* r = llamar(m);
    if (!r) return false;
    Iter it, sub;
    bool ok = d.iter_init(r, it.b) && d.iter_type(it.b) == T_VARIANT;
    if (ok) {
      d.iter_recurse(it.b, sub.b);
      ok = d.iter_type(sub.b) == tipo;
      if (ok && tipo == T_OBJPATH && ruta_obj) {
        const char* sp = nullptr;
        d.iter_get_basic(sub.b, &sp);
        *ruta_obj = sp ? sp : "";   // la cadena es de la respuesta: se copia antes de liberarla
      } else if (ok && salida) {
        d.iter_get_basic(sub.b, salida);
      }
    }
    d.message_unref(r);
    return ok;
  };
  auto poner = [&](const char* ruta, const char* nombre, int tipo, const void* valor) -> bool {
    void* m = d.new_method_call(GC, ruta, "org.freedesktop.DBus.Properties", "Set");
    const char* iface = "org.freedesktop.GeoClue2.Client";
    d.append_args(m, T_STRING, &iface, T_STRING, &nombre, T_INVALID);
    Iter it, sub;
    d.iter_init_append(m, it.b);
    const char firma[2] = {(char)tipo, 0};
    d.iter_open(it.b, T_VARIANT, firma, sub.b);
    d.iter_append_basic(sub.b, tipo, valor);
    d.iter_close(it.b, sub.b);
    void* r = llamar(m);
    if (r) d.message_unref(r);
    return r != nullptr;
  };

  void* m = d.new_method_call(GC, "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager", "GetClient");
  // GeoClue espera unos segundos a que aparezca el agente del escritorio del usuario antes de responder
  void* r = llamar(m, (int)(std::max(timeout, 3.0) * 1000) + 2000);
  if (!r) return std::nullopt;   // no hay GeoClue en este sistema
  const char* cp = nullptr;
  std::string cliente;
  if (d.get_args(r, &e, T_OBJPATH, &cp, T_INVALID) && cp) cliente = cp;
  d.message_unref(r);
  d.error_free(&e);
  d.error_init(&e);
  if (cliente.empty()) return std::nullopt;

  const char* id = "nexus-office";
  unsigned nivel = 8;   // GCLUE_ACCURACY_LEVEL_EXACT
  poner(cliente.c_str(), "DesktopId", T_STRING, &id);
  poner(cliente.c_str(), "RequestedAccuracyLevel", T_UINT32, &nivel);
  void* ini = d.new_method_call(GC, cliente.c_str(), "org.freedesktop.GeoClue2.Client", "Start");
  void* ri = llamar(ini, 5000);
  if (!ri) {
    if (err.empty())
      err = "GeoClue no dejo usar la ubicacion (añade [nexus-office] allowed=true system=true users= a "
            "/etc/geoclue/geoclue.conf o acepta el aviso del escritorio)";
    return std::nullopt;
  }
  d.message_unref(ri);

  std::optional<Fix> out;
  auto fin = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(timeout * 1000));
  std::string loc;
  while (std::chrono::steady_clock::now() < fin) {
    if (propiedad(cliente.c_str(), "org.freedesktop.GeoClue2.Client", "Location", T_OBJPATH, nullptr, &loc) && loc != "/" &&
        !loc.empty()) {
      const char* LI = "org.freedesktop.GeoClue2.Location";
      Fix f;
      f.fuente = "geoclue";
      bool ok = propiedad(loc.c_str(), LI, "Latitude", T_DOUBLE, &f.lat) &&
                propiedad(loc.c_str(), LI, "Longitude", T_DOUBLE, &f.lon);
      if (ok) {
        propiedad(loc.c_str(), LI, "Accuracy", T_DOUBLE, &f.precision);
        double v = NAN;
        if (propiedad(loc.c_str(), LI, "Altitude", T_DOUBLE, &v) && v > -1e300) f.altitud = v;
        if (propiedad(loc.c_str(), LI, "Speed", T_DOUBLE, &v) && v >= 0) f.velocidad = v;
        if (propiedad(loc.c_str(), LI, "Heading", T_DOUBLE, &v) && v >= 0) f.rumbo = v;
        f.t = now();
        if (coordenadas_validas(f.lat, f.lon)) out = f;
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  void* st = d.new_method_call(GC, cliente.c_str(), "org.freedesktop.GeoClue2.Client", "Stop");
  if (void* rs = llamar(st)) d.message_unref(rs);
  if (!out && err.empty()) err = "GeoClue no dio la posicion a tiempo";
  return out;
}
#endif

std::optional<Fix> leer_manual() {
  json m = cfg("manual", json::object());
  if (!m.is_object() || !m.contains("lat") || !m.contains("lon") || !m["lat"].is_number() || !m["lon"].is_number())
    return std::nullopt;
  Fix f;
  f.lat = m["lat"].get<double>();
  f.lon = m["lon"].get<double>();
  if (!coordenadas_validas(f.lat, f.lon)) return std::nullopt;
  f.fuente = "manual";
  f.detalle = jstr(m, "nombre");
  f.precision = m.contains("precision") && m["precision"].is_number() ? m["precision"].get<double>() : -1;
  f.t = now();
  return f;
}

std::string pista_plataforma() {
  if (sys::is_android())
    return "En Ajustes > Ubicación pulsa «Usar el GPS del móvil» y acepta el permiso; si está en segundo plano, abre "
           "la app un momento.";
#ifdef _WIN32
  return "Activa la ubicación en Configuración > Privacidad y seguridad > Ubicación (también «aplicaciones de "
         "escritorio»), conecta un GPS (ubicacion.serie) o pon una posición fija en Ajustes > Ubicación.";
#elif defined(__APPLE__)
  return "En macOS usa un GPS con gpsd o por USB (ubicacion.serie), la del navegador, o pon una posición fija en "
         "Ajustes > Ubicación.";
#else
  return "Instala GeoClue o gpsd con un receptor GPS, indica el puerto del GPS (ubicacion.serie), activa la del "
         "navegador o pon una posición fija en Ajustes > Ubicación.";
#endif
}

}  // namespace

// ------------------------------------------------------------------ piezas puras
bool coordenadas_validas(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
         !(lat == 0 && lon == 0);
}

std::optional<std::pair<double, double>> parse_coordenadas(const std::string& s) {
  std::string t = trim(s);
  for (char& c : t)
    if (c == ';' || c == '\t') c = ' ';
  // "43.26, -2.93" / "43.26 -2.93" / "43,26 -2,93" no: la coma decimal solo si hay espacio de separador
  size_t sep = t.find(", ");
  std::string a, b;
  if (sep != std::string::npos) {
    a = t.substr(0, sep);
    b = t.substr(sep + 2);
  } else if ((sep = t.find(',')) != std::string::npos && t.find(',', sep + 1) == std::string::npos) {
    a = t.substr(0, sep);
    b = t.substr(sep + 1);
  } else if ((sep = t.find(' ')) != std::string::npos) {
    a = t.substr(0, sep);
    b = t.substr(sep + 1);
  } else {
    return std::nullopt;
  }
  a = trim(a);
  b = trim(b);
  auto num = [](std::string x, double& out) {
    std::replace(x.begin(), x.end(), ',', '.');
    char* end = nullptr;
    out = std::strtod(x.c_str(), &end);
    return end && *end == 0 && !x.empty();
  };
  double la = 0, lo = 0;
  if (!num(a, la) || !num(b, lo) || !coordenadas_validas(la, lo)) return std::nullopt;
  return std::make_pair(la, lo);
}

std::optional<Fix> parse_nmea(const std::string& text) {
  std::optional<Fix> gga, rmc;
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find('\n', i);
    std::string linea = trim(text.substr(i, j == std::string::npos ? std::string::npos : j - i));
    i = j == std::string::npos ? text.size() : j + 1;
    if (linea.size() < 7 || linea[0] != '$' || !nmea_suma_ok(linea)) continue;
    auto c = campos(linea);
    std::string tipo = c[0].size() >= 6 ? c[0].substr(3, 3) : "";
    if (tipo == "GGA" && c.size() >= 10) {
      if (c[6].empty() || c[6] == "0") continue;   // sin fijacion
      Fix f;
      f.lat = nmea_grados(c[2], c[3]);
      f.lon = nmea_grados(c[4], c[5]);
      if (!coordenadas_validas(f.lat, f.lon)) continue;
      f.satelites = c[7].empty() ? -1 : std::atoi(c[7].c_str());
      double hdop = c[8].empty() ? NAN : std::atof(c[8].c_str());
      if (std::isfinite(hdop) && hdop > 0) f.precision = std::round(hdop * 5.0 * 10) / 10;   // estimacion habitual
      if (!c[9].empty()) f.altitud = std::atof(c[9].c_str());
      gga = f;
    } else if (tipo == "RMC" && c.size() >= 9) {
      if (c[2] != "A") continue;   // V = aviso: sin posicion valida
      Fix f;
      f.lat = nmea_grados(c[3], c[4]);
      f.lon = nmea_grados(c[5], c[6]);
      if (!coordenadas_validas(f.lat, f.lon)) continue;
      if (!c[7].empty()) f.velocidad = std::atof(c[7].c_str()) * 0.514444;   // nudos -> m/s
      if (!c[8].empty()) f.rumbo = std::atof(c[8].c_str());
      rmc = f;
    }
  }
  if (!gga && !rmc) return std::nullopt;
  Fix f = gga ? *gga : *rmc;
  if (gga && rmc) {
    f.velocidad = rmc->velocidad;
    f.rumbo = rmc->rumbo;
  }
  f.fuente = "nmea";
  f.t = now();
  return f;
}

std::optional<Fix> parse_gpsd(const std::string& lines) {
  auto de_tpv = [](const json& j) -> std::optional<Fix> {
    if (!j.is_object() || jstr(j, "class") != "TPV" || j.value("mode", 0) < 2) return std::nullopt;
    Fix f;
    f.lat = num_o(j, "lat");
    f.lon = num_o(j, "lon");
    if (!coordenadas_validas(f.lat, f.lon)) return std::nullopt;
    f.altitud = std::isfinite(num_o(j, "altMSL")) ? num_o(j, "altMSL") : num_o(j, "alt");
    f.velocidad = num_o(j, "speed");
    f.rumbo = num_o(j, "track");
    double eph = num_o(j, "eph");
    if (!std::isfinite(eph)) {
      double x = num_o(j, "epx"), y = num_o(j, "epy");
      if (std::isfinite(x) && std::isfinite(y)) eph = std::max(x, y);
    }
    if (std::isfinite(eph)) f.precision = eph;
    f.fuente = "gpsd";
    f.detalle = jstr(j, "device");
    f.t = now();
    return f;
  };
  size_t i = 0;
  while (i < lines.size()) {
    size_t j = lines.find('\n', i);
    std::string l = trim(lines.substr(i, j == std::string::npos ? std::string::npos : j - i));
    i = j == std::string::npos ? lines.size() : j + 1;
    if (l.empty() || l[0] != '{') continue;
    json o = json::parse(l, nullptr, false);
    if (o.is_discarded()) continue;
    if (jstr(o, "class") == "POLL" && o.contains("tpv") && o["tpv"].is_array()) {
      for (auto& t : o["tpv"])
        if (auto f = de_tpv(t)) return f;
    }
    if (auto f = de_tpv(o)) return f;
  }
  return std::nullopt;
}

std::optional<Fix> parse_fix_json(const json& j, const std::string& fuente) {
  if (!j.is_object()) return std::nullopt;
  double la = num_o(j, "lat"), lo = num_o(j, "lon");
  if (!std::isfinite(la)) la = num_o(j, "latitude");
  if (!std::isfinite(lo)) lo = num_o(j, "longitude");
  if (!coordenadas_validas(la, lo)) return std::nullopt;
  Fix f;
  f.lat = la;
  f.lon = lo;
  double p = num_o(j, "precision");
  if (!std::isfinite(p)) p = num_o(j, "accuracy");
  f.precision = std::isfinite(p) && p >= 0 ? p : -1;
  f.altitud = num_o(j, "altitud");
  if (!std::isfinite(f.altitud)) f.altitud = num_o(j, "altitude");
  f.velocidad = num_o(j, "velocidad");
  if (!std::isfinite(f.velocidad)) f.velocidad = num_o(j, "speed");
  if (std::isfinite(f.velocidad) && f.velocidad < 0) f.velocidad = NAN;
  f.rumbo = num_o(j, "rumbo");
  if (!std::isfinite(f.rumbo)) f.rumbo = num_o(j, "heading");
  if (std::isfinite(f.rumbo) && f.rumbo < 0) f.rumbo = NAN;
  f.detalle = jstr(j, "proveedor");
  double t = num_o(j, "t");
  if (std::isfinite(t) && t > 1e11) t /= 1000.0;   // milisegundos (Java, navegador)
  f.t = std::isfinite(t) && t > 0 ? t : now();
  f.fuente = fuente;
  return f;
}

double distancia_km(double lat1, double lon1, double lat2, double lon2) {
  double dlat = rad(lat2 - lat1), dlon = rad(lon2 - lon1);
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 6371.0088 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

double rumbo_grados(double lat1, double lon1, double lat2, double lon2) {
  double y = std::sin(rad(lon2 - lon1)) * std::cos(rad(lat2));
  double x = std::cos(rad(lat1)) * std::sin(rad(lat2)) - std::sin(rad(lat1)) * std::cos(rad(lat2)) * std::cos(rad(lon2 - lon1));
  double g = std::atan2(y, x) * 180.0 / PI;
  return std::fmod(g + 360.0, 360.0);
}

std::string rumbo_texto(double g) {
  static const char* N[8] = {"norte", "noreste", "este", "sureste", "sur", "suroeste", "oeste", "noroeste"};
  if (!std::isfinite(g)) return "";
  int i = (int)std::floor(std::fmod(g + 22.5 + 360.0, 360.0) / 45.0) % 8;
  return N[i];
}

json ciudad_cercana(double lat, double lon) {
  const Ciudad* mejor = nullptr;
  double dmin = 1e18;
  for (auto& c : CIUDADES) {
    double d = distancia_km(lat, lon, c.lat, c.lon);
    if (d < dmin) {
      dmin = d;
      mejor = &c;
    }
  }
  if (!mejor) return nullptr;
  // el rumbo es desde la ciudad hacia la posicion: «a 3 km al noreste de Bilbao»
  return {{"nombre", mejor->nombre},
          {"region", mejor->region},
          {"pais", mejor->pais},
          {"distancia_km", std::round(dmin * 10) / 10},
          {"direccion", dmin < 0.5 ? "" : rumbo_texto(rumbo_grados(mejor->lat, mejor->lon, lat, lon))}};
}

json buscar_lugar(const std::string& nombre, const json& lugares) {
  std::string k = clave(nombre);
  if (k.empty()) return nullptr;
  if (auto c = parse_coordenadas(nombre)) return {{"nombre", nombre}, {"lat", c->first}, {"lon", c->second}};
  if (lugares.is_array())
    for (auto& l : lugares)
      if (l.is_object() && clave(jstr(l, "nombre")) == k && l.contains("lat") && l["lat"].is_number())
        return {{"nombre", jstr(l, "nombre")}, {"lat", l["lat"]}, {"lon", l["lon"]}};
  const Ciudad* parcial = nullptr;
  for (auto& c : CIUDADES) {
    std::string n = clave(c.nombre);
    if (n == k) return {{"nombre", c.nombre}, {"lat", c.lat}, {"lon", c.lon}, {"pais", c.pais}};
    // "Donostia / San Sebastian" responde a "san sebastian" y a "donostia"
    if (!parcial && (n.find(k) != std::string::npos) && k.size() >= 4) parcial = &c;
  }
  if (parcial) return {{"nombre", parcial->nombre}, {"lat", parcial->lat}, {"lon", parcial->lon}, {"pais", parcial->pais}};
  return nullptr;
}

json fix_json(const Fix& f, double ahora) {
  auto r = [](double v, double esc) { return std::round(v * esc) / esc; };
  double edad = std::max(0.0, ahora - f.t);
  double frescura = cfg("frescura_s", 120).get<double>();
  json o = {{"lat", r(f.lat, 1e6)},
            {"lon", r(f.lon, 1e6)},
            {"fuente", f.fuente},
            {"hora", strftime_local("%H:%M:%S", f.t)},
            {"fecha", strftime_local("%d/%m/%Y", f.t)},
            {"edad_s", (long long)std::round(edad)},
            {"en_tiempo_real", f.fuente != "manual" && edad <= frescura}};
  char geo_uri[64];
  std::snprintf(geo_uri, sizeof geo_uri, "geo:%.6f,%.6f", f.lat, f.lon);
  o["geo"] = geo_uri;
  if (!f.detalle.empty()) o["detalle"] = f.detalle;
  if (f.precision >= 0) o["precision_m"] = r(f.precision, 10);
  if (std::isfinite(f.altitud)) o["altitud_m"] = r(f.altitud, 10);
  if (std::isfinite(f.velocidad)) o["velocidad_kmh"] = r(f.velocidad * 3.6, 10);
  if (std::isfinite(f.rumbo)) {
    o["rumbo_grados"] = r(f.rumbo, 10);
    o["hacia"] = rumbo_texto(f.rumbo);
  }
  if (f.satelites >= 0) o["satelites"] = f.satelites;
  // lugar guardado (Oficina, Casa, Almacen...) si esta dentro de su radio
  json lugares = cfg("lugares", json::array());
  if (lugares.is_array()) {
    double mejor = 1e18;
    for (auto& l : lugares) {
      if (!l.is_object() || !l.contains("lat") || !l["lat"].is_number() || !l.contains("lon") || !l["lon"].is_number())
        continue;
      double d = distancia_km(f.lat, f.lon, l["lat"].get<double>(), l["lon"].get<double>()) * 1000;
      double radio = l.contains("radio") && l["radio"].is_number() ? l["radio"].get<double>() : 150.0;
      if (d <= radio + std::max(0.0, std::min(f.precision, 500.0)) && d < mejor) {
        mejor = d;
        o["lugar"] = jstr(l, "nombre");
        o["lugar_distancia_m"] = (long long)std::round(d);
      }
    }
  }
  o["cerca_de"] = ciudad_cercana(f.lat, f.lon);
  return o;
}

// ------------------------------------------------------------------ consultas
std::string fichero_app() {
  std::string e = env("NEXUS_UBICACION");
  if (!e.empty()) return e;
  return S(paths::data_dir().parent_path() / "ubicacion.json");
}

void recibir(const json& body, const std::string& fuente) {
  auto f = parse_fix_json(body, fuente);
  if (!f) throw std::invalid_argument("coordenadas no validas");
  // la del navegador llega con la hora del navegador: si viene del futuro o es rara, vale la del equipo
  if (f->t > now() + 60) f->t = now();
  std::lock_guard<std::mutex> lk(mu_);
  recibidas_[fuente] = *f;
}

json consultar(const json& op) {
  if (!truthy(cfg("activada", true)))
    return {{"error", "La ubicación está desactivada en Ajustes > Ubicación"}, {"desactivada", true}};
  std::string pedida = jstr(op, "fuente");
  if (pedida.empty() || pedida == "auto") {
    json fc = cfg("fuente", "auto");
    pedida = fc.is_string() && !fc.get<std::string>().empty() ? fc.get<std::string>() : "auto";
  }
  double timeout = op.contains("timeout_s") && op["timeout_s"].is_number() ? op["timeout_s"].get<double>()
                                                                             : cfg("timeout_s", 8).get<double>();
  timeout = std::clamp(timeout, 1.0, 30.0);
  double frescura = cfg("frescura_s", 120).get<double>();
  double ahora = now();
  std::vector<std::string> avisos;
  auto quiere = [&](const char* f) { return pedida == "auto" || pedida == f; };
  auto fresca = [&](const std::optional<Fix>& f) { return f && ahora - f->t <= frescura; };

  std::optional<Fix> elegida, antigua;
  auto considerar_antigua = [&](const std::optional<Fix>& f) {
    if (f && (!antigua || f->t > antigua->t)) antigua = f;
  };
  // 1) las que empuja el dispositivo (GPS del movil, navegador): son las mas directas si estan al dia
  for (const char* fu : {"app", "navegador"}) {
    if (!quiere(fu)) continue;
    std::optional<Fix> f = std::string(fu) == "app" ? leer_app() : leer_recibida(fu);
    if (std::string(fu) == "app" && !f) f = leer_recibida("app");
    if (fresca(f)) {
      if (!elegida || (f->precision >= 0 && (elegida->precision < 0 || f->precision < elegida->precision)))
        elegida = f;
    } else {
      considerar_antigua(f);
    }
  }
  // 2) receptores GPS del equipo
  if (!elegida && quiere("gpsd")) {
    std::string e;
    elegida = leer_gpsd(timeout, e);
    if (!e.empty()) avisos.push_back(e);
  }
  if (!elegida && quiere("nmea")) {
    int baudios = (int)cfg("baudios", 9600).get<double>();
    for (auto& p : series_gps()) {
      std::string e;
      elegida = leer_serie(p, baudios, timeout, e);
      if (elegida) break;
      if (!e.empty()) avisos.push_back(e);
    }
  }
  // 3) servicio de ubicacion del sistema
#ifdef _WIN32
  if (!elegida && quiere("windows")) {
    std::string e;
    elegida = leer_windows(timeout, e);
    if (!e.empty()) avisos.push_back(e);
  }
#endif
#if defined(__linux__) && !defined(__ANDROID__)
  // sin agente del escritorio (equipo sin pantalla) GeoClue hace esperar cada consulta: en automatico, tras un fallo
  // no se vuelve a probar en unos minutos
  static std::atomic<double> geoclue_pausa{0};
  if (!elegida && quiere("geoclue") && (pedida == "geoclue" || ahora >= geoclue_pausa.load())) {
    std::string e;
    elegida = leer_geoclue(pedida == "geoclue" ? timeout : std::min(timeout, 4.0), e);
    if (!elegida && !e.empty()) geoclue_pausa = ahora + 300;
    if (!e.empty()) avisos.push_back(e);
  }
#endif
  // 4) la ultima conocida, avisando de su edad; 5) la fija de Ajustes
  bool vieja = false;
  if (!elegida && antigua && pedida != "manual") {
    elegida = antigua;
    vieja = true;
  }
  if (!elegida && quiere("manual")) elegida = leer_manual();
  if (!elegida) {
    json err = {{"error", "No hay ninguna fuente de ubicación disponible ahora mismo. " + pista_plataforma()}};
    if (!avisos.empty()) err["detalles"] = avisos;
    return err;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    ultima_ = elegida;
  }
  json o = fix_json(*elegida, ahora);
  if (vieja) o["aviso"] = "Es la última posición conocida (de hace " + std::to_string((long long)(ahora - elegida->t) / 60) +
                          " min): abre la app o activa una fuente en tiempo real para actualizarla.";
  if (elegida->fuente == "manual") o["aviso"] = "Es la posición fija puesta en Ajustes, no una medida en tiempo real.";
  // distancia a un destino: lugar guardado, ciudad de la tabla o "lat, lon"
  std::string hasta = jstr(op, "hasta");
  if (!hasta.empty()) {
    json dest = buscar_lugar(hasta, cfg("lugares", json::array()));
    if (dest.is_null()) {
      o["hasta"] = {{"error", "No conozco «" + hasta + "» sin internet: guárdalo en Ajustes > Ubicación o da sus coordenadas"}};
    } else {
      double dl = dest["lat"].get<double>(), dn = dest["lon"].get<double>();
      double km = distancia_km(elegida->lat, elegida->lon, dl, dn);
      o["hasta"] = {{"nombre", dest["nombre"]},
                    {"distancia_km", std::round(km * 10) / 10},
                    {"rumbo_grados", std::round(rumbo_grados(elegida->lat, elegida->lon, dl, dn))},
                    {"direccion", rumbo_texto(rumbo_grados(elegida->lat, elegida->lon, dl, dn))},
                    {"nota", "distancia en línea recta, no por carretera"}};
    }
  }
  o["privacidad"] = "La posición se ha leído en este equipo y no se ha enviado a ningún servicio externo.";
  return o;
}

json estado() {
  json fuentes = json::array();
  auto add = [&](const std::string& id, const std::string& nombre, bool disponible, const std::string& nota) {
    fuentes.push_back({{"id", id}, {"nombre", nombre}, {"disponible", disponible}, {"nota", nota}});
  };
  double ahora = now();
  auto app = leer_app();
  if (sys::is_android() || app)
    add("app", "GPS del móvil (app Android)", (bool)app,
        app ? "última medida hace " + std::to_string((long long)(ahora - app->t)) + " s" : "falta el permiso de ubicación");
  auto nav = leer_recibida("navegador");
  add("navegador", "Ubicación del navegador", (bool)nav,
      nav ? "última medida hace " + std::to_string((long long)(ahora - nav->t)) + " s"
          : truthy(cfg("navegador", false)) ? "esperando al navegador" : "desactivada");
  {
    std::string dir = cfg("gpsd", "127.0.0.1:2947").get<std::string>();
    std::string host = dir.substr(0, dir.rfind(':'));
    int port = dir.rfind(':') == std::string::npos ? 2947 : std::atoi(dir.substr(dir.rfind(':') + 1).c_str());
    bool loc = host.empty() || host == "localhost" || net::is_loopback_ip(host);
    bool hay = !dir.empty() && (loc ? net::port_in_use("127.0.0.1", port) : true);
    add("gpsd", "Receptor GPS con gpsd", hay, hay ? dir : "no hay gpsd en " + dir);
  }
  {
    auto s = series_gps();
    add("nmea", "GPS por USB o puerto serie (NMEA)", !s.empty(),
        s.empty() ? "indica el puerto en ubicacion.serie" : s[0]);
  }
#ifdef _WIN32
  add("windows", "Servicio de ubicación de Windows", true, "según Configuración > Privacidad > Ubicación");
#endif
#if defined(__linux__) && !defined(__ANDROID__)
  {
    void* h = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    bool hay = h != nullptr && path_exists("/usr/share/dbus-1/system-services/org.freedesktop.GeoClue2.service");
    add("geoclue", "Servicio de ubicación de Linux (GeoClue)", hay, hay ? "por D-Bus del sistema" : "no instalado");
  }
#endif
  auto man = leer_manual();
  add("manual", "Posición fija de Ajustes", (bool)man, man ? (man->detalle.empty() ? "puesta" : man->detalle) : "sin poner");
  json o = {{"activada", truthy(cfg("activada", true))},
            {"fuente", cfg("fuente", "auto")},
            {"fuentes", fuentes},
            {"config", get_config().get("ubicacion", json::object())},
            {"android", sys::is_android()}};
  std::lock_guard<std::mutex> lk(mu_);
  if (ultima_) o["ultima"] = fix_json(*ultima_, ahora);
  return o;
}

}  // namespace nx::ubicacion
