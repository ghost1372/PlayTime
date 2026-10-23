#ifndef PLAYTIME_GAMES_H
#define PLAYTIME_GAMES_H

#include <stddef.h>

#include "config.h"

/*
 * Goal module: read the installed games and their play time out of the PS5
 * system database, and edit the play time. Knows nothing about HTTP or HTML.
 *
 * Play time lives in one table per console user, tbl_iconinfo_<userId>,
 * column playedTimeOnConsole. The unit is seconds (see PLAYTIME_UNIT_SECONDS).
 */

/* Sets the title IDs that are never listed (compared case-insensitively). The
 * array is not copied, so it must stay valid. Call once at startup. */
void games_set_excluded(const char *const *titles, size_t count);

/* Optional: turns a user ID (suffix of tbl_iconinfo_<userId>) into the account
 * name shown on the console. Returns 0 and fills name, or -1 if unknown. */
typedef int (*games_user_name_fn)(const char *user_id, char *name, size_t size);
void games_set_user_name_resolver(games_user_name_fn resolver);

/* Builds a JSON document describing all installed games:
 *   {"unitSeconds":1,
 *    "users":["0314904805","0314904806"],
 *    "names":["Mahdi",null],      (account names, null when unknown)
 *    "games":[{"id":"CUSA00001","name":"...","platform":"PS4",
 *              "played":[927,1655],"last":["2026-10-10 21:19:03.116",null],
 *              "has":[1,0]}],
 *    "count":N}
 * "played", "last" and "has" have one entry per user, in the order of "users"
 * ("has" is 0 when the user has no row for that game, so it cannot be edited).
 * Returns a malloc'd, NUL-terminated string (caller frees) and sets *length, or
 * NULL on failure with a message in error_message. */
char *games_list_json(const char *database_path, size_t *length,
                      char *error_message, size_t error_message_size);

/* Resolves the icon file of a title using the icon path stored in app.db.
 * Returns 0 and fills path when the file exists, -1 otherwise (also when
 * title_id is not a listed game). */
int games_icon_path(const char *database_path, const char *title_id, char *path,
                    size_t path_size);

/* Sets playedTimeOnConsole of one game for one user (user_id is the suffix of
 * tbl_iconinfo_<userId>, as listed in "users"). `value` is in column units
 * (PLAYTIME_UNIT_SECONDS seconds each). Before the first write of a run,
 * app.db is copied to GAMES_BACKUP_PATH; if that fails nothing is written.
 * Returns 0 on success, -1 with a message in error_message otherwise. */
int games_set_playtime(const char *database_path, const char *title_id,
                       const char *user_id, long long value,
                       char *error_message, size_t error_message_size);

/* Returns 1 if title_id looks like CUSAxxxxx / PPSAxxxxx and is not excluded. */
int games_is_listed_title(const char *title_id);

/* Returns 1 if title_id is syntactically a valid title ID (9 chars, A-Z0-9). */
int games_is_valid_title_id(const char *title_id);

#endif
