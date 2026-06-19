#!/usr/bin/env python3
"""PTY-driven workflow tests for OpusEdit.

These tests exercise the real terminal binary with key streams that resemble
human editing sessions. They intentionally assert on saved files rather than
screen escape output, because file state is the durable user-visible result.
"""

from __future__ import annotations

import os
import pty
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


CTRL_S = b"\x13"
CTRL_Q = b"\x11"
CTRL_N = b"\x0e"
CTRL_O = b"\x0f"
CTRL_B = b"\x02"
CTRL_C = b"\x03"
CTRL_F = b"\x06"
CTRL_G = b"\x07"
CTRL_P = b"\x10"
CTRL_R = b"\x12"
CTRL_W = b"\x17"
CTRL_D = b"\x04"
CTRL_X = b"\x18"
CTRL_V = b"\x16"
CTRL_Z = b"\x1a"
CTRL_Y = b"\x19"
ESC = b"\x1b"
ENTER = b"\r"
BACKSPACE = b"\x7f"
DEL = b"\x1b[3~"
HOME = b"\x1b[H"
END = b"\x1b[F"
LEFT = b"\x1b[D"
UP = b"\x1b[A"
DOWN = b"\x1b[B"
RIGHT = b"\x1b[C"
PAGE_UP = b"\x1b[5~"
PAGE_DOWN = b"\x1b[6~"
CTRL_UP = b"\x1b[1;5A"
CTRL_DOWN = b"\x1b[1;5B"
MOUSE_SCROLL_UP = b"\x1b[<64;1;1M"
MOUSE_SCROLL_DOWN = b"\x1b[<65;1;1M"


class EditorSession:
    def __init__(
        self,
        binary: Path,
        filename: Path | list[Path] | tuple[Path, ...] | None,
        name: str,
    ) -> None:
        self.binary = binary
        if filename is None:
            self.filenames: list[Path] = []
        elif isinstance(filename, (list, tuple)):
            self.filenames = list(filename)
        else:
            self.filenames = [filename]
        self.name = name
        self.master: int | None = None
        self.proc: subprocess.Popen[bytes] | None = None
        self.output = bytearray()
        self.reader: threading.Thread | None = None

    def __enter__(self) -> "EditorSession":
        master, slave = pty.openpty()
        env = os.environ.copy()
        env.setdefault("TERM", "xterm")
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        argv = [str(self.binary)]
        argv.extend(str(filename) for filename in self.filenames)
        self.proc = subprocess.Popen(
            argv,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            env=env,
        )
        os.close(slave)
        self.master = master
        self.reader = threading.Thread(target=self._drain_output, daemon=True)
        self.reader.start()
        time.sleep(0.15)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self.master is not None:
            try:
                os.close(self.master)
            except OSError:
                pass
        if self.reader:
            self.reader.join(timeout=1)

    def _drain_output(self) -> None:
        assert self.master is not None
        while True:
            try:
                chunk = os.read(self.master, 8192)
            except OSError:
                break
            if not chunk:
                break
            self.output.extend(chunk)

    def send(self, data: bytes | str, pause: float = 0.08) -> None:
        if isinstance(data, str):
            data = data.encode()
        assert self.master is not None
        self.assert_running()
        try:
            os.write(self.master, data)
        except OSError as exc:
            tail = bytes(self.output[-2000:]).decode(errors="replace")
            raise AssertionError(f"{self.name}: write failed\n{tail}") from exc
        time.sleep(pause)

    def assert_running(self) -> None:
        assert self.proc is not None
        if self.proc.poll() is not None:
            tail = bytes(self.output[-2000:]).decode(errors="replace")
            raise AssertionError(
                f"{self.name}: editor exited early with {self.proc.returncode}\n{tail}"
            )

    def assert_output_contains(self, needle: bytes | str) -> None:
        if isinstance(needle, str):
            needle = needle.encode()
        if needle not in self.output:
            tail = bytes(self.output[-4000:]).decode(errors="replace")
            raise AssertionError(
                f"{self.name}: output did not contain {needle!r}\n{tail}"
            )

    def wait_exit(self, timeout: float = 8.0) -> None:
        assert self.proc is not None
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            tail = bytes(self.output[-2000:]).decode(errors="replace")
            raise AssertionError(f"{self.name}: editor did not exit\n{tail}") from exc
        if self.proc.returncode != 0:
            tail = bytes(self.output[-2000:]).decode(errors="replace")
            raise AssertionError(
                f"{self.name}: editor exited {self.proc.returncode}\n{tail}"
            )


