#!/usr/bin/env bash
# stable-diffusion.cpp para Android ARM64 (NDK): sd-cli y sd-server como "bibliotecas" de la APK (libsd-*.so), porque
# en Android solo se pueden ejecutar los programas que viven en lib/arm64-v8a. Upstream no publica binarios para Android.
#   libsd-cli.so / libsd-server.so                 CPU base (ARMv8.0)
#   libsd-cli-dotprod.so / libsd-server-dotprod.so ARMv8.2 + dotprod
#   libsd-cli-i8mm.so / libsd-server-i8mm.so       ARMv8.6 + i8mm (Snapdragon 8 Gen 1 y posteriores)
#   libsd-cli-gpu.so / libsd-server-gpu.so         OpenCL de Adreno (Snapdragon): usa Q4_0; el movil aporta libOpenCL.so
# Los de GPU se omiten si fallan (la APK sigue con CPU); sd-server se omite si el commit de sd.cpp no lo trae.
#
#   bash packaging/android/build_sd.sh <salida lib/> [trabajo] [ref-de-sd.cpp]
# Variables: ANDROID_NDK_HOME (r26 o posterior), NEXUS_GPU_HEADERS (carpeta con CL/; por defecto /usr/include)
set -euo pipefail
OUT=$(realpath -m "$1"); W=$(realpath -m "${2:-$PWD/build-sd}"); REF=${3:-master}
NDK=${ANDROID_NDK_HOME:?falta ANDROID_NDK_HOME}
HDR=${NEXUS_GPU_HEADERS:-/usr/include}
J=$(nproc); BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$W" "$OUT"
if [ ! -d "$W/sd/.git" ]; then
  git clone --recursive --depth 1 ${REF:+--branch "$REF"} https://github.com/leejet/stable-diffusion.cpp "$W/sd" 2>/dev/null \
    || { git clone --recursive https://github.com/leejet/stable-diffusion.cpp "$W/sd" && git -C "$W/sd" checkout "$REF" && git -C "$W/sd" submodule update --init --recursive; }
fi
echo "stable-diffusion.cpp $(git -C "$W/sd" rev-parse --short HEAD)" > "$OUT/VERSIONS-sd.txt"

COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static
  -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF
  -DSD_BUILD_EXAMPLES=ON -DSD_WEBP=OFF -DSD_WEBM=OFF)

# programas que genera cada compilacion (nombres cambian entre versiones: se buscan por lo que existe)
targets() { cmake --build "$1" --target help | grep -oE '^\.\.\. (sd-cli|sd-server|sd)\b' | awk '{print $2}' | sort -u; }
pick() {   # pick <build> <nombre-destino> <candidatos...>
  local b=$1 dest=$2; shift 2
  for n in "$@"; do
    f=$(find "$b" -type f -name "$n" -perm -u+x | head -n1)
    [ -n "$f" ] && { "$BIN/llvm-strip" -o "$OUT/$dest" "$f"; echo "  $dest <- $n"; return 0; }
  done
  return 1
}
build() {   # build <nombre> <sufijo> <flags...>
  local name=$1 suf=$2; shift 2
  local b=$W/build-$name
  cmake -S "$W/sd" -B "$b" "${COMMON[@]}" "$@" >/dev/null
  local t; t=$(targets "$b" | tr '\n' ' '); echo "[$name] objetivos: $t"
  for x in $t; do cmake --build "$b" --target "$x" -j"$J" >/dev/null; done
  pick "$b" "libsd-cli$suf.so" sd-cli sd || { echo "[$name] sin sd-cli"; return 1; }
  pick "$b" "libsd-server$suf.so" sd-server || echo "[$name] este sd.cpp no trae sd-server: se usara sd-cli"
}

# CPU: mismas bases que llama.cpp en packaging/build_android_runtime.sh
build base "" -DCMAKE_C_FLAGS="-march=armv8-a" -DCMAKE_CXX_FLAGS="-march=armv8-a"
build dotprod -dotprod -DCMAKE_C_FLAGS="-march=armv8.2-a+dotprod+fp16" -DCMAKE_CXX_FLAGS="-march=armv8.2-a+dotprod+fp16" \
  || echo "AVISO: sin variante dotprod"
build i8mm -i8mm -DCMAKE_C_FLAGS="-march=armv8.6-a+dotprod+fp16+i8mm" -DCMAKE_CXX_FLAGS="-march=armv8.6-a+dotprod+fp16+i8mm" \
  || echo "AVISO: sin variante i8mm"

# GPU Adreno (OpenCL). libOpenCL.so de enlace: en el movil se carga la del fabricante (/vendor/lib64)
mkdir -p "$W/inc" "$W/ocl"; [ -d "$W/inc/CL" ] || cp -r "$HDR/CL" "$W/inc/"
python3 - "$W" <<'PYEOF'
import pathlib, re, sys
w = pathlib.Path(sys.argv[1]); names = set()
for h in ("cl.h", "cl_gl.h", "cl_ext.h", "cl_egl.h"):
    names |= set(re.findall(r"CL_API_CALL\s+(cl\w+)\s*\(", (w / "inc/CL" / h).read_text()))
(w / "ocl/stub.c").write_text("".join(f"void {n}(void){{}}\n" for n in sorted(names)))
PYEOF
"$BIN/aarch64-linux-android28-clang" -shared -fPIC -Wl,-soname,libOpenCL.so -o "$W/ocl/libOpenCL.so" "$W/ocl/stub.c"
build gpu -gpu -DSD_OPENCL=ON -DGGML_OPENCL=ON -DGGML_OPENCL_EMBED_KERNELS=ON -DGGML_OPENCL_USE_ADRENO_KERNELS=ON \
  -DOpenCL_INCLUDE_DIR="$W/inc" -DOpenCL_LIBRARY="$W/ocl/libOpenCL.so" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_C_FLAGS="-march=armv8.2-a+dotprod+fp16" -DCMAKE_CXX_FLAGS="-march=armv8.2-a+dotprod+fp16" \
  || echo "AVISO: sin variante GPU (OpenCL); la APK usara CPU"
test -f "$OUT/libsd-cli.so"
ls -la "$OUT"/libsd-*.so
