# ncurses-wm

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
* F1-prefix key bindings for all WM operations (nothing captured during normal
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

All WM commands are entered via a **two-key sequence** starting with **F1**.
Outside of a WM command, every keystroke is forwarded directly to the focused
terminal window.

| Keys         | Action                                 |
|--------------|----------------------------------------|
| `F1 n`       | Open a new shell window                |
| `F1 x`       | Close the focused window               |
| `F1 Tab`     | Focus the next window                  |
| `F1 p`       | Focus the previous window              |
| `F1 m`       | Enter **move** mode (arrow keys + Esc) |
| `F1 r`       | Enter **resize** mode (arrow keys + Esc)|
| `F1 f`       | Toggle fullscreen for focused window   |
| `F1 Q`       | Quit ncwm                              |
| `F1 ?`       | Show in-app help overlay               |
| `F1 F1`      | Send a literal F1 to the active window |

In move / resize mode, press **Enter** or **Esc** (or `F1`) to return to
normal mode.

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
