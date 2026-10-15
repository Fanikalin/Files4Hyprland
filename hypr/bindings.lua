-- File manager: Files TUI
hl.unbind("SUPER + SHIFT + F")
hl.unbind("SUPER + ALT + SHIFT + F")
o.bind("SUPER + SHIFT + F", "File manager", "omarchy-launch-tui files-tui")
o.bind("SUPER + ALT + SHIFT + F", "File manager (cwd)", "omarchy-launch-tui files-tui \"$(omarchy-cmd-terminal-cwd)\"")
