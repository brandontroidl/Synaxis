/*
 * groupserv.c — Account Group Management Service
 * Copyright (c) Cathexis Development
 *
 * Manages groups of accounts for bulk access management.
 * Groups can be granted ChanServ access: ACCESS ADD !groupname <level>
 *
 * Commands:
 *   CREATE <name>              Create a new group
 *   DROP <name>                Destroy a group (founder only)
 *   JOIN <name>                Join an open group
 *   LEAVE <name>               Leave a group
 *   INVITE <name> <account>    Invite account to group (admin+)
 *   KICK <name> <account>      Remove account from group (admin+)
 *   FLAGS <name> [account] [+/-flags]  View/set member flags
 *   SET <name> <option> <value>  Set group options
 *   LIST [pattern]             List groups
 *   INFO <name>                Show group details
 *   HELP                       Show help
 *
 * Follows Synaxis modcmd + saxdb patterns for persistence.
 */

#include "conf.h"
#include "modcmd.h"
#include "nickserv.h"
#include "saxdb.h"
#include "helpfile.h"
#include "hash.h"
#include "dict.h"
#include "timeq.h"

/* Module dependencies */
static const char groupserv_module_deps[] = "nickserv";

/* ── Strings ─────────────────────────────────── */
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
    { "GSMSG_GROUP_OPEN",     "Group $b%s$b is now open (anyone can join)." },
    { "GSMSG_GROUP_INVITE",   "Group $b%s$b is now invite-only." },
    { "GSMSG_SET_SUCCESS",    "Option $b%s$b for $b%s$b set to $b%s$b." },
    { NULL, NULL }
};

/* ── Data structures ─────────────────────────── */

#define GROUP_FLAG_OPEN     0x0001  /* Anyone can join */
#define GROUP_FLAG_INVITE   0x0002  /* Invite-only */
#define GROUP_MAX_NAME      32

#define GMEMBER_FLAG_ADMIN  0x0001  /* Can invite/kick/flags */
#define GMEMBER_FLAG_FOUNDER 0x0002 /* Can drop, full control */

struct group_member {
    struct handle_info *handle;
    unsigned long flags;
    time_t joined;
};

struct group_info {
    char *name;
    struct handle_info *founder;
    unsigned long flags;
    time_t created;
    time_t modified;
    struct dict *members; /* account name -> group_member */
    char *description;
    char *url;
};

/* ── Globals ─────────────────────────────────── */

static struct dict *groups;    /* group name -> group_info */
static struct log_type *gs_log;
static struct module *groupserv_module;

#define GROUPSERV_CONF_NAME "modules/groupserv"
#define KEY_GROUPS          "groups"
#define KEY_FLAGS           "flags"
#define KEY_CREATED         "created"
#define KEY_MODIFIED        "modified"
#define KEY_DESCRIPTION     "description"
#define KEY_URL             "url"
#define KEY_FOUNDER         "founder"
#define KEY_MEMBERS         "members"
#define KEY_MEMBER_FLAGS    "mflags"
#define KEY_JOINED          "joined"

/* ── Helpers ─────────────────────────────────── */

static struct group_info *group_find(const char *name)
{
    return dict_find(groups, name, NULL);
}

static struct group_member *group_find_member(struct group_info *g, struct handle_info *hi)
{
    return dict_find(g->members, hi->handle, NULL);
}

static int group_is_admin(struct group_info *g, struct handle_info *hi)
{
    struct group_member *m = group_find_member(g, hi);
    if (!m) return 0;
    return (m->flags & (GMEMBER_FLAG_ADMIN | GMEMBER_FLAG_FOUNDER)) != 0;
}

static int group_is_founder(struct group_info *g, struct handle_info *hi)
{
    struct group_member *m = group_find_member(g, hi);
    if (!m) return 0;
    return (m->flags & GMEMBER_FLAG_FOUNDER) != 0;
}

static void free_group_member(void *data)
{
    free(data);
}

static struct group_info *group_create(const char *name, struct handle_info *founder)
{
    struct group_info *g;
    struct group_member *m;

    g = calloc(1, sizeof(*g));
    g->name = strdup(name);
    g->founder = founder;
    g->created = now;
    g->modified = now;
    g->flags = 0;
    g->members = dict_new();
    dict_set_free_data(g->members, free_group_member);

    /* Add founder as first member */
    m = calloc(1, sizeof(*m));
    m->handle = founder;
    m->flags = GMEMBER_FLAG_FOUNDER | GMEMBER_FLAG_ADMIN;
    m->joined = now;
    dict_insert(g->members, founder->handle, m);

    dict_insert(groups, g->name, g);
    return g;
}

