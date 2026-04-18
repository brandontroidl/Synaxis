/*
 * sasl_agent_enhanced.c — Enhanced SASL agent for Synaxis/X3
 *
 * Implements the services side of the Cathexis SASL S2S flow:
 *
 * Client → Cathexis: AUTHENTICATE PLAIN
 * Cathexis → X3:     <server> SASL <target> <uid>!<fd>.<cookie> C :PLAIN
 * X3 → Cathexis:     <server> SASL <target> <uid>!<fd>.<cookie> C :+
 * Client → Cathexis: AUTHENTICATE <base64>
 * Cathexis → X3:     <server> SASL <target> <uid>!<fd>.<cookie> C :<base64>
 * X3 → Cathexis:     <server> SASL <target> <uid>!<fd>.<cookie> L <account> [accts]
 * X3 → Cathexis:     <server> SASL <target> <uid>!<fd>.<cookie> D S
 *
 * Reply codes (from Cathexis m_sasl.c):
 *   C = Continue (send data to client)
 *   L = Login success (set account)
 *   D = Done (S=success, F=fail, A=abort)
 *   M = Mechanism list
 *
 * Supported mechanisms (informed by Ergo, Atheme, Anope):
 *   PLAIN    — base64(\0username\0password)
 *   EXTERNAL — TLS client certificate (CertFP-based)
 *
 * To integrate: add call_sasl_input_func() hook in proto-p10.c cmd_sasl,
 * and register this module's sasl_input handler.
 */

#include "proto-common.c"  /* for putsock, self, etc. */
#include "nickserv.h"      /* for nickserv account lookup */
#include "hash.h"
#include "base64.h"

#include <string.h>
#include <stdlib.h>

/* SASL session state */
struct sasl_session {
    char identifier[128];   /* uid!fd.cookie token from Cathexis */
    char server_num[8];     /* Source server numeric */
    char mechanism[32];     /* PLAIN, EXTERNAL */
    char account[NICKLEN + 1];
    int  step;              /* Negotiation step counter */
};

#define MAX_SASL_SESSIONS 256
static struct sasl_session sasl_sessions[MAX_SASL_SESSIONS];
static int sasl_count = 0;

static struct sasl_session *sasl_find(const char *identifier) {
    int i;
    for (i = 0; i < sasl_count; i++)
        if (!strcmp(sasl_sessions[i].identifier, identifier))
            return &sasl_sessions[i];
    return NULL;
}

static struct sasl_session *sasl_create(const char *identifier, const char *server_num) {
    struct sasl_session *s;
    if (sasl_count >= MAX_SASL_SESSIONS) return NULL;
    s = &sasl_sessions[sasl_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->identifier, identifier, sizeof(s->identifier) - 1);
    strncpy(s->server_num, server_num, sizeof(s->server_num) - 1);
    s->step = 0;
    return s;
}

static void sasl_destroy(struct sasl_session *s) {
    int idx = s - sasl_sessions;
    if (idx < sasl_count - 1)
        memmove(s, s + 1, (sasl_count - idx - 1) * sizeof(*s));
    sasl_count--;
}

static void sasl_reply(struct sasl_session *s, const char *type, const char *data) {
    if (data)
        putsock("%s SASL %s %s %s :%s", self->numeric, s->server_num,
                s->identifier, type, data);
    else
        putsock("%s SASL %s %s %s", self->numeric, s->server_num,
                s->identifier, type);
}

/*
 * sasl_handle_input — main SASL message processor
 *
 * Called from proto-p10.c cmd_sasl via call_sasl_input_func()
 *
 * Parameters match Cathexis m_sasl.c wire format:
 *   server   — source server
 *   identifier — uid!fd.cookie token
 *   subcmd   — C (client data), D (done/abort), M (mech query)
 *   data     — mechanism name or base64 payload
 *   ext      — optional extension data
 */
