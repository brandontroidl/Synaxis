/*
 * proto-inspircd.c — InspIRCd Spanning Tree Protocol Backend for Synaxis
 * Copyright (C) 2026 Cathexis Development
 *
 * Optional protocol module enabling Synaxis to link with InspIRCd 3.x/4.x
 * via the Spanning Tree protocol (version 1205/1206).
 *
 * Ported from Sigil's InspIRCd backend and adapted to Synaxis's
 * proto-common.c infrastructure (putsock, AddUser, AddChannel, etc.)
 *
 * InspIRCd wire format:
 *   :<SID> <COMMAND> [params...] :<trailing>
 *   :<UID> <COMMAND> [params...] :<trailing>
 *
 * Key differences from P10 and TS6:
 *   - FJOIN instead of BURST/SJOIN (member format: "modes,uid")
 *   - FMODE instead of OPMODE/TMODE
 *   - FTOPIC for topic bursting
 *   - CAPAB START/END for capability negotiation
 *   - METADATA for account names and TLS cert fingerprints
 *   - SINFO for server metadata
 *   - OPERTYPE for oper class info
 *   - ENDBURST instead of EOB/END_OF_BURST
 *   - ADDLINE/DELLINE for G/K/Z-lines
 *   - No SVINFO (capabilities via CAPAB instead)
 *
 * To enable: build with --enable-inspircd and set "protocol = inspircd"
 *
 * Informed by:
 *   InspIRCd 4 src/modules/m_spanningtree/ (46 files)
 *   Anope modules/protocol/inspircd.cpp (2,618 lines)
 *   Atheme modules/protocol/inspircd.c (1,750 lines)
 */

#ifdef HAVE_PROTO_INSPIRCD

#include "proto-common.c"
#include "hash.h"
#include "nickserv.h"
#include "chanserv.h"
#include "opserv.h"
#include "conf.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static char my_sid[4];
static int uid_counter = 0;

/* ── UID Generation (InspIRCd format: SID + 6 alphanumeric) ── */

static void inspircd_generate_uid(char *buf, size_t buflen) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int i, c = uid_counter++;
    if (buflen < 10) return;
    memcpy(buf, my_sid, 3);
    for (i = 5; i >= 0; i--) {
        buf[3 + i] = chars[c % 36];
        c /= 36;
    }
    buf[9] = '\0';
}

/* ── Inbound S2S Handlers ───────────────────────────────── */

/* SERVER — initial handshake (no source prefix) */
static CMD_FUNC(cmd_inspircd_server) {
    struct server *srv;
    if (argc < 4) return 0;
    /* SERVER <n> <pass> <hops> <SID> :<desc> */
    srv = AddServer(NULL, argv[0], atoi(argv[2]), 0, now, 0, argv[argc-1]);
    if (srv) srv->burst = 1;
    return 1;
}

/*
 * UID — InspIRCd user introduction
 * :SID UID <uid> <ts> <nick> <host> <dhost> <ident> <ip> <signon> +<modes> :<gecos>
 */
static CMD_FUNC(cmd_inspircd_uid) {
    struct userNode *un;
    char *uid, *nick, *ident, *hostname, *dhost, *ip, *modes, *gecos;
    time_t timestamp, signon;

    if (argc < 10) return 0;

    uid = argv[0];
    timestamp = atoi(argv[1]);
    nick = argv[2];
    hostname = argv[3];
    dhost = argv[4];
    ident = argv[5];
    ip = argv[6];
    signon = atoi(argv[7]);
    modes = (argc > 8 && argv[8][0] == '+') ? argv[8] : "+";
    gecos = argv[argc-1];

    un = AddUser(GetServerH(origin), nick, ident, dhost,
                 modes, gecos, now, ip, 0, timestamp, uid);
    if (!un) return 0;

    /* Set real hostname separately from displayed host */
    if (strcmp(hostname, dhost))
        strncpy(un->crypthost, hostname, sizeof(un->crypthost)-1);

    return 1;
}

