/* mod-botserv.c - BotServ module for Synaxis/X3
 * Copyright (C) 2026 Synaxis Development
 *
 * Channel bot management service. Users can assign service bots to
 * channels for automated moderation (kick on badwords, floods, etc.).
 *
 * Commands:
 *   BOTLIST    — List available bots
 *   ASSIGN     — Assign a bot to a channel
 *   UNASSIGN   — Remove a bot from a channel
 *   SAY        — Make the bot say something in a channel
 *   ACT        — Make the bot do an action (/me) in a channel
 *   INFO       — Show bot assignment info for a channel
 *   SET        — Configure bot behavior (kick settings, etc.)
 *   KICK       — Configure auto-kick triggers
 *   BADWORDS   — Manage per-channel badword list
 *   BOT ADD    — Create a new bot (oper)
 *   BOT DEL    — Delete a bot (oper)
 *   BOT CHANGE — Change a bot's nick/ident/host (oper)
 *   BOT LIST   — List all bots with details (oper)
 *
 * Informed by: Anope modules/botserv/, Atheme modules/botserv/
 */

#include "chanserv.h"
#include "common.h"
#include "conf.h"
#include "global.h"
#include "hash.h"
#include "modcmd.h"
#include "nickserv.h"
#include "opserv.h"
#include "proto.h"
#include "saxdb.h"
#include "services_levels.h"
#include "sno_masks.h"

#define REQUIRE_PARAMS(N) if(argc < (N)) { reply("MSG_MISSING_PARAMS", argv[0]); return 0; }
extern struct string_list *autojoin_channels;

#define MAX_BOTS 50
#define MAX_BADWORDS 100

static const struct message_entry msgtab[] = {
    { "BSMSG_BOT_ADDED", "Bot $b%s$b has been created." },
    { "BSMSG_BOT_DELETED", "Bot $b%s$b has been deleted." },
    { "BSMSG_BOT_ASSIGNED", "Bot $b%s$b has been assigned to $b%s$b." },
    { "BSMSG_BOT_UNASSIGNED", "Bot has been unassigned from $b%s$b." },
    { "BSMSG_BOT_NOT_ASSIGNED", "$b%s$b does not have a bot assigned." },
    { "BSMSG_BOT_ALREADY_ASSIGNED", "$b%s$b already has a bot assigned." },
    { "BSMSG_BOT_NOT_FOUND", "Bot $b%s$b does not exist." },
    { "BSMSG_BOT_IN_USE", "Bot $b%s$b is in use and cannot be deleted." },
    { "BSMSG_BOT_LIST_HEADER", "--- Available Bots ---" },
    { "BSMSG_BOT_LIST_ENTRY", "  $b%s$b!%s@%s — %s" },
    { "BSMSG_BOT_LIST_END", "--- End of Bot List (%d bots) ---" },
    { "BSMSG_BOT_INFO", "Bot $b%s$b is assigned to $b%s$b." },
    { "BSMSG_BADWORD_ADDED", "Badword $b%s$b added for $b%s$b." },
    { "BSMSG_BADWORD_REMOVED", "Badword $b%s$b removed from $b%s$b." },
    { "BSMSG_BADWORD_LIST_HEADER", "--- Badwords for %s ---" },
    { "BSMSG_BADWORD_LIST_ENTRY", "  %d: %s" },
    { "BSMSG_BADWORD_LIST_END", "--- End of Badwords (%d entries) ---" },
    { "BSMSG_SET_DONTKICKOPS", "$bDONTKICKOPS$b: %s" },
    { "BSMSG_SET_DONTKICKVOICES", "$bDONTKICKVOICES$b: %s" },
    { "BSMSG_SET_GREET", "$bGREET$b: %s" },
    { "BSMSG_SET_FANTASY", "$bFANTASY$b: %s" },
    { "BSMSG_KICK_BADWORDS", "$bBADWORDS$b: kick on badword match: %s" },
    { "BSMSG_KICK_BOLDS", "$bBOLDS$b: kick on bold text: %s" },
    { "BSMSG_KICK_CAPS", "$bCAPS$b: kick on excessive caps: %s" },
    { "BSMSG_KICK_COLORS", "$bCOLORS$b: kick on color codes: %s" },
    { "BSMSG_KICK_FLOOD", "$bFLOOD$b: kick on flood: %s" },
    { "BSMSG_KICK_REPEAT", "$bREPEAT$b: kick on repeated lines: %s" },
    { "BSMSG_KICK_REVERSES", "$bREVERSES$b: kick on reverse text: %s" },
    { "BSMSG_KICK_UNDERLINES", "$bUNDERLINES$b: kick on underline text: %s" },
    { "BSMSG_NOT_REGISTERED", "You must be authenticated to use this command." },
    { "BSMSG_CHANNEL_NOT_REGISTERED", "$b%s$b is not a registered channel." },
    { "BSMSG_ACCESS_DENIED", "Access denied." },
    { "BSMSG_MAX_BOTS", "Maximum number of bots (%d) reached." },
    { NULL, NULL }
};

