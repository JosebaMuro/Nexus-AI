# Manifiesto para compilar MicroPython con NexusPico congelado (lo usa .github/workflows/firmware.yml):
#   make -C ports/rp2 BOARD=RPI_PICO_W FROZEN_MANIFEST=$PWD/firmware/nexuspico/manifest.py
include("$(PORT_DIR)/boards/RPI_PICO_W/manifest.py")
module("nexuspico.py")
module("epd2in13.py")
module("main.py")
