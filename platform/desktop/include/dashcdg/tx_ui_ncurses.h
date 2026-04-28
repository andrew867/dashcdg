#ifndef DASHCDG_TX_UI_NCURSES_H
#define DASHCDG_TX_UI_NCURSES_H

#include <stddef.h>

int dashcdg_tx_ui_ncurses_wants_env(void);
int dashcdg_tx_ui_ncurses_init(void);
void dashcdg_tx_ui_ncurses_shutdown(void);
void dashcdg_tx_ui_ncurses_present_lines(const char *const *lines, int nlines);
int dashcdg_tx_ui_ncurses_drain_key(void);

#endif
