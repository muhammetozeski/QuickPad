# QuickPad

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D6)
![Language](https://img.shields.io/badge/language-C17-555555)
![Runtime](https://img.shields.io/badge/runtime-none%20(no%20CRT)-2E7D32)
![Architecture](https://img.shields.io/badge/arch-x64-6A1B9A)
![Release](https://img.shields.io/github/v/release/muhammetozeski/QuickPad)

QuickPad is a plain text editor for Windows written in C against the Win32 API. Its goal is to show a
file as soon as it is opened. A small resident process keeps a number of editor windows created,
drawn and hidden, so opening a file loads the text into one of them and makes it visible instead of
starting a program and building a window.

The executable is about 500 KB, has no installer and links no C runtime library; it depends only on
libraries that are part of Windows.

## Features

**Editing**
- New, Open (several files at once), Reload from Disk, Save, Save As with a choice of encoding, Close
- Files dropped on a window are opened
- Undo and redo; consecutive typing, backspaces and deletes are undone as one step
- Cut, copy, paste, delete, select all, and a context menu in the text area
- Find, Find Next, Find Previous, Replace and Replace All, with match case and whole word options
- Go To Line, insert time and date
- Duplicate, delete, move up, move down and join lines
- Uppercase and lowercase following the user's language, trim trailing whitespace
- Word wrap, optional auto indent, tab size 2, 4 or 8
- Word-wise caret movement and deletion with Ctrl, double-click word selection, drag selection that
  scrolls past the window edge, vertical and horizontal mouse wheel
- Input method editor support for languages such as Chinese and Japanese

**Files**
- Detects UTF-8 and UTF-16 (little and big endian, with or without a byte order mark) and falls back
  to the system ANSI code page
- Saves in the file's own encoding and line ending style (CRLF, LF or CR); both can be changed from
  the Format menu for the next save
- Open Containing Folder and Copy Full Path
- Writes to a temporary file first and swaps it in, so a failed save leaves the original intact
- Files of 1 MB and more are memory mapped while loading
- Asks before discarding unsaved changes; a modified document shows `*` in the title
- Opening a file that is already open brings its window forward

**Opening speed**
- A resident host process with a notification area icon receives every later launch
- A pool of drawn, cloaked editor windows (30 by default, 0 to 100) is kept ready; the pool is
  refilled only after windows have stopped opening and closing for five seconds
- Explorer opens associated files through a small in-process shell extension that hands the paths to
  the running host, so no process is started for each file
- Optional start with Windows: asked on the first run and changeable under **Settings > Start with
  Windows** or in the notification area menu

**Appearance**
- Always dark: title bar, menu bar, scroll bars, text area and dialogs
- Comic Sans MS by default, with Courier New where it is not installed; any font can be chosen, and
  proportional fonts are laid out with the width of each glyph
- Text size from 10 to 500 percent with Ctrl+Plus, Ctrl+Minus, Ctrl+0 or Ctrl and the mouse wheel
- Optional status bar with line, column, text size, line ending and encoding
- Always on top and full screen
- Window size is remembered

Features that are not used cost nothing: the status bar is off until it is turned on, auto indent is
off, and a window's menus are built from one shared template the first time they are opened.

## Security Architecture

- **No elevation.** The manifest requests `asInvoker`. Everything QuickPad writes is under the
  current user: its own folder and `HKEY_CURRENT_USER`.
- **Registry use is limited and visible.** `QuickPad.exe --register` writes the file association
  keys under `HKCU\Software\Classes`, the `Software\QuickPad\Capabilities` entry for Default apps,
  and the COM class of the shell extension. Start with Windows is a single `QuickPad` value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. QuickPad never changes the default app for a
  file type; Windows leaves that choice to the user.
- **Inter-process input is checked.** Launches hand paths to the host with `WM_COPYDATA`. The host
  accepts only its two message kinds and checks the size, alignment and termination of every path
  before copying it.
- **The shell extension is small.** `QuickPadShell.dll` runs inside Explorer. It uses only kernel32,
  user32 and ole32, opens no files and does no parsing; it collects the selected paths and sends them
  to the host, or starts `QuickPad.exe` when no host runs. It is embedded in the executable and
  written next to it by `--register`.
- **System libraries are loaded from System32.** Libraries loaded on demand use
  `LOAD_LIBRARY_SEARCH_SYSTEM32`, so a DLL placed next to the executable is not picked up instead.
- **Saving does not corrupt files.** Text is written to a temporary file and swapped in with
  `ReplaceFileW`, which keeps the original's attributes and permissions.
- **Signed releases.** The released executable carries an Authenticode signature.
- **No network access and no telemetry.**

## Installation

1. Download `QuickPad.exe` from the [latest release](https://github.com/muhammetozeski/QuickPad/releases/latest)
   and put it in a folder of its own that you can write to. `QuickPad.ini` and `QuickPadShell.dll`
   are created next to it.
2. Run `QuickPad.exe`. On the first run it asks whether it should start with Windows.
3. To open text files from Explorer with QuickPad, run once:

   ```
   QuickPad.exe --register
   ```

   Then choose QuickPad for the file types you want under **Settings > Apps > Default apps**. Run
   `--register` again after moving the folder.

The executable is digitally signed; to let Windows verify the signature, run `Install-Certificate.cmd`
from `SignatureTrust.zip` in the release once. The signature does not remove the SmartScreen warning
shown for downloaded programs that are not yet widely used.

### Command line

| Command | Effect |
|---|---|
| `QuickPad.exe [files]` | Opens the files, or an empty window, in the running host (or starts one) |
| `QuickPad.exe --background` | Starts the host without opening a window |
| `QuickPad.exe --register` | Registers the file associations and writes the shell extension |

### Keyboard shortcuts

| Keys | Command |
|---|---|
| Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S, Ctrl+W | New, Open, Save, Save As, Close |
| Ctrl+Z, Ctrl+Y | Undo, Redo |
| Ctrl+F, F3, Shift+F3 | Find, Find Next, Find Previous |
| Ctrl+H, Ctrl+G | Replace, Go To Line |
| Ctrl+A, F5 | Select All, Time/Date |
| Ctrl+D, Ctrl+Shift+K, Ctrl+J | Duplicate Line, Delete Line, Join Lines |
| Alt+Up, Alt+Down | Move Line Up, Move Line Down |
| Ctrl+Shift+U, Ctrl+U | Uppercase, Lowercase |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0, Ctrl+wheel | Zoom In, Zoom Out, Restore Default Zoom |
| F11 | Full Screen |

## Building

Requirements: Visual Studio with the x64 C++ build tools and a Windows SDK.

```powershell
./build.ps1                      # bin\QuickPad.exe
./build.ps1 -Test                # also builds the tools in tests\ and runs the unit tests
./build.ps1 -Release             # also copies the executable to publish\
./build.ps1 -VcVars "<path>\vcvars64.bat"   # for an installation vswhere.exe does not find
```

The running host locks `bin\QuickPad.exe`; exit it from the notification area menu before building.

### Layout

| Path | Contents |
|---|---|
| `src/main.c` | Entry point, command line, handing launches to the host |
| `src/host.c` | Resident host window, notification area icon, message loop |
| `src/editor.c` | Editor windows, window pool, menus, file dialogs, find and replace |
| `src/textview.c` | The text editing control |
| `src/document.c`, `layout.c`, `history.c`, `search.c` | Gap buffer with line index, glyph width layout and word wrap, undo history, search |
| `src/text.c`, `fileio.c` | Encoding detection and conversion, reading and saving files |
| `src/register.c`, `startup.c`, `settings.c`, `theme.c` | File associations, start with Windows, `QuickPad.ini`, dark theme |
| `src/lazyload.c`, `nocrt.c` | On-demand binding of system libraries, memory functions without the C runtime |
| `shell/QuickPadShell.c` | The Explorer open command |
| `tests/` | Unit tests and development tools (window snapshot, command sender, open benchmark) |
