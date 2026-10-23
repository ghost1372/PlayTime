/*
 * PlayTime - PS5 payload that serves a web UI listing installed games and their play time.
 *
 *   main.c    wiring: routes requests to the modules below
 *   games.c   goal: reads installed games from app.db
 *   server.c  minimal HTTP server (GET, plus POST for play time edits)
 *   web/index.html  the web page, embedded into the payload at build time
 *
 * Open the UI on the PS5 browser (launched automatically) or from any device
 * on the same network at http://<ps5-ip>:<PLAYTIME_PORT from config.h>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <ps5/kernel.h>

#include "config.h"
#include "games.h"
#include "server.h"

/*
 * Embed web/index.html into the payload at build time (no external tools
 * needed). Defines: const uint8_t web_index_html[]; const size_t web_index_html_size;
 * The path is relative to the directory `make` runs in.
 */
#define INCASSET(name, file)                    \
    __asm__(".section .rodata\n"                \
            ".global " #name "\n"               \
            ".global " #name "_end\n"           \
            ".global " #name "_size\n"          \
            ".align 16\n"                       \
            #name ":\n"                         \
            ".incbin \"" file "\"\n"            \
            #name "_end:\n"                     \
            #name "_size:\n"                    \
            ".quad " #name "_end - " #name "\n" \
            ".previous\n");                     \
    extern const uint8_t name[];                \
    extern const size_t name##_size;

INCASSET(web_index_html, "web/index.html");

/* Titles that are NOT listed: edit PLAYTIME_EXCLUDED_TITLES in config.h. */
static const char *const EXCLUDED_TITLES[] = {PLAYTIME_EXCLUDED_TITLES};

int sceUserServiceInitialize(void *arg);
int sceUserServiceGetUserName(int user_id, char *name, size_t size);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *arg);

typedef struct {
    const char *database_path;
    int port;
    uint32_t firmware_version;
} context_t;

static void format_firmware(char *buffer, size_t size, uint32_t version) {
    unsigned int major = ((version >> 28) & 0xFu) * 10u + ((version >> 24) & 0xFu);
    unsigned int minor = ((version >> 20) & 0xFu) * 10u + ((version >> 16) & 0xFu);

    snprintf(buffer, size, "%02u.%02u", major, minor);
}

static void send_json_error(int fd, int status, const char *message) {
    char body[384];
    char escaped[256];
    size_t pos = 0;

    for(; *message && pos + 2 < sizeof escaped; message++) {
        if(*message == '"' || *message == '\\') {
            escaped[pos++] = '\\';
            escaped[pos++] = *message;
        } else if((unsigned char)*message >= 0x20u) {
            escaped[pos++] = *message;
        }
    }
    escaped[pos] = '\0';
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", escaped);
    server_send(fd, status, "application/json; charset=utf-8", body,
                strlen(body), 0);
}

/* Looks up key in a raw query string ("a=1&b=2"). Values are copied as-is
 * (no percent-decoding; the UI only sends digits and title IDs). */
static int query_get(const char *query, const char *key, char *value,
                     size_t value_size) {
    size_t key_len = strlen(key);
    const char *p = query;

    while(*p) {
        const char *end = strchr(p, '&');
        size_t span = end ? (size_t)(end - p) : strlen(p);

        if(span > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            size_t n = span - key_len - 1;

            if(n >= value_size) {
                return -1;
            }
            memcpy(value, p + key_len + 1, n);
            value[n] = '\0';
            return 0;
        }
        if(!end) {
            break;
        }
        p = end + 1;
    }
    return -1;
}

