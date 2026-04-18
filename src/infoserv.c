/*
 * infoserv.c — Persistent Network Announcements Service
 * Copyright (c) Cathexis Development
 *
 * Stores announcements that are displayed to users on connect.
 * Users see only announcements posted since their last login.
 * Based on atheme InfoServ.
 *
 * Commands:
 *   POST <text>        Post a new announcement
 *   DEL <id>           Delete an announcement
 *   LIST               List all announcements
 *   OPER               Show oper-only announcements
 *   HELP               Show help
 *
 * On-connect: users see unread posts as NOTICEs.
 * Persistence: saxdb
 */

#include "conf.h"
#include "modcmd.h"
#include "nickserv.h"
#include "saxdb.h"
#include "helpfile.h"
#include "hash.h"
#include "dict.h"

#define INFOSERV_CONF_NAME "modules/infoserv"
#define MAX_POSTS          200
#define MAX_POST_LENGTH    400

#define KEY_POSTS          "posts"
#define KEY_AUTHOR         "author"
#define KEY_TEXT           "text"
#define KEY_POSTED         "posted"
#define KEY_IS_FLAGS       "isflags"

#define IS_FLAG_OPER       0x0001  /* Only shown to opers */

static const struct message_entry msgtab[] = {
    { "ISMSG_POSTED",    "Announcement #%d posted." },
    { "ISMSG_DELETED",   "Announcement #%d deleted." },
    { "ISMSG_NOT_FOUND", "Announcement #%d not found." },
    { "ISMSG_FULL",      "Maximum announcements reached (%d). Delete some first." },
    { NULL, NULL }
};

/* ── Data ────────────────────────────────────── */

struct info_post {
    unsigned int id;
    char *author;
    char *text;
    time_t posted;
    unsigned long flags;
};

static struct info_post *posts[MAX_POSTS];
static unsigned int post_count;
static unsigned int next_id = 1;
static struct log_type *is_log;
static struct module *infoserv_module;
static struct userNode *infoserv_bot;

/* ── Helpers ─────────────────────────────────── */

static struct info_post *find_post(unsigned int id)
{
    unsigned int i;
    for (i = 0; i < post_count; i++) {
        if (posts[i] && posts[i]->id == id)
            return posts[i];
    }
    return NULL;
}

static void free_post(struct info_post *p)
{
    if (!p) return;
    free(p->author);
    free(p->text);
    free(p);
}

/* ── On-connect handler ──────────────────────── */

static void infoserv_on_join(struct modeNode *mNode)
{
    struct userNode *user = mNode->user;
    unsigned int i;
    time_t last_login = 0;

    /* Only fire on first channel join (proxy for connect) */
    if (user->channels.used > 1) return;

    /* Get last login time from NickServ handle */
    if (user->handle_info)
        last_login = user->handle_info->lastseen;

    /* Show unread announcements */
    for (i = 0; i < post_count; i++) {
        if (!posts[i]) continue;
        if (posts[i]->posted <= last_login) continue;
        if ((posts[i]->flags & IS_FLAG_OPER) && !IsOper(user)) continue;

        if (infoserv_bot) {
            send_message(user, infoserv_bot, "[%s] %s",
                         posts[i]->author, posts[i]->text);
        }
    }
}

/* ── Commands ────────────────────────────────── */

static MODCMD_FUNC(cmd_ipost)
{
    struct info_post *p;
    char *text;

    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }

    if (post_count >= MAX_POSTS) {
        reply("ISMSG_FULL", MAX_POSTS);
        return 0;
    }

    text = unsplit_string(argv + 1, argc - 1, NULL);
    if (strlen(text) > MAX_POST_LENGTH)
        text[MAX_POST_LENGTH] = '\0';

    p = calloc(1, sizeof(*p));
    p->id = next_id++;
    p->author = strdup(user->handle_info->handle);
    p->text = strdup(text);
    p->posted = now;
    p->flags = 0;

    posts[post_count++] = p;
    reply("ISMSG_POSTED", p->id);
    log_module(is_log, LOG_INFO, "Post #%d by %s: %s", p->id, p->author, p->text);
    return 1;
}

