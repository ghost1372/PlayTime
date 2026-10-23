#ifndef PLAYTIME_CONFIG_H
#define PLAYTIME_CONFIG_H

/*
 * Central place for every project-specific string and constant.
 * Change them here; nothing else in the code base hard-codes them.
 * Values wrapped in #ifndef can also be overridden from the Makefile (-D...).
 */

/* Version is normally injected by the Makefile (-DPAYLOAD_VERSION). */
#ifndef PAYLOAD_VERSION
#define PAYLOAD_VERSION "dev"
#endif

/* Branding shown in the web UI */
#define PLAYTIME_APP_NAME       "PlayTime"
#define PLAYTIME_TAGLINE        "View and edit your play time"
#define PLAYTIME_AUTHOR         "Mahdi Hosseini"
#define PLAYTIME_REPO_URL       "https://github.com/ghost1372/PlayTime"

/* Web server */
#ifndef PLAYTIME_PORT
#define PLAYTIME_PORT 7788
#endif

/* Anti-CSRF header the UI sends on edits ("<name>: 1"); requests without it
 * are rejected. Matched case-insensitively. */
#define PLAYTIME_CSRF_HEADER    "X-PlayTime"

/* Console database */
#ifndef APP_DB_PATH
#define APP_DB_PATH "/system_data/priv/mms/app.db"
#endif

/* Where app.db is copied once before the first edit. */
#ifndef GAMES_BACKUP_PATH
#define GAMES_BACKUP_PATH "/data/playtime_app.db.bak"
#endif

/* Upper bound accepted for an edited value (units; ~12 years of seconds). */
#define PLAYTIME_MAX_VALUE 400000000LL

/* Seconds represented by one unit of playedTimeOnConsole. If the numbers on the
 * dashboard turn out to be off by a constant factor, this is the one place to
 * fix it (1 = the column counts seconds, which matches what the console shows).
 * It is sent to the UI as unitSeconds. */
#ifndef PLAYTIME_UNIT_SECONDS
#define PLAYTIME_UNIT_SECONDS 1
#endif

/* Maximum number of console users listed (one tbl_iconinfo_<userId> each). */
#define PLAYTIME_MAX_USERS 8

/* Titles that are NOT listed: 9-character title IDs, comma separated. */
#define PLAYTIME_EXCLUDED_TITLES \
    "PPSA01650", /* YouTube */    \
    "PPSA01651", /* YouTube */    \
    "PPSA01652"  /* YouTube */

#endif
