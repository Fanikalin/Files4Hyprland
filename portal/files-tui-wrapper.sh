#!/usr/bin/env sh
# Called by xdg-desktop-portal-termfilechooser; opens Files4Hyprland as a file chooser.
# Args: multiple directory save path out debug
multiple="$1"; directory="$2"; save="$3"; path="$4"; out="$5"; debug="$6"
[ "$debug" = 1 ] && set -x

if [ "$save" = 1 ]; then mode=save
elif [ "$directory" = 1 ]; then mode=dir
elif [ "$multiple" = 1 ]; then mode=files
else mode=file; fi

done="$out.done"
rm -f "$done"
: > "$out"

setsid -f uwsm-app -- xdg-terminal-exec --app-id=org.omarchy.files-picker.TUI.float \
  -e files-tui --pick "$mode" --out "$out" --done "$done" "$path" >/dev/null 2>&1

# wait (up to 1h) until the picker exits
i=0
while [ ! -e "$done" ] && [ "$i" -lt 12000 ]; do sleep 0.3; i=$((i+1)); done
rm -f "$done"