/* ── Bot definition ──────────────────────────────────────────── */
struct bot_info {
    char nick[64];
    char ident[32];
    char host[128];
    char gecos[256];
    struct userNode *client;
    int assigned_count;
};

/* ── Channel bot settings ────────────────────────────────────── */
struct bot_channel {
    char channame[256];
    char bot_nick[64];
    int dontkickops;
    int dontkickvoices;
    int greet;
    int fantasy;
    int kick_badwords;
    int kick_bolds;
    int kick_caps;
    int kick_colors;
    int kick_flood;
    int kick_repeat;
    int kick_reverses;
    int kick_underlines;
    struct string_list *badwords;
};

static struct log_type *BS_LOG;
static struct module *botserv_module;
static struct userNode *botserv_bot;
static dict_t bot_dict;        /* nick -> struct bot_info* */
static dict_t channel_dict;    /* channame -> struct bot_channel* */

#define BOTSERV_MIN_PARAMS(N) if (argc < (N)) { reply("MSG_MISSING_PARAMS", argv[0]); return 0; }

/* ── BOTLIST ─────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_botlist)
{
    dict_iterator_t it;
    int count = 0;
    reply("BSMSG_BOT_LIST_HEADER");
    for (it = dict_first(bot_dict); it; it = iter_next(it)) {
        struct bot_info *bi = iter_data(it);
        reply("BSMSG_BOT_LIST_ENTRY", bi->nick, bi->ident, bi->host, bi->gecos);
        count++;
    }
    reply("BSMSG_BOT_LIST_END", count);
    return 1;
}

/* ── ASSIGN ──────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_assign)
{
    struct bot_info *bi;
    struct bot_channel *bc;

    BOTSERV_MIN_PARAMS(3);
    bi = dict_find(bot_dict, argv[2], NULL);
    if (!bi) { reply("BSMSG_BOT_NOT_FOUND", argv[2]); return 0; }

    if (dict_find(channel_dict, argv[1], NULL)) {
        reply("BSMSG_BOT_ALREADY_ASSIGNED", argv[1]);
        return 0;
    }

    bc = calloc(1, sizeof(*bc));
    strncpy(bc->channame, argv[1], sizeof(bc->channame) - 1);
    strncpy(bc->bot_nick, bi->nick, sizeof(bc->bot_nick) - 1);
    bc->dontkickops = 1;
    bc->dontkickvoices = 0;
    bc->greet = 0;
    bc->fantasy = 1;
    bc->badwords = alloc_string_list(4);
    dict_insert(channel_dict, strdup(argv[1]), bc);

    bi->assigned_count++;

    /* Join the bot to the channel with +ao */
    if (bi->client) {
        struct chanNode *chan = GetChannel(argv[1]);
        if (chan) {
            struct mod_chanmode change;
            mod_chanmode_init(&change);
            change.argc = 1;
            change.args[0].mode = MODE_CHANOP | MODE_PROTECT;
            change.args[0].u.member = AddChannelUser(bi->client, chan);
            mod_chanmode_announce(bi->client, chan, &change);

            /* Set this bot as the channel's ChanServ replacement */
            if (chan->channel_info)
                chan->channel_info->channel_bot = bi->client;
            /* Remove ChanServ from the channel — the bot takes over */
            if (chanserv && GetUserMode(chan, chanserv))
                DelChannelUser(chanserv, chan, "BotServ bot assigned", 0);
        }
    }

    reply("BSMSG_BOT_ASSIGNED", bi->nick, argv[1]);
    return 1;
}

