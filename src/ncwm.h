/*
 * ncwm.h – shared types, constants and prototypes for ncwm
 */
#pragma once
#ifndef NCWM_H
#define NCWM_H

#include <ncurses.h>
#include <panel.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/types.h>

/* ── version ───────────────────────────────────────────────────── */
#define NCWM_VERSION    "0.1.0"

/* ── limits ────────────────────────────────────────────────────── */
#define MAX_WINDOWS      16
#define MAX_TITLE       128
#define SB_HEIGHT         1     /* status-bar height in rows        */
#define MIN_WIN_W        12
#define MIN_WIN_H         4

/* ── colour pairs ──────────────────────────────────────────────── */
/*
 * Pairs 1-64  → terminal content: TERM_PAIR(fg,bg) = fg*8 + bg + 1
 *               fg and bg are the eight standard terminal colours
 *               (COLOR_BLACK … COLOR_WHITE, values 0-7).
 * Pairs 65-70 → WM chrome.
 */
#define TERM_PAIR(fg, bg)   ((short)((fg) * 8 + (bg) + 1))
#define CP_BORDER           65   /* inactive window border / title  */
#define CP_FOCUSED          66   /* active window border / title    */
#define CP_SB               67   /* status-bar background           */
#define CP_SB_WIN           68   /* status-bar window entry         */
#define CP_SB_ACTIVE        69   /* status-bar active window        */
#define CP_SB_CLOCK         70   /* status-bar clock                */

/* ── key-bindings (F1 is the prefix key) ──────────────────────── */
#define WM_PREFIX           KEY_F(1)
#define WM_NEW              'n'   /* new shell window                */
#define WM_CLOSE            'x'   /* close focused window            */
#define WM_NEXT             '\t'  /* focus next window               */
#define WM_PREV             'p'   /* focus previous window           */
#define WM_MOVE             'm'   /* enter move mode                 */
#define WM_RESIZE           'r'   /* enter resize mode               */
#define WM_FULL             'f'   /* toggle fullscreen               */
#define WM_QUIT             'Q'   /* quit ncwm                       */
#define WM_HELP             '?'   /* show help overlay               */

/* ── VT100 emulator ────────────────────────────────────────────── */
#define VT_ESC_MAX  256
#define VT_PARAM_MAX 16

/* A single terminal cell */
typedef struct {
    unsigned int ch;    /* character (Unicode codepoint / ASCII)   */
    short        pair;  /* ncurses colour-pair index               */
    attr_t       attr;  /* A_BOLD | A_UNDERLINE | …               */
} Cell;

/* Parser state */
typedef enum { PS_NORM, PS_ESC, PS_CSI, PS_OSC } PState;

typedef struct {
    Cell    *cells;             /* [rows * cols]                    */
    int      rows, cols;
    int      cx, cy;            /* cursor position (0-based)        */
    int      sx, sy;            /* saved cursor                     */
    bool     cvis;              /* cursor visible                   */
    short    fg, bg;            /* current fg/bg colours (0-7)      */
    attr_t   attr;              /* current render attributes        */
    int      stop, sbot;        /* scroll region (inclusive, 0-based)*/
    bool     wrap_next;         /* deferred wrap flag               */
    bool     app_cursor;        /* application cursor key mode      */

    /* ESC/CSI/OSC parser */
    PState   pstate;
    char     ebuf[VT_ESC_MAX];
    int      elen;

    /* OSC title */
    char     title[MAX_TITLE];

    /* Alternate screen */
    Cell    *alt;               /* alternate cell buffer            */
    int      alt_cx, alt_cy;
    bool     using_alt;

    bool     dirty;             /* needs re-render                  */
} VT;

/* ── Window ────────────────────────────────────────────────────── */
typedef enum { WS_NORMAL, WS_MAX } WState;

typedef struct Win {
    int      id;
    char     title[MAX_TITLE];

    WINDOW  *outer;             /* full outer window (border+title) */
    WINDOW  *inner;             /* terminal area (inside border)    */
    PANEL   *panel;

    int      x, y, w, h;       /* outer geometry (screen coords)   */
    int      sx, sy, sw, sh;   /* saved geometry (before maximise) */

    int      mfd;               /* PTY master file-descriptor       */
    pid_t    pid;               /* child process PID                */

    VT       vt;
    WState   state;
    bool     focused;
} Win;

/* ── Window Manager ────────────────────────────────────────────── */
typedef struct {
    Win    *wins[MAX_WINDOWS];
    int     nwins;
    int     focus;              /* index into wins[], or -1         */

    int     rows, cols;         /* screen dimensions                */
    WINDOW *sb;                 /* status-bar window                */

    bool    running;
    bool    pfx;                /* waiting for WM command after F1  */
    bool    move_mode;          /* arrow keys move focused window   */
    bool    resize_mode;        /* arrow keys resize focused window */

    int     next_id;
} WM;

/* global instances / flags */
extern WM                   *g_wm;
extern volatile sig_atomic_t g_resize;
extern volatile sig_atomic_t g_got_chld;

/* ── prototypes ────────────────────────────────────────────────── */

/* wm.c */
WM  *wm_create(void);
void wm_destroy(WM *wm);
void wm_run(WM *wm);
Win *wm_new_win(WM *wm, int x, int y, int w, int h,
                const char *title, const char *shell);
void wm_close(WM *wm, int idx);
void wm_focus_idx(WM *wm, int idx);
void wm_focus_next(WM *wm);
void wm_focus_prev(WM *wm);
void wm_draw_all(WM *wm);
void wm_draw_sb(WM *wm);
void wm_on_resize(WM *wm);
void wm_on_sigchld(WM *wm);

/* window.c */
Win *win_create(int id, int x, int y, int w, int h,
                const char *title, const char *shell);
void win_destroy(Win *win);
void win_draw(Win *win, bool focused);
void win_move(Win *win, int dx, int dy, int max_x, int max_y);
void win_grow(Win *win, int dw, int dh, int max_x, int max_y);
void win_maximize(Win *win, int rows, int cols);
void win_restore(Win *win);
void win_read_pty(Win *win);
void win_write_pty(Win *win, const char *buf, int n);

/* vt100.c */
void vt_init(VT *vt, int rows, int cols);
void vt_free(VT *vt);
void vt_process(VT *vt, const char *data, int len);
void vt_render(VT *vt, WINDOW *win);
void vt_resize(VT *vt, int rows, int cols);

#endif /* NCWM_H */
