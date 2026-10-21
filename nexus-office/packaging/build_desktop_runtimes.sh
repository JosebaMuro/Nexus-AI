#!/usr/bin/env bash
# llama-server y sd-cli precompilados para todas las plataformas de escritorio, para los paquetes offline de Nexus-Office.
#   Linux x86_64 y ARM64 (estatico, musl: vale para cualquier distro), Windows x64 y ARM64, macOS ARM e Intel.
#   En x86_64 hay dos variantes: cpu (AVX2/FMA, CPUs desde 2013) y cpu-compat (SSE4.2, cualquier CPU de 64 bits).
#   Variante vulkan en Windows y Linux (x86_64 y ARM64): GPU de cualquier fabricante (NVIDIA, AMD, Intel, Qualcomm
#   Adreno, Mali...) con el driver Vulkan del sistema. En macOS la GPU (Metal) sale de packaging/build_gpu_runtimes.py.
# Se compilan con zig (PyPI) a partir de llama.cpp y stable-diffusion.cpp tal como los traen llama-cpp-python y
# stable-diffusion-cpp-python (PyPI): no hace falta GitHub.
# CUDA, ROCm y SYCL siguen pudiendo instalarse desde Sistema > Runtimes (variante oficial) con conexion.
#
#   bash packaging/build_desktop_runtimes.sh [plataforma...]   ->  .cache/desktop-runtimes/<plataforma>/{llama,sd}/<variante>/
#   NEXUS_RUNTIMES="llama" bash packaging/build_desktop_runtimes.sh ...   (solo uno de los dos)
#   NEXUS_VARIANTS="vulkan" bash packaging/build_desktop_runtimes.sh ...  (solo esas variantes)
#
# Requisitos (Ubuntu/WSL): sudo apt install cmake ninja-build python3 curl g++ glslc libvulkan-dev spirv-headers
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/.cache/desktop-build"; OUT="$ROOT/.cache/desktop-runtimes"
LLAMA_PY=0.3.36            # mismo llama.cpp que la app Android autonoma
SD_PY=0.4.7
ZIG_VER=0.17.0
J=$(nproc 2>/dev/null || echo 4)
RUNTIMES_WANTED="${NEXUS_RUNTIMES:-llama sd}"
VARIANTS_WANTED="${NEXUS_VARIANTS:-}"
say() { printf '\033[1;36m[runtimes]\033[0m %s\n' "$*"; }
mkdir -p "$W/dl" "$W/src" "$W/bin" "$OUT"
cd "$W"
PY_HOST=$(command -v python3)
[ -x zig/ziglang/zig ] || { "$PY_HOST" -m pip download -q --no-deps ziglang==$ZIG_VER --platform manylinux_2_12_x86_64 \
    --only-binary=:all: -d dl && "$PY_HOST" -c "import zipfile,glob; zipfile.ZipFile(glob.glob('dl/ziglang-$ZIG_VER-*.whl')[0]).extractall('zig')" \
    && chmod +x zig/ziglang/zig; }
sdist() {   # sdist <paquete> <version> <carpeta>: descarga de PyPI y descomprime en src/
  [ -s "dl/$3-$2.tar.gz" ] || curl -fL -o "dl/$3-$2.tar.gz" \
    "$(curl -fs "https://pypi.org/pypi/$1/$2/json" | "$PY_HOST" -c 'import json,sys;print([u for u in json.load(sys.stdin)["urls"] if u["packagetype"]=="sdist"][0]["url"])')"
  [ -d "src/$3-$2" ] || tar xzf "dl/$3-$2.tar.gz" -C src
}
sdist llama-cpp-python $LLAMA_PY llama_cpp_python
sdist stable-diffusion-cpp-python $SD_PY stable_diffusion_cpp_python
ZIG=$W/zig/ziglang/zig
# Vulkan: cabeceras aparte (no /usr/include, que mezclaria las del sistema al compilar para otra plataforma) y un
# cargador de pega por plataforma solo para enlazar: al ejecutar se usa el libvulkan.so.1 / vulkan-1.dll del driver.
VK="$W/vulkan"; mkdir -p "$VK/include"
cp -r /usr/include/vulkan /usr/include/vk_video "$VK/include/" 2>/dev/null || true
[ -d /usr/include/spirv ] && cp -r /usr/include/spirv "$VK/include/"
GLSLC=$(command -v glslc || true)
"$PY_HOST" - "$VK" <<'PYVK'
import re, sys, pathlib
vk = pathlib.Path(sys.argv[1])
names = set()
for h in ("vulkan_core.h", "vulkan_win32.h"):
    f = vk / "include" / "vulkan" / h
    if f.exists():
        names |= set(re.findall(r"VKAPI_CALL (vk\w+)\(", f.read_text()))
