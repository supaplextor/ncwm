/*
 * vt100.c – VT100/ANSI terminal emulator for ncwm
 *
 * Supports the subset of VT100/xterm sequences used by common Unix
 * programs (bash, vim, nano, htop, ls --color, …).
 */
#include <stdlib.h>
#include <string.h>
#include "ncwm.h"

#define DEF_FG  7   /* COLOR_WHITE  */
#define DEF_BG  0   /* COLOR_BLACK  */

/* ── helpers ─────────────────────────────────────────────────── */

static void cell_blank(VT *vt, Cell *c)
{
    c->ch   = ' ';
    c->pair = TERM_PAIR(vt->fg, vt->bg);
    c->attr = 0;
}

static void clear_rect(VT *vt, int r1, int c1, int r2, int c2)
{
    for (int r = r1; r <= r2 && r < vt->rows; r++)
        for (int c = c1; c <= c2 && c < vt->cols; c++)
            cell_blank(vt, &vt->cells[r * vt->cols + c]);
}

static void scroll_up(VT *vt, int n)
{
    int h = vt->sbot - vt->stop + 1;
    if (n <= 0) return;
    if (n >= h) { clear_rect(vt, vt->stop, 0, vt->sbot, vt->cols - 1); return; }
    memmove(&vt->cells[vt->stop * vt->cols],
            &vt->cells[(vt->stop + n) * vt->cols],
            (size_t)(h - n) * (size_t)vt->cols * sizeof(Cell));
    clear_rect(vt, vt->sbot - n + 1, 0, vt->sbot, vt->cols - 1);
}

static void scroll_down(VT *vt, int n)
{
    int h = vt->sbot - vt->stop + 1;
    if (n <= 0) return;
    if (n >= h) { clear_rect(vt, vt->stop, 0, vt->sbot, vt->cols - 1); return; }
    memmove(&vt->cells[(vt->stop + n) * vt->cols],
            &vt->cells[vt->stop * vt->cols],
            (size_t)(h - n) * (size_t)vt->cols * sizeof(Cell));
    clear_rect(vt, vt->stop, 0, vt->stop + n - 1, vt->cols - 1);
}

static void do_lf(VT *vt)
{
    vt->wrap_next = false;
    if (vt->cy == vt->sbot)
        scroll_up(vt, 1);
    else if (vt->cy < vt->rows - 1)
        vt->cy++;
}

static void put_char(VT *vt, unsigned int ch)
{
    if (vt->wrap_next) {
        vt->cx = 0;
        do_lf(vt);
        vt->wrap_next = false;
    }
    if (vt->cx >= 0 && vt->cx < vt->cols &&
        vt->cy >= 0 && vt->cy < vt->rows) {
        Cell *c = &vt->cells[vt->cy * vt->cols + vt->cx];
        c->ch   = ch;
        c->pair = TERM_PAIR(vt->fg, vt->bg);
        c->attr = vt->attr;
    }
    if (++(vt->cx) >= vt->cols) {
        vt->cx = vt->cols - 1;
        vt->wrap_next = true;
    }
}

/* ── CSI parameter parser ────────────────────────────────────── */

static int parse_params(const char *s, int len, int *p)
{
    int n = 0, v = 0;
    bool got = false;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            got = true;
        } else if (c == ';') {
            if (n < VT_PARAM_MAX) p[n++] = got ? v : 0;
            v = 0; got = false;
        }
    }
    if ((got || n > 0) && n < VT_PARAM_MAX)
        p[n++] = got ? v : 0;
    return n;
}

/* Return params[idx] if present and > 0, else def */
static int P(const int *p, int n, int idx, int def)
{
    return (idx < n && p[idx] > 0) ? p[idx] : def;
}

/* ── SGR (Select Graphic Rendition) ─────────────────────────── */

