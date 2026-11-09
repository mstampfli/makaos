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
- `Alt+Return` - also opens foot (a host desktop usually grabs Super before the
  guest sees it, so this one always works when MakaOS runs in a window)
- `$mod+d` — open the **tofi** launcher (`ls /bin | tofi`; pick a program → it runs)
- `Alt+s` - `/bin/sdl3_hello`; `Alt+d` - `/bin/dplaunch` (DarkPlaces/Quake, if it was
  built); `Alt+g` - `/bin/glfwtest` (only when it was built)
- `$mod+Shift+q` — close the focused window
- `$mod+1..9` — switch workspace; `$mod+Shift+1..9` — move window to workspace
- `$mod+h/j/k/l` or arrows — move focus; `$mod+Shift+…` — move window
- `$mod+f` — fullscreen; `$mod+Shift+space` — toggle floating
- `$mod+Shift+c` — reload config; `$mod+Shift+e` — exit sway

(Full default sway bindings apply — see `/etc/sway/config`.)

The keyboard layout is Swiss (ch): sway loads the precompiled keymap
`/etc/keymap.xkb` (from `scripts/configs/ch.xkb`) via `xkb_file`, because the
in-guest libxkbcommon cannot compile the "ch" layout from the xkb tree.

## Settings

sway is configuration-driven; there is no separate GUI control-center (none of
the wlroots settings GUIs are toolkit-portable). Settings live in three places:

1. **Persistent config — `/etc/sway/config`.** Drives the wallpaper, the bar,
   keybindings, output layout, input options. Edit it (on the build host today,
   since no in-image text editor is ported yet) and reload with `$mod+Shift+c`.
   - Wallpaper: `output * bg /usr/share/backgrounds/sway/wallpaper.png fill`
     (or `output * bg #1a1b26 solid_color`).
   - Bar: the `bar { … }` block (position, colors, `status_command`). The clock is
     `status_command /bin/makaclock` (a small native helper; the image has no
     `date`/`sleep`).
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
`/usr/share/backgrounds/sway/wallpaper.png`, and writes `/etc/sway/config` from
sway's default config with the wallpaper line, the `bar {}` block (DejaVu Sans
Mono font, makaclock status), `set $menu` pointing at tofi, the extra Alt
bindings, and the `xkb_file` keymap line. The
ports are built by `scripts/port-{sway,swaybg,tofi,wlr-randr}.sh` (and pulled in
by `scripts/run-port-chain.sh`).

Porting these required a few standard libc additions (in `userland/libc/`):
`dprintf`, `reallocarray`, `fts(3)`, and `fstatat`/`dirfd` (via a synthetic
dir-fd→path registry, since the kernel has no `openat`). The ext2 root
filesystem is 512 MiB by default (`EXT2_SECTORS` in `build.sh`) to hold the DE
stack, the xkb tree and the 83 MB `libSDL3.so`; the JDK/LWJGL and Minecraft test
images grow it to 1 GiB and 3 GiB.
