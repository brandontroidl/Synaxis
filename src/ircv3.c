/* ircv3.c - IRCv3 protocol extensions for X3
 * Copyright 2024-2026 Cathexis/X3 Development Team
 */

#include "ircv3.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* ─── Initialization ─── */

void
ircv3_init(void)
{
    /* Currently no state to initialize, but reserved for future use */
}

/* ─── Message Tags ─── */

/* Unescape IRCv3 tag value:  \: → ; \s → space \\ → \ \r → CR \n → LF */
static char *
tag_unescape(const char *in)
{
    size_t len = strlen(in);
    char *out = malloc(len + 1);
    size_t i, o;

    if (!out) return NULL;

    for (i = o = 0; in[i]; i++) {
        if (in[i] == '\\' && in[i+1]) {
            i++;
            switch (in[i]) {
            case ':':  out[o++] = ';';  break;
            case 's':  out[o++] = ' ';  break;
            case '\\': out[o++] = '\\'; break;
            case 'r':  out[o++] = '\r'; break;
            case 'n':  out[o++] = '\n'; break;
            default:   out[o++] = in[i]; break;
            }
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* Escape a tag value for IRCv3 wire format */
static size_t
tag_escape(const char *in, char *out, size_t out_len)
{
    size_t i, o = 0;

    for (i = 0; in[i] && o + 2 < out_len; i++) {
        switch (in[i]) {
        case ';':  out[o++] = '\\'; out[o++] = ':';  break;
        case ' ':  out[o++] = '\\'; out[o++] = 's';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        default:   out[o++] = in[i]; break;
        }
    }
    out[o] = '\0';
    return o;
}

int
ircv3_tags_parse(const char *raw, struct ircv3_tags *out)
{
    char *buf, *saveptr, *token;

    memset(out, 0, sizeof(*out));
    if (!raw || !*raw)
        return 0;

    buf = strdup(raw);
    if (!buf)
        return -1;

    for (token = strtok_r(buf, ";", &saveptr);
         token && out->count < IRCV3_MAX_TAGS;
         token = strtok_r(NULL, ";", &saveptr))
    {
        char *eq = strchr(token, '=');
        struct ircv3_tag *t = &out->tags[out->count];

        if (eq) {
            *eq = '\0';
            t->key = strdup(token);
            t->value = tag_unescape(eq + 1);
        } else {
            t->key = strdup(token);
            t->value = NULL;
        }

        if (!t->key) {
            free(buf);
            ircv3_tags_free(out);
            return -1;
        }
        out->count++;
    }

    free(buf);
    return 0;
}

int
ircv3_tags_format(const struct ircv3_tags *tags, char *buf, size_t buf_len)
{
    size_t pos = 0;
    unsigned int i;

    if (!tags || tags->count == 0) {
        buf[0] = '\0';
        return 0;
    }

    buf[pos++] = '@';
    for (i = 0; i < tags->count && pos + 2 < buf_len; i++) {
        if (i > 0)
            buf[pos++] = ';';

        size_t klen = strlen(tags->tags[i].key);
        if (pos + klen + 1 >= buf_len)
            break;
        memcpy(buf + pos, tags->tags[i].key, klen);
        pos += klen;

        if (tags->tags[i].value) {
            buf[pos++] = '=';
            pos += tag_escape(tags->tags[i].value, buf + pos, buf_len - pos);
        }
    }
    buf[pos++] = ' ';
    buf[pos] = '\0';
    return (int)pos;
}

void
ircv3_tags_free(struct ircv3_tags *tags)
{
    unsigned int i;
    for (i = 0; i < tags->count; i++) {
        free(tags->tags[i].key);
        free(tags->tags[i].value);
    }
    memset(tags, 0, sizeof(*tags));
}

int
ircv3_tags_add(struct ircv3_tags *tags, const char *key, const char *value)
{
    struct ircv3_tag *t;

    if (tags->count >= IRCV3_MAX_TAGS)
        return -1;

    /* If key already exists, update it */
    for (unsigned int i = 0; i < tags->count; i++) {
        if (strcmp(tags->tags[i].key, key) == 0) {
            free(tags->tags[i].value);
            tags->tags[i].value = value ? strdup(value) : NULL;
            return 0;
        }
    }

    t = &tags->tags[tags->count];
    t->key = strdup(key);
    t->value = value ? strdup(value) : NULL;
    if (!t->key) return -1;
    tags->count++;
    return 0;
}

const char *
ircv3_tags_get(const struct ircv3_tags *tags, const char *key)
{
    for (unsigned int i = 0; i < tags->count; i++) {
        if (strcmp(tags->tags[i].key, key) == 0)
            return tags->tags[i].value;
    }
    return NULL;
}

void
ircv3_tags_remove(struct ircv3_tags *tags, const char *key)
{
    for (unsigned int i = 0; i < tags->count; i++) {
        if (strcmp(tags->tags[i].key, key) == 0) {
            free(tags->tags[i].key);
            free(tags->tags[i].value);
            if (i + 1 < tags->count)
                memmove(&tags->tags[i], &tags->tags[i+1],
                        (tags->count - i - 1) * sizeof(struct ircv3_tag));
            tags->count--;
            return;
        }
    }
}

/* ─── Server-Time ─── */

void
ircv3_format_time(char *buf, size_t buf_len)
{
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    snprintf(buf, buf_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             tv.tv_usec / 1000);
}

void
ircv3_format_time_ts(time_t ts, char *buf, size_t buf_len)
{
    struct tm tm;
    gmtime_r(&ts, &tm);
    snprintf(buf, buf_len, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ─── Message ID ─── */

void
ircv3_generate_msgid(char *buf, size_t buf_len)
{
    unsigned char raw[16]; /* 128-bit random */
    crypto_random_bytes(raw, sizeof(raw));
    crypto_b64_encode(raw, sizeof(raw), buf);
    /* Truncate to fit buffer */
    if (buf_len > 0 && strlen(buf) >= buf_len)
        buf[buf_len - 1] = '\0';
}

/* ─── Batch ─── */

int
ircv3_batch_start(struct ircv3_batch *batch, const char *type)
{
    unsigned char raw[8];
    char b64[16];

    memset(batch, 0, sizeof(*batch));
    crypto_random_bytes(raw, sizeof(raw));
    crypto_b64_encode(raw, sizeof(raw), b64);
    /* Use first IRCV3_BATCH_ID_LEN chars, strip = padding */
    strncpy(batch->id, b64, IRCV3_BATCH_ID_LEN);
    batch->id[IRCV3_BATCH_ID_LEN] = '\0';
    /* Remove any = or / characters */
    for (char *p = batch->id; *p; p++) {
        if (*p == '=' || *p == '/')
            *p = 'x';
    }
    strncpy(batch->type, type, sizeof(batch->type) - 1);
    batch->active = 1;
    return 0;
}

/* ─── Standard Replies ─── */

int
ircv3_standard_reply(const char *type, const char *command,
                     const char *code, const char *text,
                     char *buf, size_t buf_len)
{
    return snprintf(buf, buf_len, "%s %s %s :%s", type, command, code, text);
}

/* ─── Tag-Aware Message Building ─── */

static char tagged_msg_buf[IRCV3_MAX_TAGS_LEN + 512 + 2];

const char *
ircv3_build_tagged_message(const struct ircv3_tags *tags, const char *message)
{
    int tlen;

    if (!tags || tags->count == 0) {
        strncpy(tagged_msg_buf, message, sizeof(tagged_msg_buf) - 1);
        tagged_msg_buf[sizeof(tagged_msg_buf) - 1] = '\0';
        return tagged_msg_buf;
    }

    tlen = ircv3_tags_format(tags, tagged_msg_buf, sizeof(tagged_msg_buf) - 512);
    if (tlen < 0) {
        strncpy(tagged_msg_buf, message, sizeof(tagged_msg_buf) - 1);
        return tagged_msg_buf;
    }

    strncpy(tagged_msg_buf + tlen, message, sizeof(tagged_msg_buf) - tlen - 1);
    tagged_msg_buf[sizeof(tagged_msg_buf) - 1] = '\0';
    return tagged_msg_buf;
}

const char *
ircv3_build_timed_message(const char *message)
{
    struct ircv3_tags tags;
    char timebuf[32], msgidbuf[24];

    memset(&tags, 0, sizeof(tags));
    ircv3_format_time(timebuf, sizeof(timebuf));
    ircv3_generate_msgid(msgidbuf, sizeof(msgidbuf));

    ircv3_tags_add(&tags, "time", timebuf);
    ircv3_tags_add(&tags, "msgid", msgidbuf);

    const char *result = ircv3_build_tagged_message(&tags, message);
    ircv3_tags_free(&tags);
    return result;
}