/* ── UNASSIGN ────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_unassign)
{
    struct bot_channel *bc;
    struct bot_info *bi;

    BOTSERV_MIN_PARAMS(2);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    bi = dict_find(bot_dict, bc->bot_nick, NULL);
    if (bi) {
        bi->assigned_count--;
        if (bi->client) {
            struct chanNode *chan = GetChannel(argv[1]);
            if (chan) {
                /* Clear the channel's bot assignment */
                if (chan->channel_info)
                    chan->channel_info->channel_bot = NULL;
                DelChannelUser(bi->client, chan, "Unassigned", 0);
                /* Bring ChanServ back to the channel */
                if (chanserv && chan->channel_info && !GetUserMode(chan, chanserv))
                    AddChannelUser(chanserv, chan);
            }
        }
    }

    free_string_list(bc->badwords);
    dict_remove(channel_dict, argv[1]);
    reply("BSMSG_BOT_UNASSIGNED", argv[1]);
    return 1;
}

/* ── SAY ─────────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_say)
{
    struct bot_channel *bc;
    struct bot_info *bi;

    BOTSERV_MIN_PARAMS(3);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    bi = dict_find(bot_dict, bc->bot_nick, NULL);
    if (bi && bi->client)
        irc_privmsg(bi->client, argv[1], unsplit_string(argv+2, argc-2, NULL));
    else
        irc_privmsg(botserv_bot, argv[1], unsplit_string(argv+2, argc-2, NULL));

    return 1;
}

/* ── ACT ─────────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_act)
{
    struct bot_channel *bc;
    struct bot_info *bi;
    char buf[512];

    BOTSERV_MIN_PARAMS(3);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    snprintf(buf, sizeof(buf), "\001ACTION %s\001", unsplit_string(argv+2, argc-2, NULL));
    bi = dict_find(bot_dict, bc->bot_nick, NULL);
    if (bi && bi->client)
        irc_privmsg(bi->client, argv[1], buf);
    else
        irc_privmsg(botserv_bot, argv[1], buf);

    return 1;
}

/* ── INFO ────────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_info)
{
    struct bot_channel *bc;

    BOTSERV_MIN_PARAMS(2);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    reply("BSMSG_BOT_INFO", bc->bot_nick, bc->channame);
    reply("BSMSG_SET_DONTKICKOPS", bc->dontkickops ? "ON" : "OFF");
    reply("BSMSG_SET_DONTKICKVOICES", bc->dontkickvoices ? "ON" : "OFF");
    reply("BSMSG_SET_GREET", bc->greet ? "ON" : "OFF");
    reply("BSMSG_SET_FANTASY", bc->fantasy ? "ON" : "OFF");
    reply("BSMSG_KICK_BADWORDS", bc->kick_badwords ? "ON" : "OFF");
    reply("BSMSG_KICK_CAPS", bc->kick_caps ? "ON" : "OFF");
    reply("BSMSG_KICK_COLORS", bc->kick_colors ? "ON" : "OFF");
    reply("BSMSG_KICK_FLOOD", bc->kick_flood ? "ON" : "OFF");
    reply("BSMSG_KICK_REPEAT", bc->kick_repeat ? "ON" : "OFF");
    return 1;
}

/* ── BADWORDS ────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_badwords)
{
    struct bot_channel *bc;
    unsigned int i;

    BOTSERV_MIN_PARAMS(2);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    if (argc < 3 || !irccasecmp(argv[2], "LIST")) {
        reply("BSMSG_BADWORD_LIST_HEADER", argv[1]);
        for (i = 0; i < bc->badwords->used; i++)
            reply("BSMSG_BADWORD_LIST_ENTRY", i + 1, bc->badwords->list[i]);
        reply("BSMSG_BADWORD_LIST_END", bc->badwords->used);
    } else if (!irccasecmp(argv[2], "ADD") && argc > 3) {
        string_list_append(bc->badwords, strdup(argv[3]));
        reply("BSMSG_BADWORD_ADDED", argv[3], argv[1]);
    } else if (!irccasecmp(argv[2], "DEL") && argc > 3) {
        int idx = atoi(argv[3]) - 1;
        if (idx >= 0 && (unsigned)idx < bc->badwords->used) {
            reply("BSMSG_BADWORD_REMOVED", bc->badwords->list[idx], argv[1]);
            string_list_delete(bc->badwords, idx);
        }
    }
    return 1;
}

/* ── SET ─────────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_set)
{
    struct bot_channel *bc;

    BOTSERV_MIN_PARAMS(3);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    if (argc < 4) {
        /* Show all settings */
        reply("BSMSG_SET_DONTKICKOPS", bc->dontkickops ? "ON" : "OFF");
        reply("BSMSG_SET_DONTKICKVOICES", bc->dontkickvoices ? "ON" : "OFF");
        reply("BSMSG_SET_GREET", bc->greet ? "ON" : "OFF");
        reply("BSMSG_SET_FANTASY", bc->fantasy ? "ON" : "OFF");
        return 1;
    }

    int val = enabled_string(argv[3]);
    if (!irccasecmp(argv[2], "DONTKICKOPS")) bc->dontkickops = val;
    else if (!irccasecmp(argv[2], "DONTKICKVOICES")) bc->dontkickvoices = val;
    else if (!irccasecmp(argv[2], "GREET")) bc->greet = val;
    else if (!irccasecmp(argv[2], "FANTASY")) bc->fantasy = val;

    return 1;
}

