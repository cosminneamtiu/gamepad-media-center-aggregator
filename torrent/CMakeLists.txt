# GMCA — on-device torrent streaming engine + PoC.
#
# This builds INDEPENDENTLY of the borealis app so the engine can be iterated and
# the PoC tested without configuring/compiling the whole media center:
#
#     cmake -B build-torrent torrent -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-torrent -j
#     ./build-torrent/torrent_poc <magnet|infohash|file.torrent> [fileIdx] [opts]
#
# It is ALSO included from the root CMakeLists via `add_subdirectory(torrent)`
# when -DENABLE_TORRENT=ON (OFF by default), so the module never touches the
# Switch/Vita/PS4/desktop app builds unless explicitly requested.
#
# Sources live under ../app/include/torrent and ../app/src/torrent (repo layout).

cmake_minimum_required(VERSION 3.18)

# Only declare a standalone project when configured on our own (not as a subdir).
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gmca_torrent CXX)
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif ()
endif ()

set(TORRENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TORRENT_INCLUDE ${TORRENT_ROOT}/app/include)
set(TORRENT_SRC ${TORRENT_ROOT}/app/src/torrent)
set(TORRENT_VENDOR ${TORRENT_ROOT}/app/vendor)

set(TORRENT_ENGINE_SOURCES
    ${TORRENT_SRC}/bencode.cpp
    ${TORRENT_SRC}/sha1.cpp
    ${TORRENT_SRC}/crypto.cpp
    ${TORRENT_SRC}/mse.cpp
    ${TORRENT_SRC}/log.cpp
    ${TORRENT_SRC}/util.cpp
    ${TORRENT_SRC}/socket.cpp
    ${TORRENT_SRC}/transport.cpp
    ${TORRENT_SRC}/utp.cpp
    ${TORRENT_SRC}/metadata.cpp
    ${TORRENT_SRC}/http_client.cpp
    ${TORRENT_SRC}/storage.cpp
    ${TORRENT_SRC}/piece_picker.cpp
    ${TORRENT_SRC}/peer.cpp
    ${TORRENT_SRC}/tracker.cpp
    ${TORRENT_SRC}/http_server.cpp
    ${TORRENT_SRC}/engine.cpp
)

# --- libutp (µTP / BEP-29 transport), vendored under app/vendor/libutp ---------
# Reference C++ implementation of µTP (LEDBAT over UDP), MIT-licensed, no Boost,
# no TLS — pure transport. Built as its OWN static lib so its third-party warnings
# and build flags (matching upstream's Makefile: -DPOSIX, -fno-exceptions,
# -fno-rtti) stay isolated from the engine. It only ever talks to our UDP shim via
# the callbacks wired in app/src/torrent/utp.cpp. The vendored source is unmodified
# (see app/vendor/libutp/VENDOR.txt for the upstream commit).
set(LIBUTP_DIR ${TORRENT_VENDOR}/libutp)
set(LIBUTP_SOURCES
    ${LIBUTP_DIR}/utp_internal.cpp
    ${LIBUTP_DIR}/utp_utils.cpp
    ${LIBUTP_DIR}/utp_hash.cpp
    ${LIBUTP_DIR}/utp_callbacks.cpp
    ${LIBUTP_DIR}/utp_api.cpp
    ${LIBUTP_DIR}/utp_packedsockaddr.cpp
)
add_library(utp STATIC ${LIBUTP_SOURCES})
target_include_directories(utp PUBLIC ${LIBUTP_DIR})
target_compile_features(utp PRIVATE cxx_std_11)
if (NOT WIN32)
    # POSIX selects the BSD-sockets path in utp_types.h (SOCKADDR_STORAGE etc.).
    # Every GMCA target is POSIX/BSD-sockets (desktop, libnx bsd, openorbis, and
    # vitasdk's newlib BSD layer) — Windows would need a winsock branch (unused).
    target_compile_definitions(utp PRIVATE POSIX)
endif ()
# libutp uses neither exceptions nor RTTI (matches upstream); -w silences the
# vendored code's warnings without touching the engine's own warning policy.
target_compile_options(utp PRIVATE -fno-exceptions -fno-rtti -w)
if (PLATFORM_PSV)
    # vitasdk newlib lacks the POSIX IN6_IS_ADDR_V4MAPPED macro libutp expects;
    # force-include a tiny compat shim so the vendored source builds unmodified.
    target_compile_options(utp PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/libutp_vita_compat.h)
endif ()

# HTTP (trackers/web seeds) uses libcurl.
find_package(Threads REQUIRED)

