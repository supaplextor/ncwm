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

    /* Borland C++ style dialog chrome */
    init_pair(CP_DLG,         COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_DLG_TITLE,   COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_DLG_BTN,     COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_DLG_BTN_SEL, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_DLG_SHADOW,  COLOR_BLACK, COLOR_BLACK);
    init_pair(CP_DLG_INPUT,   COLOR_BLACK, COLOR_CYAN);
}

/* ── help overlay (Borland C++ style) ───────────────────────── */

static void show_help(WM *wm)
{
    static const char *lines[] = {
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
        "  Mouse: click window to focus",
        "  Mouse: drag title bar to move",
        "  Mouse: drag [+] corner to resize",
        "  Mouse: double-click title to maximise/restore",
        "  Mouse: click [x] to close, [M] to restore",
        "  Mouse: click status-bar entry to focus",
        "",
        "  Press any key to close.",
    };
    int nlines = (int)(sizeof(lines) / sizeof(lines[0]));

    int w = 52;
    for (int i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        if (l + 4 > w) w = l + 4;
    }
    int h = nlines + 2;   /* top border + rows + bottom border */
    int y = (wm->rows - h) / 2;
    int x = (wm->cols - w) / 2;
    if (y < 0) y = 0;
    if (x < 0) x = 0;

    WINDOW *hw = newwin(h, w, y, x);
    if (!hw) return;

    wbkgd(hw, COLOR_PAIR(CP_DLG));
    wattr_set(hw, A_BOLD, CP_DLG_TITLE, NULL);
    box(hw, 0, 0);

    /* title in top border */
    const char *title = " ncwm " NCWM_VERSION " -- Help ";
    int tlen = (int)strlen(title);
    if (tlen < w - 2) mvwaddnstr(hw, 0, (w - tlen) / 2, title, tlen);

    for (int i = 0; i < nlines; i++) {
        if (lines[i][0] == '\0') continue;
        wattr_set(hw, 0, CP_DLG, NULL);
        mvwaddnstr(hw, i + 1, 2, lines[i], w - 4);
    }
    wrefresh(hw);

    nodelay(stdscr, FALSE);
    getch();
    nodelay(stdscr, TRUE);

    delwin(hw);
    clearok(stdscr, TRUE);
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) wm->wins[i]->vt.dirty = true;
}

/* ── Borland C++-style Yes/No confirmation dialog ────────────── */

