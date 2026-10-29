#!/usr/bin/env bash
# llama-server con GPU y NPU para la app autonoma (Android ARM64), enlazado con la libc de Android (NDK) para poder
# cargar los drivers del movil. Backends cargados al arrancar (GGML_BACKEND_DL): si el movil no tiene un driver, ese
# backend se salta y Nexus-Office sigue con los demas.
#   - Vulkan: Mali, Immortalis, Xclipse, PowerVR...
#   - OpenCL: Adreno (kernels de Qualcomm)
#   - Hexagon (NPU de Snapdragon 8 Gen 2 o posterior, HTP v73/v75/v79/v81): solo con el Hexagon SDK
#   - CPU en todas las variantes de ARM
#
#   bash packaging/build_android_gpu.sh <llama.cpp> <salida lib/> [trabajo]
# Variables: ANDROID_NDK_HOME (r26 o posterior; r28b con Hexagon), HEXAGON_SDK_ROOT (opcional, activa la NPU),
#   NEXUS_GPU_HEADERS (CL/, vulkan/, vk_video/, spirv/; por defecto /usr/include: opencl-c-headers, libvulkan-dev,
#   spirv-headers), NEXUS_SPIRV_CMAKE (por defecto /usr/share/cmake/SPIRV-Headers).
# La imagen ghcr.io/snapdragon-toolchain/arm64-android:v0.7 trae el NDK r28b y el Hexagon SDK 6.6
# (lo usa .github/workflows/android-npu.yml).
set -euo pipefail
L=$(realpath "$1"); OUT=$(realpath -m "$2"); W=$(realpath -m "${3:-$PWD/build-android-gpu}")
NDK=${ANDROID_NDK_HOME:?falta ANDROID_NDK_HOME}
HDR=${NEXUS_GPU_HEADERS:-/usr/include}
J=$(nproc)
SR=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot
mkdir -p "$W/inc" "$W/ocl" "$OUT"
for d in CL vulkan vk_video spirv; do [ -d "$W/inc/$d" ] || cp -r "$HDR/$d" "$W/inc/"; done
# libOpenCL.so de enlace (solo los nombres): en el movil se carga el del fabricante (/vendor/lib64)
python3 - "$W" <<'PYEOF'
import pathlib, re, sys
w = pathlib.Path(sys.argv[1]); names = set()
for h in ("cl.h", "cl_gl.h", "cl_ext.h", "cl_egl.h"):
    names |= set(re.findall(r"CL_API_CALL\s+(cl\w+)\s*\(", (w / "inc/CL" / h).read_text()))
(w / "ocl/stub.c").write_text("".join(f"void {n}(void){{}}\n" for n in sorted(names)))
PYEOF
"$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang -shared -fPIC -Wl,-soname,libOpenCL.so \
  -o "$W/ocl/libOpenCL.so" "$W/ocl/stub.c"
NPU=()
if [ -n "${HEXAGON_SDK_ROOT:-}" ] && [ -d "$HEXAGON_SDK_ROOT" ]; then
  # el modulo del host (libggml-hexagon.so) abre libcdsprpc.so del movil con dlopen: sin ella no hay NPU y sigue igual
  NPU=(-DGGML_HEXAGON=ON -DHEXAGON_SDK_ROOT="$HEXAGON_SDK_ROOT" -DPREBUILT_LIB_DIR=android_aarch64)
  [ -n "${HEXAGON_TOOLS_ROOT:-}" ] && NPU+=(-DHEXAGON_TOOLS_ROOT="$HEXAGON_TOOLS_ROOT")
fi
cmake -S "$L" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_NATIVE=OFF \
  -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_VULKAN=ON -DGGML_OPENCL=ON -DGGML_OPENCL_EMBED_KERNELS=ON \
  -DGGML_OPENCL_USE_ADRENO_KERNELS=ON -DOpenCL_INCLUDE_DIR="$W/inc" -DOpenCL_LIBRARY="$W/ocl/libOpenCL.so" \
  -DVulkan_INCLUDE_DIR="$W/inc" -DVulkan_LIBRARY="$SR/usr/lib/aarch64-linux-android/28/libvulkan.so" \
  -DVulkan_GLSLC_EXECUTABLE="$NDK/shader-tools/linux-x86_64/glslc" \
  -DSPIRV-Headers_DIR="${NEXUS_SPIRV_CMAKE:-/usr/share/cmake/SPIRV-Headers}" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON "${NPU[@]}" >/dev/null
cmake --build "$W/build" -j"$J" >/dev/null
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
B=$W/build/bin
rm -f "$OUT"/libggml*.so "$OUT/libOpenCL.so"
$STRIP -o "$OUT/libllama-server-gpu.so" "$B/llama-server"
for f in "$B/libllama-server-impl.so" "$B/libllama-common.so" "$B/libmtmd.so" "$B/libllama.so" "$B"/libggml*.so \
         "$SR/usr/lib/aarch64-linux-android/libc++_shared.so"; do
  case $(basename "$f") in libggml-htp-*) continue ;; esac
  $STRIP -o "$OUT/$(basename "$f")" "$f"
done
if [ ${#NPU[@]} -gt 0 ]; then
  # programas de la NPU (se ejecutan en el DSP; FastRPC los busca en ADSP_LIBRARY_PATH): sin tocar
  find "$W/build" -name 'libggml-htp-v*.so' -exec cp {} "$OUT/" \;
  ls "$OUT"/libggml-hexagon.so "$OUT"/libggml-htp-v*.so >/dev/null
fi
