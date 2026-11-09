#!/usr/bin/env bash
# Packages Bunbun for Linux into dist/:
#   Bunbun-<version>-amd64.deb          Debian, Ubuntu and derivatives (apt install ./file.deb)
#   Bunbun-<version>-x86_64.AppImage    most other distributions (chmod +x, run)
#
#   ./scripts/package-linux.sh             build (CMake, into build-linux/), then package
#   ./scripts/package-linux.sh --no-build  package what's already in build-linux/
#   ./scripts/package-linux.sh --appdir    stop at dist/Bunbun.AppDir (run its AppRun to try it),
#                                          without fetching appimagetool
#
# Build on the oldest system you want to support: the binaries need that
# glibc or newer. Release CI uses Ubuntu 22.04 (glibc 2.35).
#
# The AppImage carries Bunbun's own programs, the settings UI, gtk-layer-shell
# and libsoup 3 (lyrics), and uses the system's GTK 3 (on every GNOME and KDE
# desktop). The settings window also needs WebKitGTK 4.1; without it, the
# island opens settings.ini in a text editor instead.
#
# appimagetool is downloaded once into .toolchain/ (set APPIMAGETOOL to use
# your own). CMAKE_ARGS passes extra options to the CMake configure step.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="$(tr -d ' \r\n' < VERSION)"
BUILD=build-linux
DIST=dist
ROOT="$DIST/linux-root"   # the installed tree (/usr), shared by both packages
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage"

NO_BUILD=0 APPDIR_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --no-build) NO_BUILD=1 ;;
        --appdir) APPDIR_ONLY=1 ;;
        *) echo "unknown option $arg" >&2; exit 2 ;;
    esac
done

if [ "$NO_BUILD" = 0 ]; then
    # shellcheck disable=SC2086
    cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr ${CMAKE_ARGS:-}
    cmake --build "$BUILD"
fi
for f in "$BUILD/bunbun" "$BUILD/bunbun-hook" "$BUILD/bunbun-settings" "$BUILD/settings-ui/index.html"; do
    [ -e "$f" ] || { echo "missing $f; build first (see docs/building.md)" >&2; exit 1; }
done