/* ── KICK ────────────────────────────────────────────────────── */
static MODCMD_FUNC(cmd_kick)
{
    struct bot_channel *bc;

    BOTSERV_MIN_PARAMS(3);
    bc = dict_find(channel_dict, argv[1], NULL);
    if (!bc) { reply("BSMSG_BOT_NOT_ASSIGNED", argv[1]); return 0; }

    if (argc < 4) {
        reply("BSMSG_KICK_BADWORDS", bc->kick_badwords ? "ON" : "OFF");
        reply("BSMSG_KICK_BOLDS", bc->kick_bolds ? "ON" : "OFF");
        reply("BSMSG_KICK_CAPS", bc->kick_caps ? "ON" : "OFF");
        reply("BSMSG_KICK_COLORS", bc->kick_colors ? "ON" : "OFF");
        reply("BSMSG_KICK_FLOOD", bc->kick_flood ? "ON" : "OFF");
        reply("BSMSG_KICK_REPEAT", bc->kick_repeat ? "ON" : "OFF");
        reply("BSMSG_KICK_REVERSES", bc->kick_reverses ? "ON" : "OFF");
        reply("BSMSG_KICK_UNDERLINES", bc->kick_underlines ? "ON" : "OFF");
        return 1;
    }

    int val = enabled_string(argv[3]);
    if (!irccasecmp(argv[2], "BADWORDS")) bc->kick_badwords = val;
    else if (!irccasecmp(argv[2], "BOLDS")) bc->kick_bolds = val;
    else if (!irccasecmp(argv[2], "CAPS")) bc->kick_caps = val;
    else if (!irccasecmp(argv[2], "COLORS")) bc->kick_colors = val;
    else if (!irccasecmp(argv[2], "FLOOD")) bc->kick_flood = val;
    else if (!irccasecmp(argv[2], "REPEAT")) bc->kick_repeat = val;
    else if (!irccasecmp(argv[2], "REVERSES")) bc->kick_reverses = val;
    else if (!irccasecmp(argv[2], "UNDERLINES")) bc->kick_underlines = val;

    return 1;
}