# On the console cross-compiles, curl/mbedtls come from the switch-*/vita-*/ps4-…
# portlibs and are located via pkg-config against the toolchain sysroot — the same
# mechanism the app itself uses for its non-desktop deps (root CMakeLists `else()`
# branch, pkg_search_module). find_package(CURL) / find_package(OpenSSL) are the
# desktop paths and do not apply there. On every console we also flip the SHA-1
# backend to mbedtls (mbedcrypto, already pulled in by the curl portlib) and skip
# the CLI PoC, which is a desktop driver with no meaning inside a .nro/.vpk/.pkg:
#   - Switch : libnx `bsd` sockets   (socket.cpp #if __SWITCH__)   [verified]
#   - Vita   : SceNet rewrite         (socket.cpp #elif __vita__)   [verified compile]
#   - PS4    : openorbis FreeBSD BSD   (socket.cpp POSIX #else)      [verified compile]
if (PLATFORM_SWITCH OR PLATFORM_PSV OR PLATFORM_PS4)
    # Console: mbedtls SHA-1 (mbedcrypto, already pulled in by the curl portlib) and
    # no CLI PoC (a desktop driver, meaningless inside a .nro/.vpk/.pkg).
    set(TORRENT_SHA1_MBEDTLS ON CACHE BOOL "" FORCE)
    if (NOT DEFINED TORRENT_POC)
        set(TORRENT_POC OFF CACHE BOOL "" FORCE)
    endif ()
    if (PLATFORM_PS4)
        # openorbis ships curl as a plain lib+header WITHOUT a libcurl.pc, so
        # pkg-config cannot see it. find_package(CURL) locates it against the
        # toolchain sysroot (CMAKE_FIND_ROOT_PATH), exactly as the app root does.
        find_package(CURL REQUIRED)
    else ()
        # switch-curl / vita curl ship a libcurl.pc found via the sysroot pkg-config.
        find_package(PkgConfig REQUIRED)
        pkg_search_module(CURL REQUIRED libcurl)
    endif ()
else ()
    find_package(CURL REQUIRED)
endif ()

add_library(torrent_engine STATIC ${TORRENT_ENGINE_SOURCES})
target_include_directories(torrent_engine PUBLIC ${TORRENT_INCLUDE})
target_compile_features(torrent_engine PUBLIC cxx_std_17)
target_link_libraries(torrent_engine PUBLIC Threads::Threads)
# µTP transport: app/src/torrent/utp.cpp includes libutp's utp.h and links libutp.
# PRIVATE — libutp is an implementation detail hidden behind PeerTransport.
target_link_libraries(torrent_engine PRIVATE utp)
# Switch/Vita curl comes from pkg-config (CURL_* vars); PS4 (openorbis) and desktop
# come from find_package(CURL), which exports the CURL::libcurl imported target.
if (PLATFORM_SWITCH OR PLATFORM_PSV)
    target_include_directories(torrent_engine PUBLIC ${CURL_INCLUDE_DIRS})
    target_link_directories(torrent_engine PUBLIC ${CURL_LIBRARY_DIRS})
    target_link_libraries(torrent_engine PUBLIC ${CURL_LIBRARIES})
else ()
    target_link_libraries(torrent_engine PUBLIC CURL::libcurl)
endif ()

# SHA-1 backend: default is CommonCrypto on Apple / a bundled fallback elsewhere.
# Flip these on to use a system crypto lib (desktop) or the console seam:
#   -DTORRENT_SHA1_OPENSSL=ON   (links OpenSSL::Crypto)
#   -DTORRENT_SHA1_MBEDTLS=ON   (console: mbedtls; auto-ON under PLATFORM_SWITCH)
option(TORRENT_SHA1_OPENSSL "Use OpenSSL libcrypto for SHA-1" OFF)
option(TORRENT_SHA1_MBEDTLS "Use mbedtls for SHA-1 (console seam)" OFF)
if (TORRENT_SHA1_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(torrent_engine PRIVATE TORRENT_SHA1_OPENSSL)
    target_link_libraries(torrent_engine PUBLIC OpenSSL::Crypto)
elseif (TORRENT_SHA1_MBEDTLS)
    target_compile_definitions(torrent_engine PRIVATE TORRENT_SHA1_MBEDTLS)
    # mbedcrypto provides mbedtls_sha1 (mbedtls 3.x API). It is already linked into
    # the app via switch-curl; name it explicitly so a standalone static-lib link
    # (or a test harness) resolves the symbol too.
    target_link_libraries(torrent_engine PUBLIC mbedcrypto)
endif ()

# The PoC executable. Built when standalone, or when the root asks for it.
option(TORRENT_POC "Build the torrent_poc executable" ON)
if (TORRENT_POC)
    add_executable(torrent_poc ${CMAKE_CURRENT_SOURCE_DIR}/poc.cpp)
    target_link_libraries(torrent_poc PRIVATE torrent_engine)
    target_compile_features(torrent_poc PRIVATE cxx_std_17)
endif ()