static void free_group(void *data)
{
    struct group_info *g = data;
    dict_delete(g->members);
    free(g->name);
    free(g->description);
    free(g->url);
    free(g);
}

/* ── Commands ────────────────────────────────── */

static MODCMD_FUNC(cmd_gcreate)
{
    struct handle_info *hi;
    char *name;

    REQUIRE_PARAMS(2);
    name = argv[1];

    if (!(hi = user->handle_info)) {
        reply("MSG_AUTHENTICATE");
        return 0;
    }
    if (strlen(name) > GROUP_MAX_NAME) {
        reply("GSMSG_NOT_FOUND", name);
        return 0;
    }
    if (group_find(name)) {
        reply("GSMSG_ALREADY_EXISTS", name);
        return 0;
    }
    group_create(name, hi);
    reply("GSMSG_CREATED", name);
    return 1;
}

static MODCMD_FUNC(cmd_gdrop)
{
    struct group_info *g;

    REQUIRE_PARAMS(2);
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_founder(g, user->handle_info)) {
        reply("GSMSG_NOT_FOUNDER", g->name);
        return 0;
    }
    reply("GSMSG_DROPPED", g->name);
    dict_remove(groups, g->name);
    return 1;
}

static MODCMD_FUNC(cmd_gjoin)
{
    struct group_info *g;
    struct group_member *m;

    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (group_find_member(g, user->handle_info)) {
        reply("GSMSG_ALREADY_MEMBER", g->name);
        return 0;
    }
    if (!(g->flags & GROUP_FLAG_OPEN)) {
        reply("GSMSG_GROUP_INVITE", g->name);
        return 0;
    }
    m = calloc(1, sizeof(*m));
    m->handle = user->handle_info;
    m->flags = 0;
    m->joined = now;
    dict_insert(g->members, user->handle_info->handle, m);
    reply("GSMSG_JOINED", g->name);
    return 1;
}

static MODCMD_FUNC(cmd_gleave)
{
    struct group_info *g;

    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_find_member(g, user->handle_info)) {
        reply("GSMSG_NOT_MEMBER", g->name);
        return 0;
    }
    if (group_is_founder(g, user->handle_info)) {
        reply("GSMSG_NOT_FOUNDER", g->name);
        return 0;
    }
    dict_remove(g->members, user->handle_info->handle);
    reply("GSMSG_LEFT", g->name);
    return 1;
}

static MODCMD_FUNC(cmd_ginvite)
{
    struct group_info *g;
    struct group_member *m;
    struct handle_info *target;

    REQUIRE_PARAMS(3);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_admin(g, user->handle_info)) {
        reply("GSMSG_NOT_ADMIN", g->name);
        return 0;
    }
    target = get_handle_info(argv[2]);
    if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
    if (group_find_member(g, target)) {
        reply("GSMSG_ALREADY_MEMBER", g->name);
        return 0;
    }
    m = calloc(1, sizeof(*m));
    m->handle = target;
    m->flags = 0;
    m->joined = now;
    dict_insert(g->members, target->handle, m);
    reply("GSMSG_INVITED", target->handle, g->name);
    return 1;
}

static MODCMD_FUNC(cmd_gkick)
{
    struct group_info *g;
    struct handle_info *target;

    REQUIRE_PARAMS(3);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_admin(g, user->handle_info)) {
        reply("GSMSG_NOT_ADMIN", g->name);
        return 0;
    }
    target = get_handle_info(argv[2]);
    if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
    if (!dict_find(g->members, target->handle, NULL)) {
        reply("GSMSG_NOT_MEMBER", g->name);
        return 0;
    }
    if (group_is_founder(g, target)) {
        reply("GSMSG_NOT_FOUNDER", g->name);
        return 0;
    }
    dict_remove(g->members, target->handle);
    reply("GSMSG_KICKED", target->handle, g->name);
    return 1;
}