static void do_sgr(VT *vt, int *p, int n)
{
    if (n == 0) {
        vt->fg = DEF_FG; vt->bg = DEF_BG; vt->attr = 0;
        return;
    }
    for (int i = 0; i < n; i++) {
        switch (p[i]) {
        case 0:  vt->fg = DEF_FG; vt->bg = DEF_BG; vt->attr = 0; break;
        case 1:  vt->attr |=  A_BOLD;       break;
        case 2:  vt->attr |=  A_DIM;        break;
        case 4:  vt->attr |=  A_UNDERLINE;  break;
        case 5: case 6: vt->attr |= A_BLINK; break;
        case 7:  vt->attr |=  A_REVERSE;    break;
        case 8:  vt->attr |=  A_INVIS;      break;
        case 22: vt->attr &= ~(A_BOLD | A_DIM);   break;
        case 24: vt->attr &= ~A_UNDERLINE;  break;
        case 25: vt->attr &= ~A_BLINK;      break;
        case 27: vt->attr &= ~A_REVERSE;    break;
        case 28: vt->attr &= ~A_INVIS;      break;
        case 39: vt->fg = DEF_FG; break;
        case 49: vt->bg = DEF_BG; break;
        default:
            if      (p[i] >= 30  && p[i] <= 37)  vt->fg = p[i] - 30;
            else if (p[i] >= 40  && p[i] <= 47)  vt->bg = p[i] - 40;
            else if (p[i] >= 90  && p[i] <= 97)  { vt->fg = p[i] - 90; vt->attr |= A_BOLD; }
            else if (p[i] >= 100 && p[i] <= 107) vt->bg = p[i] - 100;
            else if (p[i] == 38 || p[i] == 48) {
                /* 256-colour or RGB — map best-effort to 8 colours */
                if (i + 2 < n && p[i + 1] == 5) {
                    int idx = p[i + 2];
                    short col = (idx < 8) ? (short)idx
                              : (idx < 16) ? (short)(idx - 8)
                              : (short)DEF_FG;
                    if (p[i] == 38) { vt->fg = col; if (idx >= 8 && idx < 16) vt->attr |= A_BOLD; }
                    else              vt->bg = col;
                    i += 2;
                } else if (i + 4 < n && p[i + 1] == 2) {
                    i += 4; /* skip R, G, B */
                }
            }
            break;
        }
    }
}

/* ── CSI dispatcher ──────────────────────────────────────────── */

