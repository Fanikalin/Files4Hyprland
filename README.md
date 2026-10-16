# Files4Hyprland

A light, native file manager for [Hyprland](https://hypr.land) and [Omarchy](https://omarchy.org),
written in C with GTK4 (no libadwaita). One ~130 KB binary, about 90 MB of RAM, starts instantly.
It keeps the btop-like look of panels with outlines, and follows your desktop:

- **Colors** come from the current Omarchy theme and change live when you switch themes
- **Rounding and border width** come from Hyprland (`decoration:rounding`, `general:border_size`)
  and update on `hyprctl reload`
- **Light/dark mode and icon theme** of GTK follow the Omarchy theme as well
- **Font** is your fontconfig `monospace`; window opacity, blur and gaps stay Hyprland's

## Features

- Sidebar: XDG places, GTK bookmarks, Trash, and devices from GIO/udisks (click to mount,
  right-click to unmount or eject, password/LUKS prompts handled)
- Tabs (click, `+`, `×`, middle-click to close; middle-click a sidebar place to open it in a new tab)
- Location bar: back / forward / up / refresh, clickable path, editable path (`Ctrl+L`)
- Search covers subfolders: results stream in from a background thread, each with its subfolder shown
  dimmed (`files4hyprland --search TEXT [PATH]` opens with a search)
- **Tiles view** (right-click empty space > View > Tiles / Large tiles): previews of images, videos,
  PDFs and the first lines of text files (videos need `ffmpegthumbnailer` or `ffmpeg`, PDFs `poppler`)
- **Network**: scans your local networks for SMB, SFTP, FTP and NFS machines and lists them with address
  and protocol; type a server (`192.168.1.10`, `smb://host/share`, `sftp://user@host`) to connect, the
  ones you type are remembered. Needs the matching gvfs backends (`gvfs-smb`, `gvfs-nfs`, ...)
- Phones (MTP/PTP) appear as "Phone", "Phone 1", ... in the Devices list
- Every disk has its own color (derived from its name, so it stays the same on any machine; the system
  disk uses the theme's second color) shown as a line on the left and as its usage bar, which turns red
  when less than 10% is free
- Disks in the sidebar show used / total and a usage bar; right-click a file or folder
  and choose **Pin to sidebar** to keep it in the list (**Unpin** removes it)
- Right-click menus: compact, with **Open with** (associated apps, editors such as VS Code, and every other app), **Open as program** for `.sh` scripts, entries added by
  other programs (KDE service menus in `kio/servicemenus`), and on empty space **View** (tiles or details, row size)
  and **Sort by** submenus; the choice is remembered in `~/.config/files4hyprland/settings.ini`
- Default folders (Downloads, Documents, ...) and sidebar sections are shown in the system language
  (ru, uk, de, fr, es)
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

Files4Hyprland has its own signed pacman repository, so `pacman -Syu` (and `omarchy update`)
keep it up to date. One-time setup: trust the release key, add the repository, install.

```bash
curl -fsSL https://raw.githubusercontent.com/Fanikalin/Files4Hyprland/main/data/release-key.asc | sudo pacman-key --add -
sudo pacman-key --lsign-key DABFB081F06B48B502AB5BAE004D0E8617C2774B
printf '\n[files4hyprland]\nServer = https://github.com/Fanikalin/Files4Hyprland/releases/download/repo\n' | sudo tee -a /etc/pacman.conf
sudo pacman -Sy files4hyprland
```

Packages are signed with the release key (fingerprint `DABF B081 F06B 48B5 02AB  5BAE 004D 0E86 17C2 774B`),
and pacman refuses anything that isn't. To stop using the repository, delete the `[files4hyprland]`
section from `/etc/pacman.conf` and run `sudo pacman-key --delete DABFB081F06B48B502AB5BAE004D0E8617C2774B`.

Without the repository you can still install a single release by hand (no automatic updates):
download `files4hyprland-*.pkg.tar.zst` from the [latest release](https://github.com/Fanikalin/Files4Hyprland/releases/latest)
and run `sudo pacman -U ./files4hyprland-*.pkg.tar.zst`.

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
