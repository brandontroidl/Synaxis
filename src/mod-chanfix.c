/* mod-chanfix.c — Automated Channel Recovery Service
 * Copyright (c) Cathexis Development
 */

#include "chanserv.h"
#include "conf.h"
#include "modcmd.h"
#include "nickserv.h"
#include "saxdb.h"
#include "log.h"
#include "dict.h"
#include "hash.h"
#include "timeq.h"
#include "common.h"

#define REQUIRE_PARAMS(N) if(argc < (N)) { reply("MSG_MISSING_PARAMS", argv[0]); return 0; }
#define CHANFIX_CONF_NAME "modules/chanfix"
#define SNAPSHOT_INTERVAL 300
#define MAX_SCORES 4032
#define MAX_FIX_OPS 5
#define MIN_SCORE_FOR_FIX 12

const char *chanfix_module_deps[] = { NULL };

struct chanfix_score { char *account; int score; time_t last_seen; };
struct chanfix_channel { char *name; dict_t scores; unsigned long flags; time_t last_snapshot; };

#define CF_FLAG_DISABLED 0x0001

static dict_t cf_channels;
static struct log_type *cf_log;
static struct module *chanfix_module;

static void free_cf_score(void *data) { struct chanfix_score *s = data; free(s->account); free(s); }
static void free_cf_channel(void *data) { struct chanfix_channel *c = data; dict_delete(c->scores); free(c->name); free(c); }

static struct chanfix_channel *cf_get_or_create(const char *name) {
    struct chanfix_channel *c = dict_find(cf_channels, name, NULL);
    if (!c) {
        c = calloc(1, sizeof(*c)); c->name = strdup(name);
        c->scores = dict_new(); dict_set_free_data(c->scores, free_cf_score);
        dict_insert(cf_channels, c->name, c);
    }
    return c;
}

static void chanfix_snapshot(UNUSED_ARG(void *data)) {
    dict_iterator_t it;
    timeq_add(now + SNAPSHOT_INTERVAL, chanfix_snapshot, NULL);
    for (it = dict_first(channels); it; it = iter_next(it)) {
        struct chanNode *ch = iter_data(it);
        struct chanfix_channel *cfc; unsigned int ii;
        if (ch->channel_info) continue;
        if (ch->members.used < 2) continue;
        cfc = cf_get_or_create(ch->name);
        if (cfc->flags & CF_FLAG_DISABLED) continue;
        cfc->last_snapshot = now;
        for (ii = 0; ii < ch->members.used; ii++) {
            struct modeNode *mn = ch->members.list[ii];
            struct chanfix_score *s;
            if (!(mn->modes & MODE_CHANOP) || !mn->user->handle_info) continue;
            s = dict_find(cfc->scores, mn->user->handle_info->handle, NULL);
            if (!s) {
                s = calloc(1, sizeof(*s)); s->account = strdup(mn->user->handle_info->handle);
                s->score = 0; dict_insert(cfc->scores, s->account, s);
            }
            if (s->score < MAX_SCORES) s->score++;
            s->last_seen = now;
        }
    }
}

static MODCMD_FUNC(cmd_cfix) {
    struct chanNode *ch; struct chanfix_channel *cfc; dict_iterator_t it;
    int opped = 0; unsigned int ii;
    REQUIRE_PARAMS(2);
    ch = GetChannel(argv[1]); if (!ch) { reply("Channel %s not found.", argv[1]); return 0; }
    if (ch->channel_info) { reply("$b%s$b is registered — ChanFix does not manage registered channels.", argv[1]); return 0; }
    cfc = dict_find(cf_channels, argv[1], NULL);
    if (!cfc || !dict_size(cfc->scores)) { reply("No ChanFix data for $b%s$b.", argv[1]); return 0; }
    for (it = dict_first(cfc->scores); it && opped < MAX_FIX_OPS; it = iter_next(it)) {
        struct chanfix_score *s = iter_data(it);
        if (s->score < MIN_SCORE_FOR_FIX) continue;
        for (ii = 0; ii < ch->members.used; ii++) {
            struct modeNode *mn = ch->members.list[ii];
            if ((mn->modes & MODE_CHANOP) || !mn->user->handle_info) continue;
            if (!irccasecmp(mn->user->handle_info->handle, s->account)) {
                mn->modes |= MODE_CHANOP; opped++; break;
            }
        }
    }
    reply("ChanFix: opped %d user(s) in $b%s$b.", opped, argv[1]); return 1;
}

static MODCMD_FUNC(cmd_cscore) {
    struct chanfix_channel *cfc; dict_iterator_t it; int count = 0;
    REQUIRE_PARAMS(2);
    cfc = dict_find(cf_channels, argv[1], NULL);
    if (!cfc) { reply("No ChanFix data for $b%s$b.", argv[1]); return 0; }
    reply("Scores for $b%s$b:", argv[1]);
    for (it = dict_first(cfc->scores); it; it = iter_next(it)) {
        struct chanfix_score *s = iter_data(it);
        reply("  %s: $b%d$b", s->account, s->score);
        if (++count >= 20) { reply("  ...and %d more", dict_size(cfc->scores) - 20); break; }
    }
    return 1;
}

