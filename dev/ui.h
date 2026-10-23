#ifndef PLAYTIME_UI_H
#define PLAYTIME_UI_H

#include <stddef.h>

/*
 * UI module: the single-page web UI as a static string. It talks to the
 * server only through /api/info, /api/games and /icon/<TITLEID>.png.
 */
const char *ui_page(void);
size_t ui_page_length(void);

#endif
