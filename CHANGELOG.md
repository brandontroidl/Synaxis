# Synaxis Changelog

## 2.3.3 (2026-04-19)

### Removed — orphan duplicate modules and unused protocol backends
Deleted seven source files totaling 2,581 lines that were either AI-generated duplicates of already-built modules, parallel implementations with unwired integration hooks, or protocol backends for IRCds Synaxis will never link to. All removals verified by a full `./configure && make` pass that produced a working `x3` binary.

- `src/chanfix.c` (454 lines) — standalone copy of `mod-chanfix.c` with identical function signatures (`cmd_cfdisable`, `cmd_cfenable`, `cmd_cfinfo`, `cmd_cfix`, `cmd_cscore`, `chanfix_do_fix`, etc.). The `mod-` version is the one referenced by `Makefile.am` and actually built.
- `src/groupserv.c` (599 lines) — standalone duplicate of `mod-groupserv.c` (same `cmd_gcreate`, `cmd_gdrop`, `cmd_gjoin` signatures).
- `src/infoserv.c` (298 lines) — standalone duplicate of `mod-infoserv.c`.
- `src/mod-no.c` (7 lines) — null module stub (`no_init` / `no_finalize` that always return 1). Not referenced by the module loader.
- `src/proto-inspircd.c` (536 lines) — InspIRCd spanning-tree protocol backend. Synaxis is P10-only; Cathexis (its uplink) is P10-only. This backend was never reachable.
- `src/proto-ts6.c` (467 lines) — TS6 protocol backend (charybdis/ratbox family). Same reasoning as above.
- `src/sasl_agent_enhanced.c` (220 lines) — parallel SASL implementation with a comment block stating "To integrate: add `call_sasl_input_func()` hook in `proto-p10.c cmd_sasl`" — the hook was never added. The real `call_sasl_input_func` is defined in `hash.c:128` and proto-p10.c already routes through it. `sasl_handle_input`, `sasl_destroy`, `sasl_reply` from this file had zero external callers.

### Rationale
Two distinct failure modes produced this bloat: (1) duplicate `.c` files created as "new" implementations without noticing the `mod-` variant already existed and was in the build, and (2) protocol/feature scaffolding that got written but never wired into `Makefile.am` or the runtime dispatch tables. Both kinds are cruft that misleads audits and slows searches.

### Kept (not AI bloat)
- `src/mail-smtp.c` — genuine 2007 srvx code providing an alternative SMTP-direct mail backend. The build uses `mail-sendmail.c` instead, but this file is a legitimate swap-in option, not dead code.

## 2.3.2 (2026-04-19)

### Build / GeoIP
- **Legacy libGeoIP replaced with libmaxminddb (`src/hash.c`)** — MaxMind discontinued the GeoIP Legacy databases and the companion libGeoIP C library; stock distributions no longer ship a working dataset. `set_geoip_info()` has been rewritten against libmaxminddb (the GeoIP2 / GeoLite2 MMDB API): `MMDB_open` + `MMDB_lookup_string` + `MMDB_get_value`. Supports both City and Country MMDB files; falls back from city to country automatically. DMA/area code fields are zeroed in the struct because MaxMind dropped that US-only metadata in GeoIP2.
- **Config keys unchanged** — `services/opserv/geoip_data_file` (Country DB) and `services/opserv/geoip_city_data_file` (City DB, preferred when set) continue to work. Point them at `GeoLite2-Country.mmdb` / `GeoLite2-City.mmdb` instead of the old `.dat` files.
- **`configure.ac`** — `AC_CHECK_LIB(GeoIP, GeoIP_open)` → `AC_CHECK_LIB(maxminddb, MMDB_open)`; header check for `GeoIP.h`/`GeoIPCity.h` → `maxminddb.h`.
- **`src/config.h.in`** — `HAVE_GEOIP_H` / `HAVE_GEOIPCITY_H` / `HAVE_LIBGEOIP` retired; replaced with `HAVE_MAXMINDDB_H` / `HAVE_LIBMAXMINDDB`.
- **Build deps** — install `libmaxminddb-dev` (Debian/Ubuntu) or `libmaxminddb-devel` (Fedora/RHEL). The old `libgeoip-dev` is no longer a dependency and can be removed.

## 2.3.1 (2026-04-12)

### ChanServ
- **PROTECT/DEPROTECT commands** — New ChanServ commands for +a (protect/admin) mode. Requires coowner (400) access.
- **OWNER/DEOWNER commands** — New ChanServ commands for +q (owner) mode. Requires owner (500) access.
- **handle_auth() mode fix** — NickServ identify now mirrors JOIN handler mode logic: UL_OWNER→+qo, UL_COOWNER→+ao, UL_OP→+o, UL_HALFOP→+h, UL_PEON→+v.

