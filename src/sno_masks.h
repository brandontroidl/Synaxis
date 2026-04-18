/*
 * synaxis_sno_fix.c
 *
 * Problem: Synaxis/X3's irc_sno() sends raw numeric masks that must
 *          match Cathexis's SNO_* defines. Currently X3 hardcodes mask
 *          values or uses undefined constants when sending SNO notices.
 *
 * Fix:     Define SNO_* constants matching Cathexis client.h and use
 *          them in all irc_sno() calls throughout X3.
 *
 * ═══════════════════════════════════════════════════════════════
 * ADD TO: src/proto-p10.c (or a new src/sno_masks.h)
 *
 * These values MUST match Cathexis include/client.h exactly.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef SYNAXIS_SNO_MASKS_H
#define SYNAXIS_SNO_MASKS_H

/* Standard P10 snomask values — must match Cathexis client.h */
#define SNO_OLDSNO      0x1       /* o - old unsorted messages */
#define SNO_SERVKILL    0x2       /* k - server kills (collisions) */
#define SNO_OPERKILL    0x4       /* K - oper kills */
#define SNO_HACK2       0x8       /* D - desyncs */
#define SNO_HACK3       0x10      /* s - temporary desyncs */
#define SNO_UNAUTH      0x20      /* u - unauthorized connections */
#define SNO_TCPCOMMON   0x40      /* e - TCP/socket errors */
#define SNO_TOOMANY     0x80      /* f - too many connections */
#define SNO_HACK4       0x100     /* h - Uworld actions */
#define SNO_GLINE       0x200     /* g - G-lines */
#define SNO_NETWORK     0x400     /* n - net join/break */
#define SNO_IPMISMATCH  0x800     /* i - IP mismatches */
#define SNO_THROTTLE    0x1000    /* t - throttle notices */
#define SNO_OLDREALOP   0x2000    /* r - oper-only messages */
#define SNO_CONNEXIT    0x4000    /* c - client connect/exit */
#define SNO_AUTO        0x8000    /* G - auto G-lines */
#define SNO_DEBUG       0x10000   /* d - debug messages */
#define SNO_NICKCHG     0x20000   /* N - nick changes */
#define SNO_AUTH        0x40000   /* A - IAuth notices */
#define SNO_WEBIRC      0x80000   /* w - WebIRC notices */

/* ─── NEW masks (must match Cathexis additions) ─── */
#define SNO_SHUN        0x100000  /* S - shun notices */
#define SNO_ZLINE       0x200000  /* Z - Z-line notices */
#define SNO_SASL        0x400000  /* a - SASL authentication */
#define SNO_SACMD       0x800000  /* C - SA* command usage */
#define SNO_FLOOD       0x1000000 /* F - flood/excess notices */
#define SNO_TLS         0x2000000 /* T - TLS connection info */
#define SNO_ACCOUNT     0x4000000 /* R - account changes */
#define SNO_SPAMF       0x8000000 /* P - spamfilter matches */

#endif /* SYNAXIS_SNO_MASKS_H */

/*
 * ═══════════════════════════════════════════════════════════════
 * USAGE CHANGES in Synaxis modules:
 *
 * ═══ src/nickserv.c ═══
 *
 * When a user registers:
 *   irc_sno(SNO_ACCOUNT, "ACCOUNT: %s registered account %s",
 *           user->nick, hi->handle);
 *
 * When a user identifies:
 *   irc_sno(SNO_ACCOUNT, "ACCOUNT: %s identified as %s",
 *           user->nick, hi->handle);
 *
 * When a user drops their account:
 *   irc_sno(SNO_ACCOUNT, "ACCOUNT: %s dropped account %s",
 *           user->nick, hi->handle);
 *
 * ═══ src/chanserv.c ═══
 *
 * When a channel is registered:
 *   irc_sno(SNO_ACCOUNT, "CHANSERV: %s registered channel %s",
 *           user->nick, ci->channel->name);
 *
 * ═══ src/opserv.c ═══
 *
 * When an SA* command is used (SAJOIN, SAPART, SANICK, etc.):
 *   irc_sno(SNO_SACMD, "OPSERV: %s used %s %s %s",
 *           user->nick, command, target, args);
 *
 * When a GLINE is set by opserv:
 *   irc_sno(SNO_GLINE, "OPSERV: %s added G-line on %s: %s",
 *           user->nick, mask, reason);
 *
 * When a SHUN is set:
 *   irc_sno(SNO_SHUN, "OPSERV: %s added shun on %s: %s",
 *           user->nick, mask, reason);
 *
 * ═══ SASL authentication (sasl_agent_enhanced.c) ═══
 *
 * On successful SASL auth:
 *   irc_sno(SNO_SASL, "SASL: %s authenticated as %s via PLAIN",
 *           identifier, account);
 *
 * On failed SASL auth:
 *   irc_sno(SNO_SASL, "SASL: %s failed PLAIN authentication for %s",
 *           identifier, attempted_account);
 *
 * ═══════════════════════════════════════════════════════════════
 */
