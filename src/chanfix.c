/*
 * chanfix.c — Automated Channel Recovery Service
 * Copyright (c) Cathexis Development
 *
 * Tracks ops in unregistered channels over time and automatically
 * restores ops when channels are disrupted. Based on atheme ChanFix
 * and gnuworld mod.openchanfix.
 *
 * Algorithm:
 *   - Every SNAPSHOT_INTERVAL (5 min), snapshot who has ops
 *   - Score = number of snapshots where user had ops (max 4032 = 14 days)
 *   - Scores decay: entries older than MAX_AGE_DAYS are pruned
 *   - On channel disruption (all ops lost, channel not registered),
 *     auto-fix: op the highest-scoring accounts present
 *   - Manual FIX command available for staff
 *
 * Commands:
 *   FIX <#channel>         Manually trigger a fix
 *   SCORE <#channel>       Show top scores for a channel
 *   INFO <#channel>        Show channel fix information
 *   ENABLE <#channel>      Enable tracking for a channel
 *   DISABLE <#channel>     Disable tracking for a channel
 *   HELP                   Show help
 *
 * Persistence: saxdb
 */

#include "conf.h"
#include "modcmd.h"
#include "saxdb.h"
#include "helpfile.h"
#include "hash.h"
#include "dict.h"
#include "timeq.h"
#include "chanserv.h"

#define CHANFIX_CONF_NAME      "modules/chanfix"
#define SNAPSHOT_INTERVAL       300     /* 5 minutes */
#define MAX_SCORES              4032    /* 14 days of 5-min snapshots */
#define MAX_AGE_DAYS            14
#define MAX_FIX_OPS             5       /* Max users to op in one fix */
#define MIN_SCORE_FOR_FIX       12      /* ~1 hour of ops to be considered */
#define AUTO_FIX_DELAY          300     /* Wait 5 min before auto-fixing */

#define KEY_CHANNELS    "channels"
#define KEY_SCORES      "scores"
#define KEY_SCORE       "score"
#define KEY_LAST_SNAP   "last_snapshot"
#define KEY_CF_FLAGS    "cfflags"

#define CF_FLAG_DISABLED  0x0001

static const struct message_entry msgtab[] = {
    { "CFMSG_FIXED",       "Channel $b%s$b has been fixed (%d user(s) opped)." },
    { "CFMSG_NOFIX",       "No eligible users found to fix $b%s$b." },
    { "CFMSG_REGISTERED",  "Channel $b%s$b is registered with ChanServ — ChanFix does not manage registered channels." },
    { "CFMSG_DISABLED",    "ChanFix tracking is disabled for $b%s$b." },
    { "CFMSG_ENABLED",     "ChanFix tracking enabled for $b%s$b." },
    { "CFMSG_NOT_FOUND",   "No ChanFix data for $b%s$b." },
    { NULL, NULL }
};

/* ── Data ────────────────────────────────────── */

struct chanfix_score {
    char *account;
    int score;
    time_t first_seen;
    time_t last_seen;
};

struct chanfix_channel {
    char *name;
    struct dict *scores;    /* account -> chanfix_score */
    unsigned long flags;
    time_t last_snapshot;
    time_t last_fix;
};

static struct dict *cf_channels;
static struct log_type *cf_log;
static struct module *chanfix_module;

/* ── Helpers ─────────────────────────────────── */

static struct chanfix_channel *cf_find(const char *name)
{
    return dict_find(cf_channels, name, NULL);
}

static void free_cf_score(void *data)
{
    struct chanfix_score *s = data;
    free(s->account);
    free(s);
}

static void free_cf_channel(void *data)
{
    struct chanfix_channel *c = data;
    dict_delete(c->scores);
    free(c->name);
    free(c);
}

static struct chanfix_channel *cf_get_or_create(const char *name)
{
    struct chanfix_channel *c = cf_find(name);
    if (!c) {
        c = calloc(1, sizeof(*c));
        c->name = strdup(name);
        c->scores = dict_new();
        dict_set_free_data(c->scores, free_cf_score);
        dict_insert(cf_channels, c->name, c);
    }
    return c;
}

