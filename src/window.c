/*
 * window.c – per-window PTY creation, drawing, move/resize for ncwm
 */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ncwm.h"

/* inner terminal area dimensions */
#define INNER_W(win)  ((win)->w - 2)
#define INNER_H(win)  ((win)->h - 2)

/* ── helpers ─────────────────────────────────────────────────── */

/* Draw the window border and title into win->outer */
static void draw_chrome(Win *win, bool focused)
{
    wbkgd(win->outer, COLOR_PAIR(focused ? CP_FOCUSED : CP_BORDER));
    wattr_set(win->outer, 0, focused ? CP_FOCUSED : CP_BORDER, NULL);
    box(win->outer, 0, 0);

    /* title in the top border line */
    int max_title = win->w - 4;
    char buf[MAX_TITLE + 8];
    snprintf(buf, sizeof(buf), " %d:%s ", win->id, win->title);
    int tlen = (int)strlen(buf);
    if (tlen > max_title) tlen = max_title;

    int tx = (win->w - tlen) / 2;
    if (tx < 1) tx = 1;

    wattr_set(win->outer, focused ? A_BOLD : 0,
              focused ? CP_FOCUSED : CP_BORDER, NULL);
    mvwaddnstr(win->outer, 0, tx, buf, tlen);

    /* mode indicator in top-right corner */
    if (win->state == WS_MAX) {
        wattr_set(win->outer, A_BOLD,
                  focused ? CP_FOCUSED : CP_BORDER, NULL);
        mvwaddstr(win->outer, 0, win->w - 4, "[M]");
    }
}

/* ── public API ──────────────────────────────────────────────── */

Win *win_create(int id, int x, int y, int w, int h,
                const char *title, const char *shell)
{
    if (w < MIN_WIN_W || h < MIN_WIN_H) return NULL;

    Win *win = calloc(1, sizeof(*win));
    if (!win) return NULL;

    win->id    = id;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->state = WS_NORMAL;
    strncpy(win->title, title ? title : "shell", MAX_TITLE - 1);

    /* create ncurses windows */
    win->outer = newwin(h, w, y, x);
    if (!win->outer) { free(win); return NULL; }

    win->inner = derwin(win->outer, INNER_H(win), INNER_W(win), 1, 1);
    if (!win->inner) { delwin(win->outer); free(win); return NULL; }

    win->panel = new_panel(win->outer);
    if (!win->panel) {
        delwin(win->inner); delwin(win->outer); free(win); return NULL;
    }

    scrollok(win->inner, FALSE);
    keypad(win->inner, FALSE);

    /* initialise VT100 */
    memset(&win->vt, 0, sizeof(win->vt));
    vt_init(&win->vt, INNER_H(win), INNER_W(win));

    /* spawn child process in a PTY */
    struct winsize ws = {
        .ws_row = (unsigned short)INNER_H(win),
        .ws_col = (unsigned short)INNER_W(win),
    };

    win->pid = forkpty(&win->mfd, NULL, NULL, &ws);
    if (win->pid < 0) {
        /* PTY creation failed – show error in window */
        win->mfd = -1;
        win->pid = -1;
    } else if (win->pid == 0) {
        /* child */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        const char *sh = shell ? shell : "/bin/sh";
        char *const args[] = { (char *)sh, NULL };
        execvp(sh, args);
        /* exec failed */
        _exit(127);
    } else {
        /* parent: set master fd non-blocking */
        int flags = fcntl(win->mfd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(win->mfd, F_SETFL, flags | O_NONBLOCK);
    }

    return win;
}

void win_destroy(Win *win)
{
    if (!win) return;

    /* kill child process */
    if (win->pid > 0) {
        kill(win->pid, SIGHUP);
    }
    if (win->mfd >= 0) close(win->mfd);

    /* free VT */
    vt_free(&win->vt);

    /* remove panel + windows */
    if (win->panel) del_panel(win->panel);
    if (win->inner) delwin(win->inner);
    if (win->outer) delwin(win->outer);

    free(win);
}

void win_draw(Win *win, bool focused)
{
    draw_chrome(win, focused);

    /* render terminal content into inner window (derwin shares outer's buffer) */
    vt_render(&win->vt, win->inner);

    /* mark outer window as touched so update_panels() picks up all changes */
    touchwin(win->outer);
}

/* Move the outer window by (dx, dy), clamped to screen */
void win_move(Win *win, int dx, int dy, int max_x, int max_y)
{
    int nx = win->x + dx;
    int ny = win->y + dy;

    /* clamp */
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx + win->w > max_x) nx = max_x - win->w;
    if (ny + win->h > max_y) ny = max_y - win->h;

    win->x = nx; win->y = ny;
    move_panel(win->panel, ny, nx);
    win->vt.dirty = true;
}

/* Resize by (dw, dh), clamped to screen and minimum size */
void win_grow(Win *win, int dw, int dh, int max_x, int max_y)
{
    int nw = win->w + dw;
    int nh = win->h + dh;

    if (nw < MIN_WIN_W) nw = MIN_WIN_W;
    if (nh < MIN_WIN_H) nh = MIN_WIN_H;
    if (win->x + nw > max_x) nw = max_x - win->x;
    if (win->y + nh > max_y) nh = max_y - win->y;

    if (nw == win->w && nh == win->h) return;

    win->w = nw; win->h = nh;

    /* recreate ncurses windows */
    del_panel(win->panel);
    delwin(win->inner);
    delwin(win->outer);

    win->outer = newwin(nh, nw, win->y, win->x);
    win->inner = derwin(win->outer, INNER_H(win), INNER_W(win), 1, 1);
    win->panel = new_panel(win->outer);

    scrollok(win->inner, FALSE);

    /* resize VT and notify PTY */
    vt_resize(&win->vt, INNER_H(win), INNER_W(win));

    if (win->mfd >= 0) {
        struct winsize ws = {
            .ws_row = (unsigned short)INNER_H(win),
            .ws_col = (unsigned short)INNER_W(win),
        };
        ioctl(win->mfd, TIOCSWINSZ, &ws);
    }
    if (win->pid > 0)
        kill(win->pid, SIGWINCH);
}

void win_maximize(Win *win, int rows, int cols)
{
    if (win->state == WS_MAX) return;
    win->sx = win->x; win->sy = win->y;
    win->sw = win->w; win->sh = win->h;
    win->state = WS_MAX;
    win_grow(win, cols - win->w, rows - SB_HEIGHT - win->h,
             cols, rows - SB_HEIGHT);
    win_move(win, -win->x, -win->y, cols, rows - SB_HEIGHT);
}

void win_restore(Win *win)
{
    if (win->state != WS_MAX) return;
    win->state = WS_NORMAL;
    win_move(win, win->sx - win->x, win->sy - win->y,
             win->sx + win->sw, win->sy + win->sh);
    win_grow(win, win->sw - win->w, win->sh - win->h,
             win->sx + win->sw, win->sy + win->sh);
    win->x = win->sx; win->y = win->sy;
    win->w = win->sw; win->h = win->sh;
}

/* Read available PTY output and feed to VT100 */
void win_read_pty(Win *win)
{
    if (win->mfd < 0) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(win->mfd, buf, sizeof(buf))) > 0) {
        vt_process(&win->vt, buf, (int)n);
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* PTY closed */
        close(win->mfd);
        win->mfd = -1;
    }
}

void win_write_pty(Win *win, const char *buf, int n)
{
    if (win->mfd < 0 || n <= 0) return;
    int written = 0;
    while (written < n) {
        ssize_t r = write(win->mfd, buf + written, (size_t)(n - written));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += (int)r;
    }
}
