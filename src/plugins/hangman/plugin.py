"""Hangman — Channel word guessing game.

Commands:
  hangman start [length]  — Start a new game
  hangman end             — End the current game
  hangman guess <letter>  — Guess a letter
"""

import random
import re

DICTIONARY = "/usr/share/dict/words"
MAN_PARTS = 7  # Total parts before death


class Game:
    def __init__(self, irc, target, length=0):
        self.irc = irc
        self.target = target
        self.man = 0
        self.guesses = {}
        self.word = ""

        length = int(length) if str(length).isdigit() else 0
        self.length = length if 3 <= length <= 30 else random.randint(5, 8)

        if self._pick_word():
            self._msg("HANGMAN is starting!")
            self.show()
        else:
            self._msg("Could not find a suitable word. Aborting.")
            self.man = 9999

    def _pick_word(self):
        try:
            with open(DICTIONARY) as f:
                words = [w.strip() for w in f
                         if len(w.strip()) == self.length and w.strip().isalpha()]
            if not words:
                return False
            self.word = random.choice(words).lower()
            return True
        except FileNotFoundError:
            self._msg(f"Dictionary not found: {DICTIONARY}")
            return False

    def _msg(self, text):
        self.irc.send_target_privmsg(self.irc.service, self.target, text)

    @property
    def masked(self):
        return "".join(c if c in self.guesses else "*" for c in self.word)

    def won(self):
        if self.man >= MAN_PARTS:
            return False
        return None if any(c not in self.guesses for c in self.word) else True

    def show(self):
        p = lambda n, s: s if self.man >= n else " "
        self._msg(f" /---{p(1, ',')}")
        self._msg(f" |   {p(2, 'o')}")
        self._msg(f" |  {p(4, '/')}{p(3, '|')}{p(5, chr(92))}")
        self._msg(f" |  {p(6, '/')} {p(7, chr(92))}")
        self._msg(f" ====")
        self._msg(f" {self.masked}  ({len(self.word)} letters)")
        result = self.won()
        if result is True:
            self._msg("YOU WON!")
        elif result is False:
            self._msg(f"HANGED! The word was: {self.word}")

    def guess(self, irc, letter):
        self.irc = irc
        if self.won() is not None:
            self._msg("Game over. Start a new one!")
            return
        letter = letter.lower()
        if len(letter) != 1 or not letter.isalpha():
            self._msg("Guess a single letter.")
            return
        if letter in self.guesses:
            self._msg(f"Already guessed '{letter}'! Penalty!")
            self.man += 1
        elif letter in self.word:
            self._msg("Correct!")
            self.guesses[letter] = True
        else:
            self._msg("Wrong!")
            self.guesses[letter] = True
            self.man += 1
        self.show()


class Hangman:
    def __init__(self, handler, irc):
        self.handler = handler
        self.name = "hangman"
        self.games = {}

        handler.addcommand(self.name, "start", self.start)
        handler.addcommand(self.name, "end", self.end)
        handler.addcommand(self.name, "guess", self.guess)

    def _target(self, irc):
        return irc.target if irc.target else irc.caller

    def start(self, irc, arg):
        t = self._target(irc)
        if t in self.games and self.games[t].won() is None:
            irc.reply("A game is already in progress. End it first.")
            return
        length = int(arg) if arg.isdigit() else 0
        self.games[t] = Game(irc, t, length)

    def end(self, irc, _arg):
        t = self._target(irc)
        if t in self.games:
            irc.reply(f"Game ended by {irc.caller}.")
            del self.games[t]
        else:
            irc.reply("No game in progress.")

    def guess(self, irc, arg):
        t = self._target(irc)
        if t in self.games:
            self.games[t].guess(irc, arg)
        else:
            irc.reply("No game in progress. Start one!")


Class = Hangman
