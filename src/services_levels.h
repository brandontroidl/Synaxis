/*
 * Synaxis/X3 — src/services_levels.h
 * Copyright (C) 2026 Synaxis Development
 *
 * Named services access level constants for Synaxis/X3.
 * These map to the numeric opserv_level values stored per-account.
 *
 * ═══════════════════════════════════════════════════════════════
 *                   SERVICES HIERARCHY
 *
 * Level 0:    Regular User         — No services operator access
 * Level 100:  Services Operator    — Basic services administration
 * Level 400:  Services Admin       — Full services management
 * Level 900:  Services Root Admin  — All services access + SA* commands
 * Level 1000: Owner                — Hardcoded root, cannot be demoted
 *
 * SA* Command Requirement (CRITICAL):
 *   Services Root Administrator (opserv_level >= 900) can trigger
 *   SA* operations (SAJOIN, SAMODE, etc.) ONLY IF they ALSO have
 *   +N (Network Administrator) on the IRCd.
 *
 *   This dual-check prevents privilege escalation:
 *   - A services root without +N on IRC cannot use SA* commands
 *   - A +N oper without services root cannot use SA* via services
 *   - Only someone with BOTH has full SA* capability
 *
 * IRCd Oper Level Mapping:
 *   Local IRCop (+O)        — No services level equivalent
 *   IRCop (+o)              — Typically Services Operator
 *   Server Admin (+a)       — Typically Services Administrator
 *   Network Admin (+N)      — Must be Services Root for SA* via services
 *   Service Bot (+k)        — Not a human operator level
 *
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef INCLUDED_services_levels_h
#define INCLUDED_services_levels_h

/* ── Named access levels ─────────────────────────────────────── */

/** Services Operator — can use basic OpServ commands:
 *  TRACE, WHOIS, ACCESS, SESSION, AKILL (with restrictions),
 *  channel suspend, nick suspend */
#define SVCSLEVEL_OPER          100

/** Services Administrator — can manage services settings:
 *  JUPE, NOOP, FORBID, NEWS, all ChanServ admin commands,
 *  NickServ SASET, GETEMAIL, BotServ management */
#define SVCSLEVEL_ADMIN         400

/** Services Root Administrator — full services access:
 *  All OpServ commands including RESTART, DIE, SET, MODULE.
 *  Can trigger SA* operations but ONLY with +N on IRC.
 *  Can assign/modify other services opers. */
#define SVCSLEVEL_SRA           900

/** Owner — hardcoded root account. Cannot be demoted except
 *  by another owner. The initial bootstrap account. */
#define SVCSLEVEL_OWNER         1000

/* ── Helper macros ───────────────────────────────────────────── */

/** Check if a user has at least the given services level */
#define HasServicesLevel(user, level) \
    ((user)->handle_info && (user)->handle_info->opserv_level >= (level))

/** Check if a user is a Services Operator */
#define IsServicesOper(user)    HasServicesLevel(user, SVCSLEVEL_OPER)

/** Check if a user is a Services Administrator */
#define IsServicesAdmin(user)   HasServicesLevel(user, SVCSLEVEL_ADMIN)

/** Check if a user is a Services Root Administrator */
#define IsServicesRoot(user)    HasServicesLevel(user, SVCSLEVEL_SRA)

/*
 * NETWORK ADMINISTRATOR (+N) AUTH FLOW:
 *
 * The +N user mode is NEVER granted by /OPER alone.
 * Even if the oper block has "netadmin = yes", the user stays at +a
 * (Server Admin) until they authenticate with NickServ.
 *
 * Flow:
 *   1. User /OPERs → gets +o/+a but NOT +N (m_oper.c check)
 *   2. User /MSG NickServ IDENTIFY → Synaxis sends ACCOUNT to IRCd
 *   3. IRCd receives ACCOUNT → m_account.c checks:
 *      if (IsAnOper(user) && HasPriv(PRIV_NETADMIN) && !IsNetAdmin)
 *        → SetNetAdmin + send_umode_out → user gains +N
 *   4. If user /MSG NickServ LOGOUT or deauths:
 *      IRCd receives ACCOUNT U → m_account.c checks:
 *      if (IsNetAdmin) → ClearNetAdmin → user drops to +a
 *
 * This means:
 *   - An oper who never identifies never gets +N
 *   - An oper who identifies gets +N automatically
 *   - An oper who logs out loses +N immediately
 *   - SA* commands via services still require CanServicesUseSA
 *     (opserv_level >= 900 AND IsNetAdmin), which now implies auth
 *
 * The auto_admin mode string in NickServ config must NOT contain +N.
 * Synaxis strips 'N' from auto_admin as a safety measure.
 */

/** Check if a user is the Owner */
#define IsServicesOwner(user)   HasServicesLevel(user, SVCSLEVEL_OWNER)

/**
 * Can this user use SA* commands via services?
 *
 * Requires BOTH:
 *   1. Services Root Administrator (opserv_level >= 900)
 *   2. Network Administrator (+N) on the IRCd
 *
 * This is the CRITICAL dual-check that prevents privilege escalation.
 * Services bots (+k) bypass this check — they use SA* via S2S directly.
 */
#define CanServicesUseSA(user) \
    (IsServicesRoot(user) && IsNetAdmin(user))

/**
 * Get a human-readable name for a user's services level.
 */
static inline const char *services_level_name(unsigned short level)
{
    if (level >= SVCSLEVEL_OWNER) return "Owner";
    if (level >= SVCSLEVEL_SRA)   return "Services Root Administrator";
    if (level >= SVCSLEVEL_ADMIN) return "Services Administrator";
    if (level >= SVCSLEVEL_OPER)  return "Services Operator";
    return "User";
}

#endif /* INCLUDED_services_levels_h */
