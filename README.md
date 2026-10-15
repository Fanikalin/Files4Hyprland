# Files4Hyprland

A light, native file manager for [Hyprland](https://hypr.land) and [Omarchy](https://omarchy.org),
written in C with GTK4 (no libadwaita). One ~130 KB binary, about 90 MB of RAM, starts instantly.
It keeps the btop-like look of panels with outlines, and follows your desktop:

- **Colors** come from the current Omarchy theme and change live when you switch themes
- **Rounding and border width** come from Hyprland (`decoration:rounding`, `general:border_size`)
  and update on `hyprctl reload`
- **Font** is your fontconfig `monospace`; window opacity, blur and gaps stay Hyprland's

## Features

- Sidebar: XDG places, GTK bookmarks, Trash, and devices from GIO/udisks (click to mount,
  right-click to unmount or eject, password/LUKS prompts handled)
- Tabs (click, `+`, `×`, middle-click to close; middle-click a sidebar place to open it in a new tab)
- Location bar: back / forward / up / refresh, clickable path, editable path (`Ctrl+L`)
- Search covers subfolders: results stream in from a background thread, each with its subfolder shown
  dimmed (`files4hyprland --search TEXT [PATH]` opens with a search)
- Click to select, double-click to open, `Ctrl`/`Shift`+click for several; right-click menus
- **System drag and drop**: drag files out to any app (browser, chat, editor, other file managers)
  and drop files in from any app. Drop on a folder, a sidebar place or disk, a tab or a path segment.
  Inside the app a drop moves, from other apps it copies; hold `Ctrl` to copy or `Shift` to move.
  Dropping on Trash trashes
- Rename in place (`F2`), new folder / file created and named in place
- Undo / redo (toolbar buttons, `Ctrl+Z`, `Ctrl+Shift+Z` / `Ctrl+Y`) for rename, move, copy, trash
  (restores from Trash) and new folder / file; the button tooltip says what will be undone
- Cut / copy / paste compatible with Nautilus, Thunar, Dolphin (`x-special/gnome-copied-files`)
- Live updates when files change on disk; sortable columns; hidden files toggle; free space in the status line
- File chooser mode, so it can replace the system "Open / Save" dialog

## Install (Arch Linux)

Download and install the package from the latest
[release](https://github.com/Fanikalin/Files4Hyprland/releases/latest):

```bash
cd /tmp && curl -LO https://github.com/Fanikalin/Files4Hyprland/releases/download/v2.2.0/files4hyprland-2.2.0-1-x86_64.pkg.tar.zst
sudo pacman -U ./files4hyprland-2.2.0-1-x86_64.pkg.tar.zst
```

(`pacman -U <url>` directly does not work because release packages are not GPG-signed and pacman
requires a `.sig` for remote files; installing a downloaded file is fine.)

**Files** appears in the application menu. To also make it the default folder handler, add
`SUPER+SHIFT+F` (Omarchy) and use it as the file-chooser dialog, run once as your user:

```bash
files4hyprland-setup --all      # or any of: --default --hypr --portal
```

Everything `files4hyprland-setup` adds is marked: `files4hyprland-setup --undo` removes it,
`sudo pacman -Rns files4hyprland` runs the undo for you, and upgrades refresh what you enabled.

### Build from source

```bash
meson setup build --prefix=/usr && meson compile -C build && sudo meson install -C build
```

Needs GTK ≥ 4.12, GLib/GIO, gvfs + udisks2 (devices, Trash).

## Use as the system file chooser

Install [`xdg-desktop-portal-termfilechooser`](https://aur.archlinux.org/packages/xdg-desktop-portal-termfilechooser)
(AUR), then run `files4hyprland-setup --portal`. Apps that use the XDG portal open Files4Hyprland
in a floating window when you press "Choose file". Running apps may need a restart.
Try it by hand: `files4hyprland --pick file|files|dir|save --out FILE [PATH]`.

## Keys

| Key | Action |
| --- | --- |
| `Enter` / double-click | open |
| `Backspace`, `Alt+←/→/↑`, mouse back/forward | up, back, forward, up |
| `Ctrl+C/X/V`, `Delete`, `F2` | copy, cut, paste, trash, rename |
| `Ctrl+Z`, `Ctrl+Shift+Z` / `Ctrl+Y` | undo, redo |
| `Ctrl+Shift+N`, `Ctrl+H`, `F5` | new folder, hidden files, refresh |
| `Ctrl+L`, `Ctrl+F` or `/` | edit path, search |
| `Ctrl+T`, `Ctrl+W`, `Ctrl+Tab` | new, close, next tab |
| `Ctrl+Alt+T`, `Ctrl+Q` | terminal here, quit |

## License

MIT