static MODCMD_FUNC(cmd_cfinfo) {
    struct chanfix_channel *cfc;
    REQUIRE_PARAMS(2);
    cfc = dict_find(cf_channels, argv[1], NULL);
    if (!cfc) { reply("No ChanFix data for $b%s$b.", argv[1]); return 0; }
    reply("ChanFix info for $b%s$b:", argv[1]);
    reply("  Accounts: %d", dict_size(cfc->scores));
    reply("  Flags:    %s", (cfc->flags & CF_FLAG_DISABLED) ? "DISABLED" : "active");
    return 1;
}

static MODCMD_FUNC(cmd_cfenable) {
    struct chanfix_channel *cfc;
    REQUIRE_PARAMS(2);
    cfc = cf_get_or_create(argv[1]); cfc->flags &= ~CF_FLAG_DISABLED;
    reply("ChanFix enabled for $b%s$b.", argv[1]); return 1;
}

static MODCMD_FUNC(cmd_cfdisable) {
    struct chanfix_channel *cfc;
    REQUIRE_PARAMS(2);
    cfc = dict_find(cf_channels, argv[1], NULL);
    if (!cfc) { reply("No ChanFix data for $b%s$b.", argv[1]); return 0; }
    cfc->flags |= CF_FLAG_DISABLED;
    reply("ChanFix disabled for $b%s$b.", argv[1]); return 1;
}

static int chanfix_saxdb_read(struct dict *db) {
    struct dict *cdb = database_get_data(db, "channels", RECDB_OBJECT);
    dict_iterator_t it;
    if (!cdb) return 0;
    for (it = dict_first(cdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct chanfix_channel *c = cf_get_or_create(iter_key(it));
        const char *str;
        str = database_get_data(obj, "cfflags", RECDB_QSTRING);
        if (str) c->flags = strtoul(str, NULL, 0);
        {
            struct dict *sdb = database_get_data(obj, "scores", RECDB_OBJECT);
            if (sdb) {
                dict_iterator_t sit;
                for (sit = dict_first(sdb); sit; sit = iter_next(sit)) {
                    struct record_data *srd = iter_data(sit);
                    struct dict *sobj = srd->d.object;
                    struct chanfix_score *s = calloc(1, sizeof(*s));
                    s->account = strdup(iter_key(sit));
                    str = database_get_data(sobj, "score", RECDB_QSTRING);
                    if (str) s->score = atoi(str);
                    dict_insert(c->scores, s->account, s);
                }
            }
        }
    }
    return 0;
}

static int chanfix_saxdb_write(struct saxdb_context *ctx) {
    dict_iterator_t it, sit;
    saxdb_start_record(ctx, "channels", 1);
    for (it = dict_first(cf_channels); it; it = iter_next(it)) {
        struct chanfix_channel *c = iter_data(it);
        saxdb_start_record(ctx, c->name, 0);
        saxdb_write_int(ctx, "cfflags", c->flags);
        saxdb_start_record(ctx, "scores", 1);
        for (sit = dict_first(c->scores); sit; sit = iter_next(sit)) {
            struct chanfix_score *s = iter_data(sit);
            saxdb_start_record(ctx, s->account, 0);
            saxdb_write_int(ctx, "score", s->score);
            saxdb_end_record(ctx);
        }
        saxdb_end_record(ctx);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

int chanfix_init(void) {
    const char *str;
    cf_log = log_register_type("ChanFix", "file:chanfix.log");
    cf_channels = dict_new(); dict_set_free_data(cf_channels, free_cf_channel);
    chanfix_module = module_register("ChanFix", cf_log, "mod-chanfix.help", NULL);
    modcmd_register(chanfix_module, "FIX",     cmd_cfix,      2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(chanfix_module, "SCORE",   cmd_cscore,    2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(chanfix_module, "INFO",    cmd_cfinfo,    2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(chanfix_module, "ENABLE",  cmd_cfenable,  2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(chanfix_module, "DISABLE", cmd_cfdisable, 2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    saxdb_register("ChanFix", chanfix_saxdb_read, chanfix_saxdb_write);
    timeq_add(now + SNAPSHOT_INTERVAL, chanfix_snapshot, NULL);

    str = conf_get_data("modules/chanfix/bot", RECDB_QSTRING);
    if (!str) str = conf_get_data("modules/chanfix/nick", RECDB_QSTRING);
    if (str) {
        const char *modes = conf_get_data("modules/chanfix/modes", RECDB_QSTRING);
        struct userNode *bot = AddLocalUser(str, str, NULL, "Automated Channel Recovery", modes);
        if (bot) service_register(bot);
    }
    return 1;
}
int chanfix_finalize(void) { return 1; }
