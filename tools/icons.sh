#!/bin/sh
# Rebuild the embedded alpha masks from Heroicons 2.2.0 (MIT).
set -eu
cd "$(dirname "$0")/.."
icon_work=$(mktemp -d)
trap 'rm -r "$icon_work"' EXIT
origin=https://raw.githubusercontent.com/tailwindlabs/heroicons/v2.2.0/optimized/24/outline
for icon_name in computer-desktop view-columns eye adjustments-horizontal square-2-stack \
        folder-open plus x-mark check arrow-left chevron-up chevron-down arrow-path power; do
    curl -fsSL --retry 3 "$origin/$icon_name.svg" -o "$icon_work/$icon_name.svg"
done
python3 - "$icon_work" <<'PY'
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

source = Path(sys.argv[1])
ns = '{http://www.w3.org/2000/svg}'
def paths(name):
    return ''.join(f'<path d="{node.get("d")}"/>' for node in
                   ET.parse(source / (name + '.svg')).findall(ns + 'path'))
def outline(body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" '
            f'stroke="white" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">{body}</svg>')
desktop = paths('computer-desktop')
column = paths('view-columns')
icons = [
    f'<defs><mask id="overlap"><rect width="24" height="24" fill="white" stroke="none"/>'
    f'<rect x="1" y="7" width="15" height="15" rx="2" fill="black" stroke="black"/></mask></defs>'
    f'<g mask="url(#overlap)"><g transform="translate(6 -1) scale(.8)">{desktop}</g></g>'
    f'<g transform="translate(-1 6) scale(.8)">{desktop}</g>',
    desktop, column, f'<g transform="rotate(90 12 12)">{column}</g>',
    # The two-above arrangement uses the same rounded outline as rectangle-group.
    '<rect x="2.5" y="3.5" width="8" height="6.5" rx="1.1"/>'
    '<rect x="13.5" y="3.5" width="8" height="6.5" rx="1.1"/>'
    '<rect x="8" y="13.5" width="8" height="6.5" rx="1.1"/>',
]
icons += [paths(name) for name in ('eye', 'adjustments-horizontal', 'square-2-stack',
    'folder-open', 'plus', 'x-mark', 'check', 'arrow-left', 'chevron-up', 'chevron-down', 'arrow-path', 'power')]
with open('assets/icons.alpha', 'wb') as atlas:
    for icon in icons:
        png = subprocess.check_output(['rsvg-convert', '-w', '32', '-h', '32'], input=outline(icon).encode())
        atlas.write(subprocess.check_output(['magick', 'png:-', '-alpha', 'extract', '-depth', '8', 'gray:-'], input=png))
PY
