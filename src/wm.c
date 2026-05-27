/*
 * wm.c – window-manager core: event loop, focus, status-bar
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ncwm.h"

/* ── colour initialisation ───────────────────────────────────── */

static void init_colors(void)
{
    /* 64 terminal content pairs: TERM_PAIR(fg,bg) = fg*8+bg+1 */
    for (int fg = 0; fg < 8; fg++)
        for (int bg = 0; bg < 8; bg++)
            init_pair(TERM_PAIR(fg, bg), (short)fg, (short)bg);

    /* WM chrome */
    init_pair(CP_BORDER,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_FOCUSED,   COLOR_BLACK,  COLOR_CYAN);
    init_pair(CP_SB,        COLOR_WHITE,  COLOR_BLACK);
    init_pair(CP_SB_WIN,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_SB_ACTIVE, COLOR_BLACK,  COLOR_GREEN);
    init_pair(CP_SB_CLOCK,  COLOR_YELLOW, COLOR_BLACK);
}

/* ── help overlay ────────────────────────────────────────────── */

static void show_help(WM *wm)
{
    static const char *lines[] = {
        "ncwm " NCWM_VERSION " – keyboard shortcuts",
        "",
        "  F1 n      New shell window",
        "  F1 x      Close focused window",
        "  F1 Tab    Focus next window",
        "  F1 p      Focus previous window",
        "  F1 m      Move mode  (arrow keys + Enter/Esc to exit)",
        "  F1 r      Resize mode (arrow keys + Enter/Esc to exit)",
        "  F1 f      Toggle fullscreen",
        "  F1 Q      Quit ncwm",
        "  F1 ?      This help screen",
        "",
        "  Press any key to close this help.",
    };
    int nlines = (int)(sizeof(lines) / sizeof(lines[0]));

    int w = 50, h = nlines + 2;
    int y = (wm->rows - h) / 2;
    int x = (wm->cols - w) / 2;
    if (y < 0) y = 0;
    if (x < 0) x = 0;

    WINDOW *hw = newwin(h, w, y, x);
    if (!hw) return;

    wattr_set(hw, A_BOLD, CP_FOCUSED, NULL);
    box(hw, 0, 0);
    for (int i = 0; i < nlines; i++) {
        wmove(hw, i + 1, 2);
        wattr_set(hw, (i == 0) ? A_BOLD : 0, CP_FOCUSED, NULL);
        waddnstr(hw, lines[i], w - 4);
    }
    wrefresh(hw);

    /* wait for any key */
    nodelay(stdscr, FALSE);
    getch();
    nodelay(stdscr, TRUE);

    delwin(hw);

    /* force full redraw */
    clearok(stdscr, TRUE);
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) wm->wins[i]->vt.dirty = true;
}

/* ── status bar ──────────────────────────────────────────────── */

void wm_draw_sb(WM *wm)
{
    WINDOW *sb = wm->sb;
    werase(sb);
    wattr_set(sb, 0, CP_SB, NULL);
    wbkgd(sb, COLOR_PAIR(CP_SB));

    /* window list on the left */
    int col = 0;
    for (int i = 0; i < wm->nwins; i++) {
        Win *win = wm->wins[i];
        if (!win) continue;

        char label[MAX_TITLE + 8];
        snprintf(label, sizeof(label), " %d:%s ", win->id,
                 win->vt.title[0] ? win->vt.title : win->title);

        bool active = (i == wm->focus);
        int max_w = wm->cols / 2 - col - 1;
        if (max_w <= 0) break;
        wattr_set(sb, active ? A_BOLD : 0,
                  active ? CP_SB_ACTIVE : CP_SB_WIN, NULL);
        mvwaddnstr(sb, 0, col, label, max_w);
        col += (int)strlen(label);
        if (col >= wm->cols / 2) break;
    }

    /* mode indicator */
    if (wm->move_mode || wm->resize_mode || wm->pfx) {
        const char *mode = wm->move_mode   ? "[MOVE]"
                         : wm->resize_mode ? "[RESIZE]"
                                           : "[CMD]";
        wattr_set(sb, A_BOLD | A_BLINK, CP_SB_ACTIVE, NULL);
        mvwaddstr(sb, 0, wm->cols / 2 - 5, mode);
    }

    /* clock on the right */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M:%S", tm);

    int clk_col = wm->cols - (int)strlen(clk) - 2;
    if (clk_col > col) {
        wattr_set(sb, A_BOLD, CP_SB_CLOCK, NULL);
        mvwaddstr(sb, 0, clk_col, clk);
    }

    wnoutrefresh(sb);
}

/* ── focus management ────────────────────────────────────────── */