def assert_file(path: Path, expected: bytes) -> None:
    actual = path.read_bytes()
    if actual != expected:
        raise AssertionError(
            f"{path.name}: expected {expected!r}, got {actual!r}"
        )


class TextModel:
    def __init__(self) -> None:
        self.rows = [""]
        self.cx = 0
        self.cy = 0

    def insert_text(self, text: str) -> None:
        for ch in text:
            self.rows[self.cy] = (
                self.rows[self.cy][: self.cx] + ch + self.rows[self.cy][self.cx :]
            )
            self.cx += 1

    def newline(self) -> None:
        row = self.rows[self.cy]
        self.rows[self.cy] = row[: self.cx]
        self.rows.insert(self.cy + 1, row[self.cx :])
        self.cy += 1
        self.cx = 0

    def backspace(self) -> None:
        if self.cx > 0:
            row = self.rows[self.cy]
            self.rows[self.cy] = row[: self.cx - 1] + row[self.cx :]
            self.cx -= 1
        elif self.cy > 0:
            prev_len = len(self.rows[self.cy - 1])
            self.rows[self.cy - 1] += self.rows.pop(self.cy)
            self.cy -= 1
            self.cx = prev_len

    def delete(self) -> None:
        row = self.rows[self.cy]
        if self.cx < len(row):
            self.rows[self.cy] = row[: self.cx] + row[self.cx + 1 :]
        elif self.cy + 1 < len(self.rows):
            self.rows[self.cy] += self.rows.pop(self.cy + 1)

    def left(self) -> None:
        if self.cx > 0:
            self.cx -= 1
        elif self.cy > 0:
            self.cy -= 1
            self.cx = len(self.rows[self.cy])

    def right(self) -> None:
        if self.cx < len(self.rows[self.cy]):
            self.cx += 1
        elif self.cy + 1 < len(self.rows):
            self.cy += 1
            self.cx = 0

    def up(self) -> None:
        if self.cy > 0:
            self.cy -= 1
            self.cx = min(self.cx, len(self.rows[self.cy]))

    def down(self) -> None:
        if self.cy + 1 < len(self.rows):
            self.cy += 1
            self.cx = min(self.cx, len(self.rows[self.cy]))

    def home(self) -> None:
        self.cx = 0

    def end(self) -> None:
        self.cx = len(self.rows[self.cy])

    def bytes(self) -> bytes:
        return ("\n".join(self.rows) + "\n").encode()


def scenario_basic_edit_save_quit(binary: Path, root: Path) -> None:
    target = root / "basic.txt"
    with EditorSession(binary, target, "basic edit") as ed:
        ed.send(b"hello" + b"\x01" + ENTER + b"world")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"hello\nworld\n")


def scenario_fast_escape_preserves_following_keys(binary: Path, root: Path) -> None:
    target = root / "fast-escape.txt"
    with EditorSession(binary, target, "fast escape preserves keys") as ed:
        ed.send(b"abc")
        ed.send(ESC + b"iX", pause=0.2)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abcX\n")


def scenario_undo_redo(binary: Path, root: Path) -> None:
    target = root / "undo-redo.txt"
    with EditorSession(binary, target, "undo redo") as ed:
        ed.send(b"abc")
        ed.send(CTRL_Z)
        ed.send(CTRL_Y)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abc\n")


def scenario_undo_back_to_saved_quits_cleanly(binary: Path, root: Path) -> None:
    target = root / "undo-clean.txt"
    target.write_bytes(b"abc\n")
    with EditorSession(binary, target, "undo back to saved is clean") as ed:
        ed.send(b"X")
        ed.send(CTRL_Z)
        ed.send(CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abc\n")


def scenario_newline_undo_redo(binary: Path, root: Path) -> None:
    target = root / "newline-redo.txt"
    with EditorSession(binary, target, "newline undo redo") as ed:
        ed.send(ENTER)
        ed.send(CTRL_Z)
        ed.send(CTRL_Y)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"\n")


