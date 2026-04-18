/*
 * proto-ts6.c — TS6 Protocol Backend for Synaxis
 * Copyright (C) 2026 Cathexis Development
 *
 * Optional protocol module enabling Synaxis to link with TS6-based IRCds:
 *   Charybdis, ircd-hybrid, Plexus, ircd-ratbox, Solanum
 *
 * Ported from Sigil's TS6 backend and adapted to Synaxis's
 * proto-common.c infrastructure (putsock, irc_introduce, etc.)
 *
 * TS6 wire format:
 *   :<SID> <COMMAND> [params...] :<trailing>
 *   :<UID> <COMMAND> [params...] :<trailing>
 *
 * Key differences from P10:
 *   - SID (3 chars) instead of numeric
 *   - UID (SID + 6 alphanumeric) instead of base64 numeric
 *   - SJOIN instead of BURST for channel sync
 *   - TMODE instead of OPMODE for forced modes
 *   - ENCAP for extensions (SASL, etc.)
 *   - SVINFO for TS version negotiation
 *   - EOB (End Of Burst) marker per server
 *   - No base64 encoding of numerics
 *
 * To enable: build with --enable-ts6 and set "protocol = ts6" in config
 *
 * Informed by:
 *   Charybdis src/s_serv.c (TS6 handshake)
 *   Atheme modules/protocol/charybdis.c (services-side TS6)
 *   Anope modules/protocol/hybrid.cpp (services-side TS6)
 *   Solanum ircd/s_serv.c (modern TS6 extensions)
 */

#ifdef HAVE_PROTO_TS6

#include "proto-common.c"
#include "hash.h"
#include "nickserv.h"
#include "chanserv.h"
#include "opserv.h"
#include "conf.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static char my_sid[4];  /* 3-char SID + null */
static int uid_counter = 0;

/* ── UID Generation ─────────────────────────────────────── */

static void ts6_generate_uid(char *buf, size_t buflen) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int i, c = uid_counter++;
    if (buflen < 10) return; /* SID(3) + 6 chars + null */
    memcpy(buf, my_sid, 3);
    for (i = 5; i >= 0; i--) {
        buf[3 + i] = chars[c % 36];
        c /= 36;
    }
    buf[9] = '\0';
}

/* ── Inbound S2S Handlers ───────────────────────────────── */

/*
 * TS6 PASS/CAPAB/SERVER/SVINFO handshake
 * After receiving: PASS <password> TS 6 :<SID>
 *                  CAPAB :<capabilities>
 *                  SERVER <name> <hops> :<description>
 *                  SVINFO <TS_CURRENT> <TS_MIN> 0 :<timestamp>
 */
static CMD_FUNC(cmd_ts6_pass) {
    /* PASS <password> TS <version> :<SID> */
    if (argc >= 4) {
        /* Extract remote SID from trailing param */
        log_module(MAIN_LOG, LOG_DEBUG, "TS6 PASS: remote SID = %s", argv[argc-1]);
    }
    return 1;
}

static CMD_FUNC(cmd_ts6_server) {
    /* :SID SERVER <name> <hops> :<description> */
    struct server *srv;
    if (argc < 3) return 0;
    srv = AddServer(GetServerH(origin), argv[0], atoi(argv[1]), 0,
                    now, 0, argv[argc-1]);
    if (!srv) return 0;
    srv->burst = 1;
    return 1;
}

static CMD_FUNC(cmd_ts6_sid) {
    /* :SID SID <name> <hops> <SID> :<description> */
    struct server *srv;
    if (argc < 4) return 0;
    srv = AddServer(GetServerH(origin), argv[0], atoi(argv[1]), 0,
                    now, 0, argv[argc-1]);
    if (!srv) return 0;
    /* Store SID→server mapping */
    srv->burst = 1;
    return 1;
}

/*
 * UID — TS6 user introduction
 * :SID UID <nick> <hops> <ts> +<modes> <user> <host> <ip> <uid> :<gecos>
 */
static CMD_FUNC(cmd_ts6_uid) {
    struct userNode *un;
    char *nick, *user_name, *hostname, *ip, *uid, *modes, *realname;
    time_t timestamp;

    if (argc < 9) return 0;

    nick = argv[0];
    timestamp = atoi(argv[2]);
    modes = argv[3];
    user_name = argv[4];
    hostname = argv[5];
    ip = argv[6];
    uid = argv[7];
    realname = argv[argc-1];

    un = AddUser(GetServerH(origin), nick, user_name, hostname,
                 modes, realname, now, ip, 0, timestamp, uid);
    if (!un) return 0;
    return 1;
}