void wm_focus_idx(WM *wm, int idx)
{
    if (idx < 0 || idx >= wm->nwins || !wm->wins[idx]) return;

    /* unfocus old */
    if (wm->focus >= 0 && wm->focus < wm->nwins && wm->wins[wm->focus]) {
        wm->wins[wm->focus]->focused = false;
        wm->wins[wm->focus]->vt.dirty = true;
    }

    wm->focus = idx;
    Win *win = wm->wins[idx];
    win->focused = true;
    win->vt.dirty = true;
    top_panel(win->panel);
}

void wm_focus_next(WM *wm)
{
    if (wm->nwins == 0) return;
    int start = (wm->focus + 1) % wm->nwins;
    for (int i = 0; i < wm->nwins; i++) {
        int idx = (start + i) % wm->nwins;
        if (wm->wins[idx]) { wm_focus_idx(wm, idx); return; }
    }
}

void wm_focus_prev(WM *wm)
{
    if (wm->nwins == 0) return;
    int start = (wm->focus - 1 + wm->nwins) % wm->nwins;
    for (int i = 0; i < wm->nwins; i++) {
        int idx = (start - i + wm->nwins) % wm->nwins;
        if (wm->wins[idx]) { wm_focus_idx(wm, idx); return; }
    }
}

/* ── window management ───────────────────────────────────────── */

Win *wm_new_win(WM *wm, int x, int y, int w, int h,
                const char *title, const char *shell)
{
    if (wm->nwins >= MAX_WINDOWS) return NULL;

    /* find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm->wins[i]) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    /* clamp to screen */
    if (w > wm->cols) w = wm->cols;
    if (h > wm->rows - SB_HEIGHT) h = wm->rows - SB_HEIGHT;
    if (x + w > wm->cols) x = wm->cols - w;
    if (y + h > wm->rows - SB_HEIGHT) y = wm->rows - SB_HEIGHT - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    Win *win = win_create(wm->next_id++, x, y, w, h, title, shell);
    if (!win) return NULL;

    wm->wins[slot] = win;
    if (slot >= wm->nwins) wm->nwins = slot + 1;

    wm_focus_idx(wm, slot);
    return win;
}

void wm_close(WM *wm, int idx)
{
    if (idx < 0 || idx >= wm->nwins || !wm->wins[idx]) return;

    win_destroy(wm->wins[idx]);
    wm->wins[idx] = NULL;

    /* compact array */
    int new_n = 0;
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) new_n = i + 1;
    wm->nwins = new_n;

    /* re-focus */
    if (wm->nwins == 0) {
        wm->focus = -1;
    } else {
        /* focus the window before the closed one */
        int nf = idx - 1;
        if (nf < 0) nf = 0;
        while (nf < wm->nwins && !wm->wins[nf]) nf++;
        if (nf >= wm->nwins) nf = wm->nwins - 1;
        while (nf >= 0 && !wm->wins[nf]) nf--;
        wm->focus = -1;
        if (nf >= 0) wm_focus_idx(wm, nf);
    }

    /* force full redraw to clear the closed window's area */
    clearok(stdscr, TRUE);
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) wm->wins[i]->vt.dirty = true;
}

/* ── handle terminal resize ──────────────────────────────────── */

void wm_on_resize(WM *wm)
{
    endwin();
    refresh();
    clear();

    getmaxyx(stdscr, wm->rows, wm->cols);

    /* rebuild status bar */
    if (wm->sb) delwin(wm->sb);
    wm->sb = newwin(SB_HEIGHT, wm->cols, wm->rows - SB_HEIGHT, 0);

    /* clamp all windows to new screen */
    for (int i = 0; i < wm->nwins; i++) {
        Win *win = wm->wins[i];
        if (!win) continue;

        int nx = win->x, ny = win->y, nw = win->w, nh = win->h;
        if (nw > wm->cols) nw = wm->cols;
        if (nh > wm->rows - SB_HEIGHT) nh = wm->rows - SB_HEIGHT;
        if (nx + nw > wm->cols) nx = wm->cols - nw;
        if (ny + nh > wm->rows - SB_HEIGHT) ny = wm->rows - SB_HEIGHT - nh;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;

        /* move then resize */
        if (nx != win->x || ny != win->y)
            win_move(win, nx - win->x, ny - win->y, wm->cols, wm->rows - SB_HEIGHT);
        if (nw != win->w || nh != win->h)
            win_grow(win, nw - win->w, nh - win->h, wm->cols, wm->rows - SB_HEIGHT);
    }
}

/* ── SIGCHLD: reap dead children ─────────────────────────────── */

