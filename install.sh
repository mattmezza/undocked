#!/bin/sh
set -eu

version=${1:-latest}
if [ "$#" -gt 1 ] || { [ "$version" != latest ] &&
    ! printf '%s\n' "$version" | LC_ALL=C grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'; }; then
    echo 'Usage: install.sh [vX.Y.Z]' >&2
    exit 2
fi
case "$(uname -s):$(uname -m)" in
    Linux:x86_64) arch=x86_64 ;;
    Linux:aarch64|Linux:arm64) arch=aarch64 ;;
    *) echo 'Undocked releases support Linux x86_64 and aarch64.' >&2; exit 1 ;;
esac
base=https://github.com/mattmezza/undocked/releases
if [ "$version" = latest ]; then
    # Resolve and pin one tag so assets come from the same release.
    url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$base/latest")
    version=${url##*/}
    if ! printf '%s\n' "$version" | LC_ALL=C grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo 'Undocked has no published release.' >&2
        exit 1
    fi
fi
prefix=${PREFIX:-"$HOME/.local"}
bindir=$prefix/bin
case ${XDG_CONFIG_HOME:-} in
    /*) config=$XDG_CONFIG_HOME/undocked ;;
    *) config=$HOME/.config/undocked ;;
esac
work=$(mktemp -d)
pending=
trap 'test -z "$pending" || unlink "$pending"; rm -r "$work"' EXIT
download=$base/download/$version
asset=undocked-linux-$arch
curl -fsSL --retry 3 -o "$work/$asset" "$download/$asset"
curl -fsSL --retry 3 -o "$work/$asset.sha256" "$download/$asset.sha256"
(cd "$work" && sha256sum -c "$asset.sha256")
mkdir -p "$bindir" "$config"
if [ ! -e "$config/config.ini" ] && [ ! -L "$config/config.ini" ]; then
    curl -fsSL --retry 3 -o "$work/config.ini" "$download/config.ini"
    curl -fsSL --retry 3 -o "$work/config.ini.sha256" "$download/config.ini.sha256"
    (cd "$work" && sha256sum -c config.ini.sha256)
    (set -C; umask 022; cat "$work/config.ini" > "$config/config.ini") || test -e "$config/config.ini"
fi
pending=$(mktemp "$bindir/.undocked.XXXXXX")
install -m 755 "$work/$asset" "$pending"
mv -f "$pending" "$bindir/undocked"
pending=
printf 'Installed %s at %s\n' "$version" "$bindir/undocked"