/*
 * SJOIN — TS6 channel burst
 * :SID SJOIN <ts> <#channel> +<modes> [params] :<statuslist>
 *
 * statuslist format: "@+uid @uid +uid uid"
 * @ = op, + = voice, % = halfop (if supported)
 */
static CMD_FUNC(cmd_ts6_sjoin) {
    struct chanNode *cn;
    char *channame;
    time_t ts;
    const char *members;

    if (argc < 4) return 0;

    ts = atoi(argv[0]);
    channame = argv[1];
    members = argv[argc-1];

    cn = GetChannel(channame);
    if (!cn) {
        cn = AddChannel(channame, now, NULL, NULL, NULL);
    }
    if (ts < cn->timestamp) cn->timestamp = ts;

    /* Parse members: "@+UID @UID +UID UID" */
    {
        char buf[MAXLEN];
        char *tok, *save;
        strncpy(buf, members, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            char *p = tok;
            int flags = 0;
            while (*p == '@' || *p == '+' || *p == '%' || *p == '~' || *p == '&') {
                if (*p == '@') flags |= MODE_CHANOP;
                else if (*p == '+') flags |= MODE_VOICE;
                else if (*p == '%') flags |= MODE_HALFOP;
                else if (*p == '~') flags |= MODE_OWNER;
                else if (*p == '&') flags |= MODE_PROTECT;
                p++;
            }
            struct userNode *un = GetUserN(p);
            if (un) {
                struct modeNode *mn = AddChannelUser(un, cn);
                if (mn && flags) mn->modes = flags;
            }
        }
    }
    return 1;
}

/* TMODE — TS6 forced mode change */
static CMD_FUNC(cmd_ts6_tmode) {
    struct chanNode *cn;
    struct mod_chanmode *change;

    if (argc < 3) return 0;
    /* argv[0] = ts, argv[1] = #channel, argv[2..] = modes */
    cn = GetChannel(argv[1]);
    if (!cn) return 0;

    change = mod_chanmode_parse(cn, argv+2, argc-2,
                                MCP_FROM_SERVER | MCP_ALLOW_OVB, 0);
    if (change) {
        mod_chanmode_announce(GetServerH(origin), cn, change);
        mod_chanmode_free(change);
    }
    return 1;
}

/* NICK — nick change */
static CMD_FUNC(cmd_ts6_nick) {
    struct userNode *un;
    if (argc < 2) return 0;
    un = GetUserH(origin);
    if (!un) un = GetUserN(origin);
    if (!un) return 0;
    NickChange(un, argv[0], 1);
    return 1;
}

/* QUIT */
static CMD_FUNC(cmd_ts6_quit) {
    struct userNode *un = GetUserH(origin);
    if (!un) un = GetUserN(origin);
    if (un) DelUser(un, NULL, 0, argc > 0 ? argv[0] : "Quit");
    return 1;
}

/* PRIVMSG/NOTICE — handled by existing infrastructure */
/* KICK/PART/JOIN — handled by existing infrastructure */

/* ENCAP — TS6 extension wrapper */
static CMD_FUNC(cmd_ts6_encap) {
    /* :SID ENCAP <target> <subcommand> [params...] */
    if (argc < 2) return 0;
    /* Route SASL via existing infrastructure */
    if (!strcmp(argv[1], "SASL") && argc >= 5) {
        struct server *serv = GetServerH(origin);
        if (serv)
            call_sasl_input_func(serv, argv[2], argv[3], argv[4],
                                 argc > 5 ? argv[argc-1] : NULL);
    }
    /* Route LOGIN for account tracking */
    else if (!strcmp(argv[1], "LOGIN") && argc >= 3) {
        struct userNode *un = GetUserH(origin);
        if (!un) un = GetUserN(origin);
        if (un) {
            strncpy(un->handle_info_name, argv[2], sizeof(un->handle_info_name)-1);
            call_account_func(un, argv[2], 0, NULL, NULL);
        }
    }
    return 1;
}

/* EOB — End of Burst */
static CMD_FUNC(cmd_ts6_eob) {
    struct server *srv = GetServerH(origin);
    if (srv) {
        srv->burst = 0;
        log_module(MAIN_LOG, LOG_INFO, "TS6: %s end of burst", srv->name);
    }
    return 1;
}