static MODCMD_FUNC(cmd_gflags)
{
    struct group_info *g;
    struct group_member *m;
    struct handle_info *target;

    REQUIRE_PARAMS(2);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }

    if (argc < 3) {
        /* List all member flags */
        dict_iterator_t it;
        reply("Members of $b%s$b:", g->name);
        for (it = dict_first(g->members); it; it = iter_next(it)) {
            m = iter_data(it);
            char flags[16] = "";
            if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(flags, "F");
            if (m->flags & GMEMBER_FLAG_ADMIN) strcat(flags, "A");
            if (!flags[0]) strcpy(flags, "-");
            reply("  %s [%s]", iter_key(it), flags);
        }
        return 1;
    }

    if (!group_is_admin(g, user->handle_info)) {
        reply("GSMSG_NOT_ADMIN", g->name);
        return 0;
    }

    target = get_handle_info(argv[2]);
    if (!target) { reply("MSG_HANDLE_UNKNOWN", argv[2]); return 0; }
    m = group_find_member(g, target);
    if (!m) { reply("GSMSG_NOT_MEMBER", g->name); return 0; }

    if (argc >= 4) {
        const char *flagstr = argv[3];
        int adding = 1;
        for (; *flagstr; flagstr++) {
            switch (*flagstr) {
            case '+': adding = 1; break;
            case '-': adding = 0; break;
            case 'A': case 'a':
                if (adding) m->flags |= GMEMBER_FLAG_ADMIN;
                else m->flags &= ~GMEMBER_FLAG_ADMIN;
                break;
            }
        }
        char flags[16] = "";
        if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(flags, "F");
        if (m->flags & GMEMBER_FLAG_ADMIN) strcat(flags, "A");
        if (!flags[0]) strcpy(flags, "-");
        reply("GSMSG_FLAGS_SET", target->handle, g->name, flags);
    }
    return 1;
}

static MODCMD_FUNC(cmd_gset)
{
    struct group_info *g;
    const char *opt, *val;

    REQUIRE_PARAMS(4);
    if (!user->handle_info) { reply("MSG_AUTHENTICATE"); return 0; }
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }
    if (!group_is_founder(g, user->handle_info)) {
        reply("GSMSG_NOT_FOUNDER", g->name);
        return 0;
    }

    opt = argv[2];
    val = argv[3];

    if (!irccasecmp(opt, "OPEN")) {
        if (!irccasecmp(val, "ON") || !irccasecmp(val, "YES") || !strcmp(val, "1"))
            g->flags |= GROUP_FLAG_OPEN;
        else
            g->flags &= ~GROUP_FLAG_OPEN;
        reply("GSMSG_SET_SUCCESS", opt, g->name, val);
    } else if (!irccasecmp(opt, "DESCRIPTION")) {
        free(g->description);
        g->description = strdup(val);
        reply("GSMSG_SET_SUCCESS", opt, g->name, val);
    } else if (!irccasecmp(opt, "URL")) {
        free(g->url);
        g->url = strdup(val);
        reply("GSMSG_SET_SUCCESS", opt, g->name, val);
    } else {
        reply("Unknown option: %s", opt);
        return 0;
    }
    g->modified = now;
    return 1;
}

static MODCMD_FUNC(cmd_glist)
{
    dict_iterator_t it;
    const char *pattern = argc > 1 ? argv[1] : "*";
    int count = 0;

    reply("Groups matching $b%s$b:", pattern);
    for (it = dict_first(groups); it; it = iter_next(it)) {
        struct group_info *g = iter_data(it);
        if (match_ircglob(g->name, pattern)) {
            const char *open = (g->flags & GROUP_FLAG_OPEN) ? " [open]" : "";
            reply("  $b%s$b — %d members, founded by %s%s",
                  g->name, dict_size(g->members),
                  g->founder ? g->founder->handle : "(unknown)", open);
            count++;
        }
    }
    reply("%d group(s) found.", count);
    return 1;
}

static MODCMD_FUNC(cmd_ginfo)
{
    struct group_info *g;
    dict_iterator_t it;

    REQUIRE_PARAMS(2);
    g = group_find(argv[1]);
    if (!g) { reply("GSMSG_NOT_FOUND", argv[1]); return 0; }

    reply("Information on group $b%s$b:", g->name);
    reply("  Founder:     %s", g->founder ? g->founder->handle : "(unknown)");
    reply("  Members:     %d", dict_size(g->members));
    reply("  Created:     %s", ctime(&g->created));
    if (g->description)
        reply("  Description: %s", g->description);
    if (g->url)
        reply("  URL:         %s", g->url);
    reply("  Flags:       %s%s",
          (g->flags & GROUP_FLAG_OPEN) ? "OPEN " : "",
          (g->flags & GROUP_FLAG_INVITE) ? "INVITE" : "");
    reply("  Members:");
    for (it = dict_first(g->members); it; it = iter_next(it)) {
        struct group_member *m = iter_data(it);
        char flags[16] = "";
        if (m->flags & GMEMBER_FLAG_FOUNDER) strcat(flags, "Founder ");
        if (m->flags & GMEMBER_FLAG_ADMIN) strcat(flags, "Admin ");
        if (!flags[0]) strcpy(flags, "Member");
        reply("    %s [%s]", iter_key(it), flags);
    }
    return 1;
}

