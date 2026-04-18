                         Synaxis IRC Services 2.3.1

Synaxis is IRC services for Cathexis IRCd — ChanServ, NickServ,
OpServ, HelpServ, Global, SpamServ, BotServ, HostServ, MemoServ.

Features: +q/+a/+o/+h/+v prefix modes, 4-tier oper hierarchy,
SA* commands, IRCv3, OpenSSL, Argon2id, Python 3, GCC 15 clean.

Install:
  autoreconf -fi
  ./configure --prefix=$HOME/services \
    --enable-modules=blacklist,botserv,helpserv,hostserv,memoserv,\
    no,python,qserver,snoop,sockcheck,track,webtv
  make && make install

Copyright 2024-2026 Cathexis Development (GPLv3)
Based on X3/srvx, Copyright 2000-2006 srvx Development Team
https://github.com/brandontroidl/Synaxis
