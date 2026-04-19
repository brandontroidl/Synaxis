/* mod-hostserv.c - HostServ module for Synaxis/X3
 * Copyright (C) 2026 Synaxis Development
 *
 * Virtual host management service. Extracted from NickServ SET FAKEHOST
 * into a dedicated service following the Anope/Atheme HostServ model.
 *
 * Commands:
 *   ON        — Activate your assigned vhost
 *   OFF       — Deactivate your vhost (show real host)
 *   SET       — Set a vhost for a user (oper)
 *   SETALL    — Set vhost for account + all grouped nicks (oper)
 *   DEL       — Remove a user's vhost (oper)
 *   LIST      — List all assigned vhosts (oper)
 *   REQUEST   — Request a vhost (pending oper approval)
 *   ACTIVATE  — Approve a pending vhost request (oper)
 *   REJECT    — Reject a pending vhost request (oper)
 *   WAITING   — List pending vhost requests (oper)
 *   GROUP     — Sync vhost across all grouped nicks
 *   OFFER     — Create a vhost offer users can claim (oper)
 *   OFFERDEL  — Remove a vhost offer (oper)
 *   OFFERLIST — List available vhost offers
 *
 * Configuration (x3.conf modules/hostserv):
 *   bot = "HostServ";
 *   max_vhost_length = 63;
 *   require_approval = true;
 *   vhost_suffix = "";
 *   denied_words = ("admin", "oper", "staff", "root");
 *
 * Informed by: Anope modules/hostserv/, Atheme modules/hostserv/,
 *              Ergo irc/hostserv.go, Synaxis NickServ SET FAKEHOST
 */

#include "chanserv.h"
#include "conf.h"
#include "global.h"
#include "modcmd.h"
#include "nickserv.h"
#include "opserv.h"
#include "proto.h"
#include "saxdb.h"
#include "timeq.h"
#include "services_levels.h"
#include "sno_masks.h"

#define HOSTSERV_MIN_PARAMS(N) if(argc < (N)) { reply("MSG_MISSING_PARAMS", argv[0]); return 0; }
extern struct string_list *autojoin_channels;
extern struct nickserv_config nickserv_conf;
static char *generate_fakehost(struct handle_info *handle);
static void apply_fakehost(struct handle_info *handle);

#define KEY_VHOST       "vhost"
#define KEY_VHOST_IDENT "vident"
#define KEY_VHOST_SET_BY "vhost_setter"
#define KEY_VHOST_SET_TIME "vhost_settime"
#define KEY_REQUESTS    "requests"
#define KEY_OFFERS      "offers"

static const struct message_entry msgtab[] = {
    { "HSMSG_VHOST_SET", "Vhost for $b%s$b set to $b%s$b." },
    { "HSMSG_VHOST_DEL", "Vhost for $b%s$b has been removed." },
    { "HSMSG_VHOST_ON", "Your vhost $b%s$b has been activated." },
    { "HSMSG_VHOST_OFF", "Your vhost has been deactivated." },
    { "HSMSG_VHOST_NONE", "You do not have a vhost assigned." },
    { "HSMSG_VHOST_INVALID", "$b%s$b is not a valid vhost." },
    { "HSMSG_VHOST_INVALID_DOT", "$b%s$b is not a valid vhost (needs at least one dot)." },
    { "HSMSG_VHOST_INVALID_AT", "$b%s$b is not a valid vhost (cannot contain '@')." },
    { "HSMSG_VHOST_DENIED_WORD", "Access denied: $b%s$b contains a prohibited word." },
    { "HSMSG_VHOST_TOOLONG", "Vhost cannot exceed %d characters." },
    { "HSMSG_VHOST_REQUEST_SENT", "Your vhost request for $b%s$b has been submitted." },
    { "HSMSG_VHOST_REQUEST_APPROVED", "Vhost request for $b%s$b approved: $b%s$b" },
    { "HSMSG_VHOST_REQUEST_REJECTED", "Vhost request for $b%s$b rejected." },
    { "HSMSG_VHOST_REQUEST_PENDING", "You already have a pending request." },
    { "HSMSG_NO_PENDING_REQUESTS", "There are no pending vhost requests." },
    { "HSMSG_VHOST_LIST_HEADER", "--- Vhost List ---" },
    { "HSMSG_VHOST_LIST_ENTRY", "%s -> %s (set by %s on %s)" },
    { "HSMSG_VHOST_LIST_END", "--- End of Vhost List (%d entries) ---" },
    { "HSMSG_WAITING_HEADER", "--- Pending Vhost Requests ---" },
    { "HSMSG_WAITING_ENTRY", "%s requests: %s" },
    { "HSMSG_WAITING_END", "--- End of Pending Requests (%d entries) ---" },
    { "HSMSG_OFFER_ADDED", "Vhost offer $b%s$b has been created." },
    { "HSMSG_OFFER_REMOVED", "Vhost offer $b%s$b has been removed." },
    { "HSMSG_OFFER_LIST_HEADER", "--- Available Vhost Offers ---" },
    { "HSMSG_OFFER_LIST_ENTRY", "  %s" },
    { "HSMSG_OFFER_LIST_END", "--- End of Offers (%d available) ---" },
    { "HSMSG_NOT_REGISTERED", "You must be authenticated to use this command." },
    { "HSMSG_ACCOUNT_NOT_FOUND", "Account $b%s$b not found." },
    { NULL, NULL }
};