void wm_on_sigchld(WM *wm)
{
    pid_t pid;
    int   status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < wm->nwins; i++) {
            Win *win = wm->wins[i];
            if (win && win->pid == pid) {
                win->pid = -1;
                if (win->mfd >= 0) {
                    close(win->mfd);
                    win->mfd = -1;
                }
                /* auto-close the window */
                wm_close(wm, i);
                break;
            }
        }
    }
}

/* ── draw all ────────────────────────────────────────────────── */

void wm_draw_all(WM *wm)
{
    for (int i = 0; i < wm->nwins; i++) {
        Win *win = wm->wins[i];
        if (!win || !win->vt.dirty) continue;
        win_draw(win, win->focused);
    }

    /* composite all panels onto the virtual screen */
    update_panels();

    /* status bar (not managed by panels library) */
    wm_draw_sb(wm);

    /* position cursor in the focused window's terminal area;
     * this wnoutrefresh must come last so doupdate places
     * the hardware cursor correctly */
    Win *fw = (wm->focus >= 0 && wm->focus < wm->nwins)
              ? wm->wins[wm->focus] : NULL;
    if (fw && fw->vt.cvis) {
        wmove(fw->inner, fw->vt.cy, fw->vt.cx);
        wnoutrefresh(fw->inner);
    }

    doupdate();
}

/* ── keyboard input → key sequence bytes ─────────────────────── */

static int key_to_seq(Win *win, int key, char *out)
{
    const bool app = win ? win->vt.app_cursor : false;

    switch (key) {
    case KEY_UP:        return snprintf(out, 8, app ? "\033OA" : "\033[A");
    case KEY_DOWN:      return snprintf(out, 8, app ? "\033OB" : "\033[B");
    case KEY_RIGHT:     return snprintf(out, 8, app ? "\033OC" : "\033[C");
    case KEY_LEFT:      return snprintf(out, 8, app ? "\033OD" : "\033[D");
    case KEY_HOME:      return snprintf(out, 8, app ? "\033OH" : "\033[H");
    case KEY_END:       return snprintf(out, 8, app ? "\033OF" : "\033[F");
    case KEY_PPAGE:     return snprintf(out, 8, "\033[5~");
    case KEY_NPAGE:     return snprintf(out, 8, "\033[6~");
    case KEY_DC:        return snprintf(out, 8, "\033[3~");
    case KEY_IC:        return snprintf(out, 8, "\033[2~");
    case KEY_BACKSPACE: out[0] = 127; return 1;
    case KEY_BTAB:      return snprintf(out, 8, "\033[Z");
    case KEY_F(2):      return snprintf(out, 8, "\033OQ");
    case KEY_F(3):      return snprintf(out, 8, "\033OR");
    case KEY_F(4):      return snprintf(out, 8, "\033OS");
    case KEY_F(5):      return snprintf(out, 8, "\033[15~");
    case KEY_F(6):      return snprintf(out, 8, "\033[17~");
    case KEY_F(7):      return snprintf(out, 8, "\033[18~");
    case KEY_F(8):      return snprintf(out, 8, "\033[19~");
    case KEY_F(9):      return snprintf(out, 8, "\033[20~");
    case KEY_F(10):     return snprintf(out, 8, "\033[21~");
    case KEY_F(11):     return snprintf(out, 8, "\033[23~");
    case KEY_F(12):     return snprintf(out, 8, "\033[24~");
    case '\n': case '\r': out[0] = '\r'; return 1;
    default:
        if (key >= 1 && key <= 127) { out[0] = (char)key; return 1; }
        return 0;
    }
}

/* ── keyboard handler ────────────────────────────────────────── */

