# Undocked

Arrange your X11 displays, save layouts, and switch automatically when your dock
changes. Undocked puts the display editor and hotplug watcher in one
C program. It talks to RandR directly and uses [CUI](https://github.com/mattmezza/cui)
for the editor.

![Undocked displaying a three-monitor layout with two external displays above a laptop](media/layout.png)

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/mattmezza/undocked/main/install.sh | sh
```

The installer verifies a checksum and installs a statically linked Linux binary
for x86_64 or aarch64 in `~/.local/bin`. Run it again to upgrade, or pass a version
with `sh -s -- v0.1.1`. Set `PREFIX` to choose another install prefix. You need
an X11 session with RandR 1.3 or newer.

Open the editor with `undocked`. For automatic switching, start the daemon from
your X session startup script.

```sh
undocked daemon &
```

The editor uses the `Undocked` window class and the X11 dialog type so mwm opens
it centered and floating. The installer refreshes a running daemon in the
current X session. Existing `tray` settings can be removed from `config.ini`.

## Arrange displays

- Drag the numbered displays to arrange them. Edges snap together. Use the arrow
  keys to move the selected display, or hold Shift for larger steps.
- Click **Identify** to see the same numbers on the active screens. Connector
  names appear on the layout, including for disabled displays.
- Click a display to open its full-height settings drawer. Select its resolution to open the list
  of supported modes. **Arrange** opens the layout suggestions and Mirror or
  Extend.
- Click **Apply** and then **Keep**. Undocked restores the previous layout if the
  countdown expires. If the old layout is unavailable, it tries a connected
  output at a supported mode.
- Open **Presets** to load a saved layout, see its script path, or toggle its
  **Autoapply** checkbox. **New preset** opens a dialog to save the active layout.

![Settings for a selected display showing its connector, resolution, and refresh rate](media/display-details.png)

![The Presets screen showing two layouts with their file paths and Autoapply checkboxes](media/preset.png)

![The New preset dialog over the dimmed Presets screen](media/preset-dialog.png)

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
`accent`, `onAccent`, `warning`, and `cornerRadius`. Set `corner_radius` under
`[ui]` to override the Xresources radius. The default is 4 px. Set
`undocked.font` and `undocked.mono` to font file paths, or use the corresponding
INI settings. Display numbers, connector names, resolutions, and refresh rates
use the mono face.

## Launch and scripts

Add this to [Launch](https://github.com/mattmezza/launch)'s `config.ini`.
Search for a saved preset, Mirror or Extend, an arrangement, or an action for a
specific connector such as **Disable DP-1** or **Make DP-1 primary**. Resolution
and refresh-rate changes open a second picker with the connector's supported
modes. Layout changes open Undocked for **Keep** or **Revert**, and both options
are also available in Launch while confirmation is pending.

```ini
[workflow Displays]
keywords=monitor screen dock preset mirror resolution enable disable refresh rate
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

A window manager such as mwm can use the same CLI from the user's X11 session.
Run `undocked` to open the editor, `undocked launch` for the action menu, or
`undocked identify` to show display numbers. Use `status --json` for display
state and `presets --json` for saved presets and whether they match. The CLI
connects to the session daemon and starts it on demand when needed.

## Build

Source builds need a C11 toolchain, make, pkg-config, and the X11, RandR,
FreeType, and HarfBuzz development libraries. CUI and the fonts are bundled.

```sh
make
make check
sudo make clean install
```

`make check` uses Xvfb and xdotool. `make release name=v0.1.1` pushes a version
tag. GitHub Actions builds, tests, and publishes the static binaries for both
architectures with their checksums.

Undocked is [MIT-licensed](LICENSE). It currently supports X11. The layout and
preset model is independent of the display backend.
