/* The online lobby directory (port/server/lobby/worker.js) from the 3DS:
 * HTTPS requests on a background thread (lobby_3ds.c), one at a time. */
#ifndef PORT_LOBBY_3DS_H
#define PORT_LOBBY_3DS_H
#include <stddef.h>

/* the lobby's address: sdmc:/3ds/mvc2/lobby_url.txt, else the build's
 * MVC2_LOBBY_URL; "" = none (rooms by address only) */
void lobby_init(const char *dir);
const char *lobby_url(void);
int lobby_available(void);
void lobby_shutdown(void);

/* a request: path ("/v1/list"), form ("a=1&b=2"; for a GET the query);
 * returns a ticket, or -1 when the queue is full */
int lobby_request(const char *path, const char *form, int get);
/* 0 still running, 1 done (body: the response, status: HTTP status), -1 failed
 * (body: why); a finished ticket is forgotten after it was read */
int lobby_result(int ticket, char *body, size_t cap, int *status);
void lobby_forget(int ticket);

/* form building: appends key=value (URL-encoded) */
void lobby_form(char *buf, size_t cap, const char *key, const char *value);
/* the value of "key=" in a response, "" if none */
const char *lobby_value(const char *body, const char *key, char *out, size_t cap);
#endif
