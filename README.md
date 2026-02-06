# OpusEdit

A lightweight, terminal-based text editor written in pure C. No external UI libraries — just `<termios.h>` and VT100 escape sequences.

![C](https://img.shields.io/badge/language-C11-blue)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

## Features

- **Zero dependencies** — no ncurses, no third-party libraries
- **Syntax highlighting** for C/C++ (keywords, types, strings, numbers, comments)
- **Undo/redo** with intelligent grouping (`Ctrl-Z` / `Ctrl-Y`)
- **Incremental search** with live highlighting and arrow-key cycling
- **Flicker-free rendering** via double-buffered output (single `write()` per refresh)
- **Full cursor navigation** — arrows, Home/End, Page Up/Down
- **Status bar** — filename, modified indicator, filetype, cursor position
- **Safe file I/O** — permission preservation, `fsync` on save
- **Graceful signal handling** — `SIGWINCH` (resize), `SIGTERM`, `SIGINT`
- **Memory-safe teardown** — all allocations freed on exit

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
```

## Key Bindings

| Key | Action |
|-----|--------|
| `Ctrl-S` | Save (prompts for filename if new) |
| `Ctrl-Q` | Quit (press twice to force-quit with unsaved changes) |
| `Ctrl-F` | Incremental search |
| `Ctrl-Z` | Undo (groups consecutive typing into chunks) |
| `Ctrl-Y` | Redo |
| `Arrow keys` | Move cursor |
| `Home` / `End` | Jump to beginning / end of line |
| `Page Up` / `Page Down` | Scroll by screenful |
| `Backspace` / `Delete` | Delete character |
| `Enter` | Insert newline |

### Search Mode

While in search (`Ctrl-F`):

| Key | Action |
|-----|--------|
| Type text | Search incrementally |
| `→` / `↓` | Next match |
| `←` / `↑` | Previous match |
| `Enter` | Confirm and jump to match |
| `Escape` | Cancel and return to original position |

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
