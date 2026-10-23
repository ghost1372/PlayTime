#ifndef PLAYTIME_SERVER_H
#define PLAYTIME_SERVER_H

#include <stddef.h>

/*
 * Minimal single-threaded HTTP/1.1 server. It parses the request line and one
 * header and hands GET/POST requests to a handler; it knows nothing about games or HTML.
 */

typedef struct {
    const char *method;  /* "GET" or "POST" */
    const char *path;    /* query string removed */
    const char *query;   /* text after '?', "" if none (raw, not decoded) */
    int has_custom_header; /* request carried "<PLAYTIME_CSRF_HEADER>: 1" (CSRF guard) */
} server_request_t;

/* Called once per GET/POST request (POST bodies are ignored; parameters travel
 * in the query string). The handler must send exactly one response with
 * server_send()/server_send_file(). The server closes the connection after. */
typedef void (*server_handler_t)(int client_fd, const server_request_t *request,
                                 void *user);

/* Binds and listens on all interfaces. Returns the listening socket, or -1
 * (for example when the port is already in use). */
int server_start(int port);

/* Serves requests until accept() fails for good. Returns -1 in that case. */
int server_run(int listen_fd, server_handler_t handler, void *user);

/* Writes the console's LAN IPv4 address ("127.0.0.1" if none is found). */
void server_local_ip(char *buffer, size_t buffer_size);

/* Sends a complete response. cacheable=0 sends no-store headers. */
void server_send(int client_fd, int status, const char *content_type,
                 const char *body, size_t length, int cacheable);

/* Streams a regular file as the response body. Returns 0, or -1 if the file
 * could not be opened (nothing has been sent in that case). */
int server_send_file(int client_fd, const char *file_path,
                     const char *content_type);

#endif
