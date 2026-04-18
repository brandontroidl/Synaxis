"""
modpython.py — Synaxis Python 3 Scripting Framework
Copyright (c) Cathexis Development

Main module loaded by mod-python.c. Provides:
  - irc class: context object passed to every event callback
  - handler class: dispatches events, manages plugins and commands

Python 3.10+ required. Tested with Python 3.13.

Setup in x3.conf:
  "modules" {
    "python" {
      "scripts_dir" "/home/you/services/share/x3";
      "main_module" "modpython";
    };
  };

Bind commands:
  /msg OperServ BIND OperServ py run *python.run
  /msg OperServ BIND OperServ py reload *python.reload
  /msg OperServ BIND OperServ py command *python.command
"""

from __future__ import annotations
import importlib
import os
import sys
import traceback

try:
    import _svc
except ImportError:
    class _svc:
        """Mock for testing outside Synaxis."""
        @staticmethod
        def dump(msg): print(f"[dump] {msg}")
        @staticmethod
        def log_module(level, msg): print(f"[log:{level}] {msg}")
        @staticmethod
        def send_target_privmsg(src, tgt, msg): print(f">{src}>{tgt}: {msg}")
        @staticmethod
        def send_target_notice(src, tgt, msg): print(f"-{src}-{tgt}: {msg}")
        @staticmethod
        def kill(src, tgt, reason): print(f"[kill] {tgt}: {reason}")
        @staticmethod
        def kick(src, tgt, chan, reason): print(f"[kick] {tgt} from {chan}: {reason}")
        @staticmethod
        def get_user(nick): return None
        @staticmethod
        def get_info(): return {}
        @staticmethod
        def get_channel(name): return None
        @staticmethod
        def get_account(name): return None
        @staticmethod
        def fakehost(tgt, host): pass
        @staticmethod
        def sanick(src, old, new): pass
        @staticmethod
        def saquit(src, tgt, reason): pass
        @staticmethod
        def sajoin(src, tgt, chan): pass


class irc:
    """IRC context object created by C code for every event callback.

    Attributes:
        service  — service nick handling the event (e.g. "ChanServ")
        caller   — nick of the user who triggered the event
        target   — channel name (if applicable) or empty string
    """

    def __init__(self, service="", caller="", target=""):
        self.service = service or ""
        self.caller = caller or ""
        self.target = target or ""

    def reply(self, msg):
        """Send a reply to the caller via the current service."""
        if self.caller:
            _svc.send_target_privmsg(self.service, self.caller, str(msg))

    def send_target_privmsg(self, source, target, msg):
        """Send a PRIVMSG from source to target."""
        _svc.send_target_privmsg(source, target, str(msg))

    def send_target_notice(self, source, target, msg):
        """Send a NOTICE from source to target."""
        _svc.send_target_notice(source, target, str(msg))