void sasl_handle_input(struct server *source, const char *identifier,
                       const char *subcmd, const char *data, const char *ext)
{
    struct sasl_session *s;
    struct handle_info *hi;

    if (!subcmd || !subcmd[0]) return;

    s = sasl_find(identifier);

    /* Client abort */
    if (subcmd[0] == 'D') {
        if (s) sasl_destroy(s);
        return;
    }

    /* Mechanism query */
    if (subcmd[0] == 'S' || !strcmp(data, "PLAIN") || !strcmp(data, "EXTERNAL")) {
        if (!s) {
            s = sasl_create(identifier, source->numeric);
            if (!s) return;
        }
    }

    if (subcmd[0] != 'C') return;

    /* Step 0: Client sends mechanism name */
    if (s && s->step == 0) {
        if (!strcmp(data, "PLAIN")) {
            strncpy(s->mechanism, "PLAIN", sizeof(s->mechanism));
            s->step = 1;
            /* Request credentials: send "+" (continue) */
            sasl_reply(s, "C", "+");
            return;
        } else if (!strcmp(data, "EXTERNAL")) {
            strncpy(s->mechanism, "EXTERNAL", sizeof(s->mechanism));
            s->step = 1;
            sasl_reply(s, "C", "+");
            return;
        } else {
            /* Unsupported mechanism */
            sasl_reply(s, "M", "PLAIN,EXTERNAL");
            sasl_reply(s, "D", "F");
            sasl_destroy(s);
            return;
        }
    }

    if (!s) return;

    /* Step 1: Client sends credentials */
    if (s->step == 1) {
        if (!strcmp(s->mechanism, "PLAIN")) {
            /* Decode PLAIN: base64(\0authzid\0authcid\0password)
             * Matches Ergo's SASL PLAIN handling and Atheme's saslserv */
            char decoded[512];
            int decoded_len;
            char *authzid, *authcid, *password;
            char *p;

            decoded_len = base64_decode(decoded, sizeof(decoded), data, strlen(data));
            if (decoded_len < 3) {
                sasl_reply(s, "D", "F");
                sasl_destroy(s);
                return;
            }
            decoded[decoded_len] = '\0';

            /* Parse \0authzid\0authcid\0password */
            authzid = decoded;
            p = memchr(decoded, '\0', decoded_len);
            if (!p) { sasl_reply(s, "D", "F"); sasl_destroy(s); return; }
            authcid = p + 1;
            p = memchr(authcid, '\0', decoded_len - (authcid - decoded));
            if (!p) { sasl_reply(s, "D", "F"); sasl_destroy(s); return; }
            password = p + 1;

            /* If authzid is empty, use authcid (standard PLAIN behavior) */
            if (!*authzid) authzid = authcid;

            /* Look up the account via NickServ */
            hi = get_handle_info(authcid);
            if (!hi) {
                log_module(MAIN_LOG, LOG_INFO, "SASL PLAIN: unknown account %s",
                           authcid);
                sasl_reply(s, "D", "F");
                sasl_destroy(s);
                return;
            }

            /* Verify password (uses Synaxis crypto module — Argon2id if available) */
            if (!checkpass(password, hi->passwd)) {
                log_module(MAIN_LOG, LOG_INFO, "SASL PLAIN: bad password for %s",
                           authcid);
                sasl_reply(s, "D", "F");
                sasl_destroy(s);
                return;
            }

            /* Success! Send login confirmation */
            strncpy(s->account, hi->handle, sizeof(s->account) - 1);
            sasl_reply(s, "L", hi->handle);
            sasl_reply(s, "D", "S");
            log_module(MAIN_LOG, LOG_INFO, "SASL PLAIN: %s authenticated as %s",
                       identifier, hi->handle);
            sasl_destroy(s);
            return;

        } else if (!strcmp(s->mechanism, "EXTERNAL")) {
            /* EXTERNAL: authenticate via CertFP
             * The data field may be "+" (use default) or an authzid
             * We need the client's CertFP which was sent via FINGERPRINT token
             *
             * Matches Atheme p10-generic.c sasl EXTERNAL flow */
            char *authzid = (!strcmp(data, "+") || !*data) ? NULL : (char *)data;

            /* TODO: Look up CertFP for this session's client
             * This requires storing the mapping from SASL identifier to
             * the user's FINGERPRINT data, which Cathexis sends separately */
            sasl_reply(s, "D", "F"); /* Stub: EXTERNAL not yet fully wired */
            sasl_destroy(s);
            return;
        }
    }
}
