cmake_minimum_required(VERSION 3.16)
project(AirMirror VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# AirMirror - an AirPlay mirroring receiver for Windows that renders the
# incoming iPhone/iPad screen inside a photorealistic device frame.
#
# Core protocol implementation is UxPlay's libairplay (GPLv3). We do NOT use
# UxPlay's GStreamer renderer; video/audio are decoded with FFmpeg and drawn
# with Direct3D 11 + DirectComposition, which keeps the runtime small and the
# pipeline zero-copy.
# ---------------------------------------------------------------------------

if(NOT WIN32)
  message(FATAL_ERROR "AirMirror targets Windows only.")
endif()

enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(UXPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/UxPlay")
if(NOT EXISTS "${UXPLAY_DIR}/lib/raop.c")
  message(FATAL_ERROR
    "UxPlay sources not found at ${UXPLAY_DIR}.\n"
    "Run: git clone --depth 1 https://github.com/FDH2/UxPlay.git vendor/UxPlay")
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenSSL 1.1.1 REQUIRED)
pkg_search_module(PLIST REQUIRED libplist-2.0)

# Prefer the stripped FFmpeg from scripts/build-ffmpeg-mini.sh when present:
# it is a few MB instead of the ~115 MB the full MSYS2 package drags in.
set(FFMPEG_MINI "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg-mini")
if(EXISTS "${FFMPEG_MINI}/lib/pkgconfig")
  message(STATUS "Using minimal FFmpeg from ${FFMPEG_MINI}")
  set(ENV{PKG_CONFIG_PATH} "${FFMPEG_MINI}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  set(FFMPEG_RUNTIME_DIR "${FFMPEG_MINI}/bin" CACHE INTERNAL "")
else()
  message(STATUS "Using system FFmpeg (run scripts/build-ffmpeg-mini.sh for a small build)")
endif()

pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswresample libswscale)

# libplist >= 2.1 / 2.3 changed API details that UxPlay guards with these.
pkg_check_modules(PLIST_21 libplist-2.0>=2.1.0)
pkg_check_modules(PLIST_23 libplist-2.0>=2.3.0)

# ---------------------------------------------------------------------------
# Bundled third-party pieces of UxPlay
# ---------------------------------------------------------------------------
add_library(llhttp STATIC
  ${UXPLAY_DIR}/lib/llhttp/api.c
  ${UXPLAY_DIR}/lib/llhttp/http.c
  ${UXPLAY_DIR}/lib/llhttp/llhttp.c)
target_include_directories(llhttp PUBLIC ${UXPLAY_DIR}/lib/llhttp)

add_library(playfair STATIC
  ${UXPLAY_DIR}/lib/playfair/playfair.c
  ${UXPLAY_DIR}/lib/playfair/omg_hax.c
  ${UXPLAY_DIR}/lib/playfair/hand_garble.c
  ${UXPLAY_DIR}/lib/playfair/modified_md5.c
  ${UXPLAY_DIR}/lib/playfair/sap_hash.c)
target_include_directories(playfair PUBLIC ${UXPLAY_DIR}/lib/playfair)

# Built-in mDNS responder: this is what lets us advertise _airplay._tcp and
# _raop._tcp with no Apple Bonjour / mDNSResponder.exe installed.
add_library(dnssd STATIC
  ${UXPLAY_DIR}/lib/mdnsd/mdnsd.c
  ${UXPLAY_DIR}/lib/mdnsd/dnssd_mdnsd.c)
target_include_directories(dnssd PUBLIC ${UXPLAY_DIR}/lib/mdnsd ${UXPLAY_DIR}/lib)
target_compile_definitions(dnssd PRIVATE WIN32 _WIN32_WINNT=0x0601)
target_link_libraries(dnssd PUBLIC ws2_32 iphlpapi)

# ---------------------------------------------------------------------------
# libairplay: UxPlay's protocol core (RTSP/RAOP/FairPlay/mirroring)
# ---------------------------------------------------------------------------
set(AIRPLAY_SRC
  ${UXPLAY_DIR}/lib/airplay_video.c
  ${UXPLAY_DIR}/lib/byteutils.c
  ${UXPLAY_DIR}/lib/compat.c
  ${UXPLAY_DIR}/lib/crypto.c
  ${UXPLAY_DIR}/lib/dnssd.c
  ${UXPLAY_DIR}/lib/fairplay_playfair.c
  ${UXPLAY_DIR}/lib/http_request.c
  ${UXPLAY_DIR}/lib/http_response.c
  ${UXPLAY_DIR}/lib/httpd.c
  ${UXPLAY_DIR}/lib/logger.c
  ${UXPLAY_DIR}/lib/mirror_buffer.c
  ${UXPLAY_DIR}/lib/netutils.c
  ${UXPLAY_DIR}/lib/pairing.c
  ${UXPLAY_DIR}/lib/raop.c
  ${UXPLAY_DIR}/lib/raop_buffer.c
  ${UXPLAY_DIR}/lib/raop_ntp.c
  ${UXPLAY_DIR}/lib/raop_rtp.c
  ${UXPLAY_DIR}/lib/raop_rtp_mirror.c
  ${UXPLAY_DIR}/lib/srp.c
  ${UXPLAY_DIR}/lib/utils.c)

add_library(airplay STATIC ${AIRPLAY_SRC})
target_include_directories(airplay PUBLIC
  ${UXPLAY_DIR}/lib
  ${UXPLAY_DIR}/lib/playfair
  ${UXPLAY_DIR}/lib/llhttp
  ${PLIST_INCLUDE_DIRS})
target_compile_definitions(airplay PUBLIC
  WIN32 _WIN32_WINNT=0x0601 NOHOLD
  OPENSSL_API_COMPAT=0x10101000L)
if(PLIST_21_FOUND)
  target_compile_definitions(airplay PUBLIC PLIST_210)
endif()
if(PLIST_23_FOUND)
  target_compile_definitions(airplay PUBLIC PLIST_230)
endif()
target_link_directories(airplay PUBLIC ${PLIST_LIBRARY_DIRS})
target_link_libraries(airplay PUBLIC
  playfair dnssd llhttp
  ${PLIST_LIBRARIES}
  OpenSSL::Crypto
  ws2_32 wsock32 iphlpapi)

# ---------------------------------------------------------------------------
# AirMirror application
# ---------------------------------------------------------------------------

# App icon + version info (installer/icon.ico, generated from logo.png).
set(AIRMIRROR_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/installer/icon.ico")
configure_file(src/resource.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resource.rc @ONLY)

add_executable(airmirror WIN32
  src/main.cpp
  src/app.cpp
  src/airplay_server.cpp
  src/device_db.cpp
  src/video_decoder.cpp
  src/audio_player.cpp
  src/renderer.cpp
  src/skin.cpp
  src/gpu.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/resource.rc)

target_include_directories(airmirror PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FFMPEG_INCLUDE_DIRS})
target_link_directories(airmirror PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_compile_definitions(airmirror PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)

target_link_libraries(airmirror PRIVATE
  airplay
  ${FFMPEG_LIBRARIES}
  d3d11 dxgi dcomp d3dcompiler dwmapi windowscodecs
  ole32 oleaut32 uuid avrt winmm shell32 user32 gdi32)

# Ship a self-contained exe: no libstdc++/libgcc/winpthread DLLs required.
# -municode selects the wWinMain entry point; the static flags keep the exe
# free of libstdc++/libgcc/winpthread DLL dependencies.
#
# Note: libwinpthread.a's whole-archive objects carry their own embedded
# VS_VERSION_INFO (id 1) that wins over resource.rc's during ld's resource
# merge regardless of link-line order (a binutils quirk) - airmirror.exe's
# Properties > Details tab shows MinGW's strings, not ours. The app icon
# (also from resource.rc) is unaffected and embeds correctly.
target_link_options(airmirror PRIVATE
  -municode
  -static-libgcc -static-libstdc++
  -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic
  -mwindows)

# Put the private FFmpeg next to the exe so a dev build runs without PATH
# fiddling (Windows searches the executable's own directory first).
if(FFMPEG_RUNTIME_DIR)
  file(GLOB FFMPEG_DLLS "${FFMPEG_RUNTIME_DIR}/*.dll")
  foreach(dll ${FFMPEG_DLLS})
    add_custom_command(TARGET airmirror POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:airmirror>")
  endforeach()
endif()

# Device skins live beside the exe so a dev build finds them too.
add_custom_command(TARGET airmirror POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:airmirror>/assets")

install(TARGETS airmirror RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)