static bool dialog_confirm(WM *wm, const char *title, const char *msg)
{
    int msg_len  = (int)strlen(msg);
    int titl_len = (int)strlen(title);
    int inner_w  = msg_len > titl_len ? msg_len : titl_len;
    if (inner_w < 16) inner_w = 16;   /* minimum to fit two buttons */
    int dw = inner_w + 4;
    int dh = 6;
    int dy = (wm->rows - dh) / 2;
    int dx = (wm->cols - dw) / 2;
    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;
    if (dx + dw > wm->cols) dw = wm->cols - dx;
    if (dy + dh > wm->rows) dh = wm->rows - dy;

    /* layout:
     * row 0  ┌─── Title ────┐   (CP_DLG_TITLE border)
     * row 1  │              │
     * row 2  │  message     │   (CP_DLG body)
     * row 3  │              │
     * row 4  │  [ Yes ] [No]│   (buttons)
     * row 5  └──────────────┘
     */
    const char *lbl_yes = "[ Yes ]";   /* 7 chars */
    const char *lbl_no  = "[ No  ]";   /* 7 chars */
    int btn_row   = 4;
    /* 7 + 2 + 7 = 16; centre inside dw */
    int btn_start = (dw - 16) / 2;
    int btn0_col  = btn_start;          /* Yes */
    int btn1_col  = btn_start + 9;      /* No  */

    WINDOW *dlg = newwin(dh, dw, dy, dx);
    if (!dlg) return false;

    int sel = 0;   /* 0 = Yes, 1 = No */

    nodelay(stdscr, FALSE);
    for (;;) {
        wbkgd(dlg, COLOR_PAIR(CP_DLG));
        werase(dlg);
        wattr_set(dlg, A_BOLD, CP_DLG_TITLE, NULL);
        box(dlg, 0, 0);

        /* title centred in top border */
        {
            char tbuf[MAX_TITLE + 4];
            snprintf(tbuf, sizeof(tbuf), " %s ", title);
            int tlen = (int)strlen(tbuf);
            if (tlen > dw - 4) tlen = dw - 4;
            mvwaddnstr(dlg, 0, (dw - tlen) / 2, tbuf, tlen);
        }

        /* message */
        wattr_set(dlg, 0, CP_DLG, NULL);
        mvwaddnstr(dlg, 2, 2, msg, dw - 4);

        /* Yes button */
        wattr_set(dlg, sel == 0 ? A_BOLD : 0,
                  sel == 0 ? CP_DLG_BTN_SEL : CP_DLG_BTN, NULL);
        mvwaddstr(dlg, btn_row, btn0_col, lbl_yes);

        /* No button */
        wattr_set(dlg, sel == 1 ? A_BOLD : 0,
                  sel == 1 ? CP_DLG_BTN_SEL : CP_DLG_BTN, NULL);
        mvwaddstr(dlg, btn_row, btn1_col, lbl_no);

        wrefresh(dlg);

        int key = getch();
        if (key == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                int rel_r = me.y - dy;
                int rel_c = me.x - dx;
                if (me.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
                    if (rel_r == btn_row) {
                        if (rel_c >= btn0_col && rel_c < btn0_col + 7)
                            { sel = 0; break; }
                        if (rel_c >= btn1_col && rel_c < btn1_col + 7)
                            { sel = 1; break; }
                    }
                }
            }
        } else if (key == '\t' || key == KEY_LEFT || key == KEY_RIGHT) {
            sel = 1 - sel;
        } else if (key == '\r' || key == '\n' || key == ' ') {
            break;
        } else if (key == 27) {              /* Esc → No  */
            sel = 1; break;
        } else if (key == 'y' || key == 'Y') {
            sel = 0; break;
        } else if (key == 'n' || key == 'N') {
            sel = 1; break;
        }
    }
    nodelay(stdscr, TRUE);

    delwin(dlg);
    clearok(stdscr, TRUE);
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) wm->wins[i]->vt.dirty = true;
    return sel == 0;
}

/* ── Borland C++-style single-line text-input dialog ─────────── */