/* ── BOT ADD/DEL/CHANGE (oper) ───────────────────────────────── */
static MODCMD_FUNC(cmd_bot_add)
{
    struct bot_info *bi;

    BOTSERV_MIN_PARAMS(5);
    if (dict_size(bot_dict) >= MAX_BOTS) { reply("BSMSG_MAX_BOTS", MAX_BOTS); return 0; }
    if (dict_find(bot_dict, argv[1], NULL)) { reply("BSMSG_BOT_ALREADY_ASSIGNED", argv[1]); return 0; }

    bi = calloc(1, sizeof(*bi));
    strncpy(bi->nick, argv[1], sizeof(bi->nick) - 1);
    strncpy(bi->ident, argv[2], sizeof(bi->ident) - 1);
    strncpy(bi->host, argv[3], sizeof(bi->host) - 1);
    strncpy(bi->gecos, unsplit_string(argv+4, argc-4, NULL), sizeof(bi->gecos) - 1);

    /* Create the IRC client */
    const char *modes = conf_get_data("modules/botserv/modes", RECDB_QSTRING);
    bi->client = AddLocalUser(bi->nick, bi->ident, bi->host, bi->gecos, modes);
    if (bi->client) service_register(bi->client);

    dict_insert(bot_dict, strdup(bi->nick), bi);
    reply("BSMSG_BOT_ADDED", bi->nick);
    return 1;
}

static MODCMD_FUNC(cmd_bot_del)
{
    struct bot_info *bi;

    BOTSERV_MIN_PARAMS(2);
    bi = dict_find(bot_dict, argv[1], NULL);
    if (!bi) { reply("BSMSG_BOT_NOT_FOUND", argv[1]); return 0; }
    if (bi->assigned_count > 0) { reply("BSMSG_BOT_IN_USE", argv[1]); return 0; }

    if (bi->client) DelUser(bi->client, NULL, 1, "Bot deleted");
    dict_remove(bot_dict, argv[1]);
    reply("BSMSG_BOT_DELETED", argv[1]);
    return 1;
}


/* ═══ TOY/GAME + BOT SPEECH COMMANDS (moved from ChanServ) ═══ */

static MODCMD_FUNC(cmd_emote)
{
    char *msg;
    assert(argc >= 2);
    if(channel)
    {
        /* CTCP is so annoying. */
        msg = unsplit_string(argv + 1, argc - 1, NULL);
        send_channel_message(channel, cmd->parent->bot, "\001ACTION %s\001", msg);
    }
    else if(*argv[1] == '*' && argv[1][1] != '\0')
    {
        struct handle_info *hi;
        struct userNode *authed;

        REQUIRE_PARAMS(3);
        msg = unsplit_string(argv + 2, argc - 2, NULL);

        if (!(hi = get_handle_info(argv[1] + 1)))
        {
            reply("MSG_HANDLE_UNKNOWN", argv[1] + 1);
            return 0;
        }

        for (authed = hi->users; authed; authed = authed->next_authed)
            send_target_message(5, authed->nick, cmd->parent->bot, "\001ACTION %s\001", msg);
    }
    else if(GetUserH(argv[1]))
    {
        msg = unsplit_string(argv + 2, argc - 2, NULL);
        send_target_message(5, argv[1], cmd->parent->bot, "\001ACTION %s\001", msg);
    }
    else
    {
        reply("MSG_NOT_TARGET_NAME");
        return 0;
    }
    return 1;
}