names = sorted(names)
(vk / "stub.c").write_text("".join(f"void {n}(void) {{}}\n" for n in names))
(vk / "vulkan-1.def").write_text("LIBRARY vulkan-1.dll\nEXPORTS\n" + "".join(f"  {n}\n" for n in names))
PYVK
vk_stub() {   # vk_stub <objetivo zig> <sistema>: ruta del cargador de pega para enlazar
  local d="$VK/$1"; mkdir -p "$d"
  if [ "$2" = Windows ]; then
    local m=i386:x86-64; [[ "$1" == aarch64* ]] && m=arm64
    [ -s "$d/libvulkan-1.a" ] || "$ZIG" dlltool -d "$VK/vulkan-1.def" -l "$d/libvulkan-1.a" -m $m -D vulkan-1.dll
    echo "$d/libvulkan-1.a"
  else
    [ -s "$d/libvulkan.so" ] || "$ZIG" cc -target "$1" -shared -o "$d/libvulkan.so" -Wl,-soname,libvulkan.so.1 "$VK/stub.c"
    echo "$d/libvulkan.so"
  fi
}
COMMON="-DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_BACKEND_DL=OFF -DGGML_CPU_ALL_VARIANTS=OFF"
# runtime:carpeta de llama.cpp/sd.cpp:objetivo cmake:ejecutable:opciones
RT_DEFS=(
  "llama:$W/src/llama_cpp_python-$LLAMA_PY/vendor/llama.cpp:llama-server:llama-server:-DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON"
  "sd:$W/src/stable_diffusion_cpp_python-$SD_PY/vendor/stable-diffusion.cpp:sd-cli:sd-cli:-DSD_BUILD_EXAMPLES=ON -DSD_WEBP=OFF -DSD_WEBM=OFF"
)