class handler:
    """Main event handler. C code creates one instance and calls methods for events."""

    def __init__(self):
        self.plugins = {}
        self.hooks = {}      # event_name -> [(callback, args, extra)]
        self.commands = {}    # "plugin.cmd" -> callback
        _svc.log_module(1, "Python handler initialized")
        self._load_plugins()

    # ─── Plugin system ───

    def addhook(self, event, callback, args=None, extra=None):
        """Register a callback for an IRC event."""
        if event not in self.hooks:
            self.hooks[event] = []
        self.hooks[event].append((callback, args, extra))

    def addcommand(self, plugin_name, command, callback):
        """Register a plugin command."""
        key = f"{plugin_name}.{command}".lower()
        self.commands[key] = callback

    def _fire_hooks(self, event, irc_obj, *args):
        """Fire all hooks registered for an event."""
        for callback, hook_args, extra in self.hooks.get(event, []):
            try:
                callback(irc_obj, *args)
            except Exception:
                _svc.log_module(3, f"Hook error in {event}: {traceback.format_exc()}")

    def _load_plugins(self):
        """Load plugins from plugins/ directory."""
        plugins_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plugins")
        if not os.path.isdir(plugins_dir):
            _svc.log_module(1, f"No plugins directory at {plugins_dir}")
            return

        if plugins_dir not in sys.path:
            sys.path.insert(0, os.path.dirname(plugins_dir))

        for name in sorted(os.listdir(plugins_dir)):
            if name.startswith(".") or name.startswith("__"):
                continue
            path = os.path.join(plugins_dir, name)
            if not os.path.isdir(path):
                continue
            plugin_file = os.path.join(path, "plugin.py")
            if not os.path.exists(plugin_file):
                continue
            try:
                mod = importlib.import_module(f"plugins.{name}.plugin")
                importlib.reload(mod)
                cls = getattr(mod, "Class", None) or getattr(mod, "Plugin", None)
                if cls:
                    dummy_irc = irc()
                    instance = cls(self, dummy_irc)
                    self.plugins[name] = instance
                    _svc.log_module(1, f"Loaded plugin: {name}")
                else:
                    _svc.log_module(2, f"Plugin {name}: no Class or Plugin found")
            except Exception:
                _svc.log_module(3, f"Failed to load plugin {name}: {traceback.format_exc()}")

    # ─── Event callbacks from C ───

    def init(self, irc_obj, *args):
        """Called after Python is fully initialized."""
        _svc.log_module(1, f"Python framework ready — {len(self.plugins)} plugin(s), "
                        f"{len(self.commands)} command(s), {sum(len(v) for v in self.hooks.values())} hook(s)")
        return 1

    def join(self, irc_obj, channel, nick):
        self._fire_hooks("join", irc_obj, channel, nick)
        return 1

    def server_link(self, server_name):
        self._fire_hooks("server_link", irc(), server_name)
        return 1

    def new_user(self, irc_obj, *args):
        self._fire_hooks("new_user", irc_obj, *args)
        return 1

    def nick_change(self, irc_obj, nick, old_nick):
        self._fire_hooks("nick_change", irc_obj, nick, old_nick)
        return 1

    def del_user(self, irc_obj, *args):
        self._fire_hooks("del_user", irc_obj, *args)
        return 1

    def topic(self, irc_obj, *args):
        self._fire_hooks("topic", irc_obj, *args)
        return 1

    def part(self, irc_obj, *args):
        self._fire_hooks("part", irc_obj, *args)
        return 1

    def kick(self, irc_obj, *args):
        self._fire_hooks("kick", irc_obj, *args)
        return 1

    def account(self, irc_obj, *args):
        self._fire_hooks("account", irc_obj, *args)
        return 1

    def oper(self, irc_obj, *args):
        self._fire_hooks("oper", irc_obj, *args)
        return 1

    def channel_mode(self, irc_obj, *args):
        self._fire_hooks("channel_mode", irc_obj, *args)
        return 1

    def user_mode(self, irc_obj, *args):
        self._fire_hooks("user_mode", irc_obj, *args)
        return 1

    def new_channel(self, irc_obj, *args):
        self._fire_hooks("new_channel", irc_obj, *args)
        return 1

    def del_channel(self, irc_obj, *args):
        self._fire_hooks("del_channel", irc_obj, *args)
        return 1

    def handle_rename(self, irc_obj, *args):
        self._fire_hooks("handle_rename", irc_obj, *args)
        return 1

    def failpw(self, irc_obj, *args):
        self._fire_hooks("failpw", irc_obj, *args)
        return 1

    def allowauth(self, irc_obj, *args):
        self._fire_hooks("allowauth", irc_obj, *args)
        return 1

    def merge(self, irc_obj, *args):
        self._fire_hooks("merge", irc_obj, *args)
        return 1

    def cmd_run(self, irc_obj, *args):
        """Handle: py run <code>"""
        code = " ".join(str(a) for a in args) if args else ""
        if not code:
            irc_obj.reply("Usage: run <python code>")
            return
        try:
            result = eval(code)
            irc_obj.reply(f"Result: {result}")
        except SyntaxError:
            try:
                exec(code)
                irc_obj.reply("Executed.")
            except Exception as e:
                irc_obj.reply(f"Error: {e}")
        except Exception as e:
            irc_obj.reply(f"Error: {e}")

    def cmd_command(self, irc_obj, plugin_name, command, *args):
        """Handle: py command <plugin> <cmd> [args]"""
        key = f"{plugin_name}.{command}".lower()
        callback = self.commands.get(key)
        if not callback:
            irc_obj.reply(f"Unknown command: {plugin_name} {command}")
            return
        try:
            arg_str = " ".join(str(a) for a in args) if args else ""
            callback(irc_obj, arg_str)
        except Exception as e:
            irc_obj.reply(f"Error: {e}")