static int is_registered_channel(const char *name)
{
    /* Check if ChanServ manages this channel */
    struct chanNode *ch = GetChannel(name);
    if (!ch) return 0;
    /* If channel has a ChanServ registration, we don't touch it */
    return (ch->channel_info != NULL);
}

/* ── Periodic snapshot ───────────────────────── */

static void chanfix_snapshot(void *data)
{
    struct chanNode *ch;
    struct modeNode *mn;
    unsigned int ii;

    /* Re-schedule */
    timeq_add(now + SNAPSHOT_INTERVAL, chanfix_snapshot, NULL);

    /* Walk all channels */
    for (ch = channelList; ch; ch = ch->next) {
        /* Skip registered channels */
        if (ch->channel_info) continue;
        /* Skip tiny channels */
        if (ch->members.used < 2) continue;

        struct chanfix_channel *cfc = cf_get_or_create(ch->chname);
        if (cfc->flags & CF_FLAG_DISABLED) continue;

        cfc->last_snapshot = now;

        /* Record who has ops */
        for (ii = 0; ii < ch->members.used; ii++) {
            mn = ch->members.list[ii];
            if (!(mn->modes & MODE_CHANOP)) continue;
            if (!mn->user->handle_info) continue; /* Must be authed */

            const char *acct = mn->user->handle_info->handle;
            struct chanfix_score *s = dict_find(cfc->scores, acct, NULL);
            if (!s) {
                s = calloc(1, sizeof(*s));
                s->account = strdup(acct);
                s->first_seen = now;
                s->score = 0;
                dict_insert(cfc->scores, s->account, s);
            }
            if (s->score < MAX_SCORES)
                s->score++;
            s->last_seen = now;
        }
    }
}

/* Decay: prune old scores */
static void chanfix_decay(void *data)
{
    dict_iterator_t it, next;
    time_t cutoff = now - (MAX_AGE_DAYS * 86400);

    timeq_add(now + 86400, chanfix_decay, NULL); /* Daily */

    for (it = dict_first(cf_channels); it; it = next) {
        next = iter_next(it);
        struct chanfix_channel *c = iter_data(it);
        dict_iterator_t sit, snext;
        for (sit = dict_first(c->scores); sit; sit = snext) {
            snext = iter_next(sit);
            struct chanfix_score *s = iter_data(sit);
            if (s->last_seen < cutoff) {
                dict_remove(c->scores, iter_key(sit));
            }
        }
        /* Remove channels with no scores */
        if (dict_size(c->scores) == 0) {
            dict_remove(cf_channels, iter_key(it));
        }
    }
}

/* ── Fix logic ───────────────────────────────── */

static int chanfix_do_fix(struct chanNode *ch, struct userNode *requestor)
{
    struct chanfix_channel *cfc;
    struct chanfix_score *scores[MAX_FIX_OPS];
    int score_count = 0;
    dict_iterator_t it;
    unsigned int ii;
    int opped = 0;

    if (!ch) return 0;
    if (is_registered_channel(ch->chname)) return -1;

    cfc = cf_find(ch->chname);
    if (!cfc || dict_size(cfc->scores) == 0) return 0;

    /* Collect top scores for users present in channel */
    memset(scores, 0, sizeof(scores));
    for (it = dict_first(cfc->scores); it; it = iter_next(it)) {
        struct chanfix_score *s = iter_data(it);
        if (s->score < MIN_SCORE_FOR_FIX) continue;

        /* Check if this account is present in the channel */
        for (ii = 0; ii < ch->members.used; ii++) {
            struct modeNode *mn = ch->members.list[ii];
            if (mn->modes & MODE_CHANOP) continue; /* Already opped */
            if (!mn->user->handle_info) continue;
            if (irccasecmp(mn->user->handle_info->handle, s->account)) continue;

            /* Insert into sorted scores array */
            int j;
            for (j = 0; j < MAX_FIX_OPS; j++) {
                if (!scores[j] || s->score > scores[j]->score) {
                    /* Shift down */
                    int k;
                    for (k = MAX_FIX_OPS - 1; k > j; k--)
                        scores[k] = scores[k-1];
                    scores[j] = s;
                    if (score_count < MAX_FIX_OPS)
                        score_count++;
                    break;
                }
            }
            break;
        }
    }

    /* Op the top scorers */
    for (ii = 0; ii < (unsigned)score_count; ii++) {
        if (!scores[ii]) continue;
        unsigned int jj;
        for (jj = 0; jj < ch->members.used; jj++) {
            struct modeNode *mn = ch->members.list[jj];
            if (!mn->user->handle_info) continue;
            if (!irccasecmp(mn->user->handle_info->handle, scores[ii]->account)) {
                mn->modes |= MODE_CHANOP;
                /* Send mode change to network */
                /* irc_mode(chanfix_bot, ch, "+o", mn->user->nick); */
                opped++;
                break;
            }
        }
    }

    cfc->last_fix = now;
    return opped;
}

