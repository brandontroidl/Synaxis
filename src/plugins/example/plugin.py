"""
Example Plugin — Template for building Synaxis Python plugins.

This demonstrates all available features:
  - Event hooks (join, part, nick change, etc.)
  - Custom commands
  - Using the _svc C bridge for IRC operations

To use this plugin:
  1. Copy this directory to plugins/yourplugin/
  2. Rename the class and update Class = YourPlugin
  3. Bind commands:
     /msg OperServ BIND OperServ py command *python.command

  Then: /msg OperServ py command yourplugin yourcommand args

Available _svc functions:
  _svc.send_target_privmsg(source, target, message)
  _svc.send_target_notice(source, target, message)
  _svc.kill(source, target, reason)
  _svc.kick(source, target, channel, reason)
  _svc.fakehost(target, hostname)
  _svc.sanick(source, old_nick, new_nick)
  _svc.saquit(source, target, reason)
  _svc.sajoin(source, target, channel)
  _svc.get_user(nick)       — returns dict with user info or None
  _svc.get_info()           — returns dict of service names
  _svc.get_channel(name)    — returns dict with channel info or None
  _svc.get_account(name)    — returns dict with account info or None
  _svc.log_module(level, msg) — 1=info, 2=warning, 3=error
  _svc.dump(text)           — debug log
"""

import _svc


class Example:
    def __init__(self, handler, irc):
        """Called once when the plugin is loaded.

        Args:
            handler: The modpython handler instance.
            irc: An irc context object (may be empty at load time).
        """
        self.handler = handler
        self.name = "example"

        # Register commands (accessible via: py command example <cmd> <args>)
        handler.addcommand(self.name, "hello", self.cmd_hello)
        handler.addcommand(self.name, "info", self.cmd_info)
        handler.addcommand(self.name, "whois", self.cmd_whois)

        # Register event hooks (uncomment the ones you need)
        # handler.addhook("join", self.on_join)
        # handler.addhook("part", self.on_part)
        # handler.addhook("nick_change", self.on_nick_change)
        # handler.addhook("new_user", self.on_connect)
        # handler.addhook("del_user", self.on_quit)
        # handler.addhook("topic", self.on_topic)
        # handler.addhook("kick", self.on_kick)
        # handler.addhook("channel_mode", self.on_mode)

    # ─── Commands ───

    def cmd_hello(self, irc, args):
        """Responds with a greeting."""
        irc.reply(f"Hello, {irc.caller}! Args: {args or '(none)'}")

    def cmd_info(self, irc, args):
        """Shows plugin and environment info."""
        try:
            info = _svc.get_info()
            irc.reply(f"Services: {', '.join(f'{k}={v}' for k, v in info.items())}")
        except Exception:
            irc.reply("Service info not available.")

    def cmd_whois(self, irc, nick):
        """Looks up a user by nick."""
        if not nick:
            irc.reply("Usage: example whois <nick>")
            return
        try:
            user = _svc.get_user(nick.strip())
            if user:
                irc.reply(f"{nick}: {user}")
            else:
                irc.reply(f"{nick}: not found or not online.")
        except Exception as e:
            irc.reply(f"Error: {e}")

    # ─── Event hooks ───

    def on_join(self, irc, channel, nick):
        """Fired when a user joins a channel."""
        _svc.log_module(1, f"[example] {nick} joined {channel}")

    def on_part(self, irc, *args):
        """Fired when a user parts a channel."""
        pass

    def on_nick_change(self, irc, new_nick, old_nick):
        """Fired when a user changes nick."""
        _svc.log_module(1, f"[example] {old_nick} -> {new_nick}")

    def on_connect(self, irc, *args):
        """Fired when a new user connects."""
        pass

    def on_quit(self, irc, *args):
        """Fired when a user disconnects."""
        pass

    def on_topic(self, irc, *args):
        """Fired when a channel topic changes."""
        pass

    def on_kick(self, irc, *args):
        """Fired when a user is kicked."""
        pass

    def on_mode(self, irc, *args):
        """Fired when channel modes change."""
        pass


Class = Example