/* POST /api/playtime?id=<TITLEID>&user=<userId>&value=<units> */
static void handle_set_playtime(int fd, const server_request_t *req,
                                const context_t *ctx) {
    char id[16];
    char user[24];
    char number[24];
    char error[256];
    char *end = NULL;
    long long value;

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    /* Browsers cannot add a custom header cross-origin without a preflight,
     * which this server never answers: this blocks drive-by requests from
     * other web pages on the network. */
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    if(query_get(req->query, "id", id, sizeof id) != 0 ||
       query_get(req->query, "user", user, sizeof user) != 0 ||
       query_get(req->query, "value", number, sizeof number) != 0 ||
       number[0] == '\0') {
        send_json_error(fd, 400, "Missing or invalid parameters.");
        return;
    }
    value = strtoll(number, &end, 10);
    if(!end || *end != '\0') {
        send_json_error(fd, 400, "Value must be a whole number.");
        return;
    }
    if(games_set_playtime(ctx->database_path, id, user, value, error,
                          sizeof error) != 0) {
        send_json_error(fd, 500, error);
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

static void handle_request(int fd, const server_request_t *req, void *user) {
    const context_t *ctx = (const context_t *)user;
    const char *path = req->path;

    if(strcmp(path, "/api/playtime") == 0) {
        handle_set_playtime(fd, req, ctx);
        return;
    }
    if(strcmp(req->method, "GET") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }

    if(strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        server_send(fd, 200, "text/html; charset=utf-8", (const char *)web_index_html,
                    web_index_html_size, 0);
    } else if(strcmp(path, "/api/info") == 0) {
        char ip[64];
        char firmware[16];
        char body[768];

        /* Branding strings come from config.h; avoid " and \ in them. */
        server_local_ip(ip, sizeof ip);
        format_firmware(firmware, sizeof firmware, ctx->firmware_version);
        snprintf(body, sizeof body,
                 "{\"ip\":\"%s\",\"port\":%d,\"firmware\":\"%s\","
                 "\"version\":\"%s\",\"name\":\"%s\",\"tagline\":\"%s\","
                 "\"author\":\"%s\",\"repo\":\"%s\",\"csrf\":\"%s\"}",
                 ip, ctx->port, firmware, PAYLOAD_VERSION, PLAYTIME_APP_NAME,
                 PLAYTIME_TAGLINE, PLAYTIME_AUTHOR, PLAYTIME_REPO_URL,
                 PLAYTIME_CSRF_HEADER);
        server_send(fd, 200, "application/json; charset=utf-8", body,
                    strlen(body), 0);
    } else if(strcmp(path, "/api/games") == 0) {
        char error[256];
        size_t length = 0;
        char *json = games_list_json(ctx->database_path, &length, error,
                                     sizeof error);

        if(!json) {
            send_json_error(fd, 500, error);
            return;
        }
        server_send(fd, 200, "application/json; charset=utf-8", json, length, 0);
        free(json);
    } else if(strncmp(path, "/icon/", 6) == 0) {
        char title_id[16];
        char icon[256];
        size_t len = strlen(path + 6);

        /* Expect exactly "<TITLEID>.png"; games_icon_path() validates the ID. */
        if(len == 13 && strcmp(path + 6 + 9, ".png") == 0) {
            memcpy(title_id, path + 6, 9);
            title_id[9] = '\0';
            if(games_icon_path(ctx->database_path, title_id, icon,
                               sizeof icon) == 0 &&
               server_send_file(fd, icon, "image/png") == 0) {
                return;
            }
        }
        server_send(fd, 404, "text/plain", "Not found", 9, 0);
    } else {
        server_send(fd, 404, "text/plain", "Not found", 9, 0);
    }
}

/* User IDs in app.db table names are the decimal console user IDs. */
static int resolve_user_name(const char *user_id, char *name, size_t size) {
    char raw[32] = {0};
    char *end = NULL;
    long id = strtol(user_id, &end, 10);

    if(!end || *end != '\0' || id <= 0 || size < sizeof raw) {
        return -1;
    }
    if(sceUserServiceGetUserName((int)id, raw, sizeof raw) != 0 || !raw[0]) {
        return -1;
    }
    raw[sizeof raw - 1] = '\0';
    snprintf(name, size, "%s", raw);
    return 0;
}

static void launch_ps5_browser(int port) {
    char url[64];

    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    (void)sceSystemServiceLaunchWebBrowser(url, NULL);
}

int main(void) {
    context_t ctx;
    int listen_fd;

    /* Name shown in the running-process list (see config.h). */
    syscall(SYS_thr_set_name, -1, PAYLOAD_PROCESS_NAME);

    ctx.database_path = APP_DB_PATH;
    ctx.port = PLAYTIME_PORT;
    ctx.firmware_version = kernel_get_fw_version();

    (void)sceUserServiceInitialize(NULL);
    games_set_user_name_resolver(resolve_user_name);
    games_set_excluded(EXCLUDED_TITLES,
                       sizeof EXCLUDED_TITLES / sizeof EXCLUDED_TITLES[0]);

    listen_fd = server_start(ctx.port);
    if(listen_fd < 0) {
        /* Port busy: an earlier instance is still serving, just open its UI. */
        launch_ps5_browser(ctx.port);
        return 0;
    }

    launch_ps5_browser(ctx.port);
    server_run(listen_fd, handle_request, &ctx);
    return 0;
}
