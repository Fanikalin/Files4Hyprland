#!/usr/bin/env sh
# Called by xdg-desktop-portal-termfilechooser; opens Files4Hyprland as the file chooser.
# Args: multiple directory save path out debug
multiple="$1"; directory="$2"; save="$3"; path="$4"; out="$5"; debug="$6"
[ "$debug" = 1 ] && set -x

if [ "$save" = 1 ]; then mode=save
elif [ "$directory" = 1 ]; then mode=dir
elif [ "$multiple" = 1 ]; then mode=files
else mode=file; fi

exec files4hyprland --pick "$mode" --out "$out" "$path"
