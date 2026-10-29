#!/bin/sh
# Prueba nexus-flash contra la ROM falsa de cada chip: detectar, grabar (con MD5 donde la ROM lo tiene),
# grabar en una direccion distinta de 0 y la configuracion por el puerto serie tras grabar las luces.
set -eu
AQUI=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$AQUI/../build}
CXX=${CXX:-c++}
mkdir -p "$OUT"
$CXX -std=c++17 -O2 -Wall -Wextra -o "$OUT/nexus-flash" "$AQUI/../nexus_flash_cli.cpp"
$CXX -std=c++17 -O2 -Wall -Wextra -o "$OUT/rom_falsa" "$AQUI/rom_falsa.cpp"
T=$(mktemp -d)
trap 'kill $ROM 2>/dev/null || true; rm -rf "$T"' EXIT
ROM=0
mkdir -p "$T/bin"
fallos=0
for chip in esp8266 esp32 esp32s2 esp32s3 esp32c2 esp32c3 esp32c5 esp32c6; do
  # imagen de prueba de ~300 KB que no cae en bloques de 1 KB justos
  head -c 307203 /dev/urandom > "$T/bin/nexusled-$chip.bin"
  "$OUT/rom_falsa" $chip "$T/tty" "$T/volcado" 2>"$T/rom.log" &
  ROM=$!
  i=0; while [ ! -e "$T/tty" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
  det=$("$OUT/nexus-flash" chip --puerto "$T/tty" --espera 5 | tail -1)
  "$OUT/nexus-flash" luces --puerto "$T/tty" --bin "$T/bin" --espera 5 --wifi MiRed "clave \"rara\"" --leds 30 > "$T/salida.txt" 2>&1 || true
  kill $ROM; wait $ROM 2>/dev/null || true
  n=$(wc -c < "$T/bin/nexusled-$chip.bin")
  if [ "${det%% *}" != "$chip" ]; then echo "FALLO $chip: detectado '$det'"; fallos=$((fallos+1)); continue; fi
  head -c "$n" "$T/volcado" > "$T/inicio"
  if ! cmp "$T/bin/nexusled-$chip.bin" "$T/inicio" >/dev/null 2>&1; then
    echo "FALLO $chip: la flash no coincide"; cat "$T/salida.txt" "$T/rom.log"; fallos=$((fallos+1)); continue
  fi
  if ! grep -q '"ok":true' "$T/salida.txt" || ! grep -q '"pin":2\|"pin":16' "$T/rom.log"; then
    echo "FALLO $chip: sin respuesta a la configuracion"; cat "$T/salida.txt" "$T/rom.log"; fallos=$((fallos+1)); continue
  fi
  if [ $chip = esp8266 ]; then grep -q '"pin":2' "$T/rom.log" || { echo "FALLO esp8266: pin por defecto"; fallos=$((fallos+1)); }; fi
  echo "ok $chip ($det, $n bytes)"
done
# direccion distinta de 0 y sin verificar
head -c 5000 /dev/urandom > "$T/parte.bin"
"$OUT/rom_falsa" esp32 "$T/tty" "$T/volcado" 2>"$T/rom.log" &
ROM=$!
i=0; while [ ! -e "$T/tty" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
"$OUT/nexus-flash" grabar "$T/parte.bin" --puerto "$T/tty" --direccion 0x10000 --espera 5 --json > "$T/salida.txt"
kill $ROM; wait $ROM 2>/dev/null || true
dd if="$T/volcado" bs=1024 skip=64 count=5 2>/dev/null | head -c 5000 > "$T/trozo"; if cmp "$T/parte.bin" "$T/trozo" >/dev/null && grep -q '"fase":"verificar","pct":100' "$T/salida.txt"; then
  echo "ok grabar en 0x10000 con --json"
else
  echo "FALLO grabar en 0x10000"; cat "$T/salida.txt"; fallos=$((fallos+1))
fi
# un chip que no contesta: tiene que rendirse con un mensaje claro
if "$OUT/nexus-flash" chip --puerto /dev/null --espera 1 2>"$T/err.txt"; then echo "FALLO: /dev/null no deberia contestar"; fallos=$((fallos+1));
else echo "ok sin respuesta: $(tail -1 "$T/err.txt" | cut -c1-80)..."; fi
[ $fallos -eq 0 ] && echo "nexus-flash: todas las pruebas bien" || { echo "nexus-flash: $fallos fallos"; exit 1; }