static MODCMD_FUNC(cmd_ioperpost)
{
    struct info_post *p;
    char *text;

    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }

    if (post_count >= MAX_POSTS) {
        reply("ISMSG_FULL", MAX_POSTS);
        return 0;
    }

    text = unsplit_string(argv + 1, argc - 1, NULL);
    if (strlen(text) > MAX_POST_LENGTH)
        text[MAX_POST_LENGTH] = '\0';

    p = calloc(1, sizeof(*p));
    p->id = next_id++;
    p->author = strdup(user->handle_info->handle);
    p->text = strdup(text);
    p->posted = now;
    p->flags = IS_FLAG_OPER;

    posts[post_count++] = p;
    reply("ISMSG_POSTED", p->id);
    return 1;
}

static MODCMD_FUNC(cmd_idel)
{
    unsigned int id, i;

    REQUIRE_PARAMS(2);
    id = atoi(argv[1]);

    for (i = 0; i < post_count; i++) {
        if (posts[i] && posts[i]->id == id) {
            free_post(posts[i]);
            /* Compact the array */
            for (; i + 1 < post_count; i++)
                posts[i] = posts[i + 1];
            posts[--post_count] = NULL;
            reply("ISMSG_DELETED", id);
            return 1;
        }
    }
    reply("ISMSG_NOT_FOUND", id);
    return 0;
}

static MODCMD_FUNC(cmd_ilist)
{
    unsigned int i;

    if (post_count == 0) {
        reply("No announcements.");
        return 1;
    }

    reply("Network announcements (%d):", post_count);
    for (i = 0; i < post_count; i++) {
        if (!posts[i]) continue;
        const char *flag = (posts[i]->flags & IS_FLAG_OPER) ? " [OPER]" : "";
        reply("  #%d [%s] %s%s — %s",
              posts[i]->id, posts[i]->author, posts[i]->text, flag,
              ctime(&posts[i]->posted));
    }
    reply("End of announcements.");
    return 1;
}

/* ── Persistence ─────────────────────────────── */

static int infoserv_saxdb_read(struct dict *db)
{
    struct dict *pdb;
    dict_iterator_t it;

    pdb = database_get_data(db, KEY_POSTS, RECDB_OBJECT);
    if (!pdb) return 0;

    for (it = dict_first(pdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct info_post *p;
        const char *str;

        if (post_count >= MAX_POSTS) break;

        p = calloc(1, sizeof(*p));
        p->id = atoi(iter_key(it));
        if (p->id >= next_id) next_id = p->id + 1;

        str = database_get_data(obj, KEY_AUTHOR, RECDB_QSTRING);
        p->author = strdup(str ? str : "unknown");

        str = database_get_data(obj, KEY_TEXT, RECDB_QSTRING);
        p->text = strdup(str ? str : "");

        str = database_get_data(obj, KEY_POSTED, RECDB_QSTRING);
        if (str) p->posted = strtoul(str, NULL, 0);

        str = database_get_data(obj, KEY_IS_FLAGS, RECDB_QSTRING);
        if (str) p->flags = strtoul(str, NULL, 0);

        posts[post_count++] = p;
    }
    return 0;
}

static int infoserv_saxdb_write(struct saxdb_context *ctx)
{
    unsigned int i;
    char id_str[16];

    saxdb_start_record(ctx, KEY_POSTS, 1);
    for (i = 0; i < post_count; i++) {
        if (!posts[i]) continue;
        snprintf(id_str, sizeof(id_str), "%u", posts[i]->id);
        saxdb_start_record(ctx, id_str, 0);
        saxdb_write_string(ctx, KEY_AUTHOR, posts[i]->author);
        saxdb_write_string(ctx, KEY_TEXT, posts[i]->text);
        saxdb_write_int(ctx, KEY_POSTED, posts[i]->posted);
        saxdb_write_int(ctx, KEY_IS_FLAGS, posts[i]->flags);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

/* ── Module init ─────────────────────────────── */

int infoserv_init(void)
{
    IS_LOG = log_register_type("InfoServ", "file:infoserv.log");

    infoserv_module = module_register("InfoServ", IS_LOG, INFOSERV_CONF_NAME, NULL);
    modcmd_register(infoserv_module, "POST",     cmd_ipost,     2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "OPERPOST", cmd_ioperpost, 2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "DEL",      cmd_idel,      2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "LIST",     cmd_ilist,     1, 0, NULL);

    message_register_table(msgtab);
    saxdb_register("InfoServ", infoserv_saxdb_read, infoserv_saxdb_write);

    /* Register join hook for on-connect announcements */
    reg_join_func(infoserv_on_join);

    log_module(IS_LOG, LOG_INFO, "InfoServ module initialized (%d posts loaded).", post_count);
    return 1;
}

int infoserv_finalize(void) { return 1; }