def scenario_command_trim_duplicate(binary: Path, root: Path) -> None:
    target = root / "trim-dup.txt"
    with EditorSession(binary, target, "trim duplicate command") as ed:
        ed.send(b"abc   ")
        ed.send(CTRL_D)
        ed.send(ESC, pause=0.2)
        ed.send(b":trim" + ENTER)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abc\nabc\n")


def scenario_visual_line_copy_paste(binary: Path, root: Path) -> None:
    target = root / "visual-paste.txt"
    with EditorSession(binary, target, "visual line paste") as ed:
        ed.send(b"one" + ENTER + b"two")
        ed.send(ESC, pause=0.2)
        ed.send(b"Vyp")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"one\ntwo\ntwo\n")


def scenario_replace_all(binary: Path, root: Path) -> None:
    target = root / "replace.txt"
    with EditorSession(binary, target, "replace all") as ed:
        ed.send(b"foo foo" + ENTER + b"bar")
        ed.send(CTRL_R)
        ed.send(b"foo" + ENTER)
        ed.send(b"qux" + ENTER)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"qux qux\nbar\n")


def scenario_search_then_insert(binary: Path, root: Path) -> None:
    target = root / "search.txt"
    with EditorSession(binary, target, "search then insert") as ed:
        ed.send(b"hay" + ENTER + b"needle")
        ed.send(CTRL_F)
        ed.send(b"needle" + ENTER)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"hay\n!needle\n")


def scenario_multi_buffer_open_save(binary: Path, root: Path) -> None:
    first = root / "first.txt"
    second = root / "second.txt"
    second.write_bytes(b"existing\n")
    with EditorSession(binary, first, "multi buffer open save") as ed:
        ed.send(b"new")
        ed.send(CTRL_S)
        ed.send(CTRL_O)
        ed.send(str(second) + "\r")
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"new\n")
    assert_file(second, b"!existing\n")


def scenario_multi_cursor_insert(binary: Path, root: Path) -> None:
    target = root / "multi-cursor.txt"
    with EditorSession(binary, target, "multi cursor insert") as ed:
        ed.send(b"aa" + ENTER + b"aa")
        ed.send(CTRL_UP)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"aa!\naa!\n")


def scenario_unsaved_close_confirmation(binary: Path, root: Path) -> None:
    target = root / "discarded.txt"
    with EditorSession(binary, target, "unsaved close confirmation") as ed:
        ed.send(b"discard me")
        ed.send(CTRL_W)
        ed.send(CTRL_W)
        ed.send(CTRL_W)
        ed.send(CTRL_Q)
        ed.wait_exit()
    if target.exists():
        raise AssertionError("discarded buffer unexpectedly wrote a file")


def scenario_unnamed_save_as_prompt(binary: Path, root: Path) -> None:
    target = root / "prompt-save-as.txt"
    with EditorSession(binary, None, "unnamed save-as prompt") as ed:
        ed.send(b"draft")
        ed.send(CTRL_S)
        ed.send(str(target) + "\r")
        ed.send(CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"draft\n")


def scenario_unicode_save_as_prompt(binary: Path, root: Path) -> None:
    target = root / "unicode-cafe-\u00e9.txt"
    with EditorSession(binary, None, "unicode save-as prompt") as ed:
        ed.send(b"draft")
        ed.send(CTRL_S)
        ed.send(str(target).encode("utf-8") + ENTER)
        ed.send(CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"draft\n")


def scenario_save_as_command_wq(binary: Path, root: Path) -> None:
    original = root / "command-original.txt"
    renamed = root / "command-renamed.txt"
    with EditorSession(binary, original, "command save-as wq") as ed:
        ed.send(b"renamed")
        ed.send(ESC, pause=0.2)
        ed.send(f":wq {renamed}".encode() + ENTER)
        ed.wait_exit()
    if original.exists():
        raise AssertionError(":wq <path> unexpectedly wrote the original file")
    assert_file(renamed, b"renamed\n")


