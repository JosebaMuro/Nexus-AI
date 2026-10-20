#!/usr/bin/env bash
# Runtime nativo de la app Nexus-Office autonomo (Android ARM64), sin SDK ni NDK de Google:
#   - Python 3.12 compilado para aarch64-linux-musl con zig (con ssl, sqlite3, ctypes, lzma, bz2, zlib)
#   - dependencias de Nexus-Office: ruedas musllinux_aarch64 de PyPI
#   - llama-server estatico en tres variantes de CPU (ARMv8, ARMv8.2+dotprod, ARMv8.6+i8mm)
#   - opcional, con el NDK: llama-server con GPU (Vulkan/OpenCL) enlazado con la libc de Android
#   - el cargador de musl (libc.so de musl para arm64 de Ubuntu ports)
# Resultado en .cache/android-runtime/ (lo usa packaging/build_apk_autonomo.py).
#
# Requisitos (Ubuntu/WSL): sudo apt install python3.12 cmake ninja-build autoconf automake libtool tcl curl \
#                                            binutils-aarch64-linux-gnu qemu-user-static
#   bash packaging/build_android_runtime.sh            # todo
#   bash packaging/build_android_runtime.sh --probar   # ademas arranca Nexus-Office emulando ARM64 (qemu) y lo comprueba
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=$ROOT/.cache/android-runtime
W=${NEXUS_RUNTIME_WORK:-$ROOT/.cache/android-runtime-build}
J=$(nproc)
export PIP_BREAK_SYSTEM_PACKAGES=1 PIP_DISABLE_PIP_VERSION_CHECK=1   # pip solo escribe en --target / -d, nunca en el sistema
PY_HOST=$(command -v python3.12) || { echo "Falta python3.12 (sudo apt install python3.12)"; exit 1; }
say() { printf '\033[36m[runtime]\033[0m %s\n' "$*"; }
UBU=http://archive.ubuntu.com/ubuntu/pool/main
PORTS=http://ports.ubuntu.com/ubuntu-ports/pool/universe
LLAMA_PY=0.3.36            # llama-cpp-python: su paquete fuente de PyPI trae llama.cpp completo (con el servidor)
mkdir -p "$W"/{dl,src,bin} "$OUT"
cd "$W"

fetch() { [ -s "dl/$(basename "$1")" ] || curl -fL --retry 4 -o "dl/$(basename "$1")" "$1"; }
say "Descargando fuentes (Ubuntu) y zig (PyPI)"
fetch $UBU/p/python3.12/python3.12_3.12.3.orig.tar.xz
fetch $UBU/z/zlib/zlib_1.3.dfsg+really1.3.1.orig.tar.gz
fetch $UBU/b/bzip2/bzip2_1.0.8.orig.tar.gz
fetch $UBU/x/xz-utils/xz-utils_5.8.1.orig.tar.xz
fetch $UBU/libf/libffi/libffi_3.4.6.orig.tar.gz
fetch $UBU/s/sqlite3/sqlite3_3.46.1.orig.tar.xz
fetch $UBU/o/openssl/openssl_3.5.5.orig.tar.gz
fetch $PORTS/m/musl/musl_1.2.5-3build1_arm64.deb
[ -x zig/ziglang/zig ] || { "$PY_HOST" -m pip download -q --no-deps ziglang==0.17.0 --platform manylinux_2_12_x86_64 \
    --only-binary=:all: -d dl && unzip -q -o dl/ziglang-0.17.0-*.whl -d zig; }
[ -s dl/llama_cpp_python-$LLAMA_PY.tar.gz ] || curl -fL -o dl/llama_cpp_python-$LLAMA_PY.tar.gz \
    "$(curl -fs https://pypi.org/pypi/llama-cpp-python/$LLAMA_PY/json | "$PY_HOST" -c 'import json,sys;print(json.load(sys.stdin)["urls"][0]["url"])')"