/*
 * FJOIN — InspIRCd channel burst
 * :SID FJOIN <channel> <ts> +<modes> [params] :<memberlist>
 *
 * memberlist format: "modes,uid modes,uid"
 * Example: "o,912AAAAAA v,912AAAAAB ,912AAAAAC"
 */
static CMD_FUNC(cmd_inspircd_fjoin) {
    struct chanNode *cn;
    char *channame;
    time_t ts;
    const char *members;

    if (argc < 4) return 0;

    channame = argv[0];
    ts = atoi(argv[1]);
    members = argv[argc-1];

    cn = GetChannel(channame);
    if (!cn)
        cn = AddChannel(channame, now, NULL, NULL, NULL);
    if (ts < cn->timestamp)
        cn->timestamp = ts;

    /* Parse modes from argv[2] if present */
    if (argc > 3 && argv[2][0] == '+') {
        struct mod_chanmode *change = mod_chanmode_parse(cn, argv+2, argc-3,
                                        MCP_FROM_SERVER, 0);
        if (change) {
            mod_chanmode_announce(GetServerH(origin), cn, change);
            mod_chanmode_free(change);
        }
    }

    /* Parse member list: "modes,uid modes,uid" */
    {
        char buf[MAXLEN];
        char *tok, *save;
        strncpy(buf, members, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            char *comma = strchr(tok, ',');
            int flags = 0;
            const char *uid_str;
            if (comma) {
                *comma = '\0';
                uid_str = comma + 1;
                for (char *m = tok; *m; m++) {
                    if (*m == 'o') flags |= MODE_CHANOP;
                    else if (*m == 'h') flags |= MODE_HALFOP;
                    else if (*m == 'v') flags |= MODE_VOICE;
                    else if (*m == 'a') flags |= MODE_PROTECT;
                    else if (*m == 'q') flags |= MODE_OWNER;
                }
            } else {
                uid_str = tok;
            }
            struct userNode *un = GetUserN(uid_str);
            if (un) {
                struct modeNode *mn = AddChannelUser(un, cn);
                if (mn && flags) mn->modes = flags;
            }
        }
    }
    return 1;
}

/* FMODE — forced mode change */
static CMD_FUNC(cmd_inspircd_fmode) {
    struct chanNode *cn;
    struct mod_chanmode *change;

    if (argc < 3) return 0;
    /* argv[0] = channel/user, argv[1] = ts, argv[2..] = modes */
    cn = GetChannel(argv[0]);
    if (!cn) return 0;

    change = mod_chanmode_parse(cn, argv+2, argc-2,
                                MCP_FROM_SERVER | MCP_ALLOW_OVB, 0);
    if (change) {
        mod_chanmode_announce(GetServerH(origin), cn, change);
        mod_chanmode_free(change);
    }
    return 1;
}

/* FTOPIC — topic burst */
static CMD_FUNC(cmd_inspircd_ftopic) {
    struct chanNode *cn;
    if (argc < 4) return 0;
    /* :SID FTOPIC <channel> <ts> <setter> :<topic> */
    cn = GetChannel(argv[0]);
    if (!cn) return 0;
    SetChannelTopic(cn, NULL, argv[argc-1], atoi(argv[1]));
    return 1;
}

/* NICK — nick change */
static CMD_FUNC(cmd_inspircd_nick) {
    struct userNode *un;
    if (argc < 2) return 0;
    un = GetUserH(origin);
    if (!un) un = GetUserN(origin);
    if (un) NickChange(un, argv[0], 1);
    return 1;
}

/* QUIT */
static CMD_FUNC(cmd_inspircd_quit) {
    struct userNode *un = GetUserH(origin);
    if (!un) un = GetUserN(origin);
    if (un) DelUser(un, NULL, 0, argc > 0 ? argv[0] : "Quit");
    return 1;
}

/* PING/PONG */
static CMD_FUNC(cmd_inspircd_ping) {
    if (argc >= 1)
        putsock(":%s PONG %s %s", my_sid, self->name, argv[0]);
    return 1;
}

/* ENDBURST — InspIRCd's EOB */
static CMD_FUNC(cmd_inspircd_endburst) {
    struct server *srv = GetServerH(origin);
    if (srv) {
        srv->burst = 0;
        log_module(MAIN_LOG, LOG_INFO, "InspIRCd: %s end of burst", srv->name);
    }
    return 1;
}

