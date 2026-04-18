# Synaxis

**Version 2.3.1** — IRC services for the Cathexis P10 protocol.

Copyright (c) Cathexis Development. Forked from X3/srvx.

## Features

- **11 service bots** — NickServ, ChanServ, OperServ, Global, SpamServ, BotServ, HostServ, MemoServ, GroupServ, ChanFix, InfoServ
- **15 compiled modules** — blacklist, botserv, chanfix, groupserv, helpserv, hostserv, infoserv, memoserv, no, python, qserver, snoop, sockcheck, track, webtv
- **BotServ** — Custom channel bots with fantasy commands, vanity nicks, +ao status, auto-heal on join, saxdb persistence
- **Owner/Protect modes** — +q (owner) and +a (admin) auto-set by ChanServ, PROTECT/OWNER commands
- **S2S HMAC signing** — per-message HMAC-SHA256 authentication on the uplink
- **AES-256-GCM database encryption** — all data encrypted at rest in mondo.db
- **Python 3.13 scripting** — extensible plugin framework with 16 event handlers
- **Argon2id passwords** — modern password hashing via libargon2
- **SASL support** — SASL PLAIN authentication via NickServ
- **Post-quantum ready** — optional liboqs integration (ML-KEM-768, ML-DSA-65)

## Building

Dependencies: `gcc`, `libssl-dev`, `libargon2-dev`, `libsodium-dev`, `libtre-dev`, `libgeoip-dev`, `python3-dev`

Optional: `liboqs` (post-quantum cryptography)

```bash
./configure --prefix=$HOME/services \
  --enable-modules=blacklist,botserv,helpserv,hostserv,memoserv,no,python,qserver,snoop,sockcheck,track,webtv,groupserv,chanfix,infoserv
make
make install
```

## Configuration

Copy `x3.conf.example` to `$PREFIX/etc/x3.conf` and edit. Key settings:

- `uplinks` — address, port, password matching the ircd Connect block
- `server` — hostname, numeric, cloaking keys (must match ircd)
- `server/s2s_hmac` — set to `"1"` to enable per-message HMAC signing
- `server/db_encryption_key` — passphrase for AES-256-GCM database encryption
- `server/admin` — account name that gets level 1000 on first registration

Module bots use the `"bot"` config key (with `"nick"` as fallback).

## Running

```bash
cd $PREFIX/bin
./x3 -d -f     # foreground with debug output
./x3 -d        # daemonize with debug logging
```

## License

GNU General Public License v3+
