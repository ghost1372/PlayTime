#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <strings.h>

#include "config.h"
#include "server.h"

#define REQUEST_MAX 4096
#define IO_TIMEOUT_SECONDS 5

static const char *status_text(int status) {
    switch(status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

static void send_all(int fd, const char *data, size_t length) {
    while(length > 0) {
        ssize_t n = send(fd, data, length, 0);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            return;
        }
        if(n == 0) {
            return;
        }
        data += n;
        length -= (size_t)n;
    }
}

static void send_header(int fd, int status, const char *content_type,
                        size_t length, int cacheable) {
    char header[512];
    int n = snprintf(header, sizeof header,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Connection: close\r\n\r\n",
                     status, status_text(status), content_type, length,
                     cacheable
                         ? "Cache-Control: public, max-age=86400\r\n"
                         : "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
                           "Pragma: no-cache\r\n");
    if(n > 0) {
        send_all(fd, header, (size_t)n);
    }
}

void server_send(int client_fd, int status, const char *content_type,
                 const char *body, size_t length, int cacheable) {
    send_header(client_fd, status, content_type, length, cacheable);
    if(length > 0) {
        send_all(client_fd, body, length);
    }
}

int server_send_file(int client_fd, const char *file_path,
                     const char *content_type) {
    char chunk[16384];
    struct stat st;
    int fd = open(file_path, O_RDONLY);

    if(fd < 0) {
        return -1;
    }
    if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }

    send_header(client_fd, 200, content_type, (size_t)st.st_size, 1);
    for(;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if(n < 0 && errno == EINTR) {
            continue;
        }
        if(n <= 0) {
            break;
        }
        send_all(client_fd, chunk, (size_t)n);
    }
    close(fd);
    return 0;
}

void server_local_ip(char *buffer, size_t buffer_size) {
    struct ifaddrs *list = NULL;
    struct ifaddrs *entry;

    if(buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "127.0.0.1");
    if(getifaddrs(&list) != 0) {
        return;
    }
    for(entry = list; entry; entry = entry->ifa_next) {
        const struct sockaddr_in *addr;
        char text[INET_ADDRSTRLEN];

        if(!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        addr = (const struct sockaddr_in *)entry->ifa_addr;
        if(addr->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ||
           addr->sin_addr.s_addr == 0u) {
            continue;
        }
        if(inet_ntop(AF_INET, &addr->sin_addr, text, sizeof text)) {
            snprintf(buffer, buffer_size, "%s", text);
            break;
        }
    }
    freeifaddrs(list);
}

int server_start(int port) {
    struct sockaddr_in addr;
    int opt = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) {
        return -1;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if(bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
       listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Case-insensitive search for "<PLAYTIME_CSRF_HEADER>: 1" among the header lines. */
static int has_custom_header(const char *headers) {
    static const char NAME[] = PLAYTIME_CSRF_HEADER;
    const size_t name_len = sizeof NAME - 1;
    const char *p = headers;

    while(*p) {
        if(strncasecmp(p, NAME, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while(*p == ' ' || *p == '\t') {
                p++;
            }
            return *p == '1';
        }
        p = strstr(p, "\r\n");
        if(!p) {
            break;
        }
        p += 2;
    }
    return 0;
}

/* Reads until the end of the headers; the request line and the CSRF header are
 * used, everything else is ignored. */
static int read_request(int fd, char *method, size_t method_size, char *path,
                        size_t path_size, char *query, size_t query_size,
                        int *custom_header) {
    char buf[REQUEST_MAX];
    size_t used = 0;
    char *line_end;
    char *space1;
    char *space2;
    char *q;
    size_t n;

    while(used < sizeof buf - 1) {
        ssize_t got = recv(fd, buf + used, sizeof buf - 1 - used, 0);
        if(got < 0 && errno == EINTR) {
            continue;
        }
        if(got <= 0) {
            break;
        }
        used += (size_t)got;
        buf[used] = '\0';
        if(strstr(buf, "\r\n\r\n")) {
            break;
        }
    }
    if(used == 0) {
        return -1;
    }
    buf[used] = '\0';

    line_end = strstr(buf, "\r\n");
    if(!line_end) {
        return -1;
    }
    *custom_header = has_custom_header(line_end + 2);
    *line_end = '\0';

    space1 = strchr(buf, ' ');
    if(!space1) {
        return -1;
    }
    *space1++ = '\0';
    space2 = strchr(space1, ' ');
    if(space2) {
        *space2 = '\0';
    }
    query[0] = '\0';
    q = strchr(space1, '?');
    if(q) {
        *q++ = '\0';
        if(strlen(q) >= query_size) {
            return -1;
        }
        memcpy(query, q, strlen(q) + 1);
    }

    n = strlen(buf);
    if(n == 0 || n >= method_size) {
        return -1;
    }
    memcpy(method, buf, n + 1);

    n = strlen(space1);
    if(n == 0 || n >= path_size || space1[0] != '/') {
        return -1;
    }
    memcpy(path, space1, n + 1);
    return 0;
}

int server_run(int listen_fd, server_handler_t handler, void *user) {
    /* A write to a closed socket must not kill the payload. */
    signal(SIGPIPE, SIG_IGN);

    for(;;) {
        char method[16];
        char path[512];
        char query[256];
        int custom = 0;
        struct timeval tv;
        int client_fd = accept(listen_fd, NULL, NULL);

        if(client_fd < 0) {
            if(errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return -1;
        }

        /* An idle connection must not freeze the single-threaded server. */
        tv.tv_sec = IO_TIMEOUT_SECONDS;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        if(read_request(client_fd, method, sizeof method, path, sizeof path,
                        query, sizeof query, &custom) != 0) {
            server_send(client_fd, 400, "text/plain", "Bad request", 11, 0);
        } else if(strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
            server_send(client_fd, 405, "text/plain", "Method not allowed", 18, 0);
        } else {
            server_request_t request;

            request.method = method;
            request.path = path;
            request.query = query;
            request.has_custom_header = custom;
            handler(client_fd, &request, user);
        }
        close(client_fd);
    }
}
