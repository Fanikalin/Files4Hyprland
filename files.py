#!/usr/bin/env python3
"""Files: a mouse-friendly TUI file manager themed from the Omarchy theme.

Layout: tabs on top, an Explorer-like location bar (back/forward/up, clickable
breadcrumbs, search), an action bar, a sidebar with Places and Devices, the file
list and a status line.  Everything is clickable; keyboard shortcuts also work.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote, urlparse

from rich.text import Text
from textual import events, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.markup import escape
from textual.message import Message
from textual.screen import ModalScreen
from textual.theme import Theme
from textual.widgets import DataTable, Input, Label, OptionList, Static
from textual.widgets.option_list import Option

HOME = Path.home()
THEME_FILE = HOME / ".local/state/omarchy/current/theme/colors.toml"
TRASH = HOME / ".local/share/Trash/files"

# Nerd Font glyphs
I_FOLDER, I_FILE, I_LINK = "", "", ""
I_HOME, I_DESKTOP, I_DOCS, I_DOWN = "", "", "", ""
I_PICS, I_MUSIC, I_VIDEO, I_TRASH, I_DISK = "", "", "", "", ""
I_USB = ""

PLACES = [
    ("Home", HOME, I_HOME),
    ("Desktop", HOME / "Desktop", I_DESKTOP),
    ("Documents", HOME / "Documents", I_DOCS),
    ("Downloads", HOME / "Downloads", I_DOWN),
    ("Pictures", HOME / "Pictures", I_PICS),
    ("Music", HOME / "Music", I_MUSIC),
    ("Videos", HOME / "Videos", I_VIDEO),
    ("Trash", TRASH, I_TRASH),
]


# --------------------------------------------------------------------------- theme
def read_colors() -> dict[str, str]:
    """Resolved Omarchy theme colors (falls back to the raw colors.toml)."""
    colors: dict[str, str] = {}
    try:
        out = subprocess.run(
            ["omarchy-theme-color", "--file", str(THEME_FILE), "--all"],
            capture_output=True, text=True, timeout=3,
        ).stdout
        for line in out.splitlines():
            if "\t" in line:
                k, v = line.split("\t", 1)
                colors[k] = v
    except Exception:
        pass
    if not colors:
        try:
            colors = {k: v for k, v in tomllib.loads(THEME_FILE.read_text()).items() if isinstance(v, str)}
        except Exception:
            pass
    return colors


def make_theme(name: str, c: dict[str, str]) -> Theme:
    bg = c.get("background", "#000000")
    return Theme(
        name=name,
        primary=c.get("accent", "#8d8d8d"),
        secondary=c.get("blue", "#8d8d8d"),
        warning=c.get("yellow", "#cecece"),
        error=c.get("red", "#ff6060"),
        success=c.get("green", "#b6b6b6"),
        accent=c.get("cyan", "#b0b0b0"),
        foreground=c.get("foreground", "#ffffff"),
        background=bg,
        surface=c.get("dark_background", bg),
        panel=c.get("selection", c.get("lighter_background", "#1a1a1a")),
        dark=c.get("mode", "dark") != "light",
    )


# --------------------------------------------------------------------------- helpers
def link(action: str, text: str, style: str = "") -> str:
    inner = f"[@click={action}]{text}[/]"
    return f"[{style}]{inner}[/]" if style else inner


def human(n: int) -> str:
    size = float(n)
    for unit in ("B", "K", "M", "G", "T"):
        if size < 1024 or unit == "T":
            return f"{int(size)} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return str(n)


def unique(dst: Path) -> Path:
    if not dst.exists() and not dst.is_symlink():
        return dst
    stem, suffix = (dst.name, "") if dst.is_dir() else (dst.stem, dst.suffix)
    i = 1
    while True:
        cand = dst.with_name(f"{stem} ({i}){suffix}")
        if not cand.exists() and not cand.is_symlink():
            return cand
        i += 1


@dataclass
class Entry:
    name: str
    path: Path
    is_dir: bool
    is_link: bool
    size: int
    mtime: float


def list_dir(path: Path, show_hidden: bool) -> list[Entry]:
    out = []
    with os.scandir(path) as it:
        for e in it:
            if not show_hidden and e.name.startswith("."):
                continue
            try:
                st = e.stat()
                is_dir = e.is_dir()
            except OSError:
                try:
                    st = e.stat(follow_symlinks=False)
                except OSError:
                    continue
                is_dir = False
            out.append(Entry(e.name, Path(e.path), is_dir, e.is_symlink(), st.st_size, st.st_mtime))
    return out


def read_devices() -> list[dict]:
    try:
        out = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,PATH,LABEL,SIZE,FSTYPE,MOUNTPOINTS,TYPE,RM"],
            capture_output=True, text=True, timeout=3,
        ).stdout
        data = json.loads(out)
    except Exception:
        return []
    devs: list[dict] = []

    def walk(node: dict) -> None:
        fstype = node.get("fstype")
        mps = [m for m in (node.get("mountpoints") or []) if m]
        if fstype and fstype not in ("swap", "crypto_LUKS", "LVM2_member", "linux_raw_member"):
            user_mps = [m for m in mps if m == "/" or m.startswith(("/run/media", "/media", "/mnt"))]
            label = node.get("label") or node.get("name")
            if "/" in mps:
                devs.append({"name": "System", "dev": node["path"], "size": node["size"], "mount": "/"})
            elif user_mps:
                devs.append({"name": label, "dev": node["path"], "size": node["size"], "mount": user_mps[0]})
            elif not mps:
                size = node.get("size") or ""
                if not (fstype == "vfat" and not node.get("label") and size.endswith("M")):
                    devs.append({"name": label, "dev": node["path"], "size": size, "mount": None})
        for ch in node.get("children") or []:
            walk(ch)

    for n in data.get("blockdevices", []):
        walk(n)
    return devs


# --------------------------------------------------------------------------- widgets
class LinkBar(Static):
    """Static whose [@click=...] links double as drop targets."""

    def _target(self, event) -> Path | None:
        action = event.style.meta.get("@click")
        return self.app.path_for_action(action) if isinstance(action, str) else None

    def on_mouse_move(self, event: events.MouseMove) -> None:
        t = self._target(event)
        self.app.drag_hover(event, t, t.name or "/" if t else "")

    def on_mouse_up(self, event: events.MouseUp) -> None:
        if self.app.drag_drop(event, self._target(event)):
            event.stop()


class FileTable(DataTable):
    """DataTable with Explorer-like mouse handling."""

    def _row(self, event) -> int | None:
        row = event.style.meta.get("row")
        return row if isinstance(row, int) and row >= 0 else None

    def on_mouse_down(self, event: events.MouseDown) -> None:
        row = self._row(event)
        if event.button == 1 and row is not None:
            self.app.drag_prepare(row, event)

    def on_mouse_move(self, event: events.MouseMove) -> None:
        t = self.app.target_for_row(self._row(event))
        self.app.drag_hover(event, t, (t.name or "/") if t else "")

    def on_mouse_up(self, event: events.MouseUp) -> None:
        if self.app.drag_drop(event, self.app.target_for_row(self._row(event))):
            event.stop()

    class Opened(Message):
        def __init__(self, index: int) -> None:
            super().__init__()
            self.index = index

    class Clicked(Message):
        def __init__(self, index: int, ctrl: bool, shift: bool) -> None:
            super().__init__()
            self.index, self.ctrl, self.shift = index, ctrl, shift

    class Context(Message):
        def __init__(self, index: int | None, x: int, y: int) -> None:
            super().__init__()
            self.index, self.x, self.y = index, x, y

    async def on_click(self, event: events.Click) -> None:
        event.prevent_default()
        if time.monotonic() - self.app.drag_ended < 0.3:
            return
        meta = event.style.meta
        row = meta.get("row")
        if "row" not in meta or row is None or row < 0:
            if event.button == 3:
                self.post_message(self.Context(None, event.screen_x, event.screen_y))
            return
        event.stop()
        if event.button == 3:
            self.post_message(self.Context(row, event.screen_x, event.screen_y))
        elif event.button == 1:
            if event.chain >= 2 and not (event.ctrl or event.shift):
                self.post_message(self.Opened(row))
            else:
                self.post_message(self.Clicked(row, event.ctrl, event.shift))


class Sidebar(OptionList):
    def _target(self, event) -> tuple[Path | None, bool, str]:
        idx = event.style.meta.get("option")
        if idx is None or not (0 <= idx < self.option_count):
            return None, False, ""
        oid = self.get_option_at_index(idx).id or ""
        if oid.startswith("dir:"):
            p = Path(oid[4:])
            return p, p == TRASH, ("Trash" if p == TRASH else p.name or "/")
        if oid.startswith("dev:"):
            d = self.app.device_for(oid[4:])
            if d and d["mount"]:
                return Path(d["mount"]), False, d["name"]
        return None, False, ""

    def on_mouse_move(self, event: events.MouseMove) -> None:
        t, trash, label = self._target(event)
        self.app.drag_hover(event, t, label, trash)

    def on_mouse_up(self, event: events.MouseUp) -> None:
        t, trash, _ = self._target(event)
        if self.app.drag_drop(event, t, trash):
            event.stop()

    class Context(Message):
        def __init__(self, option_id: str | None, x: int, y: int, middle: bool) -> None:
            super().__init__()
            self.option_id, self.x, self.y, self.middle = option_id, x, y, middle

    async def on_click(self, event: events.Click) -> None:
        if event.button in (2, 3):
            event.prevent_default()
            event.stop()
            idx = event.style.meta.get("option")
            oid = None
            if idx is not None and 0 <= idx < self.option_count:
                oid = self.get_option_at_index(idx).id
            self.post_message(self.Context(oid, event.screen_x, event.screen_y, event.button == 2))


class PromptScreen(ModalScreen[str | None]):
    DEFAULT_CSS = """
    PromptScreen { align: center middle; background: $background 60%; }
    PromptScreen > Vertical { width: 60; height: auto; border: round $primary; background: $surface; padding: 0 1; }
    PromptScreen Label { width: 100%; color: $primary; text-style: bold; }
    PromptScreen Input { border: none; background: $panel; height: 1; padding: 0 1; margin: 1 0 0 0; }
    PromptScreen .hint { color: $text-muted; margin: 1 0 0 0; }
    """
    BINDINGS = [Binding("escape", "cancel", show=False)]

    def __init__(self, title: str, initial: str = "") -> None:
        super().__init__()
        self.title_text, self.initial = title, initial

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Label(self.title_text)
            yield Input(value=self.initial, id="prompt")
            yield Label("Enter: OK    Esc: cancel", classes="hint")

    def on_mount(self) -> None:
        inp = self.query_one(Input)
        inp.focus()
        stem = Path(self.initial).stem if self.initial else ""
        if stem and stem != self.initial:
            inp.selection = type(inp.selection)(0, len(stem))
        else:
            inp.action_select_all()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value)

    def action_cancel(self) -> None:
        self.dismiss(None)


class ConfirmScreen(ModalScreen[bool]):
    DEFAULT_CSS = """
    ConfirmScreen { align: center middle; background: $background 60%; }
    ConfirmScreen > Vertical { width: 60; height: auto; border: round $primary; background: $surface; padding: 1 2; }
    ConfirmScreen Static { width: 100%; }
    ConfirmScreen .buttons { height: 1; margin: 1 0 0 0; }
    """
    BINDINGS = [
        Binding("escape,n", "no", show=False),
        Binding("enter,y", "yes", show=False),
    ]

    def __init__(self, text: str) -> None:
        super().__init__()
        self.text = text

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(self.text)
            yield Static("[@click=screen.yes][b $primary] Yes (Enter) [/][/]    [@click=screen.no][b] No (Esc) [/][/]", classes="buttons")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)


class MenuScreen(ModalScreen[str | None]):
    DEFAULT_CSS = """
    MenuScreen { align: left top; background: transparent; }
    MenuScreen OptionList { width: 30; height: auto; max-height: 24; border: round $primary; background: $surface; padding: 0; }
    MenuScreen OptionList > .option-list--option-highlighted { background: $panel; color: $primary; text-style: bold; }
    """
    BINDINGS = [Binding("escape", "close", show=False)]

    def __init__(self, items: list[tuple[str, str] | None], x: int, y: int) -> None:
        super().__init__()
        self.items, self.px, self.py = items, x, y

    def compose(self) -> ComposeResult:
        yield OptionList(*[None if i is None else Option(i[1], id=i[0]) for i in self.items])

    def on_mount(self) -> None:
        ol = self.query_one(OptionList)
        height = len(self.items) + 2
        ol.styles.offset = (
            max(0, min(self.px, self.app.size.width - 31)),
            max(0, min(self.py, self.app.size.height - height)),
        )
        ol.highlighted = 0
        ol.focus()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        self.dismiss(event.option.id)

    def on_click(self, event: events.Click) -> None:
        if event.widget is self:
            self.dismiss(None)

    def action_close(self) -> None:
        self.dismiss(None)


# --------------------------------------------------------------------------- app
@dataclass
class TabState:
    path: Path
    back: list[Path] = field(default_factory=list)
    fwd: list[Path] = field(default_factory=list)
    selected: set[str] = field(default_factory=set)
    cursor: str | None = None
    filter: str = ""
    sort: str = "name"
    reverse: bool = False
    anchor: int | None = None


class FilesApp(App):
    TITLE = "Files"
    ENABLE_COMMAND_PALETTE = False
    CSS = """
    Screen { background: $background; }
    Static { link-style: none; link-style-hover: bold reverse; link-color: auto; link-background: transparent; }
    #tabs { height: 1; background: $surface; color: $text-muted; padding: 0 1; text-wrap: nowrap; text-overflow: clip; }
    #nav { height: 3; border: round $primary 50%; padding: 0 1; }
    #nav:focus-within { border: round $primary; }
    #nav Static { height: 1; text-wrap: nowrap; text-overflow: clip; }
    #navbtns { width: auto; margin: 0 1 0 0; }
    #crumbs { width: 1fr; }
    #addr { width: 1fr; display: none; border: none; background: $panel; height: 1; padding: 0 1; }
    #search { width: 26; border: none; background: $panel; height: 1; padding: 0 1; margin: 0 0 0 1; }
    #search:focus, #addr:focus { background: $panel; }
    #actionrow { height: 1; }
    #actions { width: 1fr; height: 1; padding: 0 1; text-wrap: nowrap; text-overflow: clip; }
    #savename { display: none; width: 40; border: none; background: $panel; height: 1; padding: 0 1; margin: 0 1 0 0; }
    #body { height: 1fr; }
    #side { width: 28; border: round $primary 50%; border-title-color: $primary; background: $background; }
    #side:focus-within, #side:hover { border: round $primary; }
    #main { width: 1fr; border: round $primary 50%; border-title-color: $primary; }
    #main:focus-within, #main:hover { border: round $primary; }
    #status { height: 1; padding: 0 1; color: $text-muted; text-wrap: nowrap; text-overflow: clip; }
    Sidebar { background: transparent; border: none; padding: 0; height: 1fr; scrollbar-size-vertical: 1; }
    Sidebar > .option-list--option-highlighted { background: $panel; color: $primary; text-style: bold; }
    Sidebar > .option-list--option-hover { background: transparent; }
    Sidebar > .option-list--option-disabled { color: $text-muted; text-style: none; }
    FileTable { background: $background; border: none; padding: 0; height: 1fr; scrollbar-size-vertical: 1; }
    FileTable > .datatable--header { background: $surface; color: $primary; text-style: bold; }
    FileTable > .datatable--cursor { background: $panel; color: $primary; text-style: bold; }
    FileTable > .datatable--hover { background: transparent; }
    FileTable:focus, Sidebar:focus { background-tint: transparent; }
    FileTable:focus > .datatable--header { background-tint: transparent; }
    .-dragging FileTable > .datatable--hover { background: $primary 30%; }
    .-dragging Sidebar > .option-list--option-hover { background: $primary 30%; }
    Screen { layers: base overlay; }
    Screen > * { layer: base; }
    #ghost { layer: overlay; display: none; width: auto; height: 1; padding: 0 1; background: $primary; color: $background; text-style: bold; }
    FileTable > .datatable--header-hover { background: $surface; }
    FileTable > .datatable--fixed-cursor { background: $panel; }
    FileTable:focus > .datatable--cursor { background: $primary 30%; color: $foreground; text-style: bold; }
    """

    BINDINGS = [
        Binding("backspace", "up", show=False),
        Binding("alt+left", "back", show=False),
        Binding("alt+right", "forward", show=False),
        Binding("alt+up", "up", show=False),
        Binding("f5,ctrl+r", "refresh", show=False),
        Binding("f2", "rename", show=False),
        Binding("delete", "delete", show=False),
        Binding("ctrl+c,c,ctrl+insert", "copy", show=False),
        Binding("ctrl+x,x", "cut", show=False),
        Binding("ctrl+v,v,shift+insert", "paste", show=False),
        Binding("ctrl+a", "select_all", show=False),
        Binding("space", "toggle_select", show=False),
        Binding("ctrl+h", "toggle_hidden", show=False),
        Binding("ctrl+l", "edit_address", show=False),
        Binding("ctrl+f,slash", "search", show=False),
        Binding("ctrl+t", "new_tab", show=False),
        Binding("ctrl+w", "close_tab", show=False),
        Binding("ctrl+pageup,alt+pageup,[", "prev_tab", show=False),
        Binding("ctrl+pagedown,alt+pagedown,]", "next_tab", show=False),
        Binding("n", "new_folder", show=False),
        Binding("N", "new_file", show=False),
        Binding("u", "unmount_current", show=False),
        Binding("f10,menu", "context_menu", show=False),
        Binding("q,ctrl+q", "quit", show=False),
    ]

    def __init__(self, start: Path, select: str | None = None, pick: str | None = None,
                 out: Path | None = None, name: str = "") -> None:
        super().__init__()
        self.pick = pick  # None | file | files | dir | save
        self.out = out
        self.pick_name = name
        self.picked = False
        self.drag: dict | None = None
        self.drag_ended = 0.0
        self.tabs: list[TabState] = [TabState(start, cursor=select)]
        self.cur = 0
        self.show_hidden = False
        self.clip: tuple[str, list[Path]] | None = None
        self.view: list[Entry] = []
        self.devices: list[dict] = []
        self._dev_sig = None
        self._theme_n = 0
        self._theme_mtime = 0.0
        self._dir_mtime = 0.0
        self.pal: dict[str, str] = {}

    # ---- compose / mount
    def compose(self) -> ComposeResult:
        yield LinkBar(id="tabs")
        yield Static(id="ghost")
        with Horizontal(id="nav"):
            yield Static(id="navbtns")
            yield LinkBar(id="crumbs")
            yield Input(id="addr")
            yield Input(placeholder=" Search", id="search")
        with Horizontal(id="actionrow"):
            yield Static(id="actions")
            yield Input(id="savename", placeholder="File name")
        with Horizontal(id="body"):
            with Vertical(id="side"):
                yield Sidebar()
            with Vertical(id="main"):
                yield FileTable(cursor_type="row", zebra_stripes=False, id="files")
        yield Static(id="status")

    def on_mount(self) -> None:
        self.apply_theme()
        if self.pick == "save":
            box = self.query_one("#savename", Input)
            box.display = True
            box.value = self.pick_name
        table = self.query_one(FileTable)
        table.focus()
        self.refresh_devices()
        self.navigate(self.tab.path, push=False)
        self.set_interval(2.0, self.poll)
        self.set_interval(4.0, self.refresh_devices)

    def apply_theme(self) -> None:
        self.pal = read_colors()
        self._theme_n += 1
        name = f"omarchy-{self._theme_n}"
        self.register_theme(make_theme(name, self.pal))
        self.theme = name
        try:
            self._theme_mtime = THEME_FILE.stat().st_mtime
        except OSError:
            pass

    def poll(self) -> None:
        try:
            if THEME_FILE.stat().st_mtime != self._theme_mtime:
                self.apply_theme()
                self.refresh_all()
        except OSError:
            pass
        try:
            if self.tab.path.stat().st_mtime != self._dir_mtime:
                self.refresh_listing()
        except OSError:
            pass

    @property
    def tab(self) -> TabState:
        return self.tabs[self.cur]

    @property
    def primary(self) -> str:
        return self.pal.get("accent", "#8d8d8d")

    # ---- navigation
    def navigate(self, path: Path, push: bool = True, select: str | None = None) -> None:
        try:
            path = path.expanduser().resolve()
        except OSError:
            pass
        if not path.is_dir():
            self.notify(f"Not a folder: {path}", severity="error")
            return
        if not os.access(path, os.R_OK | os.X_OK):
            self.notify(f"No access: {path}", severity="error")
            return
        t = self.tab
        if push and path != t.path:
            t.back.append(t.path)
            t.fwd.clear()
        t.path = path
        t.selected.clear()
        t.filter = ""
        t.cursor = select
        t.anchor = None
        self.query_one("#search", Input).value = ""
        self.refresh_all()
        self.query_one(FileTable).scroll_home(animate=False)

    def action_back(self) -> None:
        t = self.tab
        if t.back:
            t.fwd.append(t.path)
            self.navigate(t.back.pop(), push=False)

    def action_forward(self) -> None:
        t = self.tab
        if t.fwd:
            t.back.append(t.path)
            self.navigate(t.fwd.pop(), push=False)

    def action_up(self) -> None:
        t = self.tab
        if t.path.parent != t.path:
            self.navigate(t.path.parent, select=t.path.name)

    def action_refresh(self) -> None:
        self.refresh_devices()
        self.refresh_listing()

    def action_crumb(self, i: int) -> None:
        parts = self.tab.path.parts
        self.navigate(Path(*parts[: i + 1]))

    # ---- tabs
    def action_new_tab(self) -> None:
        self.tabs.insert(self.cur + 1, TabState(self.tab.path))
        self.cur += 1
        self.refresh_all()

    def action_close_tab(self, i: int | None = None) -> None:
        i = self.cur if i is None else i
        if len(self.tabs) == 1:
            self.action_quit()
            return
        self.tabs.pop(i)
        if self.cur > i or self.cur >= len(self.tabs):
            self.cur = max(0, self.cur - 1)
        self.refresh_all()

    def action_tab(self, i: int) -> None:
        if 0 <= i < len(self.tabs) and i != self.cur:
            self.cur = i
            self.refresh_all()
            self.query_one(FileTable).focus()

    def action_prev_tab(self) -> None:
        self.action_tab((self.cur - 1) % len(self.tabs))

    def action_next_tab(self) -> None:
        self.action_tab((self.cur + 1) % len(self.tabs))

    def open_in_new_tab(self, path: Path) -> None:
        self.tabs.insert(self.cur + 1, TabState(path))
        self.cur += 1
        self.navigate(path, push=False)

    # ---- rendering
    def refresh_all(self) -> None:
        self.render_tabs()
        self.render_nav()
        self.render_actions()
        self.refresh_listing()
        self.render_sidebar(force=True)

    def render_tabs(self) -> None:
        parts = []
        for i, t in enumerate(self.tabs):
            name = "Home" if t.path == HOME else escape(t.path.name or "/")
            label = f" {I_FOLDER} {name} "
            if i == self.cur:
                parts.append(link(f"app.tab({i})", label, "b $primary on $panel") + link(f"app.close_tab({i})", "\u00d7 ", "$primary on $panel"))
            else:
                parts.append(link(f"app.tab({i})", label) + link(f"app.close_tab({i})", "\u00d7 "))
        parts.append(link("app.new_tab", "  +  ", "b $primary"))
        self.query_one("#tabs", Static).update(" ".join(parts))

    def render_nav(self) -> None:
        t = self.tab
        dim = "dim"

        def btn(action: str, glyph: str, enabled: bool) -> str:
            return link(f"app.{action}", glyph, "b $primary") if enabled else f"[{dim}]{glyph}[/]"

        nav = "  ".join([
            btn("back", "\uf060", bool(t.back)),
            btn("forward", "\uf061", bool(t.fwd)),
            btn("up", "\uf062", t.path.parent != t.path),
            btn("refresh", "\uf021", True),
        ])
        self.query_one("#navbtns", Static).update(nav)
        parts = t.path.parts
        width = max(10, self.query_one("#crumbs").size.width - 6)
        shown: list[str] = []
        total = 0
        start = 0
        for i in range(len(parts) - 1, -1, -1):
            seg = parts[i] if i else "/"
            total += len(seg) + 3
            if total > width and shown:
                start = i + 1
                break
            shown.append(link(f"app.crumb({i})", escape(seg), "b" if i == len(parts) - 1 else ""))
        shown.reverse()
        prefix = f"[{dim}]\u2026 \u203a [/]" if start else ""
        sep = f" [{dim}]\u203a[/] "
        blank = link("app.edit_address", " " * 300)
        self.query_one("#crumbs", Static).update(f"{prefix}{sep.join(shown)}{blank}")

    def render_actions(self) -> None:
        def btn(action: str, label: str, enabled: bool = True) -> str:
            if not enabled:
                return f"[dim] {label} [/]"
            return link(f"app.{action}", f" {label} ", "$primary on $panel")

        if self.pick:
            label = {"file": "\uf00c Choose", "files": "\uf00c Choose", "dir": "\uf00c Choose this folder", "save": "\uf0c7 Save"}[self.pick]
            hint = {"file": "double-click a file to pick it", "files": "Space / Ctrl+click for several files",
                    "dir": "navigate into a folder, then choose it", "save": "type a name and press Save"}[self.pick]
            self.query_one("#actions", Static).update(" ".join([
                link("app.choose", f" {label} ", "b $primary on $panel"),
                link("app.cancel_pick", " \uf00d Cancel ", "$primary on $panel"),
                btn("new_folder", "\uf07b+"),
                btn("toggle_hidden", "\uf06e" if self.show_hidden else "\uf070"),
                f"[dim] {hint}[/]",
            ]))
            return
        has_sel = bool(self.targets())
        paste_ok = self.clip is not None
        items = [
            btn("new_folder", "\uf07b+"),
            btn("new_file", "\uf15b+"),
            btn("cut", "\uf0c4", has_sel),
            btn("copy", "\uf0c5", has_sel),
            btn("paste", "\uf0ea", paste_ok),
            btn("rename", "\uf044", has_sel),
            btn("delete", "\uf1f8", has_sel),
            btn("toggle_hidden", "\uf06e" if self.show_hidden else "\uf070"),
        ]
        self.query_one("#actions", Static).update(" ".join(items))

    def sorted_view(self, entries: list[Entry]) -> list[Entry]:
        t = self.tab
        key = {
            "name": lambda e: e.name.casefold(),
            "size": lambda e: e.size,
            "mod": lambda e: e.mtime,
        }[t.sort]
        dirs = sorted((e for e in entries if e.is_dir), key=key, reverse=t.reverse)
        files = sorted((e for e in entries if not e.is_dir), key=key, reverse=t.reverse)
        return dirs + files

    def refresh_listing(self) -> None:
        t = self.tab
        table = self.query_one(FileTable)
        if table.row_count and 0 <= table.cursor_row < len(self.view):
            t.cursor = self.view[table.cursor_row].name
        try:
            entries = list_dir(t.path, self.show_hidden)
            self._dir_mtime = t.path.stat().st_mtime
            err = None
        except OSError as e:
            entries, err = [], str(e)
        if self.pick == "dir":
            entries = [e for e in entries if e.is_dir]
        if t.filter:
            q = t.filter.casefold()
            entries = [e for e in entries if q in e.name.casefold()]
        self.view = self.sorted_view(entries)
        t.selected &= {e.name for e in self.view}

        width = max(30, table.size.width)
        name_w = max(14, width - 36)
        arrow = " ▼" if t.reverse else " ▲"
        table.clear(columns=True)
        table.add_column("", width=1, key="mark")
        table.add_column("Name" + (arrow if t.sort == "name" else ""), width=name_w, key="name")
        table.add_column(Text("Size" + (arrow if t.sort == "size" else ""), justify="right"), width=8, key="size")
        table.add_column("Modified" + (arrow if t.sort == "mod" else ""), width=16, key="mod")
        pri = self.primary
        for e in self.view:
            sel = e.name in t.selected
            icon = I_FOLDER if e.is_dir else (I_LINK if e.is_link else I_FILE)
            name = Text(f"{icon} {e.name}", no_wrap=True, overflow="ellipsis")
            if e.is_dir:
                name.stylize(f"bold {pri}", 0, 1)
            if sel:
                name.stylize(f"bold {pri}")
            size = Text("" if e.is_dir else human(e.size), justify="right")
            table.add_row(
                Text("✓" if sel else " ", style=f"bold {pri}"),
                name,
                size,
                time.strftime("%Y-%m-%d %H:%M", time.localtime(e.mtime)),
                key=e.name,
            )
        if t.cursor:
            for i, e in enumerate(self.view):
                if e.name == t.cursor:
                    table.move_cursor(row=i, animate=False)
                    break
        prefix = {"file": "Select a file \u00b7 ", "files": "Select files \u00b7 ", "dir": "Select a folder \u00b7 ", "save": "Save as \u00b7 "}.get(self.pick or "", "")
        title = f"{prefix}{I_FOLDER} {t.path}" + (f"  · filter: {t.filter}" if t.filter else "")
        self.query_one("#main").border_title = title
        if err:
            self.notify(err, severity="error")
        self.render_status()
        self.render_actions()

    def render_status(self) -> None:
        t = self.tab
        n_dirs = sum(e.is_dir for e in self.view)
        text = f"{len(self.view)} items ({n_dirs} folders)"
        if t.selected:
            total = sum(e.size for e in self.view if e.name in t.selected and not e.is_dir)
            text += f"  ·  {len(t.selected)} selected ({human(total)})"
        else:
            cur = self.current_entry()
            if cur and not cur.is_dir:
                text += f"  ·  {cur.name}: {human(cur.size)}"
        if self.clip:
            verb = "cut" if self.clip[0] == "cut" else "copied"
            text += f"  ·  {len(self.clip[1])} {verb}"
        try:
            du = shutil.disk_usage(t.path)
            text += f"  ·  {human(du.free)} free of {human(du.total)}"
        except OSError:
            pass
        self.query_one("#status", Static).update(escape(text))

    # ---- sidebar
    @work(thread=True, exclusive=True, group="devices")
    def refresh_devices(self) -> None:
        devs = read_devices()
        self.call_from_thread(self.set_devices, devs)

    def set_devices(self, devs: list[dict]) -> None:
        self.devices = devs
        self.render_sidebar()

    def render_sidebar(self, force: bool = False) -> None:
        side = self.query_one(Sidebar)
        sig = (tuple((d["dev"], d["mount"], d["name"]) for d in self.devices), self.tab.path, tuple(p.is_dir() for _, p, _ in PLACES))
        if sig == self._dev_sig and not force:
            return
        self._dev_sig = sig
        keep = side.highlighted_option.id if side.highlighted_option else None
        side.clear_options()
        header = lambda s: Option(Text(s, style="bold dim"), disabled=True)  # noqa: E731
        opts: list[Option | None] = [header(" Places")]
        cur = self.tab.path
        for name, path, icon in PLACES:
            if path.is_dir() or name == "Home":
                mark = "▎" if path == cur else " "
                opts.append(Option(Text(f"{mark}{icon}  {name}"), id=f"dir:{path}"))
        opts.append(None)
        opts.append(header(" Devices"))
        for d in self.devices:
            mounted = d["mount"] is not None
            icon = I_DISK if d["mount"] in (None, "/") or d["name"] == "System" else I_USB
            mark = "▎" if mounted and cur == Path(d["mount"]) else " "
            dot = "" if mounted else "  (not mounted)"
            label = Text(f"{mark}{icon}  {d['name']}")
            label.append(f"  {d['size']}", style="dim")
            if dot:
                label.append(dot, style="dim italic")
            opts.append(Option(label, id=f"dev:{d['dev']}"))
        side.add_options(opts)
        if keep:
            for i in range(side.option_count):
                if side.get_option_at_index(i).id == keep:
                    side.highlighted = i
                    break

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        oid = event.option.id or ""
        if oid.startswith("dir:"):
            self.navigate(Path(oid[4:]))
            self.query_one(FileTable).focus()
        elif oid.startswith("dev:"):
            self.open_device(oid[4:], new_tab=False)

    def device_for(self, dev: str) -> dict | None:
        return next((d for d in self.devices if d["dev"] == dev), None)

    def open_device(self, dev: str, new_tab: bool) -> None:
        d = self.device_for(dev)
        if d is None:
            return
        if d["mount"]:
            self.go(Path(d["mount"]), new_tab)
        else:
            self.mount_device(dev, new_tab)

    def go(self, path: Path, new_tab: bool) -> None:
        if new_tab:
            self.open_in_new_tab(path)
        else:
            self.navigate(path)
        self.query_one(FileTable).focus()

    @work(thread=True)
    def mount_device(self, dev: str, new_tab: bool) -> None:
        r = subprocess.run(["udisksctl", "mount", "-b", dev], capture_output=True, text=True)
        if r.returncode == 0 and " at " in r.stdout:
            mp = r.stdout.strip().rsplit(" at ", 1)[1].rstrip(".")
            self.call_from_thread(self.after_mount, dev, Path(mp), new_tab)
        else:
            self.call_from_thread(self.notify, (r.stderr or r.stdout).strip() or "Mount failed", severity="error")

    def after_mount(self, dev: str, mp: Path, new_tab: bool) -> None:
        self.refresh_devices()
        self.go(mp, new_tab)

    @work(thread=True)
    def unmount_device(self, dev: str, mount: str) -> None:
        self.call_from_thread(self.leave_mount, mount)
        r = subprocess.run(["udisksctl", "unmount", "-b", dev], capture_output=True, text=True)
        if r.returncode == 0:
            self.call_from_thread(self.notify, f"Unmounted {dev}")
        else:
            self.call_from_thread(self.notify, (r.stderr or r.stdout).strip() or "Unmount failed", severity="error")
        self.call_from_thread(self.refresh_devices)

    def leave_mount(self, mount: str) -> None:
        for t in self.tabs:
            if t.path == Path(mount) or Path(mount) in t.path.parents:
                t.path, t.back, t.fwd = HOME, [], []
        self.refresh_all()

    def action_unmount_current(self) -> None:
        for d in self.devices:
            if d["mount"] and d["mount"] != "/" and (self.tab.path == Path(d["mount"]) or Path(d["mount"]) in self.tab.path.parents):
                self.unmount_device(d["dev"], d["mount"])
                return

    def on_sidebar_context(self, event: Sidebar.Context) -> None:
        oid = event.option_id or ""
        if oid.startswith("dir:"):
            path = Path(oid[4:])
            if event.middle:
                self.open_in_new_tab(path)
                return
            self.push_screen(
                MenuScreen([("open", "  Open"), ("tab", "  Open in new tab")], event.x, event.y),
                lambda a: self.go(path, a == "tab") if a else None,
            )
        elif oid.startswith("dev:"):
            dev = oid[4:]
            d = self.device_for(dev)
            if not d:
                return
            if event.middle:
                self.open_device(dev, new_tab=True)
                return
            items: list[tuple[str, str] | None] = [("open", "  Open"), ("tab", "  Open in new tab")]
            if d["mount"] and d["mount"] != "/":
                items.append(("umount", "  Unmount"))

            def done(a: str | None) -> None:
                if a == "open":
                    self.open_device(dev, False)
                elif a == "tab":
                    self.open_device(dev, True)
                elif a == "umount":
                    self.unmount_device(dev, d["mount"])

            self.push_screen(MenuScreen(items, event.x, event.y), done)

    # ---- file list events / selection
    def current_entry(self) -> Entry | None:
        table = self.query_one(FileTable)
        if table.row_count and 0 <= table.cursor_row < len(self.view):
            return self.view[table.cursor_row]
        return None

    def targets(self) -> list[Entry]:
        t = self.tab
        if t.selected:
            return [e for e in self.view if e.name in t.selected]
        cur = self.current_entry()
        return [cur] if cur else []

    def on_data_table_row_highlighted(self, event: DataTable.RowHighlighted) -> None:
        self.render_status()
        self.render_actions()

    def on_data_table_row_selected(self, event: DataTable.RowSelected) -> None:
        self.open_entry(event.cursor_row)

    def on_data_table_header_selected(self, event: DataTable.HeaderSelected) -> None:
        key = event.column_key.value
        if key in ("name", "size", "mod"):
            t = self.tab
            t.reverse = (not t.reverse) if t.sort == key else False
            t.sort = key
            self.refresh_listing()

    def on_file_table_clicked(self, event: FileTable.Clicked) -> None:
        t = self.tab
        table = self.query_one(FileTable)
        table.focus()
        if not (0 <= event.index < len(self.view)):
            return
        name = self.view[event.index].name
        if event.ctrl:
            t.selected ^= {name}
            t.anchor = event.index
        elif event.shift and t.anchor is not None:
            lo, hi = sorted((t.anchor, event.index))
            t.selected = {e.name for e in self.view[lo : hi + 1]}
        else:
            t.selected.clear()
            t.anchor = event.index
        table.move_cursor(row=event.index, animate=False)
        self.update_marks()

    def on_file_table_opened(self, event: FileTable.Opened) -> None:
        self.open_entry(event.index)

    def on_file_table_context(self, event: FileTable.Context) -> None:
        t = self.tab
        table = self.query_one(FileTable)
        table.focus()
        if event.index is not None and 0 <= event.index < len(self.view):
            name = self.view[event.index].name
            if name not in t.selected:
                t.selected.clear()
                self.update_marks()
            table.move_cursor(row=event.index, animate=False)
            self.show_menu(event.x, event.y, on_item=True)
        else:
            t.selected.clear()
            self.update_marks()
            self.show_menu(event.x, event.y, on_item=False)

    def action_context_menu(self) -> None:
        self.show_menu(10, 8, on_item=self.current_entry() is not None)

    def show_menu(self, x: int, y: int, on_item: bool) -> None:
        items: list[tuple[str, str] | None] = []
        if on_item:
            cur = self.current_entry()
            items += [("open", "  Open")]
            if cur and cur.is_dir and not self.tab.selected:
                items += [("tab", "  Open in new tab")]
            items += [None, ("cut", "  Cut"), ("copy", "  Copy")]
        if self.clip:
            items += [("paste", "  Paste")]
        if on_item:
            items += [None, ("rename", "  Rename"), ("delete", "  Move to Trash")]
        items += [None, ("new_folder", "  New folder"), ("new_file", "  New file"), ("refresh", "  Refresh")]
        while items and items[0] is None:
            items.pop(0)

        def done(a: str | None) -> None:
            if a == "open":
                cur = self.current_entry()
                if cur:
                    self.open_entry(self.view.index(cur))
            elif a == "tab":
                cur = self.current_entry()
                if cur:
                    self.open_in_new_tab(cur.path)
            elif a:
                getattr(self, f"action_{a}")()

        self.push_screen(MenuScreen(items, x, y), done)

    def update_marks(self) -> None:
        """Redraw selection marks without rebuilding the table."""
        t = self.tab
        table = self.query_one(FileTable)
        pri = self.primary
        for e in self.view:
            sel = e.name in t.selected
            icon = I_FOLDER if e.is_dir else (I_LINK if e.is_link else I_FILE)
            name = Text(f"{icon} {e.name}", no_wrap=True, overflow="ellipsis")
            if e.is_dir:
                name.stylize(f"bold {pri}", 0, 1)
            if sel:
                name.stylize(f"bold {pri}")
            table.update_cell(e.name, "mark", Text("✓" if sel else " ", style=f"bold {pri}"))
            table.update_cell(e.name, "name", name)
        self.render_status()
        self.render_actions()

    def action_toggle_select(self) -> None:
        cur = self.current_entry()
        if cur is None:
            return
        self.tab.selected ^= {cur.name}
        table = self.query_one(FileTable)
        row = table.cursor_row
        self.update_marks()
        if row + 1 < table.row_count:
            table.move_cursor(row=row + 1, animate=False)

    def action_select_all(self) -> None:
        t = self.tab
        t.selected = {e.name for e in self.view} if len(t.selected) != len(self.view) else set()
        self.update_marks()

    def open_entry(self, index: int) -> None:
        if not (0 <= index < len(self.view)):
            return
        e = self.view[index]
        if e.is_dir:
            self.navigate(e.path)
        elif self.pick in ("file", "files"):
            sel = [x.path for x in self.targets() if not x.is_dir] if self.pick == "files" and self.tab.selected else [e.path]
            self.finish_pick(sel)
        elif self.pick == "save":
            self.query_one("#savename", Input).value = e.name
            self.action_choose()
        else:
            try:
                subprocess.Popen(
                    ["xdg-open", str(e.path)], start_new_session=True,
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
            except OSError as exc:
                self.notify(str(exc), severity="error")

    # ---- drag and drop (inside the app)
    def path_for_action(self, action: str) -> Path | None:
        import re

        m = re.fullmatch(r"app\.(tab|crumb)\((\d+)\)", action)
        if not m:
            return None
        i = int(m.group(2))
        if m.group(1) == "tab":
            return self.tabs[i].path if i < len(self.tabs) else None
        parts = self.tab.path.parts
        return Path(*parts[: i + 1]) if i < len(parts) else None

    def target_for_row(self, row: int | None) -> Path | None:
        if row is not None and 0 <= row < len(self.view) and self.view[row].is_dir:
            return self.view[row].path
        return self.tab.path

    def drag_prepare(self, index: int, event: events.MouseDown) -> None:
        if 0 <= index < len(self.view):
            self.drag = {"index": index, "start": (event.screen_x, event.screen_y), "active": False, "paths": []}

    def drag_hover(self, event: events.MouseMove, target: Path | None, label: str, trash: bool = False) -> None:
        d = self.drag
        if not d:
            return
        if not d["active"]:
            dx = abs(event.screen_x - d["start"][0]) + abs(event.screen_y - d["start"][1])
            if dx < 2:
                return
            e = self.view[d["index"]]
            d["paths"] = [x.path for x in self.view if x.name in self.tab.selected] if e.name in self.tab.selected else [e.path]
            d["active"] = True
            self.screen.add_class("-dragging")
        n = len(d["paths"])
        what = d["paths"][0].name if n == 1 else f"{n} items"
        verb = "copy" if event.ctrl else ("trash" if trash else "move")
        text = f"{what} \u2192 {verb} to {label}" if target else f"{what} \u2014 no drop here"
        ghost = self.query_one("#ghost", Static)
        ghost.update(escape(text))
        ghost.styles.offset = (max(0, min(event.screen_x + 2, self.size.width - len(text) - 3)), min(event.screen_y + 1, self.size.height - 1))
        ghost.display = True

    def drag_end(self) -> None:
        self.drag = None
        self.screen.remove_class("-dragging")
        self.query_one("#ghost", Static).display = False
        self.drag_ended = time.monotonic()

    def drag_drop(self, event: events.MouseUp, target: Path | None, trash: bool = False) -> bool:
        d = self.drag
        if not d or not d["active"]:
            self.drag = None
            return False
        paths = d["paths"]
        self.drag_end()
        if target is None:
            return True
        if trash:
            fails = 0
            for p in paths:
                r = subprocess.run(["gio", "trash", str(p)], capture_output=True, text=True)
                fails += r.returncode != 0
            self.tab.selected.clear()
            self.refresh_listing()
            self.notify(f"Moved {len(paths) - fails} item(s) to Trash" + (f", {fails} failed" if fails else ""))
            return True
        copy = bool(event.ctrl)
        if not copy:
            paths = [p for p in paths if p.parent != target]
        if paths:
            self.tab.selected.clear()
            self.run_paste("copy" if copy else "cut", paths, target)
        return True

    def on_mouse_up(self, event: events.MouseUp) -> None:
        if self.drag:  # released over something that is not a drop target
            self.drag_end()

    # ---- picker mode
    def finish_pick(self, paths: list[Path]) -> None:
        if self.out is not None:
            try:
                self.out.write_text("\n".join(str(p) for p in paths) + "\n")
            except OSError as exc:
                self.notify(str(exc), severity="error")
                return
        self.picked = True
        self.exit()

    def action_choose(self) -> None:
        if not self.pick:
            return
        t = self.tab
        if self.pick == "dir":
            self.finish_pick([t.path])
        elif self.pick == "save":
            name = self.query_one("#savename", Input).value.strip()
            if not name or "/" in name:
                self.notify("Enter a file name", severity="error")
                return
            target = t.path / name
            if target.is_dir():
                self.notify("That name is a folder", severity="error")
            elif target.exists():
                self.push_screen(
                    ConfirmScreen(f"Replace '{escape(name)}'?"),
                    lambda ok: self.finish_pick([target]) if ok else None,
                )
            else:
                self.finish_pick([target])
        else:
            files = [e.path for e in self.targets() if not e.is_dir]
            if not files:
                self.notify("Select a file first", severity="warning")
            else:
                self.finish_pick(files if self.pick == "files" else files[:1])

    def action_cancel_pick(self) -> None:
        self.exit()

    def action_quit(self) -> None:
        self.exit()

    # ---- address / search
    def action_edit_address(self) -> None:
        addr = self.query_one("#addr", Input)
        self.query_one("#crumbs").display = False
        addr.display = True
        addr.value = str(self.tab.path)
        addr.focus()
        addr.action_select_all()

    def close_address(self) -> None:
        self.query_one("#addr").display = False
        self.query_one("#crumbs").display = True
        self.query_one(FileTable).focus()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "addr":
            raw = event.value.strip()
            if raw.startswith("file://"):
                raw = unquote(urlparse(raw).path)
            target = Path(os.path.expandvars(raw)).expanduser()
            if not target.is_absolute():
                target = self.tab.path / target
            self.close_address()
            if target.is_dir():
                self.navigate(target)
            elif target.exists():
                self.navigate(target.parent, select=target.name)
            else:
                self.notify(f"Not found: {target}", severity="error")
        elif event.input.id == "search":
            self.query_one(FileTable).focus()
        elif event.input.id == "savename":
            self.action_choose()

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "search" and event.value != self.tab.filter:
            self.tab.filter = event.value
            self.tab.cursor = None
            self.refresh_listing()

    def action_search(self) -> None:
        self.query_one("#search", Input).focus()

    def on_key(self, event: events.Key) -> None:
        if event.key == "escape" and self.drag:
            self.drag_end()
            return
        if event.key == "escape":
            if self.query_one("#addr").display:
                self.close_address()
            elif isinstance(self.focused, Input) and self.focused.id == "search":
                self.focused.value = ""
                self.query_one(FileTable).focus()
            elif self.tab.selected:
                self.tab.selected.clear()
                self.update_marks()
            elif self.pick and not isinstance(self.focused, Input):
                self.exit()

    # ---- file operations
    def action_copy(self) -> None:
        t = [e.path for e in self.targets()]
        if t:
            self.clip = ("copy", t)
            self.notify(f"Copied {len(t)} item(s)")
            self.render_status()
            self.render_actions()

    def action_cut(self) -> None:
        t = [e.path for e in self.targets()]
        if t:
            self.clip = ("cut", t)
            self.notify(f"Cut {len(t)} item(s)")
            self.render_status()
            self.render_actions()

    def action_paste(self) -> None:
        if not self.clip:
            return
        mode, items = self.clip
        self.run_paste(mode, list(items), self.tab.path)
        if mode == "cut":
            self.clip = None

    @work(thread=True)
    def run_paste(self, mode: str, items: list[Path], dest: Path) -> None:
        errors = 0
        done = 0
        for src in items:
            try:
                if not src.exists() and not src.is_symlink():
                    raise FileNotFoundError(src)
                tgt = dest / src.name
                if src.is_dir() and not src.is_symlink():
                    d = dest.resolve()
                    if d == src.resolve() or src.resolve() in d.parents:
                        raise ValueError(f"Can't put '{src.name}' into itself")
                if mode == "cut":
                    if tgt.resolve() == src.resolve():
                        continue
                    shutil.move(str(src), str(unique(tgt)))
                else:
                    tgt = unique(tgt)
                    if src.is_dir() and not src.is_symlink():
                        shutil.copytree(src, tgt, symlinks=True)
                    else:
                        shutil.copy2(src, tgt, follow_symlinks=False)
                done += 1
            except Exception as exc:  # noqa: BLE001
                errors += 1
                self.call_from_thread(self.notify, f"{src.name}: {exc}", severity="error")
        verb = "Moved" if mode == "cut" else "Pasted"
        self.call_from_thread(self.notify, f"{verb} {done} item(s)" + (f", {errors} failed" if errors else ""))
        self.call_from_thread(self.refresh_listing)

    def action_delete(self) -> None:
        targets = self.targets()
        if not targets:
            return
        what = f"'{targets[0].name}'" if len(targets) == 1 else f"{len(targets)} items"

        def done(ok: bool | None) -> None:
            if not ok:
                return
            fails = 0
            for e in targets:
                r = subprocess.run(["gio", "trash", str(e.path)], capture_output=True, text=True)
                if r.returncode != 0:
                    fails += 1
                    self.notify(f"{e.name}: {r.stderr.strip()}", severity="error")
            self.tab.selected.clear()
            self.refresh_listing()
            if not fails:
                self.notify(f"Moved {len(targets)} item(s) to Trash")

        self.push_screen(ConfirmScreen(f"Move {escape(what)} to Trash?"), done)

    def action_rename(self) -> None:
        cur = self.current_entry()
        if not cur:
            return

        def done(name: str | None) -> None:
            if not name or name == cur.name:
                return
            if "/" in name:
                self.notify("Name can't contain '/'", severity="error")
                return
            new = cur.path.with_name(name)
            if new.exists():
                self.notify(f"'{name}' already exists", severity="error")
                return
            try:
                cur.path.rename(new)
            except OSError as exc:
                self.notify(str(exc), severity="error")
                return
            self.tab.selected.clear()
            self.tab.cursor = name
            self.refresh_listing()

        self.push_screen(PromptScreen("Rename", cur.name), done)

    def _create(self, title: str, folder: bool) -> None:
        def done(name: str | None) -> None:
            if not name:
                return
            if "/" in name:
                self.notify("Name can't contain '/'", severity="error")
                return
            new = self.tab.path / name
            if new.exists():
                self.notify(f"'{name}' already exists", severity="error")
                return
            try:
                new.mkdir() if folder else new.touch()
            except OSError as exc:
                self.notify(str(exc), severity="error")
                return
            self.tab.cursor = name
            self.tab.filter = ""
            self.query_one("#search", Input).value = ""
            self.refresh_listing()

        self.push_screen(PromptScreen(title, "New folder" if folder else "New file.txt"), done)

    def action_new_folder(self) -> None:
        self._create("New folder", True)

    def action_new_file(self) -> None:
        self._create("New file", False)

    def action_toggle_hidden(self) -> None:
        self.show_hidden = not self.show_hidden
        self.refresh_listing()

    def on_resize(self, event: events.Resize) -> None:
        self.call_after_refresh(self.refresh_all)


def main() -> None:
    ap = argparse.ArgumentParser(prog="files-tui")
    ap.add_argument("path", nargs="?")
    ap.add_argument("--pick", choices=["file", "files", "dir", "save"], help="file chooser mode")
    ap.add_argument("--out", help="write the chosen path(s) here, one per line")
    ap.add_argument("--done", help="touch this file on exit")
    ap.add_argument("--name", default="", help="suggested file name for --pick save")
    args = ap.parse_args()
    for sig in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(sig, lambda *_: sys.exit(0))  # let `finally` write the done marker

    start, select, name = HOME, None, args.name
    if args.path:
        raw = args.path
        if raw.startswith("file://"):
            raw = unquote(urlparse(raw).path)
        p = Path(raw).expanduser()
        if p.is_dir():
            start = p
        elif args.pick == "save":
            start = p.parent if p.parent.is_dir() else HOME
            name = name or p.name
        elif p.exists():
            start, select = p.parent, p.name
    try:
        FilesApp(start.resolve(), select, args.pick, Path(args.out) if args.out else None, name).run()
    finally:
        if args.done:
            Path(args.done).touch()


if __name__ == "__main__":
    main()