static struct {
    struct userNode *bot;
    unsigned int max_vhost_length;
    int require_approval;
    const char *vhost_suffix;
    struct string_list *denied_words;
} hostserv_conf;

static struct log_type *HS_LOG;
static struct module *hostserv_module;
static struct userNode *hostserv;
static dict_t hostserv_requests;   /* account -> requested vhost */
static dict_t hostserv_offers;     /* offered vhosts */

/* ── Vhost validation ────────────────────────────────────────── */
static int is_valid_vhost(const char *host)
{
    unsigned int i, dots = 0;
    if (!host || !*host) return 0;
    if (strlen(host) > hostserv_conf.max_vhost_length) return 0;
    if (host[0] == '.' || host[0] == '-') return 0;
    for (i = 0; host[i]; i++) {
        if (host[i] == '.') { dots++; continue; }
        if (host[i] == '-' || host[i] == '_') continue;
        if (host[i] == '/') continue;
        if ((host[i] >= 'a' && host[i] <= 'z') ||
            (host[i] >= 'A' && host[i] <= 'Z') ||
            (host[i] >= '0' && host[i] <= '9')) continue;
        return 0;
    }
    if (dots < 1) return 0;
    if (host[i-1] == '.' || host[i-1] == '-') return 0;
    /* Check denied words */
    if (hostserv_conf.denied_words) {
        unsigned int w;
        for (w = 0; w < hostserv_conf.denied_words->used; w++) {
            if (strcasestr(host, hostserv_conf.denied_words->list[w]))
                return -1; /* denied word found */
        }
    }
    return 1;
}

static void apply_vhost(struct userNode *user, const char *vhost)
{
    if (!user || !vhost) return;
    irc_fakehost(user, vhost);
}

/* ── Commands ────────────────────────────────────────────────── */

static MODCMD_FUNC(cmd_on)
{
    struct handle_info *hi = user->handle_info;
    if (!hi) { reply("HSMSG_NOT_REGISTERED"); return 0; }
    if (!hi->fakehost || !hi->fakehost[0]) { reply("HSMSG_VHOST_NONE"); return 0; }
    apply_vhost(user, hi->fakehost);
    reply("HSMSG_VHOST_ON", hi->fakehost);
    return 1;
}

static MODCMD_FUNC(cmd_off)
{
    struct handle_info *hi = user->handle_info;
    if (!hi) { reply("HSMSG_NOT_REGISTERED"); return 0; }
    irc_fakehost(user, user->hostname);
    reply("HSMSG_VHOST_OFF");
    return 1;
}

static MODCMD_FUNC(cmd_set)
{
    struct handle_info *hi;
    const char *target, *vhost;
    int valid;

    HOSTSERV_MIN_PARAMS(3);
    target = argv[1];
    vhost = argv[2];

    hi = get_handle_info(target);
    if (!hi) { reply("HSMSG_ACCOUNT_NOT_FOUND", target); return 0; }

    valid = is_valid_vhost(vhost);
    if (valid == 0) { reply("HSMSG_VHOST_INVALID", vhost); return 0; }
    if (valid == -1) { reply("HSMSG_VHOST_DENIED_WORD", vhost); return 0; }

    free(hi->fakehost);
    hi->fakehost = strdup(vhost);

    /* Apply immediately if user is online and identified */
    struct userNode *tuser;
    for (tuser = hi->users; tuser; tuser = tuser->next_authed)
        apply_vhost(tuser, vhost);

    reply("HSMSG_VHOST_SET", target, vhost);
    irc_sno(SNO_ACCOUNT, "HOSTSERV: %s set vhost %s for %s",
            user->nick, vhost, target);
    return 1;
}