/* METADATA — key-value pairs */
static CMD_FUNC(cmd_inspircd_metadata) {
    struct userNode *un;
    if (argc < 3) return 0;
    /* :SID METADATA <target> <key> :<value> */
    if (!strcmp(argv[1], "accountname")) {
        un = GetUserN(argv[0]);
        if (!un) un = GetUserH(argv[0]);
        if (un) {
            strncpy(un->handle_info_name, argv[2], sizeof(un->handle_info_name)-1);
            call_account_func(un, argv[2], 0, NULL, NULL);
        }
    } else if (!strcmp(argv[1], "ssl_cert")) {
        un = GetUserN(argv[0]);
        if (!un) un = GetUserH(argv[0]);
        if (un && un->crypthost) {
            strncpy(un->crypthost, argv[2], sizeof(un->crypthost)-1);
        }
    }
    return 1;
}

/* SQUIT */
static CMD_FUNC(cmd_inspircd_squit) {
    struct server *srv;
    if (argc < 1) return 0;
    srv = GetServerH(argv[0]);
    if (srv) DelServer(srv, 0, argc > 1 ? argv[argc-1] : "SQUIT");
    return 1;
}

/* KILL */
static CMD_FUNC(cmd_inspircd_kill) {
    struct userNode *un;
    if (argc < 1) return 0;
    un = GetUserN(argv[0]);
    if (!un) un = GetUserH(argv[0]);
    if (un) DelUser(un, NULL, 0, argc > 1 ? argv[argc-1] : "Killed");
    return 1;
}

/* KICK */
static CMD_FUNC(cmd_inspircd_kick) {
    struct chanNode *cn;
    struct userNode *un;
    if (argc < 2) return 0;
    cn = GetChannel(argv[0]);
    un = GetUserN(argv[1]);
    if (!un) un = GetUserH(argv[1]);
    if (cn && un) {
        struct modeNode *mn = GetUserMode(cn, un);
        if (mn) DelChannelUser(un, cn, argc > 2 ? argv[argc-1] : "Kicked", 0);
    }
    return 1;
}

/* PART */
static CMD_FUNC(cmd_inspircd_part) {
    struct chanNode *cn;
    struct userNode *un;
    if (argc < 1) return 0;
    un = GetUserH(origin);
    if (!un) un = GetUserN(origin);
    cn = GetChannel(argv[0]);
    if (cn && un) DelChannelUser(un, cn, argc > 1 ? argv[argc-1] : NULL, 0);
    return 1;
}

/* CAPAB — capability negotiation (informational) */
static CMD_FUNC(cmd_inspircd_capab) {
    log_module(MAIN_LOG, LOG_DEBUG, "InspIRCd CAPAB: %s",
               argc > 0 ? argv[0] : "");
    return 1;
}

/* SID — server introduction during burst */
static CMD_FUNC(cmd_inspircd_sid) {
    struct server *srv;
    if (argc < 4) return 0;
    /* :SID SID <n> <hops> <SID> :<desc> */
    srv = AddServer(GetServerH(origin), argv[0], atoi(argv[1]),
                    0, now, 0, argv[argc-1]);
    if (srv) srv->burst = 1;
    return 1;
}

/* BURST — start of burst marker */
static CMD_FUNC(cmd_inspircd_burst) {
    struct server *srv = GetServerH(origin);
    if (srv) srv->burst = 1;
    return 1;
}

/* OPERTYPE, SINFO, ERROR — informational */
static CMD_FUNC(cmd_inspircd_noop) { return 1; }

/* SASL — InspIRCd format */
static CMD_FUNC(cmd_inspircd_sasl) {
    if (argc < 4) return 0;
    struct server *serv = GetServerH(origin);
    if (serv)
        call_sasl_input_func(serv, argv[1], argv[2], argv[3],
                             argc > 4 ? argv[argc-1] : NULL);
    return 1;
}

/* ── Outbound S2S Operations ────────────────────────────── */