static void do_csi(VT *vt, const char *buf, int len, char fin)
{
    int  p[VT_PARAM_MAX] = {0};
    int  np = 0;
    bool priv = false;
    int  off  = 0;

    if (len > 0 && (buf[0] == '?' || buf[0] == '>' || buf[0] == '!')) {
        priv = (buf[0] == '?');
        off  = 1;
    }
    np = parse_params(buf + off, len - off, p);

    switch (fin) {
    /* ── cursor movement ── */
    case 'A': vt->cy -= P(p, np, 0, 1);
              if (vt->cy < 0) vt->cy = 0;
              vt->wrap_next = false; break;
    case 'B': vt->cy += P(p, np, 0, 1);
              if (vt->cy >= vt->rows) vt->cy = vt->rows - 1;
              vt->wrap_next = false; break;
    case 'C': vt->cx += P(p, np, 0, 1);
              if (vt->cx >= vt->cols) vt->cx = vt->cols - 1;
              vt->wrap_next = false; break;
    case 'D': vt->cx -= P(p, np, 0, 1);
              if (vt->cx < 0) vt->cx = 0;
              vt->wrap_next = false; break;
    case 'E': vt->cy += P(p, np, 0, 1); vt->cx = 0;
              if (vt->cy >= vt->rows) vt->cy = vt->rows - 1;
              vt->wrap_next = false; break;
    case 'F': vt->cy -= P(p, np, 0, 1); vt->cx = 0;
              if (vt->cy < 0) vt->cy = 0;
              vt->wrap_next = false; break;
    case 'G': {
        int col = P(p, np, 0, 1) - 1;
        vt->cx = (col < 0) ? 0 : (col >= vt->cols) ? vt->cols - 1 : col;
        vt->wrap_next = false; break;
    }
    case 'H': case 'f': {
        int row = P(p, np, 0, 1) - 1;
        int col = P(p, np, 1, 1) - 1;
        vt->cy = (row < 0) ? 0 : (row >= vt->rows) ? vt->rows - 1 : row;
        vt->cx = (col < 0) ? 0 : (col >= vt->cols) ? vt->cols - 1 : col;
        vt->wrap_next = false; break;
    }
    case 'd': {
        int row = P(p, np, 0, 1) - 1;
        vt->cy = (row < 0) ? 0 : (row >= vt->rows) ? vt->rows - 1 : row;
        vt->wrap_next = false; break;
    }

    /* ── erase ── */
    case 'J': {
        int n = (np > 0) ? p[0] : 0;
        if (n == 0) {
            clear_rect(vt, vt->cy, vt->cx, vt->cy, vt->cols - 1);
            if (vt->cy + 1 < vt->rows)
                clear_rect(vt, vt->cy + 1, 0, vt->rows - 1, vt->cols - 1);
        } else if (n == 1) {
            if (vt->cy > 0)
                clear_rect(vt, 0, 0, vt->cy - 1, vt->cols - 1);
            clear_rect(vt, vt->cy, 0, vt->cy, vt->cx);
        } else {
            clear_rect(vt, 0, 0, vt->rows - 1, vt->cols - 1);
            if (n == 2) { vt->cx = 0; vt->cy = 0; }
        }
        break;
    }
    case 'K': {
        int n = (np > 0) ? p[0] : 0;
        if      (n == 0) clear_rect(vt, vt->cy, vt->cx, vt->cy, vt->cols - 1);
        else if (n == 1) clear_rect(vt, vt->cy, 0, vt->cy, vt->cx);
        else             clear_rect(vt, vt->cy, 0, vt->cy, vt->cols - 1);
        break;
    }
    case 'X': {
        int n = P(p, np, 0, 1);
        int ec = vt->cx + n - 1;
        if (ec >= vt->cols) ec = vt->cols - 1;
        clear_rect(vt, vt->cy, vt->cx, vt->cy, ec);
        break;
    }

    /* ── insert / delete lines ── */
    case 'L': { /* IL – insert lines */
        int n = P(p, np, 0, 1);
        if (vt->cy >= vt->stop && vt->cy <= vt->sbot) {
            int save_top = vt->stop;
            vt->stop = vt->cy;
            scroll_down(vt, n);
            vt->stop = save_top;
        }
        break;
    }
    case 'M': { /* DL – delete lines */
        int n = P(p, np, 0, 1);
        if (vt->cy >= vt->stop && vt->cy <= vt->sbot) {
            int save_top = vt->stop;
            vt->stop = vt->cy;
            scroll_up(vt, n);
            vt->stop = save_top;
        }
        break;
    }

    /* ── insert / delete characters ── */
    case '@': { /* ICH – insert chars */
        int n = P(p, np, 0, 1);
        int mv = vt->cols - vt->cx - n;
        if (mv > 0)
            memmove(&vt->cells[vt->cy * vt->cols + vt->cx + n],
                    &vt->cells[vt->cy * vt->cols + vt->cx],
                    (size_t)mv * sizeof(Cell));
        int ec = vt->cx + n - 1;
        if (ec >= vt->cols) ec = vt->cols - 1;
        clear_rect(vt, vt->cy, vt->cx, vt->cy, ec);
        break;
    }
    case 'P': { /* DCH – delete chars */
        int n = P(p, np, 0, 1);
        int mv = vt->cols - vt->cx - n;
        if (mv > 0)
            memmove(&vt->cells[vt->cy * vt->cols + vt->cx],
                    &vt->cells[vt->cy * vt->cols + vt->cx + n],
                    (size_t)mv * sizeof(Cell));
        clear_rect(vt, vt->cy, vt->cols - n, vt->cy, vt->cols - 1);
        break;
    }

    /* ── scroll ── */
    case 'S': scroll_up(vt, P(p, np, 0, 1));   break;
    case 'T': scroll_down(vt, P(p, np, 0, 1)); break;

    /* ── attributes ── */
    case 'm': do_sgr(vt, p, np); break;

    /* ── scroll region ── */
    case 'r': {
        int top = P(p, np, 0, 1) - 1;
        int bot = (np > 1 && p[1] > 0) ? p[1] - 1 : vt->rows - 1;
        if (top >= 0 && top < bot && bot < vt->rows) {
            vt->stop = top; vt->sbot = bot;
        } else {
            vt->stop = 0; vt->sbot = vt->rows - 1;
        }
        vt->cx = 0; vt->cy = 0;
        break;
    }

    /* ── save / restore cursor ── */
    case 's': vt->sx = vt->cx; vt->sy = vt->cy; break;
    case 'u': vt->cx = vt->sx; vt->cy = vt->sy; vt->wrap_next = false; break;

    /* ── mode set/reset ── */
    case 'h': case 'l': {
        bool set = (fin == 'h');
        if (!priv) break;
        for (int i = 0; i < np; i++) {
            switch (p[i]) {
            case 1:   vt->app_cursor = set; break;
            case 25:  vt->cvis = set; break;
            case 47: case 1047: case 1049: {
                if (set && !vt->using_alt) {
                    /* save main screen */
                    memcpy(vt->alt, vt->cells,
                           (size_t)vt->rows * (size_t)vt->cols * sizeof(Cell));
                    vt->alt_cx = vt->cx; vt->alt_cy = vt->cy;
                    /* switch to alt (clear it) */
                    Cell *tmp = vt->cells; vt->cells = vt->alt; vt->alt = tmp;
                    clear_rect(vt, 0, 0, vt->rows - 1, vt->cols - 1);
                    if (p[i] == 1049) { vt->cx = 0; vt->cy = 0; }
                    vt->using_alt = true;
                } else if (!set && vt->using_alt) {
                    /* restore main screen */
                    Cell *tmp = vt->cells; vt->cells = vt->alt; vt->alt = tmp;
                    if (p[i] == 1049) { vt->cx = vt->alt_cx; vt->cy = vt->alt_cy; }
                    vt->using_alt = false;
                }
                break;
            }
            }
        }
        break;
    }

    /* ── device attributes / status (no-op) ── */
    case 'c': case 'n': break;

    default: break;
    }

    vt->dirty = true;
}