/* BOT CHANGE — Change a bot's nick/ident/host (oper) */
static MODCMD_FUNC(cmd_bot_change)
{
    struct bot_info *bi;
    BOTSERV_MIN_PARAMS(5);
    bi = dict_find(bot_dict, argv[1], NULL);
    if (!bi) { reply("BSMSG_BOT_NOT_FOUND", argv[1]); return 0; }
    strncpy(bi->nick, argv[2], sizeof(bi->nick) - 1);
    strncpy(bi->ident, argv[3], sizeof(bi->ident) - 1);
    strncpy(bi->host, argv[4], sizeof(bi->host) - 1);
    send_message(user, botserv_bot, "Bot %s changed to %s!%s@%s.", argv[1], bi->nick, bi->ident, bi->host);
    return 1;
}

/* BOT LIST — Detailed bot listing with assignment counts (oper) */
static MODCMD_FUNC(cmd_bot_list)
{
    dict_iterator_t it;
    int count = 0;
    send_message(user, botserv_bot, "--- Bot List (detailed) ---");
    for (it = dict_first(bot_dict); it; it = iter_next(it)) {
        struct bot_info *bi = iter_data(it);
        send_message(user, botserv_bot, "  %s!%s@%s [%s] — assigned to %d channels",
                     bi->nick, bi->ident, bi->host, bi->gecos, bi->assigned_count);
        count++;
    }
    send_message(user, botserv_bot, "--- End of Bot List (%d bots) ---", count);
    return 1;
}

/* ── Database persistence ────────────────────────────────────── */

static int
botserv_saxdb_read(struct dict *db)
{
    struct dict *bots_db, *chans_db, *sub;
    dict_iterator_t it;
    const char *str;

    /* Read bots */
    bots_db = database_get_data(db, "bots", RECDB_OBJECT);
    if (bots_db) {
        for (it = dict_first(bots_db); it; it = iter_next(it)) {
            sub = GET_RECORD_OBJECT((struct record_data *)iter_data(it));
            if (!sub) continue;
            struct bot_info *bi = calloc(1, sizeof(*bi));
            str = database_get_data(sub, "nick", RECDB_QSTRING);
            if (str) strncpy(bi->nick, str, sizeof(bi->nick) - 1);
            str = database_get_data(sub, "ident", RECDB_QSTRING);
            if (str) strncpy(bi->ident, str, sizeof(bi->ident) - 1);
            str = database_get_data(sub, "host", RECDB_QSTRING);
            if (str) strncpy(bi->host, str, sizeof(bi->host) - 1);
            str = database_get_data(sub, "gecos", RECDB_QSTRING);
            if (str) strncpy(bi->gecos, str, sizeof(bi->gecos) - 1);
            bi->client = NULL; /* introduced in finalize */
            dict_insert(bot_dict, strdup(bi->nick), bi);
        }
    }

    /* Read channel assignments */
    chans_db = database_get_data(db, "channels", RECDB_OBJECT);
    if (chans_db) {
        for (it = dict_first(chans_db); it; it = iter_next(it)) {
            sub = GET_RECORD_OBJECT((struct record_data *)iter_data(it));
            if (!sub) continue;
            struct bot_channel *bc = calloc(1, sizeof(*bc));
            strncpy(bc->channame, iter_key(it), sizeof(bc->channame) - 1);
            str = database_get_data(sub, "bot", RECDB_QSTRING);
            if (str) strncpy(bc->bot_nick, str, sizeof(bc->bot_nick) - 1);
            str = database_get_data(sub, "dontkickops", RECDB_QSTRING);
            bc->dontkickops = str ? enabled_string(str) : 1;
            str = database_get_data(sub, "dontkickvoices", RECDB_QSTRING);
            bc->dontkickvoices = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "greet", RECDB_QSTRING);
            bc->greet = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "fantasy", RECDB_QSTRING);
            bc->fantasy = str ? enabled_string(str) : 1;
            str = database_get_data(sub, "kick_badwords", RECDB_QSTRING);
            bc->kick_badwords = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "kick_caps", RECDB_QSTRING);
            bc->kick_caps = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "kick_colors", RECDB_QSTRING);
            bc->kick_colors = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "kick_flood", RECDB_QSTRING);
            bc->kick_flood = str ? enabled_string(str) : 0;
            str = database_get_data(sub, "kick_repeat", RECDB_QSTRING);
            bc->kick_repeat = str ? enabled_string(str) : 0;
            bc->badwords = alloc_string_list(4);
            dict_insert(channel_dict, strdup(iter_key(it)), bc);

            /* Update assigned count */
            struct bot_info *bi = dict_find(bot_dict, bc->bot_nick, NULL);
            if (bi) bi->assigned_count++;
        }
    }

    log_module(BS_LOG, LOG_INFO, "Loaded %d bots and %d channel assignments.",
               dict_size(bot_dict), dict_size(channel_dict));
    return 0;
}

