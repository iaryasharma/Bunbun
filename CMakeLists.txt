# Bunbun for Linux. (Windows builds with ./build.sh and llvm-mingw.)
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux
#
# Needs GTK 3, gtk-layer-shell (0.6+), libsoup 3, WebKitGTK 4.1 and Cairo/Pango
# development files; docs/building.md lists the packages. The settings UI is built with npm
# when it's available (BUNBUN_BUILD_UI).
cmake_minimum_required(VERSION 3.16)

if(WIN32)
    message(FATAL_ERROR "On Windows, build with ./build.sh (llvm-mingw). This CMake build is for Linux.")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BUNBUN_VERSION LIMIT_COUNT 1)
string(STRIP "${BUNBUN_VERSION}" BUNBUN_VERSION)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" BUNBUN_VERSION_NUMBER "${BUNBUN_VERSION}")
project(Bunbun VERSION ${BUNBUN_VERSION_NUMBER} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUNBUN_BUILD_UI "Build the settings UI with npm" ON)
option(BUNBUN_BUILD_SETTINGS "Build bunbun-settings (needs WebKitGTK 4.1)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(LAYERSHELL REQUIRED IMPORTED_TARGET gtk-layer-shell-0>=0.6)
if(LAYERSHELL_VERSION VERSION_LESS 0.10)
    # 0.7 crashes the island's right-click menu (Ubuntu 22.04 ships it). The packages need 0.10.
    message(WARNING "gtk-layer-shell ${LAYERSHELL_VERSION} crashes the island's right-click menu. "
                    "Build 0.10 or newer with scripts/build-gtk-layer-shell.sh (see docs/building.md).")
endif()
# Lyrics (LRCLIB, NetEase) are fetched with libsoup 3.
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
# Full-screen detection and the hide / tuck hotkeys on X11.
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
if(BUNBUN_BUILD_SETTINGS)
    pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LINUX ${SRC}/platform/linux)

add_compile_definitions(BUNBUN_VERSION_STR="${BUNBUN_VERSION}")
if(DEFINED ENV{BUNBUN_HOMEPAGE})
    add_compile_definitions(BUNBUN_HOMEPAGE_STR="$ENV{BUNBUN_HOMEPAGE}")
endif()
add_compile_options(-Wall -Wno-maybe-uninitialized -Wno-unused-function -Wno-missing-braces -Wno-unused-variable -Wno-unused-but-set-variable)

# Shared code that still includes <windows.h> / <d2d1.h> (the mascot, the skin
# pack loader, the hook installer) builds against the stand-ins in compat/.
add_library(bunbun_shared STATIC
    ${SRC}/mascot/mascot.cpp
    ${SRC}/mascot/pack_sheet.cpp
    ${SRC}/agents/hook_installer.cpp
    ${LINUX}/compat/d2d_cairo.cpp
    ${LINUX}/compat/win32_compat.cpp
    ${LINUX}/image_codec.cpp
    ${LINUX}/mascot_png.cpp
    ${LINUX}/platform.cpp
    ${LINUX}/settings.cpp)
target_include_directories(bunbun_shared PUBLIC ${LINUX}/compat)
target_compile_options(bunbun_shared PRIVATE -Wno-sign-compare -Wno-reorder)
target_link_libraries(bunbun_shared PUBLIC PkgConfig::GTK)

# The island.
add_executable(bunbun
    ${LINUX}/island_linux.cpp
    ${LINUX}/studio_linux.cpp
    ${LINUX}/look_linux.cpp
    ${LINUX}/edgelight_linux.cpp
    ${LINUX}/timers_linux.cpp
    ${LINUX}/screentime_linux.cpp
    ${LINUX}/features_linux.cpp
    ${LINUX}/calendar_linux.cpp
    ${LINUX}/shelf_linux.cpp
    ${LINUX}/picker_linux.cpp
    ${LINUX}/tray_linux.cpp
    ${LINUX}/sources.cpp
    ${LINUX}/water_linux.cpp
    ${LINUX}/work_linux.cpp
    ${LINUX}/lyrics_linux.cpp
    ${LINUX}/mpris.cpp
    ${SRC}/agents/agent_bridge.cpp)
