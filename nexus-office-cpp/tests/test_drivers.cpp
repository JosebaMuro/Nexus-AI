// Drivers USB-serie y familia ESP: deteccion en un /sys de mentira, que se instalaria y reconocer cada chip.
#include <chrono>
#include <map>
#include <set>

#include "check.h"
#include "devices/drivers.h"
#include "devices/firmware.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;
namespace dr = devices::drivers;
namespace fw = devices::firmware;

TEST("firmware: reconoce toda la familia ESP en la salida de esptool") {
  CHECK_EQ(fw::chip_de_texto("Chip is ESP8266EX\nFeatures: WiFi"), std::string("esp8266"));
  CHECK_EQ(fw::chip_de_texto("Chip is ESP32-D0WD-V3 (revision v3.1)"), std::string("esp32"));
  CHECK_EQ(fw::chip_de_texto("Chip is ESP32-C3 (QFN32) (revision v0.4)"), std::string("esp32c3"));
  CHECK_EQ(fw::chip_de_texto("Chip type: ESP32-C5 (QFN32)"), std::string("esp32c5"));
  CHECK_EQ(fw::chip_de_texto("Chip is ESP32-S3 (QFN56)"), std::string("esp32s3"));
  CHECK_EQ(fw::chip_de_texto("Detecting chip type... ESP32-H2"), std::string("esp32h2"));
  CHECK_EQ(fw::chip_de_texto("Chip is ESP8684H2"), std::string("esp32c2"));
  CHECK_EQ(fw::chip_de_texto("sin chip"), std::string());
  std::set<std::string> chips;
  for (auto& f : fw::FAMILIA) chips.insert(jstr(f, "chip"));
  for (auto c : {"esp8266", "esp32", "esp32s2", "esp32s3", "esp32c2", "esp32c3", "esp32c5", "esp32c6", "esp32h2", "esp32p4"})
    CHECK(chips.count(c));
  for (auto& m : fw::MODELOS_LUCES) CHECK(chips.count(m));   // las luces solo en chips con Wi-Fi
}

TEST("drivers: puentes USB-serie conocidos") {
  CHECK_EQ(std::string(dr::puente(0x1A86, 0x7523)->driver), std::string("ch34x"));
  CHECK_EQ(std::string(dr::puente(0x1A86, 0x55D4)->chip), std::string("CH9102"));
  CHECK_EQ(std::string(dr::puente(0x10C4, 0xEA60)->driver), std::string("cp210x"));
  CHECK_EQ(std::string(dr::puente(0x0403, 0x6001)->driver), std::string("ftdi"));
  CHECK_EQ(std::string(dr::puente(0x303A, 0x1001)->driver), std::string("cdc"));
  CHECK(dr::puente(0x046D, 0xC52B) == nullptr);   // un raton
  CHECK_EQ(dr::detail::modulo_linux("ch34x"), std::string("ch341"));
  CHECK_EQ(dr::detail::modulo_linux("ftdi"), std::string("ftdi_sio"));
}

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {
void usb(const fs::path& root, const std::string& dev, const std::string& vid, const std::string& pid,
         const std::string& driver, const std::string& tty) {
  fs::path d = root / "bus" / "usb" / "devices" / dev, i = d / (dev + ":1.0");
  mkdirs(i);
  write_file(d / "idVendor", vid + "\n");
  write_file(d / "idProduct", pid + "\n");
  if (!driver.empty()) {
    mkdirs(root / "bus" / "usb" / "drivers" / driver);
    fs::create_directory_symlink(root / "bus" / "usb" / "drivers" / driver, i / "driver");
  }
  if (!tty.empty()) mkdirs(i / tty);
}
}  // namespace

TEST("drivers: en Linux ve que falta el modulo, que brltty se queda el CH340 y que se instalaria") {
  fs::path root = fs::temp_directory_path() / ("nexus-sys-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  usb(root, "1-1", "10c4", "ea60", "cp210x", "ttyUSB0");   // CP2102 ya funcionando
  usb(root, "1-2", "1a86", "7523", "usbfs", "");           // CH340 cogido por brltty
  usb(root, "1-3", "0403", "6001", "", "");                // FTDI sin modulo
  usb(root, "1-4", "046d", "c52b", "usbhid", "");          // un raton: no cuenta
  mkdirs(root / "usr" / "bin");
  write_file(root / "usr" / "bin" / "brltty", "");
  dr::detail::set_sysfs(root);
  json st = dr::estado();
  CHECK_EQ(st["dispositivos"].size(), (size_t)3);
  std::map<std::string, json> por;
  for (auto& d : st["dispositivos"]) por[jstr(d, "chip")] = d;
  CHECK(por["CP210x"]["instalado"] == true && jstr(por["CP210x"], "puerto") == "/dev/ttyUSB0");
  CHECK(jstr(por["CH340"], "problema") == "brltty" && por["CH340"]["instalado"] == false);
  CHECK(jstr(por["FT232R"], "problema") == "sin_modulo");
  std::set<std::string> faltan;
  for (auto& f : st["faltan"]) faltan.insert(f.get<std::string>());
  CHECK(faltan == std::set<std::string>({"ftdi", "permisos"}));
  json r = dr::instalar({}, true);
  CHECK(r["ok"] == true && r["comandos"].size() == 1);
  std::string cmd = r["comandos"][0].get<std::string>();
  CHECK(contains(cmd, "modprobe ftdi_sio") && contains(cmd, "85-brltty.rules") && contains(cmd, "ATTRS{idVendor}==\"1a86\""));
  dr::detail::set_sysfs("");
  rm_rf(root);
}
#endif
