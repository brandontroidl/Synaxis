"""Annoy — Example plugin demonstrating hooks and commands."""

import _svc


class Annoy:
    def __init__(self, handler, irc):
        self.handler = handler
        self.name = "annoy"

        # Register commands
        handler.addcommand(self.name, "dance", self.dance)
        handler.addcommand(self.name, "nickof", self.nickof)

        # Uncomment to enable join hook (will message on every join):
        # handler.addhook("join", self.on_join)

    def on_join(self, irc, channel, nick):
        """Called when a user joins a channel."""
        irc.send_target_privmsg(irc.service or "python", channel,
                                f"{nick} joined {channel}")

    def dance(self, irc, args):
        """Command: annoy dance [message]"""
        nick = irc.caller
        user = _svc.get_user(nick) if hasattr(_svc, 'get_user') else None

        reply = "Ok,"
        if user and isinstance(user, dict) and "account" in user:
            reply += f" Mr. {user['account']}"
        reply += " we can dance"
        if args:
            reply += f" {args}"
        reply += "."
        irc.reply(reply)

    def nickof(self, irc, bot):
        """Command: annoy nickof <service>"""
        try:
            info = _svc.get_info()
            if bot and bot in info:
                irc.reply(f"{bot} has nick {info[bot]}")
            else:
                irc.reply(f"Unknown. Available: {', '.join(info.keys())}")
        except Exception:
            irc.reply("Service info not available.")


Class = Annoy
