# ncwm

<img width="3840" height="2128" alt="image" src="https://github.com/user-attachments/assets/d69d4630-ea0e-495f-b93f-5ae617d4f07d" />


**ncwm** – a tiling/floating window manager that runs entirely in text-mode,
making it a lightweight replacement for desktop environments such as KDE or
LXQt when you want to work directly on a Linux virtual terminal (`tty1`,
`tty2`, …) without an X server or Wayland compositor.

Each window runs a real shell (or any programme) through a **pseudo-terminal
(PTY)** and renders its output through a built-in **VT100 / xterm terminal
emulator**.

---

## Features

* Multiple floating windows, each hosting a full terminal session
* VT100/ANSI emulation: SGR colours & attributes, cursor movement, erase,
  scroll regions, insert/delete lines/chars, alternate screen (`vim`, `htop`,
  `nano`, `bash` readline all work correctly)
* **Full mouse support** – click to focus, drag title bar to move, drag
  bottom-right corner to resize, double-click title bar to maximise/restore,
  click `[x]` button to close, click status-bar entry to switch windows
* **Borland C++-style dialog boxes** – `Ctrl-A c` opens a shell-chooser dialog;
  `Ctrl-A Q` asks for confirmation before quitting
* Ctrl-A-prefix key bindings for all WM operations (nothing captured during normal
  typing)
* Move and resize windows with arrow keys
* Maximise / restore individual windows
* Status bar showing open windows, active indicator, and a live clock
* Auto-closes a window when its child process exits
* Handles terminal resize (`SIGWINCH`) gracefully
* Runs on any colour terminal: real TTYs (`TERM=linux`), `xterm-256color`,
  `screen`, `tmux`, …

---

## Building

```
make
```

Requires **gcc**, **libncurses-dev** (≥ 6), and **libutil** (part of glibc on
Linux).  These are available on every major distribution:

```
# Debian / Ubuntu
sudo apt install build-essential libncurses-dev

# Fedora / RHEL
sudo dnf install gcc ncurses-devel
```

---

## Running

```
./ncwm            # uses $SHELL, or /bin/bash
./ncwm /bin/zsh   # use a specific shell
./ncwm --help
./ncwm --version
```

To use ncwm as a login session on `tty1`, add to `/etc/inittab` (SysV) or
create a systemd getty override, then exec `ncwm` from `~/.bash_profile` when
`$XDG_VTNR == 1`.

---

## Key Bindings

All WM commands are entered via a **two-key sequence** starting with **Ctrl-A**.
Outside of a WM command, every keystroke is forwarded directly to the focused
terminal window.

| Keys         | Action                                        |
|--------------|-----------------------------------------------|
| `Ctrl-A c`   | Open a new shell window (shell-chooser dialog)|
| `Ctrl-A x`   | Close the focused window                      |
| `Ctrl-A n`   | Focus the next window                         |
| `Ctrl-A p`   | Focus the previous window                     |
| `Ctrl-A m`   | Enter **move** mode (arrow keys + Esc)        |
| `Ctrl-A r`   | Enter **resize** mode (arrow keys + Esc)      |
| `Ctrl-A f`   | Toggle fullscreen for focused window          |
| `Ctrl-A Q`   | Quit ncwm (confirmation dialog)               |
| `Ctrl-A ?`   | Show in-app help overlay                      |
| `Ctrl-A Ctrl-A` | Send a literal Ctrl-A to the active window |

In move / resize mode, press **Enter** or **Esc** (or `Ctrl-A`) to return to
normal mode.

---

## Mouse Bindings

Mouse support is enabled automatically.  All actions operate on the window
under the pointer.

| Action                           | Result                              |
|----------------------------------|-------------------------------------|
| Left-click window                | Focus window, bring to front        |
| Left-click + drag title bar      | Move window                         |
| Double-click title bar           | Maximise / restore window           |
| Left-click `[x]` (title-bar)     | Close window                        |
| Left-click `[M]` (title-bar)     | Restore maximised window            |
| Left-click + drag `◿` corner     | Resize window                       |
| Left-click status-bar entry      | Focus the corresponding window      |

Dialog boxes accept mouse clicks on their buttons in addition to keyboard
input (`Tab`/arrows to switch focus, `Enter`/`Space` to confirm, `Esc` or
`N` to cancel).

---

## Project layout

```
ncurses-wm/
├── Makefile
└── src/
    ├── ncwm.h      shared types, constants, prototypes
    ├── main.c      entry point, signal setup, ncurses init
    ├── vt100.c     VT100/ANSI terminal emulator
    ├── window.c    per-window PTY, drawing, move / resize
    └── wm.c        event loop, focus, status bar
```

---

## Install

```
sudo make install          # installs to /usr/local/bin/ncwm
sudo make uninstall
```

Set `DESTDIR` for staging:

```
make install DESTDIR=/staging/root
```