for t in dl/*.tar.*; do [ -f "src/.$(basename "$t").ok" ] || { tar xf "$t" -C src && touch "src/.$(basename "$t").ok"; }; done
[ -d muslroot ] || dpkg-deb -x dl/musl_1.2.5-3build1_arm64.deb muslroot

ZIG=$W/zig/ziglang/zig
for t in cc c++; do   # compilador cruzado: dinamico (Python y extensiones) y estatico (llama-server)
  printf '#!/bin/sh\nexec %s %s -target aarch64-linux-musl -mcpu=generic -dynamic -Wno-date-time "$@"\n' "$ZIG" $t > bin/aarch64-linux-musl-$t
  printf '#!/bin/sh\nexec %s %s -target aarch64-linux-musl -mcpu=${NEXUS_MCPU:-generic} -Wno-date-time "$@"\n' "$ZIG" $t > bin/st-$t
done
for t in ar ranlib; do printf '#!/bin/sh\nexec %s %s "$@"\n' "$ZIG" $t > bin/aarch64-linux-musl-$t; done
printf '#!/bin/sh\nexit 0\n' > bin/aarch64-linux-musl-strip
chmod +x bin/*
export PATH=$W/bin:$PATH CC=aarch64-linux-musl-cc CXX=aarch64-linux-musl-c++ AR=aarch64-linux-musl-ar RANLIB=aarch64-linux-musl-ranlib
P=$W/sysroot
H="--host=aarch64-linux-musl --build=x86_64-linux-gnu --prefix=$P --enable-static --disable-shared"

if [ ! -f "$P/lib/libssl.a" ]; then
  say "Bibliotecas: zlib, bzip2, xz, libffi, sqlite3, OpenSSL"
  (cd src/zlib-1.3.1 && make distclean >/dev/null 2>&1 || true; CFLAGS="-O2 -fPIC" CHOST=aarch64-linux-musl ./configure --static --prefix=$P >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  (cd src/zlib-1.3.1 && aarch64-linux-musl-cc -O2 -fPIC -shared -Wl,-soname,libz.so.1 -o $W/libz.so.1 adler32.c crc32.c deflate.c \
     infback.c inffast.c inflate.c inftrees.c trees.c zutil.c compress.c uncompr.c gzclose.c gzlib.c gzread.c gzwrite.c)
  (cd src/bzip2-1.0.8 && make -j$J CC=$CC AR=$AR RANLIB=$RANLIB CFLAGS="-O2 -fPIC -D_FILE_OFFSET_BITS=64" libbz2.a >/dev/null && cp libbz2.a $P/lib/ && cp bzlib.h $P/include/)
  (cd src/xz-5.8.1 && CFLAGS="-O2 -fPIC" ./configure $H --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc --disable-nls >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  # el paquete de Debian no trae configure: se regenera (LT_SYS_SYMBOL_USCORE solo importa en macOS)
  (cd src/libffi-3.4.6 && sed -i 's/^\s*LT_SYS_SYMBOL_USCORE\s*$/sys_symbol_underscore=no/' configure.ac && libtoolize -fci >/dev/null 2>&1 && aclocal -I m4 >/dev/null 2>&1 \
     && autoheader >/dev/null 2>&1 && autoconf >/dev/null 2>&1 && automake -ac --foreign >/dev/null 2>&1 \
     && CFLAGS="-O2 -fPIC" ./configure $H --disable-docs --disable-multi-os-directory >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  (cd src/sqlite3-3.46.1 && CFLAGS="-O2 -fPIC -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE" ./configure $H --disable-tcl --disable-readline >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  (cd src/openssl-3.5.5 && ./Configure linux-aarch64 no-shared no-tests no-docs no-apps no-module --prefix=$P --libdir=lib CC=$CC AR=$AR RANLIB=$RANLIB >/dev/null \
     && make -j$J build_libs >/dev/null && make install_dev >/dev/null)
fi

if [ ! -x "$W/pyroot/nexuspy/bin/python3.12" ]; then
  say "Python 3.12 para aarch64-linux-musl"
  rm -rf pybuild && mkdir pybuild
  (cd pybuild && CFLAGS="-O2" CPPFLAGS="-I$P/include" LDFLAGS="-L$P/lib" PKG_CONFIG_LIBDIR=$P/lib/pkgconfig \
     LIBFFI_CFLAGS="-I$P/include" LIBFFI_LIBS="-L$P/lib -lffi" LIBSQLITE3_CFLAGS="-I$P/include" LIBSQLITE3_LIBS="-L$P/lib -lsqlite3" \
     ZLIB_CFLAGS="-I$P/include" ZLIB_LIBS="-L$P/lib -lz" BZIP2_CFLAGS="-I$P/include" BZIP2_LIBS="-L$P/lib -lbz2" \
     LIBLZMA_CFLAGS="-I$P/include" LIBLZMA_LIBS="-L$P/lib -llzma" READELF=readelf \
     ../src/Python-3.12.3/configure --host=aarch64-linux-musl --build=x86_64-linux-gnu --with-build-python="$PY_HOST" \
       --prefix=/nexuspy --without-ensurepip --with-openssl=$P --disable-test-modules \
       ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no >/dev/null \
     && make -j$J >/dev/null 2>&1 && rm -rf $W/pyroot && make install DESTDIR=$W/pyroot >/dev/null 2>&1)
fi

say "Dependencias de Nexus-Office (ruedas musllinux aarch64 de PyPI)"
R=$W/pyroot/nexuspy
SP=$R/lib/python3.12/site-packages
grep -E '^[a-z]' "$ROOT/requirements.txt" | grep -v pytest | awk '{print $1}' > req-android.txt
"$PY_HOST" -m pip download -q -r req-android.txt --platform musllinux_1_2_aarch64 --platform musllinux_1_1_aarch64 \
  --python-version 3.12 --implementation cp --abi cp312 --abi abi3 --abi none --only-binary=:all: -d wheels
"$PY_HOST" -m pip install -q --no-deps --upgrade --target "$SP" --platform musllinux_1_2_aarch64 --platform musllinux_1_1_aarch64 \
  --python-version 3.12 --only-binary=:all: --no-compile wheels/*.whl
# Python 3.12 se identifica como linux-gnu tambien con musl: las extensiones musllinux se renombran
find "$SP" -name '*.cpython-312-aarch64-linux-musl.so' | while read -r f; do mv "$f" "${f%-linux-musl.so}-linux-gnu.so"; done
cp $W/libz.so.1 $R/lib/

say "Manejador de SIGSYS (Android: llamadas al sistema prohibidas -> ENOSYS en vez de matar el proceso, codigo 159)"
SIG=$ROOT/android-autonomo/native/nexus_sigsys.c
mkdir -p "$OUT/lib"
aarch64-linux-musl-cc -O2 -fPIC -shared -Wl,-soname,libnexussys.so -o "$OUT/lib/libnexussys.so" "$SIG"   # Python: --preload
st-cc -O2 -c -o $W/nexus_sigsys.o "$SIG"                                                             # llama-server: enlazado

say "llama-server estatico (3 variantes de CPU)"
L=$W/src/llama_cpp_python-$LLAMA_PY/vendor/llama.cpp
cat > tc-static.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $W/bin/st-cc)
set(CMAKE_CXX_COMPILER $W/bin/st-c++)
set(CMAKE_AR $W/bin/aarch64-linux-musl-ar)
set(CMAKE_RANLIB $W/bin/aarch64-linux-musl-ranlib)
set(CMAKE_EXE_LINKER_FLAGS "-static $W/nexus_sigsys.o")
EOF
for v in "base:generic:libllama-server.so" "dotprod:generic+v8_2a+dotprod+fullfp16:libllama-server-dotprod.so" \
         "i8mm:generic+v8_6a+dotprod+fullfp16+i8mm:libllama-server-i8mm.so"; do
  IFS=: read -r name mcpu lib <<<"$v"
  # tambien si le falta el manejador de SIGSYS (builds anteriores): solo se vuelve a enlazar
  if [ ! -x build-llama-$name/bin/llama-server ] || ! grep -q "Android no permite" build-llama-$name/bin/llama-server; then
    rm -f build-llama-$name/bin/llama-server
    NEXUS_MCPU=$mcpu cmake -S "$L" -B build-llama-$name -G Ninja -DCMAKE_TOOLCHAIN_FILE=$W/tc-static.cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-static $W/nexus_sigsys.o" \
      -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF \
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON >/dev/null
    NEXUS_MCPU=$mcpu cmake --build build-llama-$name --target llama-server -j$J >/dev/null
  fi
  aarch64-linux-gnu-strip -o "$OUT/lib/$lib" build-llama-$name/bin/llama-server
done

# llama-server con GPU (Vulkan/OpenCL) y NPU (Hexagon, con HEXAGON_SDK_ROOT): packaging/build_android_gpu.sh.
# Opcional: sin NDK la APK sale sin GPU. NDK: ANDROID_NDK_HOME (r26 o posterior).
NDK=${ANDROID_NDK_HOME:-$(ls -d "$ROOT"/.cache/android/ndk/* 2>/dev/null | tail -1)}
if [ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  say "llama-server con GPU (NDK $(basename "$NDK"))${HEXAGON_SDK_ROOT:+ y NPU Hexagon}"
  ANDROID_NDK_HOME=$NDK bash "$ROOT/packaging/build_android_gpu.sh" "$L" "$OUT/lib" "$W/gpu"
else
  say "Sin NDK (ANDROID_NDK_HOME): la APK saldra sin llama-server con GPU"
fi

say "Copiando a $OUT"
cp muslroot/usr/lib/aarch64-linux-musl/libc.so "$OUT/lib/libnexusld.so"
rm -rf "$OUT/py" && cp -a "$R" "$OUT/py"
cp src/llama_cpp_python-$LLAMA_PY/vendor/llama.cpp/LICENSE "$OUT/LICENSE-llama.cpp.txt"
"$OUT/lib/libnexusld.so" --version >/dev/null 2>&1 || true
echo "python 3.12.3 · llama.cpp (llama-cpp-python $LLAMA_PY) · musl 1.2.5 · OpenSSL 3.5.5 · SQLite 3.46.1" > "$OUT/VERSIONS.txt"
du -sh "$OUT"/lib/* "$OUT/py"

if [ "${1:-}" = "--probar" ]; then
  say "Prueba: Nexus-Office emulando ARM64 (qemu), como lo arranca la app"
  T=$(mktemp -d); ln -sf "$OUT/lib/libnexusld.so" "$OUT/py/lib/libc.musl-aarch64.so.1"
  PYTHONPATH=$ROOT qemu-aarch64-static "$OUT/lib/libnexusld.so" --library-path "$OUT/py/lib:$OUT/lib" "$OUT/py/bin/python3.12" \
    -m nexus.android_main "$T" "$OUT/lib" 8799 >"$T/log" 2>&1 &
  PID=$!
  for _ in $(seq 150); do curl -sf http://127.0.0.1:8799/api/ping && break; sleep 2; done; echo
  kill $PID; wait $PID 2>/dev/null || true
  rm -f "$OUT/py/lib/libc.musl-aarch64.so.1"
fi