target_link_libraries(bunbun PRIVATE bunbun_shared PkgConfig::GTK PkgConfig::LAYERSHELL PkgConfig::SOUP PkgConfig::X11 ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(bunbun PRIVATE Threads::Threads)

# The hook relay agents run on every event: small, and with the C++ runtime
# linked in, so the copy an AppImage leaves in ~/.local/share runs anywhere.
add_executable(bunbun-hook ${SRC}/hook/bunbun_hook.cpp)
target_link_options(bunbun-hook PRIVATE -static-libstdc++ -static-libgcc)

# Unit tests for the Studio page model shared with Windows: ./build-linux/test_studio
add_executable(test_studio tests/test_studio.cpp)

if(BUNBUN_BUILD_SETTINGS)
    add_executable(bunbun-settings ${LINUX}/settings_host_linux.cpp)
    target_link_libraries(bunbun-settings PRIVATE bunbun_shared PkgConfig::WEBKIT)
endif()

# The settings UI (React + Vite), built into <build>/settings-ui next to the exes.
find_program(NPM npm)
if(BUNBUN_BUILD_UI AND NPM)
    file(GLOB_RECURSE UI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/settings-ui/src/*)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/settings-ui/index.html
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}/settings-ui sh -c "[ -d node_modules ] || npm ci --silent"
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}/settings-ui
                npm run build --silent -- --outDir ${CMAKE_BINARY_DIR}/settings-ui --emptyOutDir
        DEPENDS ${UI_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/settings-ui/index.html
        COMMENT "Building the settings UI"
        # Without VERBATIM the "||" in the first command reaches the build shell unquoted,
        # which then looks for a program called "npm ci --silent" (only seen when
        # node_modules is missing: a fresh clone or CI).
        VERBATIM)
    add_custom_target(settings_ui ALL DEPENDS ${CMAKE_BINARY_DIR}/settings-ui/index.html)
elseif(BUNBUN_BUILD_UI)
    message(WARNING "npm not found: the settings UI isn't built (bunbun-settings will say so).")
endif()

# ── Install (the .deb and the AppImage use this layout) ─────────────────────
#   <prefix>/lib/bunbun/{bunbun, bunbun-hook, bunbun-settings}
#   <prefix>/bin/bunbun -> ../lib/bunbun/bunbun
#   <prefix>/share/bunbun/settings-ui/
#   <prefix>/share/applications/bunbun.desktop, icons, metainfo
include(GNUInstallDirs)
set(PKGLIB ${CMAKE_INSTALL_LIBDIR}/bunbun)
if(CMAKE_INSTALL_LIBDIR MATCHES "^lib/")  # Debian's lib/x86_64-linux-gnu: keep it plain lib/bunbun
    set(PKGLIB lib/bunbun)
endif()
# The island looks for libraries it ships itself (gtk-layer-shell 0.10, see package-linux.sh) in
# lib/ next to it. Set for the install only, so the build tree keeps using the system's copies.
# (On the target itself: CMAKE_INSTALL_RPATH is read when a target is created, too early here.)
set_target_properties(bunbun PROPERTIES INSTALL_RPATH "\$ORIGIN/lib")
install(TARGETS bunbun bunbun-hook RUNTIME DESTINATION ${PKGLIB})
if(BUNBUN_BUILD_SETTINGS)
    install(TARGETS bunbun-settings RUNTIME DESTINATION ${PKGLIB})
endif()
install(CODE "file(MAKE_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR})
              execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ../${PKGLIB}/bunbun
                              \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/bunbun)")
if(EXISTS ${CMAKE_BINARY_DIR}/settings-ui OR (BUNBUN_BUILD_UI AND NPM))
    install(DIRECTORY ${CMAKE_BINARY_DIR}/settings-ui DESTINATION ${CMAKE_INSTALL_DATADIR}/bunbun)
endif()
install(FILES packaging/linux/bunbun.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/linux/io.github.bunbun.Bunbun.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The icon is drawn by the mascot code itself at install time.
install(CODE "foreach(size 64 128 256 512)
                  set(dir \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor/\${size}x\${size}/apps)
                  file(MAKE_DIRECTORY \${dir})
                  execute_process(COMMAND \"$<TARGET_FILE:bunbun>\" --render-icon \"\${dir}/bunbun.png\" \${size})
              endforeach()")
