#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sqlite/sqlite3.h"
#include "config.h"
#include "games.h"

#define USER_TABLE_PREFIX "tbl_iconinfo_"

/* Titles that are never listed; set from main.c via games_set_excluded(). */
static const char *const *excluded_titles = NULL;
static size_t excluded_count = 0;

void games_set_excluded(const char *const *titles, size_t count) {
    excluded_titles = titles;
    excluded_count = count;
}

typedef struct {
    char id[16];
    char name[256];
    int64_t played[PLAYTIME_MAX_USERS];
    char last[PLAYTIME_MAX_USERS][32];
    char has[PLAYTIME_MAX_USERS];
} game_t;

static games_user_name_fn user_name_resolver = NULL;

void games_set_user_name_resolver(games_user_name_fn resolver) {
    user_name_resolver = resolver;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} buffer_t;

static int buffer_reserve(buffer_t *b, size_t extra) {
    size_t needed = b->length + extra + 1;
    char *grown;
    size_t capacity;

    if(needed <= b->capacity) {
        return 0;
    }
    capacity = b->capacity ? b->capacity : 4096;
    while(capacity < needed) {
        capacity *= 2;
    }
    grown = (char *)realloc(b->data, capacity);
    if(!grown) {
        return -1;
    }
    b->data = grown;
    b->capacity = capacity;
    return 0;
}

static int buffer_append(buffer_t *b, const char *text, size_t length) {
    if(buffer_reserve(b, length) != 0) {
        return -1;
    }
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = '\0';
    return 0;
}

static int buffer_append_str(buffer_t *b, const char *text) {
    return buffer_append(b, text, strlen(text));
}

static int buffer_append_json_string(buffer_t *b, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    if(buffer_append(b, "\"", 1) != 0) {
        return -1;
    }
    for(; *p; p++) {
        char escape[8];
        int n = 0;

        switch(*p) {
            case '"':  n = snprintf(escape, sizeof escape, "\\\""); break;
            case '\\': n = snprintf(escape, sizeof escape, "\\\\"); break;
            case '\n': n = snprintf(escape, sizeof escape, "\\n"); break;
            case '\r': n = snprintf(escape, sizeof escape, "\\r"); break;
            case '\t': n = snprintf(escape, sizeof escape, "\\t"); break;
            default:
                if(*p < 0x20u) {
                    n = snprintf(escape, sizeof escape, "\\u%04x", *p);
                } else {
                    escape[0] = (char)*p;
                    n = 1;
                }
                break;
        }
        if(buffer_append(b, escape, (size_t)n) != 0) {
            return -1;
        }
    }
    return buffer_append(b, "\"", 1);
}

int games_is_valid_title_id(const char *title_id) {
    size_t i;

    if(!title_id || strlen(title_id) != 9) {
        return 0;
    }
    for(i = 0; i < 9; i++) {
        char c = title_id[i];
        if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return 0;
        }
    }
    return 1;
}