static void inspircd_irc_introduce(const char *password) {
    const char *sid = conf_get_string("server/sid", "00B");
    strncpy(my_sid, sid, 3);
    my_sid[3] = '\0';

    putsock("CAPAB START 1206");
    putsock("CAPAB END");
    putsock("SERVER %s %s 0 %s :%s",
            self->name, password, my_sid, self->description);
}

static void inspircd_irc_user(struct userNode *user) {
    char uid[10];
    inspircd_generate_uid(uid, sizeof(uid));
    /* :SID UID <uid> <ts> <nick> <host> <dhost> <ident> <ip> <signon> +<modes> :<gecos> */
    putsock(":%s UID %s %lu %s %s %s %s 127.0.0.1 %lu +oS :%s",
            my_sid, uid, (unsigned long)now, user->nick,
            user->hostname, user->hostname, user->ident,
            (unsigned long)now, user->info);
    putsock(":%s OPERTYPE Services", uid);
    /* Store UID for later reference */
    strncpy(user->numeric, uid, sizeof(user->numeric)-1);
}

static void inspircd_irc_privmsg(struct userNode *from, const char *target, const char *msg) {
    putsock(":%s PRIVMSG %s :%s", from->numeric, target, msg);
}

static void inspircd_irc_notice(struct userNode *from, const char *target, const char *msg) {
    putsock(":%s NOTICE %s :%s", from->numeric, target, msg);
}

static void inspircd_irc_wallops(const char *format, ...) {
    va_list args;
    char buf[MAXLEN];
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    putsock(":%s SNONOTICE g :%s", my_sid, buf);
}

static void inspircd_irc_join(struct userNode *user, struct chanNode *chan) {
    putsock(":%s FJOIN %s %lu + :o,%s", my_sid, chan->name,
            (unsigned long)chan->timestamp, user->numeric);
}

static void inspircd_irc_part(struct userNode *user, struct chanNode *chan,
                               const char *reason) {
    if (reason)
        putsock(":%s PART %s :%s", user->numeric, chan->name, reason);
    else
        putsock(":%s PART %s", user->numeric, chan->name);
}

static void inspircd_irc_kick(struct userNode *from, struct userNode *target,
                               struct chanNode *chan, const char *reason) {
    putsock(":%s KICK %s %s :%s", from->numeric, chan->name,
            target->numeric, reason ? reason : "");
}

static void inspircd_irc_mode(struct userNode *from, struct chanNode *chan,
                               const char *modes) {
    putsock(":%s FMODE %s %lu %s", from->numeric, chan->name,
            (unsigned long)chan->timestamp, modes);
}

static void inspircd_irc_topic(struct userNode *from, struct chanNode *chan,
                                const char *topic) {
    putsock(":%s FTOPIC %s %lu %s :%s", my_sid, chan->name,
            (unsigned long)now, from->nick, topic);
}

static void inspircd_irc_kill(struct userNode *from, struct userNode *target,
                               const char *reason) {
    putsock(":%s KILL %s :Killed (%s)", from->numeric, target->numeric, reason);
}

static void inspircd_irc_gline(const char *mask, unsigned long duration,
                                const char *reason) {
    putsock(":%s ADDLINE G %s %s %lu %lu :%s",
            my_sid, mask, self->name,
            (unsigned long)now, duration, reason);
}

static void inspircd_irc_ungline(const char *mask) {
    putsock(":%s DELLINE G %s", my_sid, mask);
}

static void inspircd_irc_opmode(struct userNode *from, struct chanNode *chan,
                                 const char *modes) {
    putsock(":%s FMODE %s %lu %s", my_sid, chan->name,
            (unsigned long)chan->timestamp, modes);
}

static void inspircd_irc_chghost(struct userNode *user, const char *newhost) {
    putsock(":%s CHGHOST %s %s", my_sid, user->numeric, newhost);
}

static void inspircd_irc_account(struct userNode *user, const char *account,
                                  unsigned long ts) {
    if (account && *account)
        putsock(":%s METADATA %s accountname :%s", my_sid, user->numeric, account);
    else
        putsock(":%s METADATA %s accountname :", my_sid, user->numeric);
}