### BotServ
- **modcmd_chanmode_announce macro** — Updated to use assigned BotServ bot instead of ChanServ for all 50 channel-facing mode operations. Fantasy commands (.op, .voice, .kick) now show as coming from the assigned bot.
- **botserv_handle_join callback** — Self-healing: ensures assigned bot is in channel on any user join. Sets channel_bot, removes ChanServ. Registered via `reg_join_func()`.
- **cmd_assign mode announce** — Uses `mod_chanmode_announce` instead of raw `AddChannelUser` mode setting. Bot properly appears with +ao in channel after assign.
- **saxdb persistence** — Full read/write functions for bots, channel assignments, and all settings. Survive restarts.

### Protocol
- **OPMODE server-originated** — `cmd_opmode` in proto-p10.c now accepts server numerics (fixes Acid LimitServ errors). Uses chanserv as internal mode source.
- **Burst +q/+a modes** — `irc_burst()` in proto-p10.c now includes MODE_OWNER (+q) and MODE_PROTECT (+a) in channel member burst modes. Previously only sent +o, +h, +v.
- **SSL linearized ring buffer** — `ioset.c` `ioset_try_write` uses static `ssl_wbuf` for contiguous `SSL_write`, handles retry pointer stability.

### Help
- **100+ help entries** — ChanServ (198 lines including SET/USET subcommands, PROTECT/OWNER), OperServ (154 lines), BotServ, HostServ, MemoServ, GroupServ, ChanFix, InfoServ all complete.

## 2.3.0 (2026-04-09)

### Security
- **S2S HMAC per-message signing** — every outgoing P10 line is signed with `@hmac=HMAC-SHA256(derived_key, message)` when `"s2s_hmac" "1"` is set in the server config. Key derivation matches Cathexis and Acid exactly: `HMAC-SHA256(link_password, "cathexis-s2s-hmac-v1")`.
- **AES-256-GCM database encryption** — all database files (x3.db) encrypted at rest when `"db_encryption_key"` is configured. Format: `[SX3E magic][version][12-byte IV][ciphertext][GCM tag]`. Transparent migration from plaintext — existing unencrypted databases are read normally and encrypted on next save.
- **Post-quantum crypto support** — compiles against liboqs when available for future PQ readiness.

### Channel Modes
- **Owner mode (+q)** — `MODE_OWNER` (0x04000000) tracked as distinct member status. ChanServ auto-sets +q for access level 500 (UL_OWNER) on join.
- **Protect/Admin mode (+a)** — `MODE_PROTECT` (0x08000000) tracked as distinct member status. ChanServ auto-sets +a for access level 400 (UL_COOWNER) on join.
- **Full +q/+a in mode announcements** — `mod_chanmode_announce()` sends proper +q/+a to the network instead of mapping to +o.

### New Modules
- **mod-groupserv** — account group management (CREATE, DROP, JOIN, LEAVE, INVITE, KICK, FLAGS, SET, LIST, INFO)
- **mod-chanfix** — automated channel recovery with periodic op scoring (FIX, SCORE, INFO, ENABLE, DISABLE)
- **mod-infoserv** — network announcements (POST, OPERPOST, DEL, LIST)
- All three modules create their own service bots from config.

### Module Fixes
- **mod-memoserv** — changed void to int return, removed exit(1), added bot nick fallback
- **mod-webtv** — removed exit(1), graceful fallback when bot key missing
- **mod-snoop / mod-track** — NULL channel guard in finalize
- **mod-python** — Python 3.13 compatibility guard, guarded cleanup
- **Bot config key** — all modules accept both `"bot"` and `"nick"` config keys with fallback

### SSL/TLS
- **Non-blocking SSL handshake** — `ioset.c` retries with `select()` on `SSL_ERROR_WANT_READ/WRITE` instead of treating as fatal
- **Ctrl+C fix** — `quit_services` check added to inner reconnect loop

### Python Scripting
- **16 handler methods** — init, join, server_link, new_user, nick_change, del_user, topic, part, kick, account, oper, channel_mode, user_mode, new_channel, del_channel, handle_rename, failpw, allowauth, merge
- **3 bundled plugins** — annoy, example, hangman
- **Fixed SyntaxWarnings** in docstrings

### Help Files
- 21 help files included and tracked in git (no longer excluded by .gitignore)

### Build System
- All `.cvsignore` files removed
- `.gitignore` rewritten — help files tracked, build artifacts properly excluded
- Updated `.mailmap`

### Configuration
- Example config (`x3.conf.example`) with all module sections
- `"s2s_hmac"` and `"db_encryption_key"` in server section

## 2.2.0
- Fork from X3 with Cathexis P10 compatibility
- Argon2id password hashing
- libsodium integration
- TRE regex library
