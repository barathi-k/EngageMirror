cmake_minimum_required(VERSION 3.16)

# macOS: Apple Silicon, Monterey or later. Must precede project(); ignored
# elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")

project(EngageMirror VERSION 1.0.2 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# EngageMirror - an AirPlay mirroring receiver for Windows and macOS that renders
# the incoming iPhone/iPad screen inside a photorealistic device frame.
#
# Core protocol implementation is UxPlay's libairplay (GPLv3). We do NOT use
# UxPlay's GStreamer renderer; video/audio are decoded with FFmpeg and drawn
# with Direct3D 11 + DirectComposition (Windows) or Metal (macOS), which keeps
# the runtime small and the pipeline zero-copy.
# ---------------------------------------------------------------------------

if(WIN32)
  enable_language(RC)
  set(PLATFORM_DEFS WIN32 _WIN32_WINNT=0x0601)
elseif(APPLE)
  enable_language(OBJCXX)
  set(PLATFORM_DEFS "")
else()
  message(FATAL_ERROR "EngageMirror targets Windows and macOS.")
endif()

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

if(APPLE)
  # Everything comes from scripts/build-deps-macos.sh as static archives, and
  # only from there: PKG_CONFIG_LIBDIR replaces the default search path, so a
  # Homebrew FFmpeg or OpenSSL can never be picked up by accident.
  set(MACOS_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/macos-deps")
  if(NOT EXISTS "${MACOS_DEPS}/lib/libavcodec.a")
    message(FATAL_ERROR "Run scripts/build-deps-macos.sh first.")
  endif()
  set(ENV{PKG_CONFIG_LIBDIR} "${MACOS_DEPS}/lib/pkgconfig")
  set(OPENSSL_ROOT_DIR "${MACOS_DEPS}")
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenSSL 1.1.1 REQUIRED)
pkg_search_module(PLIST REQUIRED libplist-2.0)

# Prefer the stripped FFmpeg from scripts/build-ffmpeg-mini.sh when present:
# it is a few MB instead of the ~115 MB the full MSYS2 package drags in.
set(FFMPEG_MINI "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg-mini")
if(APPLE)
  # Built by scripts/build-deps-macos.sh, found through PKG_CONFIG_LIBDIR.
elseif(EXISTS "${FFMPEG_MINI}/lib/pkgconfig")
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

if(WIN32)
  # Built-in mDNS responder: this is what lets us advertise _airplay._tcp and
  # _raop._tcp with no Apple Bonjour / mDNSResponder.exe installed.
  add_library(dnssd STATIC
    ${UXPLAY_DIR}/lib/mdnsd/mdnsd.c
    ${UXPLAY_DIR}/lib/mdnsd/dnssd_mdnsd.c)
  target_include_directories(dnssd PUBLIC ${UXPLAY_DIR}/lib/mdnsd ${UXPLAY_DIR}/lib)
  target_compile_definitions(dnssd PRIVATE WIN32 _WIN32_WINNT=0x0601)
  target_link_libraries(dnssd PUBLIC ws2_32 iphlpapi)
else()
  # macOS ships mDNSResponder; UxPlay's dns_sd backend talks to it through
  # <dns_sd.h>, which is part of libSystem.
  add_library(dnssd STATIC ${UXPLAY_DIR}/lib/dns_sd/dns_sd.c)
  target_include_directories(dnssd PUBLIC ${UXPLAY_DIR}/lib)
endif()

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
  ${PLATFORM_DEFS} NOHOLD
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
  OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(airplay PUBLIC ws2_32 wsock32 iphlpapi)
endif()

set(ENGAGEMIRROR_SHARED_SRC
  src/app.cpp
  src/airplay_server.cpp
  src/device_db.cpp
  src/video_decoder.cpp
  src/audio_player.cpp
  src/renderer.cpp
  src/skin.cpp)

if(APPLE)
  # ---------------------------------------------------------------------------
  # EngageMirror.app
  # ---------------------------------------------------------------------------
  set(ENGAGEMIRROR_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/macos/EngageMirror.icns")
  set_source_files_properties(${ENGAGEMIRROR_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

  add_executable(engagemirror MACOSX_BUNDLE
    ${ENGAGEMIRROR_SHARED_SRC}
    src/main_mac.mm
    src/app_mac.mm
    src/gpu_mac.mm
    src/renderer_mac.mm
    ${ENGAGEMIRROR_ICNS})

  # The shared sources include Metal/CoreVideo headers through gpu.h.
  set_source_files_properties(${ENGAGEMIRROR_SHARED_SRC} PROPERTIES LANGUAGE OBJCXX)
  target_compile_options(engagemirror PRIVATE -fobjc-arc)
  target_include_directories(engagemirror PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(engagemirror PRIVATE ${FFMPEG_STATIC_LIBRARY_DIRS})
  target_link_libraries(engagemirror PRIVATE
    airplay
    # Only the -l part: CMake splits pkg-config's "-framework X" pairs, so the
    # frameworks FFmpeg needs are named below instead.
    ${FFMPEG_STATIC_LIBRARIES}
    "-framework AppKit" "-framework Metal" "-framework QuartzCore"
    "-framework CoreVideo" "-framework CoreMedia" "-framework VideoToolbox"
    "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreText"
    "-framework CoreGraphics" "-framework ImageIO" "-framework CoreFoundation"
    "-framework CoreServices" "-framework Network")

  set_target_properties(engagemirror PROPERTIES
    OUTPUT_NAME EngageMirror
    SKIP_BUILD_RPATH TRUE # everything is linked statically
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.engagingwebinars.engagemirror"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES)

  # Device frames (local only, see assets/README.md) and the licence texts of
  # everything linked in: the binary is GPLv3 and carries LGPL/Apache/MIT code.
  set(RES "$<TARGET_BUNDLE_CONTENT_DIR:engagemirror>/Resources")
  set(DEPS_BUILD "${MACOS_DEPS}/build")
  add_custom_command(TARGET engagemirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RES}/assets" "${RES}/licenses"
    COMMAND rsync -a --delete --include=*.png --exclude=* "${CMAKE_CURRENT_SOURCE_DIR}/assets/" "${RES}/assets/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${RES}/licenses/EngageMirror-GPL-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/macos/THIRD-PARTY-NOTICES.txt" "${RES}/licenses/"
    COMMAND ${CMAKE_COMMAND} -E copy "${UXPLAY_DIR}/LICENSE" "${RES}/licenses/UxPlay-GPL-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${UXPLAY_DIR}/lib/llhttp/LICENSE-MIT" "${RES}/licenses/llhttp-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${UXPLAY_DIR}/lib/playfair/LICENSE.md" "${RES}/licenses/playfair-GPL-3.0.md"
    COMMAND ${CMAKE_COMMAND} -E copy "${DEPS_BUILD}/ffmpeg/COPYING.LGPLv2.1" "${RES}/licenses/FFmpeg-LGPL-2.1.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DEPS_BUILD}/openssl/LICENSE.txt" "${RES}/licenses/OpenSSL-Apache-2.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DEPS_BUILD}/libplist/COPYING.LESSER" "${RES}/licenses/libplist-LGPL-2.1.txt"
    VERBATIM)
  return()
endif()

# ---------------------------------------------------------------------------
# EngageMirror application
# ---------------------------------------------------------------------------

# App icon + version info (installer/icon.ico, generated from logo.png).
set(ENGAGEMIRROR_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/installer/icon.ico")
configure_file(src/resource.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resource.rc @ONLY)

add_executable(engagemirror WIN32
  ${ENGAGEMIRROR_SHARED_SRC}
  src/main.cpp
  src/gpu.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/resource.rc)

target_include_directories(engagemirror PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FFMPEG_INCLUDE_DIRS})
target_link_directories(engagemirror PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_compile_definitions(engagemirror PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)

target_link_libraries(engagemirror PRIVATE
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
# merge regardless of link-line order (a binutils quirk) - engagemirror.exe's
# Properties > Details tab shows MinGW's strings, not ours. The app icon
# (also from resource.rc) is unaffected and embeds correctly.
target_link_options(engagemirror PRIVATE
  -municode
  -static-libgcc -static-libstdc++
  -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic
  -mwindows)

# Put the private FFmpeg next to the exe so a dev build runs without PATH
# fiddling (Windows searches the executable's own directory first).
if(FFMPEG_RUNTIME_DIR)
  file(GLOB FFMPEG_DLLS "${FFMPEG_RUNTIME_DIR}/*.dll")
  foreach(dll ${FFMPEG_DLLS})
    add_custom_command(TARGET engagemirror POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:engagemirror>")
  endforeach()
endif()

# Device skins live beside the exe so a dev build finds them too.
add_custom_command(TARGET engagemirror POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:engagemirror>/assets")

install(TARGETS engagemirror RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)
