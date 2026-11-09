#!/bin/sh
# Builds gtk-layer-shell 0.10.0 into /usr/local, for the Linux island and both packages.
# Ubuntu 22.04 ships 0.7.0, which crashes the island's right-click menu, so the packages
# are built against this one instead. CI and the release workflow both run it:
#   ./scripts/build-gtk-layer-shell.sh
#   export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig   # so CMake finds this copy first
# Needs git, meson, ninja, libwayland-dev, wayland-protocols and libgtk-3-dev.
set -eu

# The tag, and the commit it must point to. If the tag ever moves, the build stops
# instead of quietly shipping different code.
TAG=v0.10.0
COMMIT=3964966d72ea3359f4ff748081493e979c080b7e
WORK="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gtk-layer-shell-$TAG"
SRC="$WORK/src"
BUILD="$WORK/build"

rm -rf "$WORK"
mkdir -p "$WORK"
git clone -q --depth 1 --branch "$TAG" https://github.com/wmww/gtk-layer-shell.git "$SRC"
if [ "$(git -C "$SRC" rev-parse HEAD)" != "$COMMIT" ]; then
    echo "$TAG no longer points at $COMMIT; not building it." >&2
    exit 1
fi

meson setup "$BUILD" "$SRC" --prefix=/usr/local --libdir=lib \
    -Dintrospection=false -Ddocs=false -Dtests=false
meson compile -C "$BUILD"

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo
$SUDO meson install -C "$BUILD"

PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --atleast-version=0.10 gtk-layer-shell-0 ||
    { echo "gtk-layer-shell did not install as 0.10 or newer." >&2; exit 1; }
echo "gtk-layer-shell $(PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --modversion gtk-layer-shell-0) installed in /usr/local"