static void handle_key(WM *wm, int key)
{
    Win *fw = (wm->focus >= 0 && wm->focus < wm->nwins)
              ? wm->wins[wm->focus] : NULL;

    /* ── move mode ── */
    if (wm->move_mode) {
        if (key == 27 || key == '\n' || key == '\r' || key == WM_PREFIX) {
            wm->move_mode = false;
        } else if (fw) {
            switch (key) {
            case KEY_UP:    win_move(fw, 0, -1, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_DOWN:  win_move(fw, 0,  1, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_LEFT:  win_move(fw, -1, 0, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_RIGHT: win_move(fw,  1, 0, wm->cols, wm->rows - SB_HEIGHT); break;
            }
            fw->vt.dirty = true;
        }
        return;
    }

    /* ── resize mode ── */
    if (wm->resize_mode) {
        if (key == 27 || key == '\n' || key == '\r' || key == WM_PREFIX) {
            wm->resize_mode = false;
        } else if (fw) {
            switch (key) {
            case KEY_UP:    win_grow(fw, 0, -1, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_DOWN:  win_grow(fw, 0,  1, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_LEFT:  win_grow(fw, -1, 0, wm->cols, wm->rows - SB_HEIGHT); break;
            case KEY_RIGHT: win_grow(fw,  1, 0, wm->cols, wm->rows - SB_HEIGHT); break;
            }
            fw->vt.dirty = true;
        }
        return;
    }

    /* ── prefix mode (F1 was pressed) ── */
    if (wm->pfx) {
        wm->pfx = false;
        switch (key) {
        case WM_NEW: {
            const char *sh = getenv("SHELL");
            if (!sh) sh = "/bin/bash";
            /* cascade new windows */
            int off = wm->nwins * 2;
            int nw  = wm->cols * 2 / 3;
            int nh  = (wm->rows - SB_HEIGHT) * 2 / 3;
            wm_new_win(wm, off % (wm->cols / 4), off % ((wm->rows - SB_HEIGHT) / 4),
                       nw, nh, "shell", sh);
            break;
        }
        case WM_CLOSE:
            if (fw) wm_close(wm, wm->focus);
            break;
        case WM_NEXT:
            wm_focus_next(wm);
            break;
        case WM_PREV:
            wm_focus_prev(wm);
            break;
        case WM_MOVE:
            wm->move_mode = true;
            if (fw) fw->vt.dirty = true;
            break;
        case WM_RESIZE:
            wm->resize_mode = true;
            if (fw) fw->vt.dirty = true;
            break;
        case WM_FULL:
            if (fw) {
                if (fw->state == WS_MAX) win_restore(fw);
                else                     win_maximize(fw, wm->rows, wm->cols);
                fw->vt.dirty = true;
            }
            break;
        case WM_QUIT:
            wm->running = false;
            break;
        case WM_HELP:
            show_help(wm);
            break;
        case WM_PREFIX: {
            /* Double F1 → send F1 to active window */
            char seq[8];
            int n = snprintf(seq, sizeof(seq), "\033OP");
            if (fw) win_write_pty(fw, seq, n);
            break;
        }
        default: break;
        }
        return;
    }

    /* ── normal mode ── */
    if (key == WM_PREFIX) {
        wm->pfx = true;
        return;
    }

    /* pass keystrokes to focused window */
    if (fw && fw->mfd >= 0) {
        char seq[16];
        int n = key_to_seq(fw, key, seq);
        if (n > 0) win_write_pty(fw, seq, n);
    }
}

/* ── WM lifecycle ────────────────────────────────────────────── */

WM *wm_create(void)
{
    WM *wm = calloc(1, sizeof(*wm));
    if (!wm) return NULL;

    getmaxyx(stdscr, wm->rows, wm->cols);
    wm->focus   = -1;
    wm->running = true;
    wm->next_id = 1;

    init_colors();

    wm->sb = newwin(SB_HEIGHT, wm->cols, wm->rows - SB_HEIGHT, 0);
    if (!wm->sb) { free(wm); return NULL; }

    wbkgd(wm->sb, COLOR_PAIR(CP_SB));
    wrefresh(wm->sb);

    return wm;
}

void wm_destroy(WM *wm)
{
    if (!wm) return;
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) { win_destroy(wm->wins[i]); wm->wins[i] = NULL; }
    if (wm->sb) delwin(wm->sb);
    free(wm);
}

/* ── main event loop ─────────────────────────────────────────── */

void wm_run(WM *wm)
{
    while (wm->running) {
        /* handle SIGWINCH */
        if (g_resize) {
            g_resize = 0;
            wm_on_resize(wm);
            for (int i = 0; i < wm->nwins; i++)
                if (wm->wins[i]) wm->wins[i]->vt.dirty = true;
        }

        /* handle SIGCHLD */
        if (g_got_chld) {
            g_got_chld = 0;
            wm_on_sigchld(wm);
        }

        /* quit when all windows are closed */
        if (wm->nwins == 0 && wm->next_id > 1)
            break;

        /* build fd_set */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;

        for (int i = 0; i < wm->nwins; i++) {
            Win *win = wm->wins[i];
            if (win && win->mfd >= 0) {
                FD_SET(win->mfd, &rfds);
                if (win->mfd > maxfd) maxfd = win->mfd;
            }
        }

        struct timeval tv = { 0, 50000 }; /* 50 ms */
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* keyboard */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int key;
            while ((key = getch()) != ERR)
                handle_key(wm, key);
        }

        /* PTY output */
        for (int i = 0; i < wm->nwins; i++) {
            Win *win = wm->wins[i];
            if (!win || win->mfd < 0) continue;
            if (FD_ISSET(win->mfd, &rfds))
                win_read_pty(win);
        }

        /* render dirty windows + status bar */
        wm_draw_all(wm);
    }
}