static bool dialog_input(WM *wm, const char *title, const char *prompt,
                          char *buf, int buflen)
{
    if (!buf || buflen < 2) return false;
    int prompt_len = (int)strlen(prompt);
    int field_w    = 24;
    int inner_w    = prompt_len + 1 + field_w + 2;
    if (inner_w < 26) inner_w = 26;
    int dw = inner_w + 4;
    int dh = 7;
    int dy = (wm->rows - dh) / 2;
    int dx = (wm->cols - dw) / 2;
    if (dy < 0) dy = 0;
    if (dx < 0) dx = 0;
    if (dx + dw > wm->cols) dw = wm->cols - dx;
    if (dy + dh > wm->rows) dh = wm->rows - dy;

    /* layout:
     * row 0  ┌─── Title ────────────────┐
     * row 1  │                          │
     * row 2  │  Prompt: [input field   ]│
     * row 3  │                          │
     * row 4  │   [  OK  ]   [Cancel]   │
     * row 5  │                          │
     * row 6  └──────────────────────────┘
     */
    int fld_row = 2;
    int fld_x   = 2 + prompt_len + 1;
    if (fld_x + field_w > dw - 2) field_w = dw - 2 - fld_x;
    if (field_w < 4) field_w = 4;

    const char *lbl_ok  = "[  OK  ]";  /* 8 chars */
    const char *lbl_can = "[Cancel]";  /* 8 chars */
    int btn_row   = 4;
    /* 8 + 2 + 8 = 18; centre inside dw */
    int btn_start = (dw - 18) / 2;
    int btn0_col  = btn_start;          /* OK     */
    int btn1_col  = btn_start + 10;     /* Cancel */

    /* local input buffer (limit to field display width and buflen) */
    char ibuf[256];
    ibuf[0] = '\0';
    if (buf && buf[0]) {
        strncpy(ibuf, buf, sizeof(ibuf) - 1);
        ibuf[sizeof(ibuf) - 1] = '\0';
    }
    int ibuf_len = (int)strlen(ibuf);
    int max_input = field_w < buflen - 1 ? field_w : buflen - 1;
    if (max_input > (int)sizeof(ibuf) - 1) max_input = (int)sizeof(ibuf) - 1;

    /* sel: 2 = text-input focus, 0 = OK, 1 = Cancel */
    int  sel    = 2;
    bool result = false;

    WINDOW *dlg = newwin(dh, dw, dy, dx);
    if (!dlg) return false;

    nodelay(stdscr, FALSE);
    for (;;) {
        wbkgd(dlg, COLOR_PAIR(CP_DLG));
        werase(dlg);
        wattr_set(dlg, A_BOLD, CP_DLG_TITLE, NULL);
        box(dlg, 0, 0);

        /* title */
        {
            char tbuf[MAX_TITLE + 4];
            snprintf(tbuf, sizeof(tbuf), " %s ", title);
            int tlen = (int)strlen(tbuf);
            if (tlen > dw - 4) tlen = dw - 4;
            mvwaddnstr(dlg, 0, (dw - tlen) / 2, tbuf, tlen);
        }

        /* prompt label */
        wattr_set(dlg, 0, CP_DLG, NULL);
        mvwaddstr(dlg, fld_row, 2, prompt);

        /* input field background */
        wattr_set(dlg, sel == 2 ? A_BOLD : 0, CP_DLG_INPUT, NULL);
        for (int c = 0; c < field_w; c++)
            mvwaddch(dlg, fld_row, fld_x + c, ' ');

        /* input field content (scroll to show end when text is long) */
        int disp_start = ibuf_len >= field_w ? ibuf_len - field_w + 1 : 0;
        int disp_len   = ibuf_len - disp_start;
        if (disp_len > field_w) disp_len = field_w;
        if (disp_len > 0)
            mvwaddnstr(dlg, fld_row, fld_x, ibuf + disp_start, disp_len);

        /* text cursor */
        if (sel == 2) {
            int cur = ibuf_len - disp_start;
            if (cur < field_w) wmove(dlg, fld_row, fld_x + cur);
        }

        /* OK button */
        wattr_set(dlg, sel == 0 ? A_BOLD : 0,
                  sel == 0 ? CP_DLG_BTN_SEL : CP_DLG_BTN, NULL);
        mvwaddstr(dlg, btn_row, btn0_col, lbl_ok);

        /* Cancel button */
        wattr_set(dlg, sel == 1 ? A_BOLD : 0,
                  sel == 1 ? CP_DLG_BTN_SEL : CP_DLG_BTN, NULL);
        mvwaddstr(dlg, btn_row, btn1_col, lbl_can);

        wrefresh(dlg);

        int key = getch();
        if (key == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                int rel_r = me.y - dy;
                int rel_c = me.x - dx;
                if (me.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
                    if (rel_r == btn_row) {
                        if (rel_c >= btn0_col && rel_c < btn0_col + 8)
                            { result = true;  break; }
                        if (rel_c >= btn1_col && rel_c < btn1_col + 8)
                            { result = false; break; }
                    }
                    /* clicking the input field focuses it */
                    if (rel_r == fld_row && rel_c >= fld_x &&
                        rel_c < fld_x + field_w)
                        sel = 2;
                }
            }
        } else if (key == 27) {                 /* Esc → Cancel */
            result = false; break;
        } else if (key == '\r' || key == '\n') {
            result = (sel != 1);                /* Enter = OK unless Cancel focused */
            break;
        } else if (key == '\t') {
            sel = (sel + 1) % 3;
        } else if (sel == 2) {
            /* typing inside the input field */
            if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
                if (ibuf_len > 0) ibuf[--ibuf_len] = '\0';
            } else if (key >= 32 && key < 127 && ibuf_len < max_input) {
                ibuf[ibuf_len++] = (char)key;
                ibuf[ibuf_len]   = '\0';
            }
        } else if (key == ' ') {                /* Space confirms focused button */
            result = (sel == 0); break;
        }
    }
    nodelay(stdscr, TRUE);

    delwin(dlg);
    clearok(stdscr, TRUE);
    for (int i = 0; i < wm->nwins; i++)
        if (wm->wins[i]) wm->wins[i]->vt.dirty = true;

    if (result) {
        snprintf(buf, (size_t)buflen, "%s", ibuf);
    }
    return result;
}

