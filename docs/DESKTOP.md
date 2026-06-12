# MakaOS desktop environment

A full wlroots-based desktop, assembled by **porting** upstream sway-ecosystem
software (no custom DE code). Launch it from a login shell with `sway`.

## Components (all ported, not written)

| Role | Program | Notes |
|------|---------|-------|
| Compositor / WM | **sway** 1.10.1 | i3-compatible tiling Wayland compositor |
| Taskbar / status bar | **swaybar** | sway's native bar (`bar {}` block in the config) |
| Wallpaper | **swaybg** 1.2.1 | layer-shell daemon; loads PNG via cairo |
| App launcher | **tofi** 0.9.1 | dmenu-style; bound to `$mod+d` |
| Terminal | **foot** 1.20.2 | bound to `$mod+Return` |
| Display settings | **wlr-randr** 0.5.0 | resolution / scale / position / transform |
| Prompts | **swaynag** | error/confirm prompts |

## Keybindings (`$mod` = **Super**/Windows key)

- `$mod+Return` — open a **foot** terminal
- `$mod+d` — open the **tofi** launcher (`ls /bin | tofi`; pick a program → it runs)
- `$mod+Shift+q` — close the focused window
- `$mod+1..9` — switch workspace; `$mod+Shift+1..9` — move window to workspace
- `$mod+h/j/k/l` or arrows — move focus; `$mod+Shift+…` — move window
- `$mod+f` — fullscreen; `$mod+Shift+space` — toggle floating
- `$mod+Shift+c` — reload config; `$mod+Shift+e` — exit sway

(Full default sway bindings apply — see `/etc/sway/config`.)

## Settings

sway is configuration-driven; there is no separate GUI control-center (none of
the wlroots settings GUIs are toolkit-portable). Settings live in three places:

1. **Persistent config — `/etc/sway/config`.** Drives the wallpaper, the bar,
   keybindings, output layout, input options. Edit it (on the build host today,
   since no in-image text editor is ported yet) and reload with `$mod+Shift+c`.
   - Wallpaper: `output * bg /usr/share/backgrounds/sway/wallpaper.png fill`
     (or `output * bg #1a1b26 solid_color`).
   - Bar: the `bar { … }` block (position, colors, `status_command`).
   - Launcher: `set $menu …`.

2. **Live changes — `swaymsg`.** Apply settings at runtime without editing the
   config or restarting:
   - `swaymsg output '*' bg /path/to/img.png fill` — change wallpaper now
   - `swaymsg bar mode hide` / `dock` — hide/show the taskbar
   - `swaymsg input '*' …`, `swaymsg reload`, `swaymsg exit`

3. **Display settings — `wlr-randr`.** Dedicated output control:
   - `wlr-randr` — list outputs and their modes
   - `wlr-randr --output <name> --mode 1280x800` — set resolution
   - `wlr-randr --output <name> --scale 1.5 --transform 90 --pos 0,0`
   - `wlr-randr --output <name> --off` / `--on`

## How it's wired

`build.sh` installs the DE binaries into `/bin`, the wallpaper asset into
`/usr/share/backgrounds/sway/wallpaper.png`, and writes `/etc/sway/config` with
the wallpaper line, the `bar {}` block, and `set $menu` pointing at tofi. The
ports are built by `scripts/port-{sway,swaybg,tofi,wlr-randr}.sh` (and pulled in
by `scripts/run-port-chain.sh`).

Porting these required a few standard libc additions (in `userland/libc/`):
`dprintf`, `reallocarray`, `fts(3)`, and `fstatat`/`dirfd` (via a synthetic
dir-fd→path registry, since the kernel has no `openat`). The ext2 image was
enlarged 128→384 MiB to hold the DE stack alongside the xkb keymap tree.