# plataforma:variante:objetivo zig:cpu:sistema cmake:procesador:opciones extra de ggml
X86_COMPAT="-DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_BMI2=OFF -DGGML_AVX512=OFF"
MAC="-DGGML_METAL=OFF -DGGML_ACCELERATE=OFF -DGGML_BLAS=OFF"
VKOPT="-DCMAKE_SKIP_RPATH=ON -DGGML_VULKAN=ON -DVulkan_INCLUDE_DIR=$VK/include -DVulkan_GLSLC_EXECUTABLE=$GLSLC"
BUILDS=(
  "linux-x86_64:vulkan:x86_64-linux-gnu.2.28:x86_64_v3:Linux:x86_64:$VKOPT"
  "linux-aarch64:vulkan:aarch64-linux-gnu.2.28:generic:Linux:aarch64:$VKOPT"
  "windows-x64:vulkan:x86_64-windows-gnu:x86_64_v3:Windows:AMD64:$VKOPT"
  "windows-arm64:vulkan:aarch64-windows-gnu:generic:Windows:ARM64:$VKOPT"
  "linux-x86_64:cpu:x86_64-linux-musl:x86_64_v3:Linux:x86_64:"
  "linux-x86_64:cpu-compat:x86_64-linux-musl:x86_64_v2:Linux:x86_64:$X86_COMPAT"
  "linux-aarch64:cpu:aarch64-linux-musl:generic:Linux:aarch64:"
  "windows-x64:cpu:x86_64-windows-gnu:x86_64_v3:Windows:AMD64:"
  "windows-x64:cpu-compat:x86_64-windows-gnu:x86_64_v2:Windows:AMD64:$X86_COMPAT"
  "windows-arm64:cpu:aarch64-windows-gnu:generic:Windows:ARM64:"
  "macos-arm64:cpu:aarch64-macos.13.0:apple_m1:Darwin:arm64:$MAC"
  "macos-x86_64:cpu:x86_64-macos.13.0:x86_64_v3:Darwin:x86_64:$MAC"
)
WANT=" ${*:-all} "
for rtdef in "${RT_DEFS[@]}"; do
 IFS=: read -r rt src target_cmake exe_base rt_opts <<<"$rtdef"
 [[ " $RUNTIMES_WANTED " == *" $rt "* ]] || continue
 for b in "${BUILDS[@]}"; do
  IFS=: read -r plat variant target mcpu sys proc extra <<<"$b"
  [[ "$WANT" == *" all "* || "$WANT" == *" $plat "* ]] || continue
  [ -z "$VARIANTS_WANTED" ] || [[ " $VARIANTS_WANTED " == *" $variant "* ]] || continue
  name="$plat-$variant"; bname="$rt-$name"; exe=$exe_base; [ "$sys" = Windows ] && exe=$exe_base.exe
  dest="$OUT/$plat/$rt/$variant"
  if [ -s "$dest/$exe" ]; then say "$bname: ya compilado"; continue; fi
  k=$name; [ "$rt" = llama ] || k=$bname     # compiladores y build propios por runtime: se pueden lanzar a la vez
  for t in cc c++; do
    printf '#!/bin/sh\nexec %s %s -target %s -mcpu=%s -Wno-date-time "$@"\n' "$ZIG" $t "$target" "$mcpu" > "bin/$k-$t"
  done
  for t in ar ranlib; do printf '#!/bin/sh\nexec %s %s "$@"\n' "$ZIG" $t > "bin/$k-$t"; done
  chmod +x bin/$k-*
  static=""; [ "$sys" = Linux ] && [[ "$target" == *musl* ]] && static="-static"   # vulkan: dinamico (carga el driver)
  if [ "$variant" = vulkan ]; then
    [ -n "$GLSLC" ] || { say "$bname: falta glslc (sudo apt install glslc libvulkan-dev)"; continue; }
    extra="$extra -DVulkan_LIBRARY=$(vk_stub "$target" "$sys")"
  fi
  cat > "tc-$k.cmake" <<TC
set(CMAKE_SYSTEM_NAME $sys)
set(CMAKE_SYSTEM_PROCESSOR $proc)
set(CMAKE_C_COMPILER $W/bin/$k-cc)
set(CMAKE_CXX_COMPILER $W/bin/$k-c++)
set(CMAKE_AR $W/bin/$k-ar)
set(CMAKE_RANLIB $W/bin/$k-ranlib)
set(CMAKE_EXE_LINKER_FLAGS "$static -s")
TC
  # macOS: zig enlaza solo; cmake busca install_name_tool (de Xcode) aunque aqui no se use
  [ "$sys" = Darwin ] && echo "set(CMAKE_INSTALL_NAME_TOOL /bin/true)" >> "tc-$k.cmake"
  bdir="build-$k"
  say "$bname: compilando $exe_base ($target, $mcpu)"
  # shellcheck disable=SC2086
  cmake -S "$src" -B "$bdir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$W/tc-$k.cmake" -DCMAKE_BUILD_TYPE=Release \
    $COMMON $rt_opts $extra > "build-$bname.log" 2>&1 || { say "$bname: FALLO cmake (ver $W/build-$bname.log)"; tail -20 "build-$bname.log"; continue; }
  cmake --build "$bdir" --target "$target_cmake" -j"$J" >> "build-$bname.log" 2>&1 || { say "$bname: FALLO (ver $W/build-$bname.log)"; tail -20 "build-$bname.log"; continue; }
  mkdir -p "$dest"
  cp "$bdir/bin/$exe" "$dest/$exe"
  cp "$src/LICENSE" "$dest/LICENSE-$rt.txt"
  echo "{\"runtime\": \"$rt\", \"source\": \"$exe_base de PyPI ($rt)\", \"target\": \"$target\", \"cpu\": \"$mcpu\", \"variant\": \"$variant\"}" > "$dest/nexus_runtime.json"
  say "$bname: listo ($(du -h "$dest/$exe" | cut -f1))"
 done
done