/* ── mouse helpers ───────────────────────────────────────────── */

/* Return the index of the topmost window at screen position (mx,my),
 * or -1 if no window covers that cell. */
static int win_at(WM *wm, int mx, int my)
{
    /* The focused window is always brought to the front via top_panel(),
     * so check it first to honour z-order. */
    if (wm->focus >= 0 && wm->focus < wm->nwins && wm->wins[wm->focus]) {
        Win *fw = wm->wins[wm->focus];
        if (mx >= fw->x && mx < fw->x + fw->w &&
            my >= fw->y && my < fw->y + fw->h)
            return wm->focus;
    }
    /* Scan remaining windows from newest to oldest (higher index = on top). */
    for (int i = wm->nwins - 1; i >= 0; i--) {
        if (i == wm->focus) continue;
        Win *win = wm->wins[i];
        if (!win) continue;
        if (mx >= win->x && mx < win->x + win->w &&
            my >= win->y && my < win->y + win->h)
            return i;
    }
    return -1;
}

/* Handle a left-button click on the status bar (y == wm->rows-SB_HEIGHT). */
static void handle_sb_click(WM *wm, int mx)
{
    int col = 0;
    for (int i = 0; i < wm->nwins; i++) {
        Win *win = wm->wins[i];
        if (!win) continue;
        char label[MAX_TITLE + 8];
        snprintf(label, sizeof(label), " %d:%s ", win->id,
                 win->vt.title[0] ? win->vt.title : win->title);
        int llen  = (int)strlen(label);
        int max_w = wm->cols / 2 - col - 1;
        if (max_w <= 0) break;
        int displayed = llen < max_w ? llen : max_w;
        if (mx >= col && mx < col + displayed) {
            wm_focus_idx(wm, i);
            return;
        }
        col += displayed;
        if (col >= wm->cols / 2) break;
    }
}

/* ── mouse event handler ─────────────────────────────────────── */