static void normalize_title_id(const char *in, char *out, size_t out_size) {
    size_t i;

    for(i = 0; in && in[i] && i + 1 < out_size; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[i] = '\0';
}

int games_is_listed_title(const char *title_id) {
    char id[16];
    size_t i;

    normalize_title_id(title_id, id, sizeof id);
    if(!games_is_valid_title_id(id)) {
        return 0;
    }
    if(strncmp(id, "CUSA", 4) != 0 && strncmp(id, "PPSA", 4) != 0) {
        return 0;
    }
    for(i = 4; i < 9; i++) {
        if(id[i] < '0' || id[i] > '9') {
            return 0;
        }
    }
    for(i = 0; i < excluded_count; i++) {
        char excluded[16];

        normalize_title_id(excluded_titles[i], excluded, sizeof excluded);
        if(strcmp(id, excluded) == 0) {
            return 0;
        }
    }
    return 1;
}

static void set_error(char *message, size_t size, const char *context,
                      sqlite3 *db) {
    snprintf(message, size, "%s: %s", context, db ? sqlite3_errmsg(db) : "error");
}

/* ---- icons -------------------------------------------------------------- */

static int starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int is_regular_file(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* The icon path stored in app.db looks like
 *   /user/appmeta/CUSA02344/icon0.png?ts=1790965106
 * Only a few known system folders are accepted, never ".." and only .png. */
static int icon_path_allowed(const char *path) {
    size_t length = strlen(path);

    if(length < 5 || strcmp(path + length - 4, ".png") != 0) {
        return 0;
    }
    if(strstr(path, "..")) {
        return 0;
    }
    return starts_with(path, "/user/appmeta/") ||
           starts_with(path, "/user/app/") ||
           starts_with(path, "/user/catalog_downloader/") ||
           starts_with(path, "/system_data/priv/appmeta/");
}

int games_icon_path(const char *database_path, const char *title_id, char *path,
                    size_t path_size) {
    static const char *const FALLBACKS[] = {
        "/user/appmeta/%s/icon0.png",
        "/system_data/priv/appmeta/%s/icon0.png",
    };
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    size_t i;

    if(!games_is_valid_title_id(title_id) || !games_is_listed_title(title_id)) {
        return -1;
    }

    if(sqlite3_open_v2(database_path, &db,
                       SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK) {
        sqlite3_busy_timeout(db, 5000);
        if(sqlite3_prepare_v2(db,
               "SELECT icon0Info FROM tbl_contentinfo WHERE titleId = ?1 LIMIT 1",
               -1, &statement, NULL) == SQLITE_OK &&
           sqlite3_bind_text(statement, 1, title_id, -1, SQLITE_STATIC) == SQLITE_OK &&
           sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char *raw = sqlite3_column_text(statement, 0);

            if(raw && strlen((const char *)raw) < path_size) {
                char *query;

                snprintf(path, path_size, "%s", (const char *)raw);
                query = strchr(path, '?');
                if(query) {
                    *query = '\0';
                }
                if(icon_path_allowed(path) && is_regular_file(path)) {
                    sqlite3_finalize(statement);
                    sqlite3_close(db);
                    return 0;
                }
            }
        }
        sqlite3_finalize(statement);
        sqlite3_close(db);
    }

    for(i = 0; i < sizeof FALLBACKS / sizeof FALLBACKS[0]; i++) {
        snprintf(path, path_size, FALLBACKS[i], title_id);
        if(is_regular_file(path)) {
            return 0;
        }
    }
    return -1;
}

/* ---- game list + play time --------------------------------------------- */

static int find_game(const game_t *games, size_t count, const char *id) {
    size_t i;

    for(i = 0; i < count; i++) {
        if(strcmp(games[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int load_games(sqlite3 *db, game_t **out_games, size_t *out_count,
                      char *error_message, size_t error_message_size) {
    static const char query[] =
        "SELECT titleId, MAX(titleName) FROM tbl_contentinfo "
        "WHERE titleId LIKE 'CUSA%' OR titleId LIKE 'PPSA%' "
        "GROUP BY titleId ORDER BY MAX(titleName) COLLATE NOCASE, titleId";
    sqlite3_stmt *statement = NULL;
    game_t *games = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int rc;

    rc = sqlite3_prepare_v2(db, query, -1, &statement, NULL);
    if(rc != SQLITE_OK) {
        set_error(error_message, error_message_size, "Read game list failed", db);
        return -1;
    }

    while((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *raw_id = sqlite3_column_text(statement, 0);
        const unsigned char *raw_name = sqlite3_column_text(statement, 1);
        char id[16];
        game_t *game;

        if(!raw_id) {
            continue;
        }
        normalize_title_id((const char *)raw_id, id, sizeof id);
        if(!games_is_listed_title(id)) {
            continue;
        }
        if(count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 64;
            game_t *grown = (game_t *)realloc(games, new_capacity * sizeof *games);

            if(!grown) {
                snprintf(error_message, error_message_size,
                         "Out of memory reading game list.");
                free(games);
                sqlite3_finalize(statement);
                return -1;
            }
            games = grown;
            capacity = new_capacity;
        }
        game = &games[count++];
        memset(game, 0, sizeof *game);
        snprintf(game->id, sizeof game->id, "%s", id);
        snprintf(game->name, sizeof game->name, "%s",
                 (raw_name && raw_name[0]) ? (const char *)raw_name : id);
    }
    if(rc != SQLITE_DONE) {
        set_error(error_message, error_message_size, "Read game list failed", db);
        free(games);
        sqlite3_finalize(statement);
        return -1;
    }
    sqlite3_finalize(statement);

    *out_games = games;
    *out_count = count;
    return 0;
}

/* Collects the per-user tables (tbl_iconinfo_<userId>). */
static int load_user_tables(sqlite3 *db, char tables[][64], size_t *count,
                            char *error_message, size_t error_message_size) {
    sqlite3_stmt *statement = NULL;
    int rc;

    *count = 0;
    rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type = 'table' "
        "AND name GLOB 'tbl_iconinfo_*' ORDER BY name",
        -1, &statement, NULL);
    if(rc != SQLITE_OK) {
        set_error(error_message, error_message_size, "Find user tables failed", db);
        return -1;
    }
    while((rc = sqlite3_step(statement)) == SQLITE_ROW && *count < PLAYTIME_MAX_USERS) {
        const unsigned char *name = sqlite3_column_text(statement, 0);

        if(name && strlen((const char *)name) < 64) {
            snprintf(tables[*count], 64, "%s", (const char *)name);
            (*count)++;
        }
    }
    sqlite3_finalize(statement);
    if(rc != SQLITE_DONE && rc != SQLITE_ROW) {
        set_error(error_message, error_message_size, "Find user tables failed", db);
        return -1;
    }
    return 0;
}

static int load_play_time(sqlite3 *db, const char *table, size_t user,
                          game_t *games, size_t game_count,
                          char *error_message, size_t error_message_size) {
    sqlite3_stmt *statement = NULL;
    char *sql = sqlite3_mprintf(
        "SELECT titleId, playedTimeOnConsole, lastPlayedDate FROM \"%w\" "
        "WHERE titleId LIKE 'CUSA%%' OR titleId LIKE 'PPSA%%'", table);
    int rc;

    if(!sql) {
        snprintf(error_message, error_message_size,
                 "Out of memory reading play time.");
        return -1;
    }
    rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    sqlite3_free(sql);
    if(rc != SQLITE_OK) {
        set_error(error_message, error_message_size, "Read play time failed", db);
        return -1;
    }

    while((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *raw_id = sqlite3_column_text(statement, 0);
        char id[16];
        int index;
        sqlite3_int64 played;
        const unsigned char *last;

        if(!raw_id) {
            continue;
        }
        normalize_title_id((const char *)raw_id, id, sizeof id);
        index = find_game(games, game_count, id);
        if(index < 0) {
            continue;
        }
        games[index].has[user] = 1;
        played = sqlite3_column_int64(statement, 1);
        games[index].played[user] = played > 0 ? played : 0;
        last = sqlite3_column_text(statement, 2);
        if(last && last[0]) {
            snprintf(games[index].last[user], sizeof games[index].last[user],
                     "%s", (const char *)last);
        }
    }
    sqlite3_finalize(statement);
    if(rc != SQLITE_DONE) {
        set_error(error_message, error_message_size, "Read play time failed", db);
        return -1;
    }
    return 0;
}

static int append_json(buffer_t *out, const game_t *games, size_t count,
                       char tables[][64], size_t user_count) {
    char number[48];
    size_t i;
    size_t u;

    snprintf(number, sizeof number, "{\"unitSeconds\":%d,\"users\":[",
             PLAYTIME_UNIT_SECONDS);
    if(buffer_append_str(out, number) != 0) {
        return -1;
    }
    for(u = 0; u < user_count; u++) {
        if((u > 0 && buffer_append_str(out, ",") != 0) ||
           buffer_append_json_string(out, tables[u] + strlen(USER_TABLE_PREFIX)) != 0) {
            return -1;
        }
    }
    if(buffer_append_str(out, "],\"names\":[") != 0) {
        return -1;
    }
    for(u = 0; u < user_count; u++) {
        char name[64];
        const char *user_id = tables[u] + strlen(USER_TABLE_PREFIX);
        int known = user_name_resolver &&
                    user_name_resolver(user_id, name, sizeof name) == 0 &&
                    name[0] != '\0';

        if((u > 0 && buffer_append_str(out, ",") != 0) ||
           (known ? buffer_append_json_string(out, name)
                  : buffer_append_str(out, "null")) != 0) {
            return -1;
        }
    }
    if(buffer_append_str(out, "],\"games\":[") != 0) {
        return -1;
    }

    for(i = 0; i < count; i++) {
        const game_t *game = &games[i];

        if((i > 0 && buffer_append_str(out, ",") != 0) ||
           buffer_append_str(out, "{\"id\":") != 0 ||
           buffer_append_json_string(out, game->id) != 0 ||
           buffer_append_str(out, ",\"name\":") != 0 ||
           buffer_append_json_string(out, game->name) != 0 ||
           buffer_append_str(out, game->id[0] == 'P' ? ",\"platform\":\"PS5\""
                                                      : ",\"platform\":\"PS4\"") != 0 ||
           buffer_append_str(out, ",\"played\":[") != 0) {
            return -1;
        }
        for(u = 0; u < user_count; u++) {
            snprintf(number, sizeof number, "%s%lld", u ? "," : "",
                     (long long)game->played[u]);
            if(buffer_append_str(out, number) != 0) {
                return -1;
            }
        }
        if(buffer_append_str(out, "],\"last\":[") != 0) {
            return -1;
        }
        for(u = 0; u < user_count; u++) {
            if((u > 0 && buffer_append_str(out, ",") != 0) ||
               (game->last[u][0]
                    ? buffer_append_json_string(out, game->last[u])
                    : buffer_append_str(out, "null")) != 0) {
                return -1;
            }
        }
        if(buffer_append_str(out, "],\"has\":[") != 0) {
            return -1;
        }
        for(u = 0; u < user_count; u++) {
            if(buffer_append_str(out, game->has[u] ? (u ? ",1" : "1")
                                                    : (u ? ",0" : "0")) != 0) {
                return -1;
            }
        }
        if(buffer_append_str(out, "]}") != 0) {
            return -1;
        }
    }

    snprintf(number, sizeof number, "],\"count\":%zu}", count);
    return buffer_append_str(out, number);
}

char *games_list_json(const char *database_path, size_t *length,
                      char *error_message, size_t error_message_size) {
    char tables[PLAYTIME_MAX_USERS][64];
    size_t user_count = 0;
    sqlite3 *db = NULL;
    game_t *games = NULL;
    size_t game_count = 0;
    buffer_t out = {0};
    size_t u;
    int rc;

    if(error_message_size) {
        error_message[0] = '\0';
    }

    rc = sqlite3_open_v2(database_path, &db,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if(rc != SQLITE_OK) {
        if(db) {
            set_error(error_message, error_message_size, "Open app.db failed", db);
            sqlite3_close(db);
        } else {
            snprintf(error_message, error_message_size,
                     "Open app.db failed (SQLite error %d).", rc);
        }
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);

    if(load_games(db, &games, &game_count, error_message, error_message_size) != 0 ||
       load_user_tables(db, tables, &user_count, error_message,
                        error_message_size) != 0) {
        goto fail;
    }
    for(u = 0; u < user_count; u++) {
        if(load_play_time(db, tables[u], u, games, game_count, error_message,
                          error_message_size) != 0) {
            goto fail;
        }
    }

    if(append_json(&out, games, game_count, tables, user_count) != 0) {
        snprintf(error_message, error_message_size,
                 "Out of memory building game list.");
        free(out.data);
        goto fail;
    }

    free(games);
    sqlite3_close(db);
    if(length) {
        *length = out.length;
    }
    return out.data;

fail:
    free(games);
    sqlite3_close(db);
    return NULL;
}

/* ---- play time editing -------------------------------------------------- */

/* Copies src to dst (overwrites). Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    char chunk[16384];
    int in = open(src, O_RDONLY);
    int out;
    int ok = 1;

    if(in < 0) {
        return -1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(out < 0) {
        close(in);
        return -1;
    }
    for(;;) {
        ssize_t n = read(in, chunk, sizeof chunk);
        char *p = chunk;

        if(n < 0 && errno == EINTR) {
            continue;
        }
        if(n < 0) {
            ok = 0;
            break;
        }
        if(n == 0) {
            break;
        }
        while(n > 0) {
            ssize_t w = write(out, p, (size_t)n);

            if(w < 0 && errno == EINTR) {
                continue;
            }
            if(w <= 0) {
                ok = 0;
                break;
            }
            p += w;
            n -= w;
        }
        if(!ok) {
            break;
        }
    }
    close(in);
    if(close(out) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

static int user_id_is_valid(const char *user_id) {
    size_t i;
    size_t length = user_id ? strlen(user_id) : 0;

    if(length == 0 || length > 20) {
        return 0;
    }
    for(i = 0; i < length; i++) {
        if(user_id[i] < '0' || user_id[i] > '9') {
            return 0;
        }
    }
    return 1;
}

int games_set_playtime(const char *database_path, const char *title_id,
                       const char *user_id, long long value,
                       char *error_message, size_t error_message_size) {
    static int backed_up = 0;
    char id[16];
    char table[64];
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    char *sql;
    int rc;

    normalize_title_id(title_id, id, sizeof id);
    if(!games_is_listed_title(id)) {
        snprintf(error_message, error_message_size, "Unknown game.");
        return -1;
    }
    if(!user_id_is_valid(user_id)) {
        snprintf(error_message, error_message_size, "Invalid user.");
        return -1;
    }
    if(value < 0 || value > PLAYTIME_MAX_VALUE) {
        snprintf(error_message, error_message_size, "Play time out of range.");
        return -1;
    }
    snprintf(table, sizeof table, USER_TABLE_PREFIX "%s", user_id);

    if(!backed_up) {
        if(copy_file(database_path, GAMES_BACKUP_PATH) != 0) {
            snprintf(error_message, error_message_size,
                     "Backup to " GAMES_BACKUP_PATH " failed, nothing changed.");
            return -1;
        }
        backed_up = 1;
    }

    rc = sqlite3_open_v2(database_path, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    if(rc != SQLITE_OK) {
        set_error(error_message, error_message_size,
                  "Open app.db for writing failed", db);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);

    /* The table name only comes from digits, but still must exist. */
    rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1",
        -1, &statement, NULL);
    if(rc != SQLITE_OK ||
       sqlite3_bind_text(statement, 1, table, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        snprintf(error_message, error_message_size, "No such user.");
        return -1;
    }
    sqlite3_finalize(statement);
    statement = NULL;

    sql = sqlite3_mprintf(
        "UPDATE \"%w\" SET playedTimeOnConsole = ?1 WHERE titleId = ?2", table);
    if(!sql) {
        sqlite3_close(db);
        snprintf(error_message, error_message_size, "Out of memory.");
        return -1;
    }
    rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    sqlite3_free(sql);
    if(rc != SQLITE_OK ||
       sqlite3_bind_int64(statement, 1, (sqlite3_int64)value) != SQLITE_OK ||
       sqlite3_bind_text(statement, 2, id, -1, SQLITE_STATIC) != SQLITE_OK) {
        set_error(error_message, error_message_size, "Prepare update failed", db);
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return -1;
    }
    rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if(rc != SQLITE_DONE) {
        set_error(error_message, error_message_size, "Write play time failed", db);
        sqlite3_close(db);
        return -1;
    }
    if(sqlite3_changes(db) == 0) {
        sqlite3_close(db);
        snprintf(error_message, error_message_size,
                 "This user has no entry for that game.");
        return -1;
    }
    sqlite3_close(db);
    return 0;
}