static void inspircd_irc_svsnick(struct userNode *user, const char *newnick) {
    putsock(":%s SVSNICK %s %s %lu",
            my_sid, user->numeric, newnick, (unsigned long)now);
}

static void inspircd_irc_svsmode(struct userNode *user, const char *modes) {
    putsock(":%s SVSMODE %s %s", my_sid, user->numeric, modes);
}

static void inspircd_irc_jupe(const char *server, const char *reason) {
    char sid[4];
    inspircd_generate_uid(sid, sizeof(sid));
    putsock(":%s SERVER %s * 0 %s :%s", my_sid, server, sid, reason);
}

static void inspircd_irc_eob(void) {
    putsock(":%s ENDBURST", my_sid);
    log_module(MAIN_LOG, LOG_INFO, "InspIRCd: Sent end of burst");
}

static void inspircd_irc_sno(unsigned int mask, const char *format, ...) {
    va_list args;
    char buf[MAXLEN];
    const char *letter = "s";

    if (mask & SNO_GLINE)         letter = "k";
    else if (mask & SNO_OPERKILL) letter = "k";
    else if (mask & SNO_SHUN)     letter = "k";
    else if (mask & SNO_SACMD)    letter = "s";
    else if (mask & SNO_SASL)     letter = "s";
    else if (mask & SNO_ACCOUNT)  letter = "s";

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    putsock(":%s SNONOTICE %s :%s", my_sid, letter, buf);
}

/* SASL reply — InspIRCd uses plain SASL */
static void inspircd_sasl_reply(const char *target_server, const char *identifier,
                                const char *type, const char *data) {
    if (data)
        putsock(":%s SASL %s %s %s :%s",
                my_sid, target_server, identifier, type, data);
    else
        putsock(":%s SASL %s %s %s",
                my_sid, target_server, identifier, type);
}

/* ── Protocol Registration ──────────────────────────────── */

void proto_inspircd_init(void) {
    dict_insert(irc_func_dict, "SERVER",   cmd_inspircd_server);
    dict_insert(irc_func_dict, "UID",      cmd_inspircd_uid);
    dict_insert(irc_func_dict, "FJOIN",    cmd_inspircd_fjoin);
    dict_insert(irc_func_dict, "FMODE",    cmd_inspircd_fmode);
    dict_insert(irc_func_dict, "FTOPIC",   cmd_inspircd_ftopic);
    dict_insert(irc_func_dict, "NICK",     cmd_inspircd_nick);
    dict_insert(irc_func_dict, "QUIT",     cmd_inspircd_quit);
    dict_insert(irc_func_dict, "PING",     cmd_inspircd_ping);
    dict_insert(irc_func_dict, "PONG",     cmd_inspircd_ping);
    dict_insert(irc_func_dict, "ENDBURST", cmd_inspircd_endburst);
    dict_insert(irc_func_dict, "METADATA", cmd_inspircd_metadata);
    dict_insert(irc_func_dict, "SQUIT",    cmd_inspircd_squit);
    dict_insert(irc_func_dict, "KILL",     cmd_inspircd_kill);
    dict_insert(irc_func_dict, "KICK",     cmd_inspircd_kick);
    dict_insert(irc_func_dict, "PART",     cmd_inspircd_part);
    dict_insert(irc_func_dict, "CAPAB",    cmd_inspircd_capab);
    dict_insert(irc_func_dict, "SID",      cmd_inspircd_sid);
    dict_insert(irc_func_dict, "BURST",    cmd_inspircd_burst);
    dict_insert(irc_func_dict, "OPERTYPE", cmd_inspircd_noop);
    dict_insert(irc_func_dict, "SINFO",    cmd_inspircd_noop);
    dict_insert(irc_func_dict, "ERROR",    cmd_inspircd_noop);
    dict_insert(irc_func_dict, "SASL",     cmd_inspircd_sasl);

    log_module(MAIN_LOG, LOG_INFO,
               "InspIRCd Spanning Tree protocol module loaded (v1206)");
}

#endif /* HAVE_PROTO_INSPIRCD */
