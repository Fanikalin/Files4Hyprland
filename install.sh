#!/usr/bin/env bash
# Install Files TUI for the current user. Options:
#   --portal   also use it as the system "choose a file" dialog (needs xdg-desktop-portal-termfilechooser)
#   --hypr     also add Omarchy/Hyprland keybindings and window rule (idempotent)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

PORTAL=0; HYPR=0
for a in "$@"; do
  case "$a" in --portal) PORTAL=1 ;; --hypr) HYPR=1 ;; *) echo "unknown option: $a" >&2; exit 2 ;; esac
done

APP="$HOME/.local/share/files-tui"
mkdir -p "$APP" "$HOME/.local/bin" "$HOME/.local/share/applications"
[ -d "$APP/venv" ] || python3 -m venv "$APP/venv"
"$APP/venv/bin/pip" install -q -r requirements.txt
install -m644 files.py "$APP/files.py"
install -m755 bin/files-tui "$HOME/.local/bin/files-tui"
install -m644 desktop/files.desktop "$HOME/.local/share/applications/files.desktop"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
xdg-mime default files.desktop inode/directory
echo "Installed. Run: files-tui [path]"

if [ "$PORTAL" = 1 ]; then
  P="$HOME/.config/xdg-desktop-portal-termfilechooser"
  mkdir -p "$P" "$HOME/.config/xdg-desktop-portal"
  install -m755 portal/files-tui-wrapper.sh "$P/files-tui-wrapper.sh"
  sed "s|@WRAPPER@|$P/files-tui-wrapper.sh|" portal/config.in > "$P/config"
  desk="$(echo "${XDG_CURRENT_DESKTOP:-Hyprland}" | cut -d: -f1 | tr '[:upper:]' '[:lower:]')"
  install -m644 portal/hyprland-portals.conf "$HOME/.config/xdg-desktop-portal/${desk}-portals.conf"
  systemctl --user restart xdg-desktop-portal xdg-desktop-portal-termfilechooser 2>/dev/null || true
  echo "File chooser portal configured (${desk}-portals.conf)."
fi

if [ "$HYPR" = 1 ]; then
  H="$HOME/.config/hypr"
  for f in bindings looknfeel; do
    if ! grep -q "files-tui" "$H/$f.lua" 2>/dev/null && ! grep -q "files-picker" "$H/$f.lua" 2>/dev/null; then
      { echo; cat "hypr/$f.lua"; } >> "$H/$f.lua"
    fi
  done
  hyprctl reload >/dev/null 2>&1 || true
  echo "Hyprland bindings added (SUPER+SHIFT+F)."
fi
