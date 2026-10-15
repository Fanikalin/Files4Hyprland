# Files4Hyprland

A mouse-friendly terminal file manager with an Explorer-like layout, built with
[Textual](https://textual.textualize.io/). Made for [Omarchy](https://omarchy.org)
(colors follow the current Omarchy theme and update live), but it runs in any terminal.

- Sidebar with Places and Devices (click to open, auto-mounts unmounted disks, right-click to unmount)
- Tabs (click, `+`, `×`; middle-click a sidebar item to open it in a new tab)
- Location bar with back / forward / up, clickable breadcrumbs, editable path and live search
- Click to select, double-click to open, `Ctrl`/`Shift`+click for multi-select, right-click context menu
- Icon-only toolbar: new folder / file, cut, copy, paste, rename, trash (via `gio trash`), hidden files
- Drag and drop: drag files onto a folder, a sidebar place or disk, a tab or a path segment to move them (hold `Ctrl` to copy; drop on Trash to trash). `Esc` cancels
- Sortable columns, hidden-file toggle, free-space status line
- File chooser mode, so it can replace the system "Open / Save" dialog

## Install (Arch Linux)

Requires a Nerd Font for icons. Download and install the package from the latest
[release](https://github.com/Fanikalin/Files4Hyprland/releases/latest):

```bash
cd /tmp && curl -LO https://github.com/Fanikalin/Files4Hyprland/releases/download/v1.2.0/files4hyprland-1.2.0-1-any.pkg.tar.zst
sudo pacman -U ./files4hyprland-1.2.0-1-any.pkg.tar.zst
```

(`pacman -U <url>` directly does not work because release packages are not GPG-signed, and pacman
requires a `.sig` for remote files; installing a downloaded file is fine.)

pacman resolves the dependencies (`python-textual`, `glib2`, `udisks2`, `xdg-utils`) from the official repos.
**Files** then shows up in the application menu. To also make it the default folder handler,
add `SUPER+SHIFT+F` and use it as the file-chooser dialog, run once as your user:

```bash
files4hyprland-setup --all      # or any of: --default --hypr --portal
```

Everything `files4hyprland-setup` adds is marked; `files4hyprland-setup --undo` removes it, and
`sudo pacman -Rns files4hyprland` runs the undo for you before removing the package.

### Updating

Download the newer package and install it the same way (`pacman -U ./file`), or build from the
PKGBUILD in `packaging/` (works with AUR helpers, e.g. `yay -S files4hyprland` once published to the AUR).
Maintainers: bump `pkgver` in `packaging/PKGBUILD`, commit, run `packaging/release.sh`.

### Without pacman

```bash
pip install textual
python3 files.py [path]
```

## Use as the system file chooser

Install [`xdg-desktop-portal-termfilechooser`](https://aur.archlinux.org/packages/xdg-desktop-portal-termfilechooser)
(AUR), then run `files4hyprland-setup --portal`. Apps that use the XDG portal will open Files when
you press "Choose file". Running apps may need a restart. Try the mode by hand with
`files-tui --pick file|files|dir|save --out FILE`.

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