rm -rf "$ROOT"
DESTDIR="$PWD/$ROOT" cmake --install "$BUILD" --prefix /usr >/dev/null
strip "$ROOT"/usr/lib/bunbun/* 2>/dev/null || true
mkdir -p "$ROOT/usr/share/doc/bunbun"
cp LICENSE "$ROOT/usr/share/doc/bunbun/copyright"
cp THIRD_PARTY_NOTICES.md "$ROOT/usr/share/doc/bunbun/" 2>/dev/null || true

# ── .deb ────────────────────────────────────────────────────────────────────
DEB="$DIST/Bunbun-$VERSION-amd64.deb"
# Staged in a temp folder: dpkg-deb needs real Unix permissions, which a
# Windows drive mounted in WSL doesn't have.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/deb"
rm -f "$DEB"
cp -r "$ROOT" "$PKG"
mkdir -p "$PKG/DEBIAN"
find "$PKG" -type d -exec chmod 755 {} +
find "$PKG" -type f -exec chmod 644 {} +
chmod 755 "$PKG"/usr/lib/bunbun/bunbun*
# Library dependencies, worked out from the binaries themselves. On newer
# Ubuntu / Debian the 64-bit-time packages (libgtk-3-0t64…) provide these names.
mkdir -p "$STAGE/shlibs/debian"
: > "$STAGE/shlibs/debian/control"
DEPENDS="$(cd "$STAGE/shlibs" && dpkg-shlibdeps -O --ignore-missing-info \
    "../deb/usr/lib/bunbun/bunbun" "../deb/usr/lib/bunbun/bunbun-settings" "../deb/usr/lib/bunbun/bunbun-hook" 2>/dev/null |
    sed -n 's/^shlibs:Depends=//p')"
[ -n "$DEPENDS" ] || DEPENDS="libgtk-3-0, libgtk-layer-shell0, libsoup-3.0-0, libwebkit2gtk-4.1-0"
# CI links gtk-layer-shell from /usr/local (see ci.yml), which dpkg-shlibdeps can't map to
# a package and so leaves out. The .deb still depends on the system package by name.
case "$DEPENDS" in *libgtk-layer-shell*) ;; *) DEPENDS="$DEPENDS, libgtk-layer-shell0" ;; esac
INSTALLED_KB="$(du -sk "$PKG/usr" | cut -f1)"
cat > "$PKG/DEBIAN/control" <<EOF
Package: bunbun
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Maintainer: Bunbun <noreply@users.noreply.github.com>
Installed-Size: $INSTALLED_KB
Depends: $DEPENDS
Recommends: xdg-utils, pulseaudio-utils | pipewire-bin | alsa-utils, libayatana-appindicator3-1 | libappindicator3-1, xdg-desktop-portal
Homepage: ${BUNBUN_HOMEPAGE:-https://github.com}
Description: Dynamic Island with a mascot that watches your coding agents
 Bunbun puts a small island at the top of the screen: the time, what's
 playing, and a mascot that reacts to coding agents (Claude Code,
 Antigravity, Cursor and others), with Allow / Deny when they need you.
EOF
dpkg-deb --root-owner-group --build "$PKG" "$STAGE/out.deb" >/dev/null
cp "$STAGE/out.deb" "$DEB"
echo "  DEB $DEB"

# ── AppImage ────────────────────────────────────────────────────────────────
APPDIR="$DIST/Bunbun.AppDir"
APPIMAGE="$DIST/Bunbun-$VERSION-x86_64.AppImage"
rm -rf "$APPDIR" "$APPIMAGE"
cp -a "$ROOT" "$APPDIR"
mkdir -p "$APPDIR/usr/lib/bunbun/lib"
# Libraries the island needs that many desktops don't have installed:
# gtk-layer-shell, and libsoup 3 (lyrics) with nghttp2. Not libpsl: every distro has
# libpsl.so.5, and the build machine's copy needs a libunistring newer systems lack.
# HTTPS itself comes from the system's glib-networking, like every GNOME/KDE app.
for want in libgtk-layer-shell libsoup-3.0 libnghttp2; do
    lib="$(ldd "$APPDIR/usr/lib/bunbun/bunbun" | awk -v w="$want" 'index($1, w) == 1 {print $3}')"
    [ -n "$lib" ] && cp -L "$lib" "$APPDIR/usr/lib/bunbun/lib/"
done
cp "$APPDIR/usr/share/applications/bunbun.desktop" "$APPDIR/bunbun.desktop"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/bunbun.png" "$APPDIR/bunbun.png"
ln -sf bunbun.png "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# Bunbun AppImage entry: the island by default; "settings" opens the settings window.
HERE="$(dirname "$(readlink -f "$0")")"
export BUNBUN_DATA_DIR="$HERE/usr/share/bunbun"
# The bundled libraries are for the island only: the settings window uses the
# system's WebKitGTK, which needs the system's own (newer) libsoup. The island puts
# the caller's path back once started (BUNBUN_PARENT_LD_LIBRARY_PATH), so what it
# opens, the settings window included, gets the system's libraries.
export BUNBUN_APPIMAGE=1
export BUNBUN_PARENT_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
ISLAND_LIBS="$HERE/usr/lib/bunbun/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
case "${1:-}" in
    settings) shift; exec "$HERE/usr/lib/bunbun/bunbun-settings" "$@" ;;
    hook)     shift; exec "$HERE/usr/lib/bunbun/bunbun-hook" "$@" ;;
    *)        LD_LIBRARY_PATH="$ISLAND_LIBS" exec "$HERE/usr/lib/bunbun/bunbun" "$@" ;;
esac
EOF
chmod +x "$APPDIR/AppRun"
if [ "$APPDIR_ONLY" = 1 ]; then
    echo "  APPDIR $APPDIR"
    (cd "$DIST" && sha256sum "$(basename "$DEB")" > SHA256SUMS-linux.txt)
    exit 0
fi

TOOL="${APPIMAGETOOL:-.toolchain/appimagetool-x86_64.AppImage}"
if [ ! -x "$TOOL" ]; then
    mkdir -p .toolchain
    echo "  GET appimagetool"
    curl -fsSL -o "$TOOL" "$APPIMAGETOOL_URL"
    chmod +x "$TOOL"
fi
# No FUSE in containers and CI: let the tool unpack itself instead.
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream "$APPDIR" "$APPIMAGE" >/dev/null
rm -rf "$APPDIR"
echo "  APPIMAGE $APPIMAGE"

(cd "$DIST" && sha256sum "$(basename "$DEB")" "$(basename "$APPIMAGE")" > SHA256SUMS-linux.txt)
ls -la "$DEB" "$APPIMAGE"