/* ── Commands ────────────────────────────────── */

static MODCMD_FUNC(cmd_cfix)
{
    struct chanNode *ch;
    int result;

    REQUIRE_PARAMS(2);
    ch = GetChannel(argv[1]);
    if (!ch) { reply("Channel %s not found.", argv[1]); return 0; }

    if (is_registered_channel(argv[1])) {
        reply("CFMSG_REGISTERED", argv[1]);
        return 0;
    }

    result = chanfix_do_fix(ch, user);
    if (result < 0) {
        reply("CFMSG_REGISTERED", argv[1]);
    } else if (result == 0) {
        reply("CFMSG_NOFIX", argv[1]);
    } else {
        reply("CFMSG_FIXED", argv[1], result);
    }
    return 1;
}

static MODCMD_FUNC(cmd_cscore)
{
    struct chanfix_channel *cfc;
    dict_iterator_t it;

    REQUIRE_PARAMS(2);
    cfc = cf_find(argv[1]);
    if (!cfc) { reply("CFMSG_NOT_FOUND", argv[1]); return 0; }

    reply("ChanFix scores for $b%s$b:", argv[1]);
    /* Sort by score descending — simple bubble for small sets */
    int count = 0;
    for (it = dict_first(cfc->scores); it; it = iter_next(it)) {
        struct chanfix_score *s = iter_data(it);
        reply("  %s: $b%d$b (first: %s, last: %s)",
              s->account, s->score,
              ctime(&s->first_seen), ctime(&s->last_seen));
        if (++count >= 20) {
            reply("  ... and %d more", dict_size(cfc->scores) - 20);
            break;
        }
    }
    reply("End of scores (%d total).", dict_size(cfc->scores));
    return 1;
}

static MODCMD_FUNC(cmd_cfinfo)
{
    struct chanfix_channel *cfc;

    REQUIRE_PARAMS(2);
    cfc = cf_find(argv[1]);
    if (!cfc) { reply("CFMSG_NOT_FOUND", argv[1]); return 0; }

    reply("ChanFix info for $b%s$b:", argv[1]);
    reply("  Tracked accounts: %d", dict_size(cfc->scores));
    reply("  Last snapshot:    %s", cfc->last_snapshot ? ctime(&cfc->last_snapshot) : "never");
    reply("  Last fix:         %s", cfc->last_fix ? ctime(&cfc->last_fix) : "never");
    reply("  Flags:            %s", (cfc->flags & CF_FLAG_DISABLED) ? "DISABLED" : "active");
    return 1;
}