/* ── ESC (two-character) sequences ──────────────────────────── */

static void do_esc(VT *vt, char c)
{
    switch (c) {
    case '7': vt->sx = vt->cx; vt->sy = vt->cy; break;       /* DECSC */
    case '8': vt->cx = vt->sx; vt->cy = vt->sy;               /* DECRC */
              vt->wrap_next = false; break;
    case 'M': /* RI – reverse index */
        if (vt->cy == vt->stop)
            scroll_down(vt, 1);
        else if (vt->cy > 0)
            vt->cy--;
        vt->wrap_next = false;
        break;
    case 'c': /* RIS – full reset */
        vt_init(vt, vt->rows, vt->cols);
        break;
    case 'D': do_lf(vt); break;  /* IND – index */
    case 'E': do_lf(vt); vt->cx = 0; break;  /* NEL */
    default:  break;
    }
    vt->dirty = true;
}

/* ── public API ──────────────────────────────────────────────── */

void vt_init(VT *vt, int rows, int cols)
{
    /* preserve existing buffers if allocated (called from RIS) */
    Cell *existing_cells = vt->cells;
    Cell *existing_alt   = vt->alt;

    memset(vt, 0, sizeof(*vt));
    vt->rows  = rows;
    vt->cols  = cols;
    vt->fg    = DEF_FG;
    vt->bg    = DEF_BG;
    vt->cvis  = true;
    vt->stop  = 0;
    vt->sbot  = rows - 1;
    vt->pstate = PS_NORM;

    size_t sz = (size_t)rows * (size_t)cols;

    vt->cells = existing_cells ? existing_cells : malloc(sz * sizeof(Cell));
    vt->alt   = existing_alt   ? existing_alt   : malloc(sz * sizeof(Cell));

    clear_rect(vt, 0, 0, rows - 1, cols - 1);
    /* clear alt buffer too */
    short save_fg = vt->fg, save_bg = vt->bg;
    vt->fg = DEF_FG; vt->bg = DEF_BG;
    Cell *main = vt->cells; vt->cells = vt->alt;
    clear_rect(vt, 0, 0, rows - 1, cols - 1);
    vt->cells = main;
    vt->fg = save_fg; vt->bg = save_bg;

    vt->dirty = true;
}