/* ── Persistence (saxdb) ─────────────────────── */

static int groupserv_saxdb_read(struct dict *db)
{
    struct dict *gdb;
    dict_iterator_t it;

    gdb = database_get_data(db, KEY_GROUPS, RECDB_OBJECT);
    if (!gdb) return 0;

    for (it = dict_first(gdb); it; it = iter_next(it)) {
        struct record_data *rd = iter_data(it);
        struct dict *obj = rd->d.object;
        struct group_info *g;
        struct handle_info *founder;
        const char *str;

        str = database_get_data(obj, KEY_FOUNDER, RECDB_QSTRING);
        founder = str ? get_handle_info(str) : NULL;

        g = calloc(1, sizeof(*g));
        g->name = strdup(iter_key(it));
        g->founder = founder;
        g->flags = 0;
        g->members = dict_new();
        dict_set_free_data(g->members, free_group_member);

        str = database_get_data(obj, KEY_FLAGS, RECDB_QSTRING);
        if (str) g->flags = strtoul(str, NULL, 0);

        str = database_get_data(obj, KEY_CREATED, RECDB_QSTRING);
        if (str) g->created = strtoul(str, NULL, 0);

        str = database_get_data(obj, KEY_MODIFIED, RECDB_QSTRING);
        if (str) g->modified = strtoul(str, NULL, 0);

        str = database_get_data(obj, KEY_DESCRIPTION, RECDB_QSTRING);
        if (str) g->description = strdup(str);

        str = database_get_data(obj, KEY_URL, RECDB_QSTRING);
        if (str) g->url = strdup(str);

        /* Read members */
        {
            struct dict *mdb = database_get_data(obj, KEY_MEMBERS, RECDB_OBJECT);
            if (mdb) {
                dict_iterator_t mit;
                for (mit = dict_first(mdb); mit; mit = iter_next(mit)) {
                    struct record_data *mrd = iter_data(mit);
                    struct dict *mobj = mrd->d.object;
                    struct handle_info *hi = get_handle_info(iter_key(mit));
                    if (!hi) continue;
                    struct group_member *m = calloc(1, sizeof(*m));
                    m->handle = hi;
                    str = database_get_data(mobj, KEY_MEMBER_FLAGS, RECDB_QSTRING);
                    if (str) m->flags = strtoul(str, NULL, 0);
                    str = database_get_data(mobj, KEY_JOINED, RECDB_QSTRING);
                    if (str) m->joined = strtoul(str, NULL, 0);
                    dict_insert(g->members, hi->handle, m);
                }
            }
        }

        dict_insert(groups, g->name, g);
    }
    return 0;
}

static int groupserv_saxdb_write(struct saxdb_context *ctx)
{
    dict_iterator_t it, mit;

    saxdb_start_record(ctx, KEY_GROUPS, 1);
    for (it = dict_first(groups); it; it = iter_next(it)) {
        struct group_info *g = iter_data(it);
        saxdb_start_record(ctx, g->name, 0);

        if (g->founder)
            saxdb_write_string(ctx, KEY_FOUNDER, g->founder->handle);
        saxdb_write_int(ctx, KEY_FLAGS, g->flags);
        saxdb_write_int(ctx, KEY_CREATED, g->created);
        saxdb_write_int(ctx, KEY_MODIFIED, g->modified);
        if (g->description)
            saxdb_write_string(ctx, KEY_DESCRIPTION, g->description);
        if (g->url)
            saxdb_write_string(ctx, KEY_URL, g->url);

        /* Write members */
        saxdb_start_record(ctx, KEY_MEMBERS, 1);
        for (mit = dict_first(g->members); mit; mit = iter_next(mit)) {
            struct group_member *m = iter_data(mit);
            saxdb_start_record(ctx, iter_key(mit), 0);
            saxdb_write_int(ctx, KEY_MEMBER_FLAGS, m->flags);
            saxdb_write_int(ctx, KEY_JOINED, m->joined);
            saxdb_end_record(ctx);
        }
        saxdb_end_record(ctx);

        saxdb_end_record(ctx);
    }
    saxdb_end_record(ctx);
    return 0;
}

/* ── Module init ─────────────────────────────── */

int groupserv_init(void)
{
    GS_LOG = log_register_type("GroupServ", "file:groupserv.log");

    groups = dict_new();
    dict_set_free_data(groups, free_group);

    groupserv_module = module_register("GroupServ", GS_LOG, GROUPSERV_CONF_NAME, NULL);
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

    log_module(GS_LOG, LOG_INFO, "GroupServ module initialized.");
    return 1;
}

int groupserv_finalize(void)
{
    return 1;
}
