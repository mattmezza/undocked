#!/bin/sh
# Run in Alpine Linux 3.23 with the source tree and an output directory mounted.
set -eu

version=${1#v}
output=$2
source_dir=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
jobs=$(getconf _NPROCESSORS_ONLN)

apk add --no-cache build-base pkgconf curl xz binutils \
    libxrandr-dev libx11-static libxcb-static libxext-static \
    freetype-dev freetype-static harfbuzz-dev harfbuzz-static \
    bzip2-static libpng-static brotli-static zlib-static glib-static \
    gettext-static pcre2-static graphite2-static

cd "$work"
build_xorg() {
    package=$1
    hash=$2
    file=$package.tar.xz
    curl -fsSL --retry 3 "https://www.x.org/releases/individual/lib/$file" -o "$file"
    printf '%s  %s\n' "$hash" "$file" | sha256sum -c -
    tar -xf "$file"
    (
        cd "$package"
        ./configure --prefix=/usr/local --enable-static --disable-shared > "$work/$package.log"
        make -j "$jobs" install >> "$work/$package.log"
    )
}

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
build_xorg libXau-1.0.12 74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb
build_xorg libXrandr-1.5.5 72b922c2e765434e9e9f0960148070bd4504b288263e2868a4ccce1b7cf2767a
cp -R "$source_dir"/Makefile "$source_dir"/src "$source_dir"/vendor "$source_dir"/assets "$work"
make -j "$jobs" STATIC=1 VERSION="$version" LDFLAGS="-static -Wl,--gc-sections,-Map,$work/link.map"
asset=undocked-linux-$(uname -m)
mkdir -p "$output"
strip -o "$output/$asset" build/undocked
if readelf -lW "$output/$asset" | grep -q ' INTERP ' ||
   readelf -dW "$output/$asset" | grep -q '(NEEDED)'; then
    echo "Unexpected dynamic linkage." >&2
    exit 1
fi
test "$("$output/$asset" --version)" = "undocked $version"
if grep -E 'lib(glib-2\.0|intl)\.a\(' "$work/link.map" | grep -q '^/'; then
    echo "Unexpected GLib or libintl objects in release binary." >&2
    exit 1
fi
(cd "$output" && sha256sum "$asset" > "$asset.sha256")