void vt_free(VT *vt)
{
    free(vt->cells);
    free(vt->alt);
    vt->cells = vt->alt = NULL;
}

void vt_process(VT *vt, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        switch (vt->pstate) {

        /* ── normal ── */
        case PS_NORM:
            switch (c) {
            case 0x07: break; /* BEL – ignore */
            case 0x08: /* BS */
                if (vt->cx > 0) { vt->cx--; vt->wrap_next = false; }
                break;
            case 0x09: { /* HT – horizontal tab */
                int nx = (vt->cx / 8 + 1) * 8;
                if (nx >= vt->cols) nx = vt->cols - 1;
                vt->cx = nx;
                break;
            }
            case 0x0A: case 0x0B: case 0x0C: /* LF / VT / FF */
                do_lf(vt);
                vt->dirty = true;
                break;
            case 0x0D: /* CR */
                vt->cx = 0;
                vt->wrap_next = false;
                break;
            case 0x1B: /* ESC */
                vt->pstate = PS_ESC;
                break;
            case 0x0E: case 0x0F: break; /* SO / SI – charset, ignore */
            default:
                if (c >= 0x20) {
                    put_char(vt, c);
                    vt->dirty = true;
                }
                break;
            }
            break;

        /* ── ESC received ── */
        case PS_ESC:
            if (c == '[') {
                vt->pstate = PS_CSI;
                vt->elen   = 0;
            } else if (c == ']') {
                vt->pstate = PS_OSC;
                vt->elen   = 0;
            } else {
                /* two-char ESC sequences; handle ESC( ESC) charset selectors */
                if (c == '(' || c == ')' || c == '*' || c == '+') {
                    /* next byte is the charset designator – consume via state */
                    vt->pstate = PS_CHARSET;
                } else {
                    do_esc(vt, (char)c);
                    vt->pstate = PS_NORM;
                }
            }
            break;

        /* ── charset designator (one byte, silently ignored) ── */
        case PS_CHARSET:
            vt->pstate = PS_NORM;
            break;

        /* ── CSI parameter accumulation ── */
        case PS_CSI:
            if (c >= 0x40 && c <= 0x7E) {
                /* final byte → dispatch */
                do_csi(vt, vt->ebuf, vt->elen, (char)c);
                vt->pstate = PS_NORM;
                vt->elen   = 0;
            } else if (c >= 0x20 && c < 0x40) {
                /* parameter / intermediate byte */
                if (vt->elen < VT_ESC_MAX - 1)
                    vt->ebuf[vt->elen++] = (char)c;
            }
            /* else: C0 inside CSI – ignore and stay in CSI */
            break;

        /* ── OSC string accumulation ── */
        case PS_OSC:
            if (c == 0x07) {
                /* BEL terminates OSC */
                vt->ebuf[vt->elen] = '\0';
                /* Parse "n;title" – set window title for n = 0, 1, 2 */
                const char *semi = memchr(vt->ebuf, ';', (size_t)vt->elen);
                if (semi) {
                    int tlen = (int)(vt->ebuf + vt->elen - (semi + 1));
                    if (tlen >= MAX_TITLE) tlen = MAX_TITLE - 1;
                    memcpy(vt->title, semi + 1, (size_t)tlen);
                    vt->title[tlen] = '\0';
                }
                vt->pstate = PS_NORM;
                vt->elen   = 0;
            } else if (c == 0x1B) {
                /* ESC inside OSC – store it; next byte may be '\' (ST) */
                if (vt->elen < VT_ESC_MAX - 1)
                    vt->ebuf[vt->elen++] = (char)c;
            } else if (c == '\\' && vt->elen > 0 &&
                       vt->ebuf[vt->elen - 1] == '\x1b') {
                /* ST (ESC \) terminates OSC */
                vt->elen--;  /* drop the stored ESC */
                vt->ebuf[vt->elen] = '\0';
                const char *semi = memchr(vt->ebuf, ';', (size_t)vt->elen);
                if (semi) {
                    int tlen = (int)(vt->ebuf + vt->elen - (semi + 1));
                    if (tlen >= MAX_TITLE) tlen = MAX_TITLE - 1;
                    memcpy(vt->title, semi + 1, (size_t)tlen);
                    vt->title[tlen] = '\0';
                }
                vt->pstate = PS_NORM;
                vt->elen   = 0;
            } else {
                if (vt->elen < VT_ESC_MAX - 1)
                    vt->ebuf[vt->elen++] = (char)c;
            }
            break;
        }
    }
}

