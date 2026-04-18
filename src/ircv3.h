/* ircv3.h - IRCv3 protocol extensions for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 *
 * Implements IRCv3 features that X3 needs to understand and generate:
 * - Message tags (@time, @account, @msgid, etc.)
 * - Server-time timestamps on outgoing messages
 * - Account-tag on messages from authenticated users
 * - Batch references for multi-message operations
 * - Standard replies (FAIL/WARN/NOTE)
 * - Message IDs for deduplication
 *
 * These work in conjunction with Cathexis IRCd's IRCv3 capabilities:
 *   extended-join, away-notify, account-notify, server-time,
 *   message-tags, echo-message, batch, labeled-response, account-tag
 */

#ifndef X3_IRCV3_H
#define X3_IRCV3_H

#include <stddef.h>
#include <time.h>

/* ─── Message Tags ─── */

#define IRCV3_MAX_TAGS      32
#define IRCV3_MAX_TAG_LEN   512     /* max length of a single tag key=value */
#define IRCV3_MAX_TAGS_LEN  4096    /* max total tags string length (IRCv3 limit: 8191) */

struct ircv3_tag {
    char *key;       /* tag name (e.g., "time", "account", "msgid") */
    char *value;     /* tag value (may be NULL for valueless tags) */
};

struct ircv3_tags {
    struct ircv3_tag tags[IRCV3_MAX_TAGS];
    unsigned int count;
};

/* Parse a tags string (the part after '@' and before the space).
 * Input format: "time=2024-01-01T00:00:00.000Z;account=nick;msgid=xxx"
 * Returns 0 on success.  Tags are allocated; free with ircv3_tags_free(). */
int ircv3_tags_parse(const char *raw, struct ircv3_tags *out);

/* Format tags into a string buffer.
 * Returns the length written, or -1 on error.
 * Output format: "@time=...;account=...;msgid=... " (including trailing space) */
int ircv3_tags_format(const struct ircv3_tags *tags, char *buf, size_t buf_len);

/* Free all allocated tag memory. */
void ircv3_tags_free(struct ircv3_tags *tags);

/* Add a tag.  key and value are copied.  value may be NULL. */
int ircv3_tags_add(struct ircv3_tags *tags, const char *key, const char *value);

/* Find a tag by key.  Returns value or NULL if not found. */
const char *ircv3_tags_get(const struct ircv3_tags *tags, const char *key);

/* Remove a tag by key. */
void ircv3_tags_remove(struct ircv3_tags *tags, const char *key);

/* ─── Server-Time ─── */

/* Generate an ISO 8601 timestamp for the current time.
 * Format: "2024-01-01T00:00:00.000Z"
 * buf must be at least 32 bytes. */
void ircv3_format_time(char *buf, size_t buf_len);

/* Generate server-time for a specific timestamp. */
void ircv3_format_time_ts(time_t ts, char *buf, size_t buf_len);

/* ─── Message ID (msgid) ─── */

/* Generate a unique message ID.
 * Format: base64-encoded 128-bit random value.
 * buf must be at least 24 bytes. */
void ircv3_generate_msgid(char *buf, size_t buf_len);

/* ─── Batch ─── */

#define IRCV3_BATCH_ID_LEN  16

struct ircv3_batch {
    char id[IRCV3_BATCH_ID_LEN + 1];  /* unique batch identifier */
    char type[64];                      /* batch type (e.g., "netsplit", "netjoin") */
    int active;
};

/* Start a new batch.  Generates a unique batch ID.
 * Returns 0 on success. */
int ircv3_batch_start(struct ircv3_batch *batch, const char *type);

/* ─── Standard Replies ─── */

/* Format an IRCv3 standard reply.
 * type: "FAIL", "WARN", or "NOTE"
 * command: the command this is about (e.g., "REGISTER")
 * code: machine-readable code (e.g., "ACCOUNT_EXISTS")
 * text: human-readable description
 * buf must be large enough.
 * Returns length written. */
int ircv3_standard_reply(const char *type, const char *command,
                         const char *code, const char *text,
                         char *buf, size_t buf_len);

/* ─── Tag-Aware Message Sending ─── */

/* Build a tagged message.  Prepends tags to the IRC protocol line.
 * Returns a static buffer (NOT thread-safe). */
const char *ircv3_build_tagged_message(const struct ircv3_tags *tags,
                                       const char *message);

/* Convenience: build a message with just @time and @msgid tags. */
const char *ircv3_build_timed_message(const char *message);

/* ─── Initialization ─── */

void ircv3_init(void);

#endif /* X3_IRCV3_H */
