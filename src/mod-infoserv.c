/* mod-infoserv.c — Persistent Network Announcements Service
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
#include "common.h"

#define REQUIRE_PARAMS(N) if(argc < (N)) { reply("MSG_MISSING_PARAMS", argv[0]); return 0; }
#define INFOSERV_CONF_NAME "modules/infoserv"
#define MAX_POSTS 200

const char *infoserv_module_deps[] = { NULL };

struct info_post { unsigned int id; char *author; char *text; time_t posted; unsigned long flags; };
#define IS_FLAG_OPER 0x0001

static struct info_post *posts[MAX_POSTS];
static unsigned int post_count;
static unsigned int next_id = 1;
static struct log_type *is_log;
static struct module *infoserv_module;

static void free_post(struct info_post *p) { if (p) { free(p->author); free(p->text); free(p); } }

static MODCMD_FUNC(cmd_ipost) {
    struct info_post *p; char *text;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    if (post_count >= MAX_POSTS) { reply("Maximum announcements reached (%d).", MAX_POSTS); return 0; }
    text = unsplit_string(argv + 1, argc - 1, NULL);
    p = calloc(1, sizeof(*p)); p->id = next_id++; p->author = strdup(user->handle_info->handle);
    p->text = strdup(text); p->posted = now; p->flags = 0;
    posts[post_count++] = p;
    reply("Announcement #%d posted.", p->id); return 1;
}

static MODCMD_FUNC(cmd_ioperpost) {
    struct info_post *p; char *text;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    if (post_count >= MAX_POSTS) { reply("Maximum announcements reached (%d).", MAX_POSTS); return 0; }
    text = unsplit_string(argv + 1, argc - 1, NULL);
    p = calloc(1, sizeof(*p)); p->id = next_id++; p->author = strdup(user->handle_info->handle);
    p->text = strdup(text); p->posted = now; p->flags = IS_FLAG_OPER;
    posts[post_count++] = p;
    reply("Oper announcement #%d posted.", p->id); return 1;
}

static MODCMD_FUNC(cmd_idel) {
    unsigned int id, i;
    REQUIRE_PARAMS(2);
    id = atoi(argv[1]);
    for (i = 0; i < post_count; i++) {
        if (posts[i] && posts[i]->id == id) {
            free_post(posts[i]);
            for (; i + 1 < post_count; i++) posts[i] = posts[i + 1];
            posts[--post_count] = NULL;
            reply("Announcement #%d deleted.", id); return 1;
        }
    }
    reply("Announcement #%d not found.", id); return 0;
}

static MODCMD_FUNC(cmd_ilist) {
    unsigned int i;
    if (!post_count) { reply("No announcements."); return 1; }
    reply("Network announcements (%d):", post_count);
    for (i = 0; i < post_count; i++) {
        if (!posts[i]) continue;
        reply("  #%d [%s] %s%s", posts[i]->id, posts[i]->author, posts[i]->text,
              (posts[i]->flags & IS_FLAG_OPER) ? " [OPER]" : "");
    }
    return 1;
}

static int infoserv_saxdb_read(struct dict *db) {
    struct dict *pdb = database_get_data(db, "posts", RECDB_OBJECT);
    dict_iterator_t it;
    if (!pdb) return 0;
    for (it = dict_first(pdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct info_post *p; const char *str;
        if (post_count >= MAX_POSTS) break;
        p = calloc(1, sizeof(*p)); p->id = atoi(iter_key(it));
        if (p->id >= next_id) next_id = p->id + 1;
        str = database_get_data(obj, "author", RECDB_QSTRING); p->author = strdup(str ? str : "?");
        str = database_get_data(obj, "text", RECDB_QSTRING); p->text = strdup(str ? str : "");
        str = database_get_data(obj, "posted", RECDB_QSTRING); if (str) p->posted = (time_t)strtoul(str, NULL, 0);
        str = database_get_data(obj, "isflags", RECDB_QSTRING); if (str) p->flags = strtoul(str, NULL, 0);
        posts[post_count++] = p;
    }
    return 0;
}

static int infoserv_saxdb_write(struct saxdb_context *ctx) {
    unsigned int i; char id_str[16];
    saxdb_start_record(ctx, "posts", 1);
    for (i = 0; i < post_count; i++) {
        if (!posts[i]) continue;
        snprintf(id_str, sizeof(id_str), "%u", posts[i]->id);
        saxdb_start_record(ctx, id_str, 0);
        saxdb_write_string(ctx, "author", posts[i]->author);
        saxdb_write_string(ctx, "text", posts[i]->text);
        saxdb_write_int(ctx, "posted", posts[i]->posted);
        saxdb_write_int(ctx, "isflags", posts[i]->flags);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

int infoserv_init(void) {
    const char *str;
    is_log = log_register_type("InfoServ", "file:infoserv.log");
    infoserv_module = module_register("InfoServ", is_log, "mod-infoserv.help", NULL);
    modcmd_register(infoserv_module, "POST",     cmd_ipost,     2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "OPERPOST", cmd_ioperpost, 2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "DEL",      cmd_idel,      2, MODCMD_REQUIRE_AUTHED, "flags", "+oper", NULL);
    modcmd_register(infoserv_module, "LIST",     cmd_ilist,     1, 0, NULL);
    saxdb_register("InfoServ", infoserv_saxdb_read, infoserv_saxdb_write);

    str = conf_get_data("modules/infoserv/bot", RECDB_QSTRING);
    if (!str) str = conf_get_data("modules/infoserv/nick", RECDB_QSTRING);
    if (str) {
        const char *modes = conf_get_data("modules/infoserv/modes", RECDB_QSTRING);
        struct userNode *bot = AddLocalUser(str, str, NULL, "Network Announcements", modes);
        if (bot) service_register(bot);
    }
    return 1;
}
int infoserv_finalize(void) { return 1; }