static int
botserv_saxdb_write(struct saxdb_context *ctx)
{
    dict_iterator_t it;

    /* Write bots */
    saxdb_start_record(ctx, "bots", 1);
    for (it = dict_first(bot_dict); it; it = iter_next(it)) {
        struct bot_info *bi = iter_data(it);
        saxdb_start_record(ctx, bi->nick, 0);
        saxdb_write_string(ctx, "nick", bi->nick);
        saxdb_write_string(ctx, "ident", bi->ident);
        saxdb_write_string(ctx, "host", bi->host);
        saxdb_write_string(ctx, "gecos", bi->gecos);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);

    /* Write channel assignments */
    saxdb_start_record(ctx, "channels", 1);
    for (it = dict_first(channel_dict); it; it = iter_next(it)) {
        struct bot_channel *bc = iter_data(it);
        saxdb_start_record(ctx, bc->channame, 0);
        saxdb_write_string(ctx, "bot", bc->bot_nick);
        saxdb_write_sint(ctx, "dontkickops", bc->dontkickops);
        saxdb_write_sint(ctx, "dontkickvoices", bc->dontkickvoices);
        saxdb_write_sint(ctx, "greet", bc->greet);
        saxdb_write_sint(ctx, "fantasy", bc->fantasy);
        saxdb_write_sint(ctx, "kick_badwords", bc->kick_badwords);
        saxdb_write_sint(ctx, "kick_caps", bc->kick_caps);
        saxdb_write_sint(ctx, "kick_colors", bc->kick_colors);
        saxdb_write_sint(ctx, "kick_flood", bc->kick_flood);
        saxdb_write_sint(ctx, "kick_repeat", bc->kick_repeat);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);

    return 0;
}

/* ── Module init ─────────────────────────────────────────────── */

/* Ensure assigned bot is in the channel whenever a user joins */
static int
botserv_handle_join(struct modeNode *mNode, UNUSED_ARG(void *extra))
{
    struct chanNode *chan = mNode->channel;
    struct bot_channel *bc;
    struct bot_info *bi;

    if (!chan || IsLocal(mNode->user))
        return 0;

    bc = dict_find(channel_dict, chan->name, NULL);
    if (!bc)
        return 0;

    bi = dict_find(bot_dict, bc->bot_nick, NULL);
    if (!bi || !bi->client)
        return 0;

    /* If the bot isn't in the channel, join it */
    if (!GetUserMode(chan, bi->client)) {
        struct mod_chanmode change;
        mod_chanmode_init(&change);
        change.argc = 1;
        change.args[0].mode = MODE_CHANOP | MODE_PROTECT;
        change.args[0].u.member = AddChannelUser(bi->client, chan);
        mod_chanmode_announce(bi->client, chan, &change);
    }

    /* Ensure channel_bot is set */
    if (chan->channel_info && !chan->channel_info->channel_bot)
        chan->channel_info->channel_bot = bi->client;

    /* Ensure ChanServ is NOT in the channel */
    if (chanserv && GetUserMode(chan, chanserv))
        DelChannelUser(chanserv, chan, "BotServ bot assigned", 0);

    return 0;
}