def scenario_command_edit_force_open(binary: Path, root: Path) -> None:
    first = root / "force-open-first.txt"
    second = root / "force-open-second.txt"
    first.write_bytes(b"first\n")
    second.write_bytes(b"second\n")
    with EditorSession(binary, first, "command e force") as ed:
        ed.send(b"DIRTY")
        ed.send(ESC, pause=0.2)
        ed.send(f":e {second}".encode() + ENTER)
        ed.assert_running()
        ed.assert_output_contains("Use :e!")
        ed.send(f":e! {second}".encode() + ENTER)
        ed.send(b"i!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"first\n")
    assert_file(second, b"!second\n")


def scenario_command_quit_refuses_dirty(binary: Path, root: Path) -> None:
    target = root / "quit-refuses-dirty.txt"
    with EditorSession(binary, target, "command quit refuses dirty") as ed:
        ed.send(b"dirty")
        ed.send(ESC, pause=0.2)
        ed.send(b":q" + ENTER)
        ed.assert_running()
        ed.assert_output_contains("Use :q!")
        ed.send(b"i!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"dirty!\n")


def scenario_command_set_options_autoindent(binary: Path, root: Path) -> None:
    target = root / "set-options.txt"
    with EditorSession(binary, target, "set options") as ed:
        ed.send(b"    x")
        ed.send(ESC, pause=0.2)
        ed.send(b":set noautoindent" + ENTER)
        ed.send(b"i" + ENTER + b"y")
        ed.send(ESC, pause=0.2)
        ed.send(b":set autoindent" + ENTER)
        ed.send(b":set nonumber" + ENTER)
        ed.send(b":set number" + ENTER)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"    x\ny\n")


def scenario_command_goto_and_prompt(binary: Path, root: Path) -> None:
    target = root / "goto.txt"
    with EditorSession(binary, target, "goto command and prompt") as ed:
        ed.send(b"one" + ENTER + b"TWO" + ENTER + b"three")
        ed.send(ESC, pause=0.2)
        ed.send(b":goto 2" + ENTER)
        ed.send(b"i!")
        ed.send(ESC, pause=0.2)
        ed.send(CTRL_G)
        ed.send(b"3" + ENTER)
        ed.send(b"i?")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"one\n!TWO\n?three\n")


def scenario_command_buffer_close_force(binary: Path, root: Path) -> None:
    first = root / "buffer-close-first.txt"
    second = root / "buffer-close-second.txt"
    first.write_bytes(b"first\n")
    second.write_bytes(b"second\n")
    with EditorSession(binary, first, "command buffer close force") as ed:
        ed.send(CTRL_O)
        ed.send(str(second) + "\r")
        ed.send(b"!")
        ed.send(ESC, pause=0.2)
        ed.send(b":bd" + ENTER)
        ed.assert_running()
        ed.assert_output_contains("Use :bd!")
        ed.send(b":bd!" + ENTER)
        ed.send(b"i?")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"?first\n")
    assert_file(second, b"second\n")


def scenario_prompt_open_cancel_keeps_buffer(binary: Path, root: Path) -> None:
    target = root / "open-cancel.txt"
    with EditorSession(binary, target, "open prompt cancel") as ed:
        ed.send(b"base")
        ed.send(CTRL_O)
        ed.send(ESC, pause=0.2)
        ed.assert_running()
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"base!\n")


def scenario_navigation_editing_keys(binary: Path, root: Path) -> None:
    target = root / "navigation.txt"
    with EditorSession(binary, target, "navigation editing keys") as ed:
        ed.send(b"abcd" + ENTER + b"efgh")
        ed.send(HOME + DEL + END + BACKSPACE + UP + END + BACKSPACE)
        ed.send(PAGE_DOWN + PAGE_UP)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abc\nfg\n")


