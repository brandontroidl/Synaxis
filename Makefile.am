EXTRA_DIST = FAQ x3.conf.example sockcheck.conf.example autogen.sh
SUBDIRS = @MY_SUBDIRS@
PY_CFLAGS = @PY_CFLAGS@
PY_LIBS = @PY_LIBS@
pythonpath = @pythonpath@

DIST_SUBDIRS = src rx

# Binary is built by recursive make in src/
# Help files are installed by install-exec-local

install-exec-local:
	$(INSTALL) -d -m 755 $(DESTDIR)$(bindir)
	$(INSTALL) -d -m 755 $(DESTDIR)$(sysconfdir)
	$(INSTALL) -d -m 755 $(DESTDIR)$(datadir)/x3
	$(INSTALL) -d -m 755 $(DESTDIR)$(localstatedir)/x3
	$(INSTALL) -d -m 755 $(DESTDIR)$(datadir)/x3/plugins
	@echo "Installing help files to $(DESTDIR)$(datadir)/x3 ..."
	@for f in $(srcdir)/src/*.help; do \
		if [ -f "$$f" ]; then $(INSTALL) -m 644 "$$f" $(DESTDIR)$(datadir)/x3; fi; \
	done
	@if [ -f $(srcdir)/sockcheck.conf.example ]; then $(INSTALL) -m 644 $(srcdir)/sockcheck.conf.example $(DESTDIR)$(datadir)/x3; fi
	@if [ -f $(srcdir)/src/modpython.py ]; then $(INSTALL) -m 644 $(srcdir)/src/modpython.py $(DESTDIR)$(datadir)/x3; fi
	@if [ -x ./install-r.sh ] && [ -d $(srcdir)/src/plugins ]; then ./install-r.sh $(srcdir)/src/plugins $(DESTDIR)$(datadir)/x3/plugins; fi
	@echo "Installing default config to $(DESTDIR)$(sysconfdir) ..."
	@if [ ! -f $(DESTDIR)$(sysconfdir)/x3.conf ]; then \
		$(INSTALL) -m 600 $(srcdir)/x3.conf.example $(DESTDIR)$(sysconfdir)/x3.conf; \
		echo "  Created $(DESTDIR)$(sysconfdir)/x3.conf (from x3.conf.example)"; \
	else \
		echo "  $(DESTDIR)$(sysconfdir)/x3.conf already exists, not overwriting"; \
	fi
	@if [ ! -f $(DESTDIR)$(sysconfdir)/sockcheck.conf ]; then \
		$(INSTALL) -m 644 $(srcdir)/sockcheck.conf.example $(DESTDIR)$(sysconfdir)/sockcheck.conf; \
		echo "  Created $(DESTDIR)$(sysconfdir)/sockcheck.conf"; \
	fi
	@if [ ! -f $(DESTDIR)$(localstatedir)/x3/sockcheck.conf ]; then \
		$(INSTALL) -m 644 $(srcdir)/sockcheck.conf.example $(DESTDIR)$(localstatedir)/x3/sockcheck.conf; \
		echo "  Created $(DESTDIR)$(localstatedir)/x3/sockcheck.conf"; \
	fi
	@echo "Creating placeholder databases in $(DESTDIR)$(localstatedir)/x3 ..."
	@for db in mondo.db sendmail.db opserv.db modcmd.db aliases.db; do \
		if [ ! -f $(DESTDIR)$(localstatedir)/x3/$$db ]; then \
			touch $(DESTDIR)$(localstatedir)/x3/$$db; \
			echo "  Created $$db"; \
		fi; \
	done
	@if [ ! -d $(DESTDIR)$(localstatedir)/x3/languages ]; then \
		mkdir -p $(DESTDIR)$(localstatedir)/x3/languages; \
		touch $(DESTDIR)$(localstatedir)/x3/languages/strings.db; \
		echo "  Created languages/strings.db"; \
	fi
	@echo
	@echo "═══════════════════════════════════════════════"
	@echo "  Synaxis $(VERSION) installed to $(prefix)"
	@echo "═══════════════════════════════════════════════"
	@echo "  Binary:    $(bindir)/x3"
	@echo "  Config:    $(sysconfdir)/x3.conf"
	@echo "  Help:      $(datadir)/x3/"
	@echo "  Database:  $(localstatedir)/x3/"
	@echo
	@echo "  Edit $(sysconfdir)/x3.conf then run:"
	@echo "    $(bindir)/x3"
	@echo "═══════════════════════════════════════════════"
	@echo