const char *botserv_module_deps[] = { NULL };
int botserv_init(void)
{
    BS_LOG = log_register_type("BotServ", "file:botserv.log");
    bot_dict = dict_new();
    channel_dict = dict_new();
    dict_set_free_keys(bot_dict, free);
    dict_set_free_keys(channel_dict, free);

    saxdb_register("BotServ", botserv_saxdb_read, botserv_saxdb_write);

    botserv_module = module_register("BotServ", BS_LOG, "mod-botserv.help", NULL);

    modcmd_register(botserv_module, "botlist",   cmd_botlist,   1, 0, NULL);
    modcmd_register(botserv_module, "assign",    cmd_assign,    3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "unassign",  cmd_unassign,  2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "say",       cmd_say,       3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "act",       cmd_act,       3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "info",      cmd_info,      2, 0, NULL);
    modcmd_register(botserv_module, "set",       cmd_set,       3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "kick",      cmd_kick,      3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "badwords",  cmd_badwords,  2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "bot add",   cmd_bot_add,   5, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(botserv_module, "bot del",   cmd_bot_del,   2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);

    /* Toy/game commands (moved from ChanServ) */
    /* Bot speech commands (moved from ChanServ) */
    modcmd_register(botserv_module, "say", cmd_say, 3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "emote",     cmd_emote,     3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(botserv_module, "bot change", cmd_bot_change, 5, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(botserv_module, "bot list",   cmd_bot_list,   1, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);

    message_register_table(msgtab);
    reg_join_func(botserv_handle_join, NULL);
    return 1;
}

int botserv_finalize(void)
{
    dict_t conf_node;
    const char *str;
    struct chanNode *chan;
    unsigned int i;
    dict_iterator_t it;

    str = "modules/botserv";
    if (!(conf_node = conf_get_data(str, RECDB_OBJECT))) return 0;

    str = database_get_data(conf_node, "bot", RECDB_QSTRING);
    if (!str) str = database_get_data(conf_node, "nick", RECDB_QSTRING);
    if (str) {
        const char *modes = conf_get_data("modules/botserv/modes", RECDB_QSTRING);
        botserv_bot = AddLocalUser(str, str, NULL, "Channel Bot Services", modes);
        service_register(botserv_bot);
    }

    if (autojoin_channels && botserv_bot) {
        for (i = 0; i < autojoin_channels->used; i++) {
            chan = AddChannel(autojoin_channels->list[i], now, "+nt", NULL, NULL);
            AddChannelUser(botserv_bot, chan)->modes |= MODE_CHANOP;
        }
    }

    /* Introduce saved bots as IRC clients and join them to assigned channels */
    for (it = dict_first(bot_dict); it; it = iter_next(it)) {
        struct bot_info *bi = iter_data(it);
        if (!bi->client) {
            bi->client = AddLocalUser(bi->nick, bi->ident, bi->host, bi->gecos, NULL);
        }
    }

    for (it = dict_first(channel_dict); it; it = iter_next(it)) {
        struct bot_channel *bc = iter_data(it);
        struct bot_info *bi = dict_find(bot_dict, bc->bot_nick, NULL);
        if (!bi || !bi->client) continue;

        chan = GetChannel(bc->channame);
        if (!chan) chan = AddChannel(bc->channame, now, "+nt", NULL, NULL);
        if (!chan) continue;

        /* Join the bot */
        if (!GetUserMode(chan, bi->client))
            AddChannelUser(bi->client, chan)->modes |= MODE_CHANOP | MODE_PROTECT;

        /* Set as channel's ChanServ replacement */
        if (chan->channel_info)
            chan->channel_info->channel_bot = bi->client;

        /* Remove ChanServ — the bot takes over */
        if (chanserv && GetUserMode(chan, chanserv))
            DelChannelUser(chanserv, chan, "BotServ bot assigned", 0);
    }

    return 1;
}