def scenario_charwise_selection_copy_paste(binary: Path, root: Path) -> None:
    target = root / "charwise-selection.txt"
    with EditorSession(binary, target, "charwise selection copy paste") as ed:
        ed.send(b"abcdef")
        ed.send(ESC, pause=0.2)
        ed.send(HOME)
        ed.send(b"v")
        ed.send(RIGHT)
        ed.send(b"y")
        ed.send(END)
        ed.send(b"p")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abcdefab\n")


def scenario_ctrl_clipboard_cut_paste(binary: Path, root: Path) -> None:
    target = root / "ctrl-clipboard.txt"
    with EditorSession(binary, target, "ctrl clipboard cut paste") as ed:
        ed.send(b"abc")
        ed.send(ESC, pause=0.2)
        ed.send(HOME)
        ed.send(b"v")
        ed.send(RIGHT)
        ed.send(CTRL_X)
        ed.send(CTRL_V)
        ed.send(END)
        ed.send(b"v")
        ed.send(HOME)
        ed.send(CTRL_C)
        ed.send(END)
        ed.send(CTRL_V)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"abcabc\n")


def scenario_replace_with_empty_string(binary: Path, root: Path) -> None:
    target = root / "replace-empty.txt"
    with EditorSession(binary, target, "replace with empty string") as ed:
        ed.send(b"banana")
        ed.send(CTRL_R)
        ed.send(b"na" + ENTER)
        ed.send(ENTER)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"ba\n")


def scenario_search_cancel_keeps_cursor(binary: Path, root: Path) -> None:
    target = root / "search-cancel.txt"
    with EditorSession(binary, target, "search cancel keeps cursor") as ed:
        ed.send(b"alpha")
        ed.send(CTRL_F)
        ed.send(b"alp")
        ed.send(ESC, pause=0.2)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"alpha!\n")


def scenario_search_arrow_cycle(binary: Path, root: Path) -> None:
    target = root / "search-cycle.txt"
    with EditorSession(binary, target, "search arrow cycle") as ed:
        ed.send(b"needle one" + ENTER + b"needle two" + ENTER + b"tail")
        ed.send(CTRL_F)
        ed.send(b"needle")
        ed.send(DOWN)
        ed.send(ENTER)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"needle one\n!needle two\ntail\n")


def scenario_save_prompt_cancel_keeps_editing(binary: Path, root: Path) -> None:
    unexpected = root / "cancelled-save.txt"
    with EditorSession(binary, None, "save prompt cancel keeps editing") as ed:
        ed.send(b"draft")
        ed.send(CTRL_S)
        ed.send(ESC, pause=0.2)
        ed.assert_running()
        ed.assert_output_contains("Save aborted")
        ed.send(b"!")
        ed.send(CTRL_Q)
        ed.send(CTRL_Q)
        ed.send(CTRL_Q)
        ed.wait_exit()
    if unexpected.exists():
        raise AssertionError("cancelled save unexpectedly wrote a file")


def scenario_open_directory_prompt_keeps_current_buffer(binary: Path, root: Path) -> None:
    target = root / "open-directory-current.txt"
    bad_dir = root / "not-a-file"
    bad_dir.mkdir()
    with EditorSession(binary, target, "open directory keeps current buffer") as ed:
        ed.send(b"safe")
        ed.send(CTRL_O)
        ed.send(str(bad_dir) + "\r")
        ed.assert_running()
        ed.assert_output_contains("not a regular file")
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"safe!\n")


def scenario_cli_multiple_files(binary: Path, root: Path) -> None:
    first = root / "cli-first.txt"
    second = root / "cli-second.txt"
    first.write_bytes(b"one\n")
    second.write_bytes(b"two\n")
    with EditorSession(binary, [first, second], "cli multiple files") as ed:
        ed.send(b"!")
        ed.send(CTRL_S)
        ed.send(CTRL_B)
        ed.send(b"?")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"?one\n")
    assert_file(second, b"!two\n")


def scenario_control_bytes_ignored(binary: Path, root: Path) -> None:
    target = root / "control-bytes.txt"
    with EditorSession(binary, target, "control bytes ignored") as ed:
        ed.send(b"A\x00\x1fB")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"AB\n")


