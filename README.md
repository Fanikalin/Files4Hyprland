# Files TUI

A mouse-friendly terminal file manager with an Explorer-like layout, built with
[Textual](https://textual.textualize.io/). Made for [Omarchy](https://omarchy.org)
(colors follow the current Omarchy theme and update live), but it runs in any terminal.

- Sidebar with Places and Devices (click to open, auto-mounts unmounted disks, right-click to unmount)
- Tabs (click, `+`, `×`; middle-click a sidebar item to open it in a new tab)
- Location bar with back / forward / up, clickable breadcrumbs, editable path and live search
- Click to select, double-click to open, `Ctrl`/`Shift`+click for multi-select, right-click context menu
- Cut / copy / paste, rename, new folder / file, move to Trash (via `gio trash`)
- Sortable columns, hidden-file toggle, free-space status line
- File chooser mode, so it can replace the system "Open / Save" dialog

## Install

Requires Python 3.11+, `xdg-utils`, `udisks2` and `glib2` (`gio`), and a Nerd Font.

```bash
git clone https://github.com/<you>/files-tui && cd files-tui
./install.sh              # app + "Files" launcher entry, default folder handler
./install.sh --hypr       # + SUPER+SHIFT+F binding and a floating rule (Omarchy/Hyprland)
./install.sh --portal     # + use as the system file chooser (see below)
```

Run it with `files-tui [path]`.

## Use as the system file chooser

Install [`xdg-desktop-portal-termfilechooser`](https://aur.archlinux.org/packages/xdg-desktop-portal-termfilechooser)
(AUR), then run `./install.sh --portal`. Apps that use the XDG portal will open Files TUI when
you press "Choose file". Running apps may need a restart. Use `files-tui --pick file|files|dir|save --out FILE`
to try the mode by hand.

## Keys

| Key | Action |
| --- | --- |
| `Enter` / double-click | open |
| `Backspace`, `Alt+←/→/↑` | up, back, forward, up |
| `Space`, `Ctrl+A` | select, select all |
| `c` `x` `v` / `Ctrl+C/X/V` | copy, cut, paste |
| `F2`, `Delete`, `n`, `N` | rename, trash, new folder, new file |
| `Ctrl+L`, `/` | edit path, search |
| `Ctrl+T`, `Ctrl+W`, `[`, `]` | new, close, previous, next tab |
| `Ctrl+H`, `F5`, `u`, `q` | hidden files, refresh, unmount, quit |

## License

MIT
