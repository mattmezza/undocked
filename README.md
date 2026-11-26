# Undocked

Arrange your X11 displays, save layouts, and switch automatically when your dock
changes. Undocked puts the display editor, tray applet, and hotplug watcher in one
C program. It talks to RandR directly and uses [CUI](https://github.com/mattmezza/cui)
for the editor.

![Undocked displaying a three-monitor layout with two external displays above a laptop](media/layout.png)

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/mattmezza/undocked/main/install.sh | sh
```

The installer verifies a checksum and installs a statically linked Linux binary
for x86_64 or aarch64 in `~/.local/bin`. Run it again to upgrade, or pass a version
with `sh -s -- v0.1.0`. Set `PREFIX` to choose another install prefix. You need
an X11 session with RandR 1.3 or newer.

Open the editor with `undocked`. For automatic switching, start the daemon from
your X session startup script. The applet uses the existing XEmbed tray in
[mwm](https://github.com/mattmezza/mwm).

```sh
undocked daemon --tray &
```

Left-click the tray icon for the editor, middle-click to identify screens, and
right-click for the Launch picker. Omit `--tray` to run the watcher alone. The
editor identifies itself as `Undocked` with an X11 dialog window type so mwm
opens it centered and floating.

## Arrange displays

- Drag the numbered displays to arrange them. Edges snap together. Use the arrow
  keys to move the selected display, or hold Shift for larger steps.
- Click **Identify** to see the same numbers on the active screens. Connector
  names appear on tiles and in the detail panel, including for disabled displays.
- Select a display to change its resolution, refresh rate, orientation, primary
  status, or power state. Pick **Mirror**, **Row**, **Column**, or **Two above**
  for a starting layout.
- Click **Apply** and then **Keep**. Undocked restores the previous layout if the
  countdown expires. If the old layout is unavailable, it tries a connected
  output at a supported mode.
- Save the active layout with **Save preset**. Set **Autoapply** for the layout
  you want when the same hardware is connected. The **Presets** menu can switch
  layouts or change their automatic setting.

![Undocked's save preset dialog with an Autoapply toggle](media/preset.png)

## Presets and configuration

Copy [config.ini](config.ini) to `$XDG_CONFIG_HOME/undocked/config.ini`
(default `~/.config/undocked/config.ini`). The file lists the defaults. Keep
this directory in Stow to sync the settings and executable `presets/*.sh` files.
Run a preset script directly, choose it from the editor, or call
`undocked preset "Office dock"`. Undocked reads the saved layout records from
the script when matching or applying it.

Automatic matching uses the full connected set, including outputs a preset
leaves disabled. Undocked uses the monitor's EDID and the connector name. A
unique EDID can follow a display to a different connector unless
`match_ports = true`. Identical EDIDs and displays without EDID use connector
names. If several automatic presets match, choose one for autoapply. An
unrecognized setup keeps its usable layout.

To reapply a wallpaper, set `after_change` under `[undocked]` and run
`undocked reload`. The hook runs once after the final layout settles, including
changes from other X11 clients. It receives `DISPLAY`, `UNDOCKED_REASON`, and
`UNDOCKED_PRESET`.

```ini
after_change = if [ -r "$HOME/.fehbg" ]; then sh "$HOME/.fehbg"; fi
```

Undocked ships Geist and Geist Mono. It inherits the mwm Xresources palette
and supports `undocked.background`, `panel`, `foreground`, `muted`, `border`,
`accent`, `onAccent`, `warning`, and `cornerRadius`. Set `undocked.font` and
`undocked.mono` to font file paths, or use the respective INI settings.

## Launch and scripts

Add this to [Launch](https://github.com/mattmezza/launch)'s `config.ini`.
The picker includes the editor, Identify, compatible presets and arrangements,
and pending confirmation actions.

```ini
[workflow Displays]
keywords=monitor screen dock preset mirror resolution
command=undocked launch
```

```sh
undocked status --json
undocked presets --json
undocked save 'Office dock' --auto
undocked preset 'Office dock' --dry-run
undocked preset 'Office dock'             # then run keep or revert
undocked layout column --yes              # apply without the countdown
undocked output DP-1 mode 2560x1440@60
undocked output DP-1 primary
undocked identify
undocked recover
```

Run `undocked --help` for the full command list. JSON output carries a schema
version, and commands report failure through their exit status.

## Build

Source builds need a C11 toolchain, make, pkg-config, and the X11, RandR,
FreeType, and HarfBuzz development libraries. CUI and the fonts are bundled.

```sh
make
make check
sudo make clean install
```

`make check` uses Xvfb and xdotool. `make release name=v0.1.0` pushes a version
tag. GitHub Actions builds, tests, and publishes the static binaries for both
architectures with their checksums.

Undocked is [MIT-licensed](LICENSE). It currently supports X11. The layout and
preset model is independent of the display backend.
