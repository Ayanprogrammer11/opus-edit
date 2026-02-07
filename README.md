# OpusEdit

A lightweight, terminal-based text editor written in pure C. No external UI libraries — just `<termios.h>` and VT100 escape sequences.

![C](https://img.shields.io/badge/language-C11-blue)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

## Features

- **Zero dependencies** — no ncurses, no third-party libraries
- **Syntax highlighting** for C/C++ and Python (keywords, strings, numbers, comments)
- **Undo/redo** with intelligent grouping (`Ctrl-Z` / `Ctrl-Y`)
- **Incremental search** with live highlighting and arrow-key cycling
- **Find-and-replace** across the buffer
- **Flicker-free rendering** via double-buffered output (single `write()` per refresh)
- **Soft line wrap** with visual-only wrapping
- **Modal editing** with Normal/Insert/Command modes
- **Visual selection** (charwise/linewise) with copy/cut/paste
- **Multi-cursor editing** with vertical cursor add
- **Multi-buffer editing** with buffer cycling and close
- **Full cursor navigation** — arrows, Home/End, Page Up/Down
- **Status bar** — filename, modified indicator, filetype, cursor position
- **Safe file I/O** — permission preservation, `fsync` on save
- **Graceful signal handling** — `SIGWINCH` (resize), `SIGTERM`, `SIGINT`
- **Memory-safe teardown** — all allocations freed on exit
- **Line numbers** with toggle (`Ctrl-L` / `:set number`)
- **Go to line** (`Ctrl-G` / `:goto <n>`)
- **Auto-indent** on newline (`:set autoindent`)
- **Duplicate line** (`Ctrl-D` / `:dup`)
- **Trim trailing whitespace** (`:trim`)

## Building

Requires a C11 compiler (GCC or Clang) and a POSIX environment (macOS or Linux).

```sh
make            # default build (-O2)
make debug      # debug build (-g -O0, AddressSanitizer + UBSan)
make release    # optimized build (-O3)
make clean      # remove build artifacts
```

The binary is output to `bin/opusedit`.

## Usage

```sh
./bin/opusedit                  # open a new file
./bin/opusedit myfile.c         # open an existing file
./bin/opusedit a.c b.c          # open multiple files in buffers
```

## Modes

- **Insert** (default): typing edits the buffer, `Enter` inserts newline
- **Normal**: navigation and commands; `i` → Insert, `:` → Command
- **Command**: `:` prompt for commands; `Esc` cancels and returns to Normal

## Key Bindings

Mode switches:

| Key | Action |
|-----|--------|
| `Esc` | Normal mode |
| `i` | Insert mode |
| `:` | Command mode |

| Key | Action |
|-----|--------|
| `Ctrl-S` | Save (prompts for filename if new) |
| `Ctrl-Q` | Quit (press twice to force-quit with unsaved changes) |
| `Ctrl-N` | New buffer |
| `Ctrl-O` | Open file in new buffer |
| `Ctrl-F` | Incremental search |
| `Ctrl-R` | Find and replace |
| `Ctrl-B` | Next buffer |
| `Ctrl-P` | Previous buffer |
| `Ctrl-W` | Close buffer (press twice if unsaved changes) |
| `Ctrl-Z` | Undo (groups consecutive typing into chunks) |
| `Ctrl-Y` | Redo |
| `Ctrl-G` | Go to line |
| `Ctrl-D` | Duplicate line |
| `Ctrl-L` | Toggle line numbers |
| `Arrow keys` | Move cursor |
| `Home` / `End` | Jump to beginning / end of line |
| `Page Up` / `Page Down` | Scroll by screenful |
| `Backspace` / `Delete` | Delete character (Insert mode) |
| `Enter` | Insert newline (Insert mode) |

Note: Ctrl shortcuts are kept as **aliases** for convenience in Normal/Command.

### Visual Selection & Clipboard

| Key | Action |
|-----|--------|
| `v` | Start/stop charwise selection (Normal mode) |
| `V` | Start/stop linewise selection (Normal mode) |
| `y` | Copy selection or current line (Normal mode) |
| `d` | Cut selection or current line (Normal mode) |
| `p` | Paste (replaces selection if active) |
| `Ctrl-C` | Copy selection or current line |
| `Ctrl-X` | Cut selection or current line |
| `Ctrl-V` | Paste (replaces selection if active) |
| `Esc` | Clear selection |

### Multi-Cursor

| Key | Action |
|-----|--------|
| `Ctrl-↑` | Add cursor above |
| `Ctrl-↓` | Add cursor below |
| `Esc` | Clear extra cursors |

### Search Mode

While in search (`Ctrl-F`):

| Key | Action |
|-----|--------|
| Type text | Search incrementally |
| `→` / `↓` | Next match |
| `←` / `↑` | Previous match |
| `Enter` | Confirm and jump to match |
| `Escape` | Cancel and return to original position |

### Replace Mode

| Key | Action |
|-----|--------|
| Type text | Enter find and replacement strings |
| `Enter` | Confirm each prompt |
| `Escape` | Cancel and return to original position |

### Command Mode

Commands (case-insensitive):

| Command | Action |
|---------|--------|
| `:w` | Save |
| `:w <path>` | Save as |
| `:q` | Quit (warns if any buffer dirty) |
| `:q!` | Force quit |
| `:wq` | Save + quit |
| `:e <path>` | Open file in current buffer |
| `:e! <path>` | Force open, discard changes |
| `:bnext` / `:bn` | Next buffer |
| `:bprev` / `:bp` | Previous buffer |
| `:bd` / `:bd!` | Close buffer / force close |
| `:find` | Incremental search |
| `:replace` | Find and replace |
| `:goto <n>` / `:line <n>` | Go to line number |
| `:set number` / `:set nonumber` | Toggle line numbers |
| `:set autoindent` / `:set noautoindent` | Toggle auto-indent |
| `:trim` | Trim trailing whitespace |
| `:dup` | Duplicate current line |
| `:help` | Show command help |

## Project Structure

```
opus-edit/
├── Makefile            # Build system (all, clean, debug, release)
├── include/
│   ├── editor.h        # Core types: erow, editor_config, abuf, syntax DB
│   ├── terminal.h      # Raw mode, window size, signal handling
│   ├── input.h         # Keypress reading, VT100 escape parsing
│   ├── buffer.h        # Dynamic row array, text mutation operations
│   ├── output.h        # Double-buffered rendering, syntax highlighting
│   ├── file_io.h       # File read/write with error handling
│   ├── find.h          # Incremental search
│   └── undo.h          # Undo/redo operation types and API
└── src/
    ├── main.c           # Entry point and main loop
    ├── editor.c         # Global state, append-buffer, prompt, lifecycle
    ├── terminal.c       # termios raw mode, ioctl, signal handlers
    ├── input.c          # Escape sequence parser, command dispatch
    ├── buffer.c         # Row operations, tab expansion, serialization
    ├── output.c         # Rendering, status bar, C syntax highlighting
    ├── file_io.c        # Safe file open/save with fsync
    ├── find.c           # Incremental search with match highlighting
    └── undo.c           # Undo/redo stack with auto-grouping
```

## Architecture

| Module | Responsibility |
|--------|---------------|
| **terminal** | Raw mode enable/disable, `TIOCGWINSZ` window size, `SIGWINCH`/`SIGTERM` handling |
| **input** | Byte-level `read()`, multi-byte escape sequence collapsing, command dispatch |
| **buffer** | Dynamic array of `erow` structs, insert/delete/split, tab expansion, serialization |
| **output** | `abuf` double-buffering, row rendering with ANSI colors, status & message bars |
| **file_io** | `fopen` for reading, `open`+`write`+`fsync` for atomic-ish saves |
| **find** | Prompt-driven incremental search, highlight overlay, bidirectional cycling |
| **undo** | Operation stack with auto-grouping, inverse computation, cursor restoration |

## Cross-Platform Notes

- macOS: compiled with `-D_DARWIN_C_SOURCE`
- Linux: compiled with `-D_GNU_SOURCE`
- File sync: uses `fdatasync()` on Linux, `fsync()` on macOS
- `O_DSYNC` availability handled via `#ifdef`

## License

MIT License. See [LICENSE](LICENSE) for details.