void vt_render(VT *vt, WINDOW *win)
{
    int h, w;
    getmaxyx(win, h, w);

    for (int r = 0; r < vt->rows && r < h; r++) {
        for (int col = 0; col < vt->cols && col < w; col++) {
            Cell *c = &vt->cells[r * vt->cols + col];
            chtype ch = (chtype)(c->ch >= 0x20 ? c->ch : (unsigned int)' ');
            ch |= c->attr | (chtype)COLOR_PAIR(c->pair);
            mvwaddch(win, r, col, ch);
        }
    }
    /* place cursor */
    if (vt->cvis && vt->cy < h && vt->cx < w)
        wmove(win, vt->cy, vt->cx);

    vt->dirty = false;
}

void vt_resize(VT *vt, int new_rows, int new_cols)
{
    if (new_rows == vt->rows && new_cols == vt->cols) return;

    Cell *nc   = malloc((size_t)new_rows * (size_t)new_cols * sizeof(Cell));
    Cell *nalt = malloc((size_t)new_rows * (size_t)new_cols * sizeof(Cell));
    if (!nc || !nalt) { free(nc); free(nalt); return; }

    /* fill with blanks */
    Cell blank = { ' ', TERM_PAIR(DEF_FG, DEF_BG), 0 };
    for (int i = 0; i < new_rows * new_cols; i++) nc[i] = nalt[i] = blank;

    /* copy old content */
    int copy_r = (vt->rows < new_rows) ? vt->rows : new_rows;
    int copy_c = (vt->cols < new_cols) ? vt->cols : new_cols;
    for (int r = 0; r < copy_r; r++)
        memcpy(&nc[r * new_cols], &vt->cells[r * vt->cols],
               (size_t)copy_c * sizeof(Cell));

    free(vt->cells);
    free(vt->alt);
    vt->cells = nc;
    vt->alt   = nalt;
    vt->rows  = new_rows;
    vt->cols  = new_cols;

    /* clamp cursor */
    if (vt->cx >= new_cols) vt->cx = new_cols - 1;
    if (vt->cy >= new_rows) vt->cy = new_rows - 1;
    vt->stop = 0;
    vt->sbot = new_rows - 1;
    vt->wrap_next = false;
    vt->dirty = true;
}