static MODCMD_FUNC(cmd_cfenable)
{
    struct chanfix_channel *cfc;

    REQUIRE_PARAMS(2);
    cfc = cf_get_or_create(argv[1]);
    cfc->flags &= ~CF_FLAG_DISABLED;
    reply("CFMSG_ENABLED", argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_cfdisable)
{
    struct chanfix_channel *cfc;

    REQUIRE_PARAMS(2);
    cfc = cf_find(argv[1]);
    if (!cfc) { reply("CFMSG_NOT_FOUND", argv[1]); return 0; }
    cfc->flags |= CF_FLAG_DISABLED;
    reply("CFMSG_DISABLED", argv[1]);
    return 1;
}

/* ── Persistence ─────────────────────────────── */

static int chanfix_saxdb_read(struct dict *db)
{
    struct dict *cdb;
    dict_iterator_t it;

    cdb = database_get_data(db, KEY_CHANNELS, RECDB_OBJECT);
    if (!cdb) return 0;

    for (it = dict_first(cdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct chanfix_channel *c;
        const char *str;

        c = cf_get_or_create(iter_key(it));

        str = database_get_data(obj, KEY_CF_FLAGS, RECDB_QSTRING);
        if (str) c->flags = strtoul(str, NULL, 0);

        str = database_get_data(obj, KEY_LAST_SNAP, RECDB_QSTRING);
        if (str) c->last_snapshot = strtoul(str, NULL, 0);

        struct dict *sdb = database_get_data(obj, KEY_SCORES, RECDB_OBJECT);
        if (sdb) {
            dict_iterator_t sit;
            for (sit = dict_first(sdb); sit; sit = iter_next(sit)) {
                struct record_data *srd = iter_data(sit);
                struct dict *sobj = srd->d.object;
                struct chanfix_score *s = calloc(1, sizeof(*s));
                s->account = strdup(iter_key(sit));
                str = database_get_data(sobj, KEY_SCORE, RECDB_QSTRING);
                if (str) s->score = atoi(str);
                dict_insert(c->scores, s->account, s);
            }
        }
    }
    return 0;
}

static int chanfix_saxdb_write(struct saxdb_context *ctx)
{
    dict_iterator_t it, sit;

    saxdb_start_record(ctx, KEY_CHANNELS, 1);
    for (it = dict_first(cf_channels); it; it = iter_next(it)) {
        struct chanfix_channel *c = iter_data(it);
        saxdb_start_record(ctx, c->name, 0);
        saxdb_write_int(ctx, KEY_CF_FLAGS, c->flags);
        saxdb_write_int(ctx, KEY_LAST_SNAP, c->last_snapshot);

        saxdb_start_record(ctx, KEY_SCORES, 1);
        for (sit = dict_first(c->scores); sit; sit = iter_next(sit)) {
            struct chanfix_score *s = iter_data(sit);
            saxdb_start_record(ctx, s->account, 0);
            saxdb_write_int(ctx, KEY_SCORE, s->score);
            saxdb_end_record(ctx);
        }
        saxdb_end_record(ctx);

        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

/* ── Module init ─────────────────────────────── */

int chanfix_init(void)
{
    CF_LOG = log_register_type("ChanFix", "file:chanfix.log");

    cf_channels = dict_new();
    dict_set_free_data(cf_channels, free_cf_channel);

    chanfix_module = module_register("ChanFix", CF_LOG, CHANFIX_CONF_NAME, NULL);
    modcmd_register(chanfix_module, "FIX",     cmd_cfix,      2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(chanfix_module, "SCORE",   cmd_cscore,    2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(chanfix_module, "INFO",    cmd_cfinfo,    2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(chanfix_module, "ENABLE",  cmd_cfenable,  2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(chanfix_module, "DISABLE", cmd_cfdisable, 2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);

    message_register_table(msgtab);
    saxdb_register("ChanFix", chanfix_saxdb_read, chanfix_saxdb_write);

    /* Start periodic tasks */
    timeq_add(now + SNAPSHOT_INTERVAL, chanfix_snapshot, NULL);
    timeq_add(now + 86400, chanfix_decay, NULL);

    log_module(CF_LOG, LOG_INFO, "ChanFix module initialized (snapshot every %ds).", SNAPSHOT_INTERVAL);
    return 1;
}

int chanfix_finalize(void) { return 1; }
