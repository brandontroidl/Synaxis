/* mod-groupserv.c — Account Group Management Service
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
#define GROUPSERV_CONF_NAME "modules/groupserv"

const char *groupserv_module_deps[] = { NULL };

static const struct message_entry msgtab[] = {
    { "GSMSG_CREATED",        "Group $b%s$b created." },
    { "GSMSG_DROPPED",        "Group $b%s$b has been dropped." },
    { "GSMSG_ALREADY_EXISTS", "Group $b%s$b already exists." },
    { "GSMSG_NOT_FOUND",      "Group $b%s$b does not exist." },
    { "GSMSG_NOT_MEMBER",     "You are not a member of $b%s$b." },
    { "GSMSG_ALREADY_MEMBER", "You are already a member of $b%s$b." },
    { "GSMSG_JOINED",         "You have joined group $b%s$b." },
    { "GSMSG_LEFT",           "You have left group $b%s$b." },
    { "GSMSG_INVITED",        "$b%s$b has been invited to $b%s$b." },
    { "GSMSG_KICKED",         "$b%s$b has been removed from $b%s$b." },
    { "GSMSG_NOT_FOUNDER",    "Only the founder of $b%s$b can do that." },
    { "GSMSG_NOT_ADMIN",      "You need admin access in $b%s$b." },
    { "GSMSG_FLAGS_SET",      "Flags for $b%s$b in $b%s$b set to $b%s$b." },
    { "GSMSG_SET_SUCCESS",    "Option $b%s$b for $b%s$b set to $b%s$b." },
    { "GSMSG_NOT_OPEN",       "Group $b%s$b is not open for joining." },
    { NULL, NULL }
};

#define GROUP_FLAG_OPEN    0x0001
#define GMEMBER_FLAG_ADMIN  0x0001
#define GMEMBER_FLAG_FOUNDER 0x0002
#define GROUP_MAX_NAME 32

struct group_member { struct handle_info *handle; unsigned long flags; time_t joined; };
struct group_info {
    char *name; struct handle_info *founder; unsigned long flags;
    time_t created; time_t modified; struct dict *members; char *description; char *url;
};

static dict_t groups;
static struct log_type *gs_log;
static struct module *groupserv_module;

#define KEY_GROUPS "groups"
#define KEY_FLAGS  "flags"
#define KEY_CREATED "created"
#define KEY_MODIFIED "modified"
#define KEY_DESCRIPTION "description"
#define KEY_URL "url"
#define KEY_FOUNDER "founder"
#define KEY_MEMBERS "members"
#define KEY_MFLAGS "mflags"
#define KEY_JOINED "joined"

static void free_group_member(void *data) { free(data); }
static void free_group(void *data) {
    struct group_info *g = data;
    dict_delete(g->members); free(g->name); free(g->description); free(g->url); free(g);
}

static struct group_info *group_find(const char *name) { return dict_find(groups, name, NULL); }
static struct group_member *group_get_member(struct group_info *g, struct handle_info *hi) {
    if (!hi) return NULL;
    return dict_find(g->members, hi->handle, NULL);
}
static int group_is_admin(struct group_info *g, struct handle_info *hi) {
    struct group_member *m = group_get_member(g, hi);
    return m && (m->flags & (GMEMBER_FLAG_ADMIN|GMEMBER_FLAG_FOUNDER));
}
static int group_is_founder(struct group_info *g, struct handle_info *hi) {
    struct group_member *m = group_get_member(g, hi);
    return m && (m->flags & GMEMBER_FLAG_FOUNDER);
}

static MODCMD_FUNC(cmd_gcreate) {
    struct group_info *g; struct group_member *m;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    if (strlen(argv[1]) > GROUP_MAX_NAME) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (group_find(argv[1])) { reply("GSMSG_ALREADY_EXISTS", argv[1]); return 0; }
    g = calloc(1, sizeof(*g)); g->name = strdup(argv[1]); g->founder = user->handle_info;
    g->created = g->modified = now; g->members = dict_new(); dict_set_free_data(g->members, free_group_member);
    m = calloc(1, sizeof(*m)); m->handle = user->handle_info; m->flags = GMEMBER_FLAG_FOUNDER|GMEMBER_FLAG_ADMIN; m->joined = now;
    dict_insert(g->members, m->handle->handle, m); dict_insert(groups, g->name, g);
    reply("GSMSG_CREATED", g->name); return 1;
}

static MODCMD_FUNC(cmd_gdrop) {
    struct group_info *g;
    REQUIRE_PARAMS(2);
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_founder(g, user->handle_info)) { reply("GSMSG_NOT_FOUNDER", g->name); return 0; }
    reply("GSMSG_DROPPED", g->name); dict_remove(groups, g->name); return 1;
}

static MODCMD_FUNC(cmd_gjoin) {
    struct group_info *g; struct group_member *m;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (group_get_member(g, user->handle_info)) { reply("GSMSG_ALREADY_MEMBER", g->name); return 0; }
    if (!(g->flags & GROUP_FLAG_OPEN)) { reply("GSMSG_NOT_OPEN", g->name); return 0; }
    m = calloc(1, sizeof(*m)); m->handle = user->handle_info; m->flags = 0; m->joined = now;
    dict_insert(g->members, user->handle_info->handle, m);
    reply("GSMSG_JOINED", g->name); return 1;
}

static MODCMD_FUNC(cmd_gleave) {
    struct group_info *g;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_get_member(g, user->handle_info)) { reply("GSMSG_NOT_MEMBER", g->name); return 0; }
    if (group_is_founder(g, user->handle_info)) { reply("GSMSG_NOT_FOUNDER", g->name); return 0; }
    dict_remove(g->members, user->handle_info->handle);
    reply("GSMSG_LEFT", g->name); return 1;
}

static MODCMD_FUNC(cmd_ginvite) {
    struct group_info *g; struct group_member *m; struct handle_info *target;
    REQUIRE_PARAMS(3);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_admin(g, user->handle_info)) { reply("GSMSG_NOT_ADMIN", g->name); return 0; }
    target = get_handle_info(argv[2]); if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
    if (group_get_member(g, target)) { reply("GSMSG_ALREADY_MEMBER", g->name); return 0; }
    m = calloc(1, sizeof(*m)); m->handle = target; m->flags = 0; m->joined = now;
    dict_insert(g->members, target->handle, m);
    reply("GSMSG_INVITED", target->handle, g->name); return 1;
}

static MODCMD_FUNC(cmd_gkick) {
    struct group_info *g; struct handle_info *target;
    REQUIRE_PARAMS(3);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_admin(g, user->handle_info)) { reply("GSMSG_NOT_ADMIN", g->name); return 0; }
    target = get_handle_info(argv[2]); if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
    if (!dict_find(g->members, target->handle, NULL)) { reply("GSMSG_NOT_MEMBER", g->name); return 0; }
    if (group_is_founder(g, target)) { reply("GSMSG_NOT_FOUNDER", g->name); return 0; }
    dict_remove(g->members, target->handle);
    reply("GSMSG_KICKED", target->handle, g->name); return 1;
}

static MODCMD_FUNC(cmd_gflags) {
    struct group_info *g; dict_iterator_t it;
    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (argc < 3) {
        reply("Members of $b%s$b:", g->name);
        for (it = dict_first(g->members); it; it = iter_next(it)) {
            struct group_member *m = iter_data(it);
            char fl[16]; fl[0] = 0;
            if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(fl, "F");
            if (m->flags & GMEMBER_FLAG_ADMIN) strcat(fl, "A");
            if (!fl[0]) strcpy(fl, "-");
            reply("  %s [%s]", iter_key(it), fl);
        }
        return 1;
    }
    if (!group_is_admin(g, user->handle_info)) { reply("GSMSG_NOT_ADMIN", g->name); return 0; }
    if (argc >= 4) {
        struct handle_info *target = get_handle_info(argv[2]);
        struct group_member *m;
        const char *fs; char fl[16];
        if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
        m = group_get_member(g, target); if (!m) { reply("GSMSG_NOT_MEMBER", g->name); return 0; }
        for (fs = argv[3]; *fs; fs++) {
            int add = 1;
            switch (*fs) {
            case '+': add = 1; break; case '-': add = 0; break;
            case 'A': case 'a': if (add) m->flags |= GMEMBER_FLAG_ADMIN; else m->flags &= ~GMEMBER_FLAG_ADMIN; break;
            }
        }
        fl[0] = 0;
        if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(fl, "F");
        if (m->flags & GMEMBER_FLAG_ADMIN) strcat(fl, "A");
        if (!fl[0]) strcpy(fl, "-");
        reply("GSMSG_FLAGS_SET", target->handle, g->name, fl);
    }
    return 1;
}

static MODCMD_FUNC(cmd_gset) {
    struct group_info *g;
    REQUIRE_PARAMS(4);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_founder(g, user->handle_info)) { reply("GSMSG_NOT_FOUNDER", g->name); return 0; }
    if (!irccasecmp(argv[2], "OPEN")) {
        if (!irccasecmp(argv[3], "ON") || !irccasecmp(argv[3], "YES") || !strcmp(argv[3], "1"))
            g->flags |= GROUP_FLAG_OPEN;
        else g->flags &= ~GROUP_FLAG_OPEN;
    } else if (!irccasecmp(argv[2], "DESCRIPTION")) {
        free(g->description); g->description = strdup(argv[3]);
    } else if (!irccasecmp(argv[2], "URL")) {
        free(g->url); g->url = strdup(argv[3]);
    } else { reply("Unknown option: %s", argv[2]); return 0; }
    g->modified = now;
    reply("GSMSG_SET_SUCCESS", argv[2], g->name, argv[3]); return 1;
}

static MODCMD_FUNC(cmd_glist) {
    dict_iterator_t it; int count = 0;
    const char *pattern = argc > 1 ? argv[1] : "*";
    reply("Groups matching $b%s$b:", pattern);
    for (it = dict_first(groups); it; it = iter_next(it)) {
        struct group_info *g = iter_data(it);
        if (match_ircglob(g->name, pattern)) {
            reply("  $b%s$b — %d members, founder %s%s", g->name, dict_size(g->members),
                  g->founder ? g->founder->handle : "?", (g->flags & GROUP_FLAG_OPEN) ? " [open]" : "");
            count++;
        }
    }
    reply("%d group(s) found.", count); return 1;
}

static MODCMD_FUNC(cmd_ginfo) {
    struct group_info *g; dict_iterator_t it;
    REQUIRE_PARAMS(2);
    g = group_find(argv[1]); if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    reply("Information on $b%s$b:", g->name);
    reply("  Founder: %s", g->founder ? g->founder->handle : "?");
    reply("  Members: %d", dict_size(g->members));
    if (g->description) reply("  Desc:    %s", g->description);
    if (g->url) reply("  URL:     %s", g->url);
    reply("  Flags:   %s", (g->flags & GROUP_FLAG_OPEN) ? "OPEN" : "CLOSED");
    reply("  Members:");
    for (it = dict_first(g->members); it; it = iter_next(it)) {
        struct group_member *m = iter_data(it);
        char fl[32]; fl[0] = 0;
        if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(fl, "Founder ");
        if (m->flags & GMEMBER_FLAG_ADMIN) strcat(fl, "Admin");
        if (!fl[0]) strcpy(fl, "Member");
        reply("    %s [%s]", iter_key(it), fl);
    }
    return 1;
}

/* saxdb persistence */
static int groupserv_saxdb_read(struct dict *db) {
    struct dict *gdb = database_get_data(db, KEY_GROUPS, RECDB_OBJECT);
    dict_iterator_t it;
    if (!gdb) return 0;
    for (it = dict_first(gdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct group_info *g; const char *str;
        g = calloc(1, sizeof(*g)); g->name = strdup(iter_key(it));
        g->members = dict_new(); dict_set_free_data(g->members, free_group_member);
        str = database_get_data(obj, KEY_FOUNDER, RECDB_QSTRING);
        if (str) g->founder = get_handle_info(str);
        str = database_get_data(obj, KEY_FLAGS, RECDB_QSTRING);
        if (str) g->flags = strtoul(str, NULL, 0);
        str = database_get_data(obj, KEY_CREATED, RECDB_QSTRING);
        if (str) g->created = (time_t)strtoul(str, NULL, 0);
        str = database_get_data(obj, KEY_DESCRIPTION, RECDB_QSTRING);
        if (str) g->description = strdup(str);
        str = database_get_data(obj, KEY_URL, RECDB_QSTRING);
        if (str) g->url = strdup(str);
        {
            struct dict *mdb = database_get_data(obj, KEY_MEMBERS, RECDB_OBJECT);
            if (mdb) {
                dict_iterator_t mit;
                for (mit = dict_first(mdb); mit; mit = iter_next(mit)) {
                    struct record_data *mrd = iter_data(mit);
                    struct dict *mobj = mrd->d.object;
                    struct handle_info *hi = get_handle_info(iter_key(mit));
                    struct group_member *m;
                    if (!hi) continue;
                    m = calloc(1, sizeof(*m)); m->handle = hi;
                    str = database_get_data(mobj, KEY_MFLAGS, RECDB_QSTRING);
                    if (str) m->flags = strtoul(str, NULL, 0);
                    str = database_get_data(mobj, KEY_JOINED, RECDB_QSTRING);
                    if (str) m->joined = (time_t)strtoul(str, NULL, 0);
                    dict_insert(g->members, hi->handle, m);
                }
            }
        }
        dict_insert(groups, g->name, g);
    }
    return 0;
}

static int groupserv_saxdb_write(struct saxdb_context *ctx) {
    dict_iterator_t it, mit;
    saxdb_start_record(ctx, KEY_GROUPS, 1);
    for (it = dict_first(groups); it; it = iter_next(it)) {
        struct group_info *g = iter_data(it);
        saxdb_start_record(ctx, g->name, 0);
        if (g->founder) saxdb_write_string(ctx, KEY_FOUNDER, g->founder->handle);
        saxdb_write_int(ctx, KEY_FLAGS, g->flags);
        saxdb_write_int(ctx, KEY_CREATED, g->created);
        if (g->description) saxdb_write_string(ctx, KEY_DESCRIPTION, g->description);
        if (g->url) saxdb_write_string(ctx, KEY_URL, g->url);
        saxdb_start_record(ctx, KEY_MEMBERS, 1);
        for (mit = dict_first(g->members); mit; mit = iter_next(mit)) {
            struct group_member *m = iter_data(mit);
            saxdb_start_record(ctx, iter_key(mit), 0);
            saxdb_write_int(ctx, KEY_MFLAGS, m->flags);
            saxdb_write_int(ctx, KEY_JOINED, m->joined);
            saxdb_end_record(ctx);
        }
        saxdb_end_record(ctx);
        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

int groupserv_init(void) {
    const char *str;
    gs_log = log_register_type("GroupServ", "file:groupserv.log");
    groups = dict_new(); dict_set_free_data(groups, free_group);
    groupserv_module = module_register("GroupServ", gs_log, "mod-groupserv.help", NULL);
    modcmd_register(groupserv_module, "CREATE",  cmd_gcreate, 2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "DROP",    cmd_gdrop,   2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "JOIN",    cmd_gjoin,   2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "LEAVE",   cmd_gleave,  2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "INVITE",  cmd_ginvite, 3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "KICK",    cmd_gkick,   3, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "FLAGS",   cmd_gflags,  2, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "SET",     cmd_gset,    4, MODCMD_REQUIRE_AUTHED, NULL);
    modcmd_register(groupserv_module, "LIST",    cmd_glist,   1, 0, NULL);
    modcmd_register(groupserv_module, "INFO",    cmd_ginfo,   2, 0, NULL);
    message_register_table(msgtab);
    saxdb_register("GroupServ", groupserv_saxdb_read, groupserv_saxdb_write);

    str = conf_get_data("modules/groupserv/bot", RECDB_QSTRING);
    if (!str) str = conf_get_data("modules/groupserv/nick", RECDB_QSTRING);
    if (str) {
        const char *modes = conf_get_data("modules/groupserv/modes", RECDB_QSTRING);
        struct userNode *bot = AddLocalUser(str, str, NULL, "Account Group Management", modes);
        if (bot) service_register(bot);
    }
    return 1;
}
int groupserv_finalize(void) { return 1; }