def scenario_model_insert_navigation_stress(binary: Path, root: Path) -> None:
    target = root / "model-stress.txt"
    model = TextModel()

    def text(ed: EditorSession, value: str) -> None:
        ed.send(value)
        model.insert_text(value)

    def key(ed: EditorSession, value: bytes, apply) -> None:
        ed.send(value)
        apply()

    with EditorSession(binary, target, "model insert navigation stress") as ed:
        text(ed, "alpha")
        key(ed, LEFT, model.left)
        key(ed, LEFT, model.left)
        text(ed, "X")
        key(ed, RIGHT, model.right)
        text(ed, "Y")
        key(ed, END, model.end)
        key(ed, ENTER, model.newline)
        text(ed, "beta")
        key(ed, HOME, model.home)
        key(ed, DEL, model.delete)
        text(ed, "B")
        key(ed, END, model.end)
        key(ed, ENTER, model.newline)
        text(ed, "gamma")
        key(ed, UP, model.up)
        key(ed, HOME, model.home)
        text(ed, "pre-")
        key(ed, DOWN, model.down)
        key(ed, END, model.end)
        key(ed, BACKSPACE, model.backspace)
        text(ed, "!")
        key(ed, UP, model.up)
        key(ed, LEFT, model.left)
        key(ed, BACKSPACE, model.backspace)
        text(ed, "?")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, model.bytes())


def scenario_rapid_paste_many_lines(binary: Path, root: Path) -> None:
    target = root / "rapid-paste.txt"
    lines = [f"line-{i:03d}" for i in range(60)]
    payload = ENTER.join(line.encode() for line in lines)
    with EditorSession(binary, target, "rapid paste many lines") as ed:
        ed.send(payload, pause=0.4)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, ("\n".join(lines) + "\n").encode())


def run_git(args: list[str], cwd: Path) -> bool:
    try:
        subprocess.run(
            ["git", *args],
            cwd=cwd,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False
    return True


def scenario_git_tracked_file_edit(binary: Path, root: Path) -> None:
    if shutil.which("git") is None:
        return

    repo = root / "repo"
    repo.mkdir()
    tracked = repo / "tracked.txt"
    tracked.write_bytes(b"base\n")
    if not run_git(["init", "-q"], repo):
        return
    if not run_git(["config", "user.email", "opusedit@example.invalid"], repo):
        return
    if not run_git(["config", "user.name", "OpusEdit Tests"], repo):
        return
    if not run_git(["add", "tracked.txt"], repo):
        return
    if not run_git(["commit", "-q", "-m", "base"], repo):
        return

    with EditorSession(binary, tracked, "git tracked file edit") as ed:
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(tracked, b"!base\n")


def scenario_new_buffer_save_as_and_cycle(binary: Path, root: Path) -> None:
    first = root / "new-buffer-first.txt"
    second = root / "new-buffer-second.txt"
    with EditorSession(binary, first, "new buffer save-as and cycle") as ed:
        ed.send(b"one")
        ed.send(CTRL_S)
        ed.send(CTRL_N)
        ed.send(b"two")
        ed.send(CTRL_S)
        ed.send(str(second) + "\r")
        ed.send(CTRL_B)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"one!\n")
    assert_file(second, b"two\n")


def scenario_buffer_cycle_shortcuts(binary: Path, root: Path) -> None:
    first = root / "cycle-first.txt"
    second = root / "cycle-second.txt"
    first.write_bytes(b"one\n")
    second.write_bytes(b"two\n")
    with EditorSession(binary, first, "buffer cycle shortcuts") as ed:
        ed.send(CTRL_O)
        ed.send(str(second) + "\r")
        ed.send(b"!")
        ed.send(CTRL_S)
        ed.send(CTRL_B)
        ed.send(b"?")
        ed.send(CTRL_S)
        ed.send(CTRL_P)
        ed.send(b"#")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(first, b"?one\n")
    assert_file(second, b"!#two\n")


