#include "dashcdg/tx_ui_ncurses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(DASHCDG_TX_UI_NCURSES) && (DASHCDG_TX_UI_NCURSES) != 0
#include <ncurses.h>
static int s_tx_ncurses_inited;
#endif

static int s_wants;

static int dashcdg_tx_streq_ci(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if ((*a | 32) != (*b | 32)) {
            return 0;
        }
    }
    return *a == *b;
}

int dashcdg_tx_ui_ncurses_wants_env(void) {
    const char *v = getenv("DASHCDG_TX_UI");

    s_wants = 0;
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    if (dashcdg_tx_streq_ci(v, "ncurses") || dashcdg_tx_streq_ci(v, "curses")) {
        s_wants = 1;
    }
    return s_wants;
}

int dashcdg_tx_ui_ncurses_init(void) {
#if !defined(DASHCDG_TX_UI_NCURSES) || (DASHCDG_TX_UI_NCURSES) == 0
    return 0;
#else
    (void) dashcdg_tx_ui_ncurses_wants_env();
    if (!s_wants) {
        return 0;
    }
    if (s_tx_ncurses_inited) {
        return 1;
    }
    if (!isatty(fileno(stdout)) || !isatty(fileno(stdin))) {
        return 0;
    }
    if (initscr() == NULL) {
        return 0;
    }
    cbreak();
    noecho();
    (void) nonl();
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    (void) curs_set(0);
    if (has_colors()) {
        (void) start_color();
        (void) use_default_colors();
    }
    s_tx_ncurses_inited = 1;
    return 1;
#endif
}

void dashcdg_tx_ui_ncurses_shutdown(void) {
#if !defined(DASHCDG_TX_UI_NCURSES) || (DASHCDG_TX_UI_NCURSES) == 0
    return;
#else
    if (!s_tx_ncurses_inited) {
        return;
    }
    s_tx_ncurses_inited = 0;
    (void) endwin();
#endif
}

void dashcdg_tx_ui_ncurses_present_lines(const char *const *lines, int nlines) {
#if !defined(DASHCDG_TX_UI_NCURSES) || (DASHCDG_TX_UI_NCURSES) == 0
    (void) lines;
    (void) nlines;
    return;
#else
    int max_r;
    int max_c;
    int r;

    if (!s_tx_ncurses_inited || lines == NULL) {
        return;
    }
    (void) getmaxyx(stdscr, max_r, max_c);
    if (max_r < 1 || max_c < 1) {
        (void) refresh();
        return;
    }
    (void) clear();
    (void) wattron(stdscr, A_BOLD);
    (void) mvaddnstr(0, 0, " DASHCDG TX (ncurses) — same keys as stdio mode; DASHCDG_TX_DASHBOARD=1 is ANSI; env DASHCDG_TX_UI=ncurses", (int) max_c);
    (void) mvaddnstr(1, 0, "------------------------------------------------------------------------------", (int) max_c);
    (void) wattroff(stdscr, A_BOLD);
    for (r = 0; r < max_r - 2 && r < nlines; r++) {
        if (lines[r] == NULL) {
            continue;
        }
        (void) mvaddnstr(r + 2, 0, lines[r], (int) max_c);
    }
    (void) refresh();
#endif
}

int dashcdg_tx_ui_ncurses_drain_key(void) {
#if !defined(DASHCDG_TX_UI_NCURSES) || (DASHCDG_TX_UI_NCURSES) == 0
    return 0;
#else
    int ch;

    if (!s_tx_ncurses_inited) {
        return 0;
    }
    ch = wgetch(stdscr);
    if (ch == ERR) {
        return 0;
    }
    if (ch == KEY_LEFT) {
        return (int) '[';
    }
    if (ch == KEY_RIGHT) {
        return (int) ']';
    }
    if (ch >= 0x01 && ch <= 0x7E) {
        return ch;
    }
    return 0;
#endif
}
