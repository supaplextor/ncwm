/*
 * main.c – entry point for ncwm (ncurses window manager)
 */
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ncwm.h"

/* global WM instance – used by signal handlers */
WM                   *g_wm       = NULL;
volatile sig_atomic_t g_resize   = 0;
volatile sig_atomic_t g_got_chld = 0;

static void on_sigwinch(int sig) { (void)sig; g_resize   = 1; }
static void on_sigchld(int sig)  { (void)sig; g_got_chld = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
            "ncwm " NCWM_VERSION " – ncurses window manager\n"
            "Usage: %s [SHELL]\n"
            "\n"
            "Runs SHELL (default: $SHELL or /bin/bash) in the first window.\n"
            "Press F1 ? for in-application help.\n",
            prog);
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    if (argc > 1 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc > 1 &&
        (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("ncwm %s\n", NCWM_VERSION);
        return 0;
    }

    /* ── signal handlers ── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_sigwinch;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler  = on_sigchld;
    sa.sa_flags    = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);  /* ignore broken PTY writes */

    /* ── initialise ncurses ── */
    initscr();

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "ncwm: terminal does not support colours\n");
        return 1;
    }
    start_color();
    use_default_colors();

    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);

    /* ── create window manager ── */
    g_wm = wm_create();
    if (!g_wm) {
        endwin();
        fprintf(stderr, "ncwm: failed to initialise window manager\n");
        return 1;
    }

    /* ── open initial shell window ── */
    const char *shell = (argc > 1) ? argv[1] : getenv("SHELL");
    if (!shell || shell[0] == '\0') shell = "/bin/bash";

    int iw = g_wm->cols * 3 / 4;
    int ih = (g_wm->rows - SB_HEIGHT) * 3 / 4;
    int ix = (g_wm->cols - iw) / 2;
    int iy = (g_wm->rows - SB_HEIGHT - ih) / 2;

    if (!wm_new_win(g_wm, ix, iy, iw, ih, "shell", shell)) {
        endwin();
        fprintf(stderr, "ncwm: failed to open initial shell window\n");
        return 1;
    }

    /* ── run ── */
    wm_run(g_wm);

    /* ── teardown ── */
    wm_destroy(g_wm);
    g_wm = NULL;

    endwin();
    return 0;
}