/* PING/PONG */
static CMD_FUNC(cmd_ts6_ping) {
    if (argc >= 1)
        putsock(":%s PONG %s %s", my_sid, self->name, argv[0]);
    return 1;
}

/* SQUIT */
static CMD_FUNC(cmd_ts6_squit) {
    struct server *srv;
    if (argc < 1) return 0;
    srv = GetServerH(argv[0]);
    if (srv) DelServer(srv, 0, argc > 1 ? argv[argc-1] : "SQUIT");
    return 1;
}

/* TOPIC */
static CMD_FUNC(cmd_ts6_topic) {
    struct chanNode *cn;
    if (argc < 2) return 0;
    cn = GetChannel(argv[0]);
    if (cn) SetChannelTopic(cn, GetUserH(origin), argv[argc-1], now);
    return 1;
}

/* KILL */
static CMD_FUNC(cmd_ts6_kill) {
    struct userNode *un;
    if (argc < 1) return 0;
    un = GetUserN(argv[0]);
    if (!un) un = GetUserH(argv[0]);
    if (un) DelUser(un, NULL, 0, argc > 1 ? argv[argc-1] : "Killed");
    return 1;
}

/* CHGHOST — IRCv3/TS6 host change */
static CMD_FUNC(cmd_ts6_chghost) {
    struct userNode *un;
    if (argc < 2) return 0;
    un = GetUserN(argv[0]);
    if (!un) un = GetUserH(argv[0]);
    if (un) {
        strncpy(un->hostname, argv[1], sizeof(un->hostname)-1);
        un->hostname[sizeof(un->hostname)-1] = '\0';
    }
    return 1;
}

/* ── Outbound S2S Operations ────────────────────────────── */

static void ts6_irc_introduce(const char *password) {
    const char *sid = conf_get_string("server/sid", "00A");
    strncpy(my_sid, sid, 3);
    my_sid[3] = '\0';

    putsock("PASS %s TS 6 :%s", password, my_sid);
    putsock("CAPAB :QS EX CHW IE KLN KNOCK TB UNKLN CLUSTER ENCAP SERVICES EUID SAVE RSFNC SAVETS_100 MLOCK");
    putsock("SERVER %s 1 :%s", self->name, self->description);
    putsock("SVINFO 6 6 0 :%lu", (unsigned long)now);
}

static void ts6_irc_user(struct userNode *user) {
    char uid[10];
    ts6_generate_uid(uid, sizeof(uid));
    /* :SID UID <nick> 1 <ts> +<modes> <user> <host> 0 <uid> :<gecos> */
    putsock(":%s UID %s 1 %lu +oS %s %s 0 %s :%s",
            my_sid, user->nick, (unsigned long)now,
            user->ident, user->hostname, uid, user->info);
}

static void ts6_irc_privmsg(struct userNode *from, const char *target, const char *msg) {
    putsock(":%s PRIVMSG %s :%s", from->numeric, target, msg);
}

static void ts6_irc_notice(struct userNode *from, const char *target, const char *msg) {
    putsock(":%s NOTICE %s :%s", from->numeric, target, msg);
}

static void ts6_irc_wallops(const char *format, ...) {
    va_list args;
    char buf[MAXLEN];
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    putsock(":%s WALLOPS :%s", my_sid, buf);
}

static void ts6_irc_join(struct userNode *user, struct chanNode *chan) {
    putsock(":%s SJOIN %lu %s + :@%s", my_sid,
            (unsigned long)chan->timestamp, chan->name, user->numeric);
}

static void ts6_irc_part(struct userNode *user, struct chanNode *chan, const char *reason) {
    if (reason)
        putsock(":%s PART %s :%s", user->numeric, chan->name, reason);
    else
        putsock(":%s PART %s", user->numeric, chan->name);
}

static void ts6_irc_kick(struct userNode *from, struct userNode *target,
                         struct chanNode *chan, const char *reason) {
    putsock(":%s KICK %s %s :%s", from->numeric, chan->name,
            target->numeric, reason ? reason : "");
}

static void ts6_irc_mode(struct userNode *from, struct chanNode *chan,
                         const char *modes) {
    putsock(":%s TMODE %lu %s %s", from->numeric,
            (unsigned long)chan->timestamp, chan->name, modes);
}

static void ts6_irc_topic(struct userNode *from, struct chanNode *chan,
                          const char *topic) {
    putsock(":%s TOPIC %s :%s", from->numeric, chan->name, topic);
}