static void handle_mouse(WM *wm, MEVENT *me)
{
    int mx = me->x;
    int my = me->y;

    /* ── mouse-motion report (button held while moving) ── */
    if (me->bstate & REPORT_MOUSE_POSITION) {
        /* drag: move focused window */
        if (wm->drag_win >= 0 && wm->drag_win < wm->nwins &&
            wm->wins[wm->drag_win]) {
            Win *dw  = wm->wins[wm->drag_win];
            int  nx  = mx - wm->drag_off_x;
            int  ny  = my - wm->drag_off_y;
            int  ddx = nx - dw->x;
            int  ddy = ny - dw->y;
            if (ddx || ddy) {
                win_move(dw, ddx, ddy, wm->cols, wm->rows - SB_HEIGHT);
                wm_draw_all(wm);
            }
        }
        /* resize: grow/shrink focused window */
        if (wm->resize_win >= 0 && wm->resize_win < wm->nwins &&
            wm->wins[wm->resize_win]) {
            Win *rw  = wm->wins[wm->resize_win];
            int  ddx = mx - wm->resize_x0;
            int  ddy = my - wm->resize_y0;
            if (ddx || ddy) {
                win_grow(rw, ddx, ddy, wm->cols, wm->rows - SB_HEIGHT);
                wm->resize_x0 = mx;
                wm->resize_y0 = my;
                wm_draw_all(wm);
            }
        }
        return;
    }

    /* ── button 1 released: end any drag or resize ── */
    if (me->bstate & BUTTON1_RELEASED) {
        wm->btn1_down  = false;
        wm->drag_win   = -1;
        wm->resize_win = -1;
        return;
    }

    /* ── double-click on title bar: maximise / restore ── */
    if (me->bstate & BUTTON1_DOUBLE_CLICKED) {
        if (my < wm->rows - SB_HEIGHT) {
            int idx = win_at(wm, mx, my);
            if (idx >= 0 && wm->wins[idx] && my == wm->wins[idx]->y) {
                Win *win = wm->wins[idx];
                if (win->state == WS_MAX) win_restore(win);
                else                     win_maximize(win, wm->rows, wm->cols);
                win->vt.dirty = true;
            }
        }
        return;
    }

    /* ── left button pressed ── */
    if (me->bstate & BUTTON1_PRESSED) {
        wm->btn1_down  = true;
        wm->drag_win   = -1;
        wm->resize_win = -1;

        /* click on the status bar */
        if (my == wm->rows - SB_HEIGHT) {
            handle_sb_click(wm, mx);
            return;
        }

        /* click inside a window area */
        int idx = win_at(wm, mx, my);
        if (idx < 0) return;

        Win *win = wm->wins[idx];
        if (idx != wm->focus) wm_focus_idx(wm, idx);

        /* title-bar row */
        if (my == win->y) {
            /* [x] / [M] button at far right */
            if (win->w >= 8 && mx >= win->x + win->w - 4 &&
                mx <= win->x + win->w - 2) {
                if (win->state == WS_MAX) {
                    win_restore(win);
                    win->vt.dirty = true;
                } else {
                    wm_close(wm, idx);
                }
                return;
            }
            /* drag (only when not maximised) */
            if (win->state != WS_MAX) {
                wm->drag_win   = idx;
                wm->drag_off_x = mx - win->x;
                wm->drag_off_y = 0;
            }
        }
        /* bottom-right corner: resize handle */
        else if (win->state == WS_NORMAL &&
                 my == win->y + win->h - 1 &&
                 mx >= win->x + win->w - 2) {
            wm->resize_win = idx;
            wm->resize_x0  = mx;
            wm->resize_y0  = my;
        }

        win->vt.dirty = true;
        return;
    }
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
        /* Use the shorter of (label length, max_w) for both rendering and
         * column tracking; advancing by the full strlen would cause col to
         * overshoot when mvwaddnstr truncated the string to max_w. */
        int llen = (int)strlen(label);
        int displayed = (llen < max_w) ? llen : max_w;
        mvwaddnstr(sb, 0, col, label, displayed);
        col += displayed;
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
            const char *default_shell = getenv("SHELL");
            if (!default_shell || default_shell[0] == '\0') default_shell = "/bin/bash";
            char shell_buf[256];
            strncpy(shell_buf, default_shell, sizeof(shell_buf) - 1);
            shell_buf[sizeof(shell_buf) - 1] = '\0';
            if (!dialog_input(wm, "New Shell Window", "Shell:", shell_buf,
                              (int)sizeof(shell_buf)))
                break;
            if (shell_buf[0] == '\0') break;
            /* cascade new windows */
            int off = wm->nwins * 2;
            int nw  = wm->cols * 2 / 3;
            int nh  = (wm->rows - SB_HEIGHT) * 2 / 3;
            wm_new_win(wm, off % (wm->cols / 4), off % ((wm->rows - SB_HEIGHT) / 4),
                       nw, nh, "shell", shell_buf);
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
            if (dialog_confirm(wm, "Quit ncwm", "Really quit ncwm?"))
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
    wm->focus      = -1;
    wm->running    = true;
    wm->next_id    = 1;
    wm->drag_win   = -1;
    wm->resize_win = -1;

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

        /* keyboard / mouse */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int key;
            while ((key = getch()) != ERR) {
                if (key == KEY_MOUSE) {
                    MEVENT me;
                    if (getmouse(&me) == OK)
                        handle_mouse(wm, &me);
                } else {
                    handle_key(wm, key);
                }
            }
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
