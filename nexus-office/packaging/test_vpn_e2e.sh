#!/usr/bin/env bash
# Prueba de extremo a extremo de la VPN de Nexus-Office en Linux (como root), sin movil:
#   - espacio de red "pc":    Nexus-Office escuchando + tunel WireGuard levantado con wg-quick y el nexusvpn.conf que genera Nexus-Office
#   - espacio de red "movil": WireGuard (wireguard-go) configurado EXACTAMENTE con el texto UAPI que genera la app
#                             Android (WgConfig.java, ejecutado con java) a partir del codigo NEXUSV1:...
#   - una red "internet" (veth) entre ambos. El movil pide /api/status a Nexus-Office por el tunel.
# Requisitos: iproute2 wireguard-tools wireguard-go openjdk (javac/java) y .cache/android/json.jar (org.json).
#   sudo bash packaging/test_vpn_e2e.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
PY=${PY:-$ROOT/venv/bin/python}; [ -x "$PY" ] || PY=$(command -v python3)
JSON_JAR=${NEXUS_JSON_JAR:-$ROOT/.cache/android/json.jar}
WORK=$(mktemp -d)
say() { printf '\033[36m[vpn-e2e]\033[0m %s\n' "$*"; }
cleanup() {
  set +e
  [ -n "${NEXUS_PID:-}" ] && kill "$NEXUS_PID" 2>/dev/null
  ip netns exec pc wg-quick down "$WORK/nexusvpn.conf" >/dev/null 2>&1
  pkill -f "wireguard-go wgmovil" 2>/dev/null
  ip netns del pc 2>/dev/null; ip netns del movil 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

say "Red de pruebas: pc (192.168.77.1) <-> movil (192.168.77.2)"
ip netns add pc; ip netns add movil
ip link add veth-pc type veth peer name veth-movil
ip link set veth-pc netns pc; ip link set veth-movil netns movil
ip -n pc addr add 192.168.77.1/24 dev veth-pc; ip -n movil addr add 192.168.77.2/24 dev veth-movil
for ns in pc movil; do ip -n $ns link set lo up; done
ip -n pc link set veth-pc up; ip -n movil link set veth-movil up

say "Nexus-Office genera el tunel del PC y el codigo del movil"
export NEXUS_DATA_DIR="$WORK/data"
CODE=$("$PY" - <<EOF
import sys; sys.path.insert(0, "$ROOT")
from nexus.config import get_config
from nexus.remote.vpn import VPN
cfg = get_config()
cfg.set("vpn.enabled", True); cfg.set("vpn.endpoints", ["192.168.77.1"]); cfg.set("server.host", "0.0.0.0")
cfg.set("server.lan_key", "clave-prueba")
r = VPN(cfg).add_peer("Movil de prueba", "clave-prueba")
print(r["code"])
EOF
)
cp "$NEXUS_DATA_DIR/vpn/nexusvpn.conf" "$WORK/nexusvpn.conf"

say "PC: wg-quick up (WireGuard en espacio de usuario)"
WG_QUICK_USERSPACE_IMPLEMENTATION=wireguard-go WG_I_PREFER_BUGGY_USERSPACE_TO_POLISHED_KMOD=1 \
  ip netns exec pc wg-quick up "$WORK/nexusvpn.conf"

say "PC: arrancando Nexus-Office en 0.0.0.0:8775"
ip netns exec pc env NEXUS_DATA_DIR="$NEXUS_DATA_DIR" "$PY" -m nexus run --host 0.0.0.0 --port 8775 --no-browser >"$WORK/nexus.log" 2>&1 &
NEXUS_PID=$!
for _ in $(seq 60); do ip netns exec pc curl -sf http://127.0.0.1:8775/api/ping >/dev/null 2>&1 && break; sleep 1; done

say "Movil: configuracion generada por la app (WgConfig.java)"
mkdir -p "$WORK/cls"
javac --release 8 -Xlint:-options -nowarn -cp "$JSON_JAR" -d "$WORK/cls" android/src/ai/nexus/office/client/WgConfig.java \
  android/test/ai/nexus/office/client/WgConfigCheck.java 2>&1 | grep -v JAVA_TOOL || true
OUT=$(java -cp "$WORK/cls:$JSON_JAR" ai.nexus.office.client.WgConfigCheck "$CODE" 2>/dev/null)
UAPI=$(printf '%s\n' "$OUT" | grep -v '^#')
URL=$(printf '%s\n' "$OUT" | sed -n 's/^#url=//p'); ADDR=$(printf '%s\n' "$OUT" | sed -n 's/^#address=//p')
ROUTE=$(printf '%s\n' "$OUT" | sed -n 's/^#route=//p')

say "Movil: wireguard-go + UAPI (como libwg-go en Android)"
ip netns exec movil env WG_I_PREFER_BUGGY_USERSPACE_TO_POLISHED_KMOD=1 wireguard-go wgmovil
SOCK=/var/run/wireguard/wgmovil.sock
for _ in $(seq 20); do [ -S "$SOCK" ] && break; sleep 0.2; done
RESP=$(printf 'set=1\n%s\n' "$UAPI" | ip netns exec movil socat - UNIX-CONNECT:$SOCK 2>/dev/null || \
       printf 'set=1\n%s\n' "$UAPI" | ip netns exec movil nc -U -q1 $SOCK)
echo "$RESP" | grep -q "errno=0" || { echo "UAPI rechazado: $RESP"; exit 1; }
ip -n movil addr add "$ADDR" dev wgmovil; ip -n movil link set wgmovil mtu 1280 up; ip -n movil route add "$ROUTE" dev wgmovil

say "Movil -> Nexus-Office por el tunel: $URL"
STATUS=$(ip netns exec movil curl -sf --max-time 15 -H "X-Nexus-Key: clave-prueba" "${URL%%\?*}api/status")
echo "$STATUS" | head -c 160; echo
SIN_CLAVE=$(ip netns exec movil curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${URL%%\?*}api/status")
ip netns exec pc wg show nexusvpn | sed -n '1,20p'
HS=$(ip netns exec pc wg show nexusvpn latest-handshakes | awk '{print $2}')
[ "${HS:-0}" -gt 0 ] && echo "$STATUS" | grep -q '"version"' && [ "$SIN_CLAVE" = 401 ] || { echo "FALLO"; cat "$WORK/nexus.log" | tail -20; exit 1; }
say "OK: negociacion WireGuard, Nexus-Office responde por la VPN y sigue exigiendo la clave de red (sin clave: $SIN_CLAVE)"
