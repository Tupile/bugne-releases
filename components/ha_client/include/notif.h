// notif: one notification pushed by Home Assistant through POST /api/notify,
// with up to two answer buttons. The answer goes back as a JSON POST to the
// reply URL sent with the notification (a Home Assistant webhook).
// Pure (no ESP, no LVGL), host-tested in test/host/test_notif.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define NOTIF_ACTIONS_MAX 2
#define NOTIF_ID_MAX      48
#define NOTIF_TITLE_MAX   48
#define NOTIF_MSG_MAX     256
#define NOTIF_ACT_ID_MAX  32
#define NOTIF_LABEL_MAX   24
#define NOTIF_URL_MAX     256

// Room for the answer body when every byte of the id and of the action id
// needs a 6-byte escape.
#define NOTIF_REPLY_MAX (24 + 6 * (NOTIF_ID_MAX + NOTIF_ACT_ID_MAX))

typedef struct {
    char id[NOTIF_ACT_ID_MAX];     // sent back to Home Assistant
    char label[NOTIF_LABEL_MAX];   // button text
} notif_action_t;

typedef struct {
    char id[NOTIF_ID_MAX];         // chosen by the sender, "" allowed
    char title[NOTIF_TITLE_MAX];   // "" allowed
    char message[NOTIF_MSG_MAX];
    char reply_url[NOTIF_URL_MAX]; // where the answer is posted
    int count;                     // actions used, 0..NOTIF_ACTIONS_MAX
    notif_action_t act[NOTIF_ACTIONS_MAX];
} notif_t;

// True for an http:// or https:// URL with something after the scheme.
bool notif_url_ok(const char *url);

// Split an http(s) URL: copy the host name (no port) into `host` and point
// `rest` at what follows it (":8123/path", "/path" or ""). False for a URL
// notif_url_ok refuses, a host that does not fit, user info ("@") or an
// IPv6 literal ("[").
bool notif_url_split(const char *url, char *host, size_t hsize, const char **rest);

// True when the notification can be shown and answered: a message, every
// action with an id and a label, and a usable reply URL when there are
// actions. A reply URL that is present must be usable even without actions.
bool notif_valid(const notif_t *n);

// Write the answer body {"id":"...","action":"..."} with JSON string
// escaping. Returns its length, or -1 (and an empty string) when it does
// not fit: never a truncated body. NOTIF_REPLY_MAX always fits.
int notif_reply_json(char *out, size_t size, const char *id, const char *action);