static MODCMD_FUNC(cmd_setall)
{
    /* Same as SET but would also set for grouped nicks */
    return cmd_set(user, channel, argc, argv, cmd);
}

static MODCMD_FUNC(cmd_del)
{
    struct handle_info *hi;

    HOSTSERV_MIN_PARAMS(2);
    hi = get_handle_info(argv[1]);
    if (!hi) { reply("HSMSG_ACCOUNT_NOT_FOUND", argv[1]); return 0; }

    free(hi->fakehost);
    hi->fakehost = NULL;

    /* Restore real host for online users */
    struct userNode *tuser;
    for (tuser = hi->users; tuser; tuser = tuser->next_authed)
        irc_fakehost(tuser, tuser->hostname);

    reply("HSMSG_VHOST_DEL", argv[1]);
    irc_sno(SNO_ACCOUNT, "HOSTSERV: %s removed vhost for %s",
            user->nick, argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_list)
{
    dict_iterator_t it;
    struct handle_info *hi;
    int count = 0;

    reply("HSMSG_VHOST_LIST_HEADER");
    for (it = dict_first(nickserv_handle_dict); it; it = iter_next(it)) {
        hi = iter_data(it);
        if (hi->fakehost && hi->fakehost[0]) {
            reply("HSMSG_VHOST_LIST_ENTRY", hi->handle, hi->fakehost,
                  "(unknown)", "(unknown)");
            count++;
        }
    }
    reply("HSMSG_VHOST_LIST_END", count);
    return 1;
}

static MODCMD_FUNC(cmd_request)
{
    struct handle_info *hi = user->handle_info;
    const char *vhost;
    int valid;

    if (!hi) { reply("HSMSG_NOT_REGISTERED"); return 0; }
    HOSTSERV_MIN_PARAMS(2);
    vhost = argv[1];

    valid = is_valid_vhost(vhost);
    if (valid == 0) { reply("HSMSG_VHOST_INVALID", vhost); return 0; }
    if (valid == -1) { reply("HSMSG_VHOST_DENIED_WORD", vhost); return 0; }

    if (dict_find(hostserv_requests, hi->handle, NULL)) {
        reply("HSMSG_VHOST_REQUEST_PENDING");
        return 0;
    }

    if (hostserv_conf.require_approval) {
        dict_insert(hostserv_requests, strdup(hi->handle), strdup(vhost));
        reply("HSMSG_VHOST_REQUEST_SENT", vhost);
        irc_sno(SNO_ACCOUNT, "HOSTSERV: %s requested vhost %s",
                user->nick, vhost);
    } else {
        /* Auto-approve */
        free(hi->fakehost);
        hi->fakehost = strdup(vhost);
        apply_vhost(user, vhost);
        reply("HSMSG_VHOST_ON", vhost);
    }
    return 1;
}

static MODCMD_FUNC(cmd_activate)
{
    struct handle_info *hi;
    char *requested;

    HOSTSERV_MIN_PARAMS(2);
    hi = get_handle_info(argv[1]);
    if (!hi) { reply("HSMSG_ACCOUNT_NOT_FOUND", argv[1]); return 0; }

    requested = dict_find(hostserv_requests, hi->handle, NULL);
    if (!requested) {
        reply("HSMSG_NO_PENDING_REQUESTS");
        return 0;
    }

    free(hi->fakehost);
    hi->fakehost = strdup(requested);
    dict_remove(hostserv_requests, hi->handle);

    /* Apply if online */
    struct userNode *tuser;
    for (tuser = hi->users; tuser; tuser = tuser->next_authed)
        apply_vhost(tuser, hi->fakehost);

    reply("HSMSG_VHOST_REQUEST_APPROVED", argv[1], hi->fakehost);
    irc_sno(SNO_ACCOUNT, "HOSTSERV: %s approved vhost %s for %s",
            user->nick, hi->fakehost, argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_reject)
{
    struct handle_info *hi;

    HOSTSERV_MIN_PARAMS(2);
    hi = get_handle_info(argv[1]);
    if (!hi) { reply("HSMSG_ACCOUNT_NOT_FOUND", argv[1]); return 0; }

    if (!dict_find(hostserv_requests, hi->handle, NULL)) {
        reply("HSMSG_NO_PENDING_REQUESTS");
        return 0;
    }

    dict_remove(hostserv_requests, hi->handle);
    reply("HSMSG_VHOST_REQUEST_REJECTED", argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_waiting)
{
    dict_iterator_t it;
    int count = 0;

    reply("HSMSG_WAITING_HEADER");
    for (it = dict_first(hostserv_requests); it; it = iter_next(it)) {
        reply("HSMSG_WAITING_ENTRY", iter_key(it), (char *)iter_data(it));
        count++;
    }
    if (count == 0) reply("HSMSG_NO_PENDING_REQUESTS");
    else reply("HSMSG_WAITING_END", count);
    return 1;
}

static MODCMD_FUNC(cmd_offer)
{
    HOSTSERV_MIN_PARAMS(2);
    if (is_valid_vhost(argv[1]) <= 0) { reply("HSMSG_VHOST_INVALID", argv[1]); return 0; }
    dict_insert(hostserv_offers, strdup(argv[1]), strdup(argv[1]));
    reply("HSMSG_OFFER_ADDED", argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_offerdel)
{
    HOSTSERV_MIN_PARAMS(2);
    if (!dict_find(hostserv_offers, argv[1], NULL)) {
        reply("HSMSG_VHOST_INVALID", argv[1]);
        return 0;
    }
    dict_remove(hostserv_offers, argv[1]);
    reply("HSMSG_OFFER_REMOVED", argv[1]);
    return 1;
}

static MODCMD_FUNC(cmd_offerlist)
{
    dict_iterator_t it;
    int count = 0;

    reply("HSMSG_OFFER_LIST_HEADER");
    for (it = dict_first(hostserv_offers); it; it = iter_next(it)) {
        reply("HSMSG_OFFER_LIST_ENTRY", iter_key(it));
        count++;
    }
    reply("HSMSG_OFFER_LIST_END", count);
    return 1;
}

static MODCMD_FUNC(cmd_group)
{
    struct handle_info *hi = user->handle_info;
    if (!hi) { reply("HSMSG_NOT_REGISTERED"); return 0; }
    if (!hi->fakehost || !hi->fakehost[0]) { reply("HSMSG_VHOST_NONE"); return 0; }
    /* Apply vhost to all authed sessions */
    struct userNode *tuser;
    for (tuser = hi->users; tuser; tuser = tuser->next_authed)
        apply_vhost(tuser, hi->fakehost);
    reply("HSMSG_VHOST_ON", hi->fakehost);
    return 1;
}

/* ── Configuration ───────────────────────────────────────────── */
static void hostserv_conf_read(void)
{
    dict_t conf_node;
    const char *str;

    str = "modules/hostserv";
    if (!(conf_node = conf_get_data(str, RECDB_OBJECT))) return;

    str = database_get_data(conf_node, "max_vhost_length", RECDB_QSTRING);
    hostserv_conf.max_vhost_length = str ? atoi(str) : 63;

    str = database_get_data(conf_node, "require_approval", RECDB_QSTRING);
    hostserv_conf.require_approval = str ? enabled_string(str) : 1;

    str = database_get_data(conf_node, "vhost_suffix", RECDB_QSTRING);
    hostserv_conf.vhost_suffix = str ? str : "";
}

/* ── SAXDB persistence ───────────────────────────────────────── */
static int hostserv_saxdb_read(struct dict *db)
{
    dict_t requests, offers;
    dict_iterator_t it;

    if ((requests = database_get_data(db, KEY_REQUESTS, RECDB_OBJECT))) {
        for (it = dict_first(requests); it; it = iter_next(it)) {
            const char *vhost = database_get_data(iter_data(it), KEY_VHOST, RECDB_QSTRING);
            if (vhost) dict_insert(hostserv_requests, strdup(iter_key(it)), strdup(vhost));
        }
    }
    if ((offers = database_get_data(db, KEY_OFFERS, RECDB_OBJECT))) {
        for (it = dict_first(offers); it; it = iter_next(it))
            dict_insert(hostserv_offers, strdup(iter_key(it)), strdup(iter_key(it)));
    }
    return 0;
}

static int hostserv_saxdb_write(struct saxdb_context *ctx)
{
    dict_iterator_t it;

    saxdb_start_record(ctx, KEY_REQUESTS, 1);
    for (it = dict_first(hostserv_requests); it; it = iter_next(it)) {
        saxdb_start_record(ctx, iter_key(it), 0);
        saxdb_write_string(ctx, KEY_VHOST, iter_data(it));
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);

    saxdb_start_record(ctx, KEY_OFFERS, 1);
    for (it = dict_first(hostserv_offers); it; it = iter_next(it))
        saxdb_write_string(ctx, iter_key(it), "1");
    saxdb_end_record(ctx);

    return 0;
}

/* ── Apply vhost on auth ─────────────────────────────────────── */
static void hostserv_check_auth(struct userNode *user, UNUSED_ARG(struct handle_info *old_handle), UNUSED_ARG(void *extra))
{
    struct handle_info *hi = user->handle_info;
    if (hi && hi->fakehost && hi->fakehost[0])
        apply_vhost(user, hi->fakehost);
}

/* ── Module lifecycle ────────────────────────────────────────── */

static char *
generate_fakehost(struct handle_info *handle)
{
    extern const char *hidden_host_suffix;
    static char buffer[HOSTLEN+1];
    char *data;
    int style = 1;
    if (!handle->fakehost) {
        data = conf_get_data("server/hidden_host_type", RECDB_QSTRING);
        if (data) style = atoi(data);
        if ((style == 1) || (style == 3))
            snprintf(buffer, sizeof(buffer), "%s.%s", handle->handle, hidden_host_suffix);
        else if (style == 2) {
            struct userNode *target;
            for (target = handle->users; target; target = target->next_authed)
                break;
            if (target)
                snprintf(buffer, sizeof(buffer), "%s", target->crypthost);
            else
                strncpy(buffer, "none", sizeof(buffer));
        }
        return buffer;
    } else if (handle->fakehost[0] == '.') {
        snprintf(buffer, sizeof(buffer), "%s.%s.%s", handle->handle, handle->fakehost+1, nickserv_conf.titlehost_suffix);
        return buffer;
    }
    return handle->fakehost;
}

static void
apply_fakehost(struct handle_info *handle)
{
    struct userNode *target;
    char *fake;

    if (!handle->users)
        return;
    fake = generate_fakehost(handle);
    for (target = handle->users; target; target = target->next_authed)
        assign_fakehost(target, fake, 1);
}


extern struct nickserv_config nickserv_conf;
static char *generate_fakehost(struct handle_info *handle);
static void apply_fakehost(struct handle_info *handle);

const char *hostserv_module_deps[] = { NULL };
int hostserv_init(void)
{
    HS_LOG = log_register_type("HostServ", "file:hostserv.log");
    hostserv_requests = dict_new();
    hostserv_offers = dict_new();
    dict_set_free_keys(hostserv_requests, free);
    dict_set_free_data(hostserv_requests, free);
    dict_set_free_keys(hostserv_offers, free);
    dict_set_free_data(hostserv_offers, free);

    conf_register_reload(hostserv_conf_read);
    reg_auth_func(hostserv_check_auth, NULL);
    saxdb_register("HostServ", hostserv_saxdb_read, hostserv_saxdb_write);

    hostserv_module = module_register("HostServ", HS_LOG, "mod-hostserv.help", NULL);

    modcmd_register(hostserv_module, "on",        cmd_on,        1, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(hostserv_module, "off",       cmd_off,       1, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(hostserv_module, "set",       cmd_set,       3, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "setall",    cmd_setall,    3, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "del",       cmd_del,       2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "list",      cmd_list,      1, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "request",   cmd_request,   2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(hostserv_module, "activate",  cmd_activate,  2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "reject",    cmd_reject,    2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "waiting",   cmd_waiting,   1, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "group",     cmd_group,     1, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(hostserv_module, "offer",     cmd_offer,     2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "offerdel",  cmd_offerdel,  2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(hostserv_module, "offerlist", cmd_offerlist, 1, 0, NULL);

    message_register_table(msgtab);
    return 1;
}

int hostserv_finalize(void)
{
    dict_t conf_node;
    const char *str;
    struct chanNode *chan;
    unsigned int i;

    str = "modules/hostserv";
    if (!(conf_node = conf_get_data(str, RECDB_OBJECT))) return 0;

    str = database_get_data(conf_node, "bot", RECDB_QSTRING);
    if (!str) str = database_get_data(conf_node, "nick", RECDB_QSTRING);
    if (str) {
        const char *modes = conf_get_data("modules/hostserv/modes", RECDB_QSTRING);
        hostserv = AddLocalUser(str, str, NULL, "Virtual Host Services", modes);
        service_register(hostserv);
    }

    if (autojoin_channels && hostserv) {
        for (i = 0; i < autojoin_channels->used; i++) {
            chan = AddChannel(autojoin_channels->list[i], now, "+nt", NULL, NULL);
            AddChannelUser(hostserv, chan)->modes |= MODE_CHANOP;
        }
    }
    return 1;
}
