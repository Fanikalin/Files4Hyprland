-- File manager: Files4Hyprland
hl.unbind("SUPER + SHIFT + F")
hl.unbind("SUPER + ALT + SHIFT + F")
o.bind("SUPER + SHIFT + F", "File manager", "files4hyprland")
o.bind("SUPER + ALT + SHIFT + F", "File manager (cwd)", "files4hyprland \"$(omarchy-cmd-terminal-cwd)\"")