static void ts6_irc_kill(struct userNode *from, struct userNode *target,
                         const char *reason) {
    putsock(":%s KILL %s :Killed (%s)", from->numeric, target->numeric, reason);
}

static void ts6_irc_gline(const char *mask, unsigned long duration,
                           const char *reason) {
    putsock(":%s KLINE * %lu %s :%s", my_sid, duration, mask, reason);
}

static void ts6_irc_ungline(const char *mask) {
    putsock(":%s UNKLINE * %s", my_sid, mask);
}

static void ts6_irc_sno(unsigned int mask, const char *format, ...) {
    va_list args;
    char buf[MAXLEN];
    const char *letter = "s";

    /* Map SNO mask to TS6 SNOTE letter */
    if (mask & SNO_GLINE)    letter = "k";
    else if (mask & SNO_OPERKILL) letter = "k";
    else if (mask & SNO_OLDSNO)   letter = "s";
    else if (mask & SNO_NETWORK)  letter = "s";
    else if (mask & SNO_SHUN)     letter = "k";
    else if (mask & SNO_SACMD)    letter = "s";
    else if (mask & SNO_SASL)     letter = "s";
    else if (mask & SNO_ACCOUNT)  letter = "s";

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    putsock(":%s ENCAP * SNOTE %s :%s", my_sid, letter, buf);
}

static void ts6_irc_account(struct userNode *user, const char *account,
                             unsigned long ts) {
    /* TS6 account: ENCAP * LOGIN <account> */
    if (account && *account)
        putsock(":%s ENCAP * SU %s %s", my_sid, user->numeric, account);
    else
        putsock(":%s ENCAP * SU %s", my_sid, user->numeric);
}

static void ts6_irc_chghost(struct userNode *user, const char *newhost) {
    putsock(":%s CHGHOST %s %s", my_sid, user->numeric, newhost);
}

static void ts6_irc_svsnick(struct userNode *user, const char *newnick) {
    putsock(":%s ENCAP * RSFNC %s %s %lu %lu",
            my_sid, user->numeric, newnick,
            (unsigned long)now, (unsigned long)user->timestamp);
}

static void ts6_irc_eob(void) {
    putsock(":%s EOB", my_sid);
    log_module(MAIN_LOG, LOG_INFO, "TS6: Sent end of burst");
}

/* SASL reply — TS6 uses ENCAP */
static void ts6_sasl_reply(const char *target_server, const char *identifier,
                           const char *type, const char *data) {
    if (data)
        putsock(":%s ENCAP %s SASL %s %s %s :%s",
                my_sid, target_server, my_sid, identifier, type, data);
    else
        putsock(":%s ENCAP %s SASL %s %s %s",
                my_sid, target_server, my_sid, identifier, type);
}

/* ── Protocol Registration ──────────────────────────────── */

void proto_ts6_init(void) {
    /* Register inbound message handlers */
    dict_insert(irc_func_dict, "PASS",    cmd_ts6_pass);
    dict_insert(irc_func_dict, "SERVER",  cmd_ts6_server);
    dict_insert(irc_func_dict, "SID",     cmd_ts6_sid);
    dict_insert(irc_func_dict, "UID",     cmd_ts6_uid);
    dict_insert(irc_func_dict, "SJOIN",   cmd_ts6_sjoin);
    dict_insert(irc_func_dict, "TMODE",   cmd_ts6_tmode);
    dict_insert(irc_func_dict, "NICK",    cmd_ts6_nick);
    dict_insert(irc_func_dict, "QUIT",    cmd_ts6_quit);
    dict_insert(irc_func_dict, "ENCAP",   cmd_ts6_encap);
    dict_insert(irc_func_dict, "EOB",     cmd_ts6_eob);
    dict_insert(irc_func_dict, "PING",    cmd_ts6_ping);
    dict_insert(irc_func_dict, "SQUIT",   cmd_ts6_squit);
    dict_insert(irc_func_dict, "TOPIC",   cmd_ts6_topic);
    dict_insert(irc_func_dict, "KILL",    cmd_ts6_kill);
    dict_insert(irc_func_dict, "CHGHOST", cmd_ts6_chghost);

    log_module(MAIN_LOG, LOG_INFO,
               "TS6 protocol module loaded (Charybdis/Hybrid/Plexus/Solanum)");
}

#endif /* HAVE_PROTO_TS6 */
