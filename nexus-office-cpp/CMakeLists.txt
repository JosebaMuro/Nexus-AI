cmake_minimum_required(VERSION 3.20)
# Nexus-Office 2.0.0-beta-cpp: la oficina para pymes reescrita en C++ (misma interfaz web, mismos datos).
project(NexusOfficeCpp VERSION 2.0.0 LANGUAGES C CXX)

set(NEXUS_VERSION "2.0.0-beta-cpp")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Recursos compartidos con la version Python (interfaz web, perfiles, agentes y paquetes de conocimiento):
# son los mismos ficheros, asi que las dos ediciones se ven y se comportan igual.
set(NEXUS_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../nexus-office/nexus" CACHE PATH "Carpeta nexus/ con web, office y agents/defaults")
# TLS para HTTPS (descargas de modelos, web_fetch): mbedtls (se descarga y compila, sin dependencias del sistema),
# openssl (el del sistema) o none (solo HTTP)
set(NEXUS_TLS "mbedtls" CACHE STRING "mbedtls | openssl | none")
option(NEXUS_STATIC_RUNTIME "Enlazar estatico el runtime de C++ (paquetes portables)" ON)
option(NEXUS_BUILD_TESTS "Compilar las pruebas" ON)
if(NEXUS_STATIC_RUNTIME)
  # MSVC: runtime estatico en todo (dependencias y pruebas incluidas) para no mezclar /MT y /MD
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(NEXUS_MBEDTLS_URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2" CACHE STRING "")
set(NEXUS_MBEDTLS_SHA256 "4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8" CACHE STRING "")

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ------------------------------------------------------------------ dependencias incluidas (third_party)
add_library(nx_sqlite3 STATIC ${TP}/sqlite3/sqlite3.c)
target_include_directories(nx_sqlite3 PUBLIC ${TP}/sqlite3)
target_compile_definitions(nx_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1
                           SQLITE_DQS=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION)

add_library(nx_miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(nx_miniz PUBLIC ${TP}/miniz)
target_compile_definitions(nx_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# bzip2 y xz (solo para leer grafos comprimidos .bz2 / .xz)
file(GLOB BZ2_SRC ${TP}/bzip2/*.c)
add_library(nx_bz2 STATIC ${BZ2_SRC})
target_include_directories(nx_bz2 PUBLIC ${TP}/bzip2)
target_compile_definitions(nx_bz2 PRIVATE BZ_NO_STDIO)
file(GLOB XZ_SRC ${TP}/xz/*.c)
add_library(nx_xz STATIC ${XZ_SRC})
target_include_directories(nx_xz PUBLIC ${TP}/xz)
target_compile_definitions(nx_xz PUBLIC XZ_USE_CRC64 XZ_DEC_ANY_CHECK)

file(GLOB PCRE2_SRC ${TP}/pcre2/src/*.c)
list(FILTER PCRE2_SRC EXCLUDE REGEX "pcre2_(jit_match|jit_misc|ucptables)\\.c$")
add_library(nx_pcre2 STATIC ${PCRE2_SRC})
target_include_directories(nx_pcre2 PUBLIC ${TP}/pcre2/src)
target_compile_definitions(nx_pcre2 PRIVATE HAVE_CONFIG_H SUPPORT_UNICODE)
target_compile_definitions(nx_pcre2 PUBLIC PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC)

set(NX_TLS_LIBS "")
set(NX_TLS_DEFS "")
if(NEXUS_TLS STREQUAL "mbedtls")
  include(FetchContent)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls URL ${NEXUS_MBEDTLS_URL} URL_HASH SHA256=${NEXUS_MBEDTLS_SHA256}
                       DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(mbedtls)
  set(NX_TLS_LIBS mbedtls mbedx509 mbedcrypto)
  set(NX_TLS_DEFS CPPHTTPLIB_MBEDTLS_SUPPORT NEXUS_HAVE_TLS)
elseif(NEXUS_TLS STREQUAL "openssl")
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  find_package(OpenSSL REQUIRED)
  set(NX_TLS_LIBS OpenSSL::SSL OpenSSL::Crypto)
  set(NX_TLS_DEFS CPPHTTPLIB_OPENSSL_SUPPORT NEXUS_HAVE_TLS)
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------------ nucleo de Nexus-Office
file(GLOB_RECURSE NEXUS_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER NEXUS_SRC EXCLUDE REGEX "/src/main\\.cpp$")
add_library(nexus_core STATIC ${NEXUS_SRC})
target_include_directories(nexus_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${TP}/httplib ${TP}/json ${TP}/ryml ${TP}/stb)
target_compile_definitions(nexus_core PUBLIC NEXUS_VERSION="${NEXUS_VERSION}" CPPHTTPLIB_THREAD_POOL_COUNT=16
                           ${NX_TLS_DEFS})
target_link_libraries(nexus_core PUBLIC nx_sqlite3 nx_miniz nx_pcre2 nx_bz2 nx_xz ${NX_TLS_LIBS} Threads::Threads)
if(WIN32)
  target_compile_definitions(nexus_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                             _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(nexus_core PUBLIC ws2_32 crypt32 bcrypt iphlpapi shell32 advapi32)
  if(MSVC)
    target_compile_options(nexus_core PUBLIC /utf-8 /bigobj /Zc:__cplusplus)
  else()
    target_compile_options(nexus_core PUBLIC -Wa,-mbig-obj)
  endif()
endif()
if(APPLE)
  target_link_libraries(nexus_core PUBLIC "-framework CoreFoundation" "-framework Security")
endif()
if(ANDROID)
  target_link_libraries(nexus_core PUBLIC log)
endif()

add_executable(nexus-office src/main.cpp)
target_link_libraries(nexus-office PRIVATE nexus_core)
# Android ARM64: el servidor sin instrucciones AdvSIMD propias (la IA va en llama-server, que si las usa). Asi
# funciona tambien con la traduccion de ARM64 de los emuladores y Chromebook x86 (ndk_translation no las tiene todas).
option(NEXUS_ANDROID_NOSIMD "Android ARM64 sin AdvSIMD en el servidor" ON)
if(ANDROID AND NEXUS_ANDROID_NOSIMD AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  foreach(t nx_sqlite3 nx_miniz nx_pcre2 nexus_core nexus-office)
    target_compile_options(${t} PRIVATE -march=armv8-a+nosimd)
  endforeach()
endif()
if(MINGW)
  target_link_options(nexus-office PRIVATE -municode)
endif()
if(NEXUS_STATIC_RUNTIME)
  if(MINGW)
    target_link_options(nexus-office PRIVATE -static)
  elseif(NOT APPLE AND NOT ANDROID)
    target_link_options(nexus-office PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

# Windows: variante sin ventana de consola para el servicio y el acceso directo (misma linea de ordenes)
if(WIN32)
  add_executable(nexus-office-w WIN32 src/main.cpp)
  target_link_libraries(nexus-office-w PRIVATE nexus_core)
  if(MSVC)
    target_link_options(nexus-office-w PRIVATE /ENTRY:wmainCRTStartup)
  else()
    target_link_options(nexus-office-w PRIVATE -municode $<$<BOOL:${NEXUS_STATIC_RUNTIME}>:-static>)
  endif()
  install(TARGETS nexus-office-w RUNTIME DESTINATION .)
endif()

# Recursos junto al ejecutable: la carpeta de compilacion queda lista para ejecutar (misma disposicion que el paquete)
add_custom_command(TARGET nexus-office POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${NEXUS_RESOURCES}/web $<TARGET_FILE_DIR:nexus-office>/nexus/web
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${NEXUS_RESOURCES}/office/perfiles $<TARGET_FILE_DIR:nexus-office>/nexus/office/perfiles
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${NEXUS_RESOURCES}/office/agentes $<TARGET_FILE_DIR:nexus-office>/nexus/office/agentes
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${NEXUS_RESOURCES}/office/conocimiento $<TARGET_FILE_DIR:nexus-office>/nexus/office/conocimiento
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${NEXUS_RESOURCES}/agents/defaults $<TARGET_FILE_DIR:nexus-office>/nexus/agents/defaults
  COMMAND ${CMAKE_COMMAND} -E copy ${NEXUS_RESOURCES}/remote/pair.html $<TARGET_FILE_DIR:nexus-office>/nexus/remote/pair.html
  COMMAND ${CMAKE_COMMAND} -E copy ${TP}/certs/cacert.pem $<TARGET_FILE_DIR:nexus-office>/nexus/certs/cacert.pem
  VERBATIM)

install(TARGETS nexus-office RUNTIME DESTINATION .)
install(DIRECTORY ${NEXUS_RESOURCES}/web DESTINATION nexus)
install(DIRECTORY ${NEXUS_RESOURCES}/office/perfiles ${NEXUS_RESOURCES}/office/agentes ${NEXUS_RESOURCES}/office/conocimiento
        DESTINATION nexus/office)
install(DIRECTORY ${NEXUS_RESOURCES}/agents/defaults DESTINATION nexus/agents)
install(FILES ${NEXUS_RESOURCES}/remote/pair.html DESTINATION nexus/remote)
install(FILES ${TP}/certs/cacert.pem DESTINATION nexus/certs)

if(NEXUS_BUILD_TESTS)
  enable_testing()
  file(GLOB TEST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(nexus-tests ${TEST_SRC})
  target_link_libraries(nexus-tests PRIVATE nexus_core)
  add_test(NAME nexus-tests COMMAND nexus-tests WORKING_DIRECTORY $<TARGET_FILE_DIR:nexus-office>)
  add_dependencies(nexus-tests nexus-office)
endif()
