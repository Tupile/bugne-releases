// ha_reply: posts the answer to a notification back to Home Assistant (the
// webhook URL that came with the notification). Separate from the lamp path
// of ha_client on purpose: that one keeps only the newest request, and an
// answer must never be dropped behind a slider drag.
#pragma once

#include <stdbool.h>

typedef enum {
    HA_REPLY_IDLE,   // nothing sent, or the last result was already read
    HA_REPLY_BUSY,   // a send is running
    HA_REPLY_OK,     // delivered (HTTP 2xx)
    HA_REPLY_FAIL,   // every attempt failed
} ha_reply_state_t;

// Start posting `json` to `url` on a background task. One send at a time:
// returns false while one is running, or when the task cannot be created.
// No authentication header: the webhook id in the URL is the secret.
bool ha_reply_send(const char *url, const char *json);

// State of the send. HA_REPLY_OK and HA_REPLY_FAIL are returned once, then
// the state is HA_REPLY_IDLE again.
ha_reply_state_t ha_reply_poll(void);