def scenario_command_find_alias(binary: Path, root: Path) -> None:
    target = root / "command-find.txt"
    with EditorSession(binary, target, "command find alias") as ed:
        ed.send(b"hay" + ENTER + b"needle")
        ed.send(ESC, pause=0.2)
        ed.send(b":find" + ENTER)
        ed.send(b"needle" + ENTER)
        ed.send(b"i!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"hay\n!needle\n")


def scenario_command_replace_and_help(binary: Path, root: Path) -> None:
    target = root / "command-replace.txt"
    with EditorSession(binary, target, "command replace and help") as ed:
        ed.send(b"red blue red")
        ed.send(ESC, pause=0.2)
        ed.send(b":help" + ENTER)
        ed.assert_output_contains("NORMAL: i insert")
        ed.send(b":replace" + ENTER)
        ed.send(b"red" + ENTER)
        ed.send(b"green" + ENTER)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"green blue green\n")


def scenario_multi_cursor_down_backspace(binary: Path, root: Path) -> None:
    target = root / "multi-cursor-down-backspace.txt"
    with EditorSession(binary, target, "multi cursor down backspace") as ed:
        ed.send(b"abc" + ENTER + b"abc")
        ed.send(UP)
        ed.send(CTRL_DOWN)
        ed.send(BACKSPACE)
        ed.send(b"!")
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, b"ab!\nab!\n")


def scenario_mouse_scroll_is_non_destructive(binary: Path, root: Path) -> None:
    target = root / "mouse-scroll.txt"
    lines = [f"scroll-{i:02d}" for i in range(30)]
    payload = ENTER.join(line.encode() for line in lines)
    with EditorSession(binary, target, "mouse scroll is non-destructive") as ed:
        ed.send(payload, pause=0.25)
        ed.send(MOUSE_SCROLL_DOWN + MOUSE_SCROLL_DOWN + MOUSE_SCROLL_UP)
        ed.send(CTRL_S + CTRL_Q)
        ed.wait_exit()
    assert_file(target, ("\n".join(lines) + "\n").encode())


SCENARIOS = [
    scenario_basic_edit_save_quit,
    scenario_fast_escape_preserves_following_keys,
    scenario_undo_redo,
    scenario_undo_back_to_saved_quits_cleanly,
    scenario_newline_undo_redo,
    scenario_command_trim_duplicate,
    scenario_visual_line_copy_paste,
    scenario_replace_all,
    scenario_search_then_insert,
    scenario_multi_buffer_open_save,
    scenario_multi_cursor_insert,
    scenario_unsaved_close_confirmation,
    scenario_unnamed_save_as_prompt,
    scenario_unicode_save_as_prompt,
    scenario_save_as_command_wq,
    scenario_command_edit_force_open,
    scenario_command_quit_refuses_dirty,
    scenario_command_set_options_autoindent,
    scenario_command_goto_and_prompt,
    scenario_command_buffer_close_force,
    scenario_prompt_open_cancel_keeps_buffer,
    scenario_navigation_editing_keys,
    scenario_charwise_selection_copy_paste,
    scenario_ctrl_clipboard_cut_paste,
    scenario_replace_with_empty_string,
    scenario_search_cancel_keeps_cursor,
    scenario_search_arrow_cycle,
    scenario_save_prompt_cancel_keeps_editing,
    scenario_open_directory_prompt_keeps_current_buffer,
    scenario_cli_multiple_files,
    scenario_control_bytes_ignored,
    scenario_model_insert_navigation_stress,
    scenario_rapid_paste_many_lines,
    scenario_git_tracked_file_edit,
    scenario_new_buffer_save_as_and_cycle,
    scenario_buffer_cycle_shortcuts,
    scenario_command_find_alias,
    scenario_command_replace_and_help,
    scenario_multi_cursor_down_backspace,
    scenario_mouse_scroll_is_non_destructive,
]


def main() -> int:
    binary = Path(os.environ.get("OPUSEDIT_BIN", "bin/opusedit")).resolve()
    if not binary.exists():
        print(f"missing editor binary: {binary}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="opusedit-workflows-") as tmp:
        root = Path(tmp)
        for scenario in SCENARIOS:
            scenario(binary, root)
            print(f"workflow: {scenario.__name__} ok")

    print(f"workflow_tests: {len(SCENARIOS)} scenarios passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
