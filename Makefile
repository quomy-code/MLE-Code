prefix?=/usr/local

# Sources live in src/. Binaries land in dist/. Objects and the unit harness land in .cache/.
mle_cache:=.cache
mle_dist:=dist
mle_srcdir:=src
mle_objdir:=$(mle_cache)/obj
mle_testdir:=$(mle_cache)/test
mle_vendor_libdir:=$(mle_cache)/vendor
ifeq ($(OS),Windows_NT)
  mle_exeext:=.exe
else
  mle_exeext:=
endif
mle_bin:=$(mle_dist)/mle$(mle_exeext)

mle_cflags:=-std=c99 -Wall -Wextra -pedantic -Wno-pointer-arith -Wno-unused-result -Wno-unused-parameter -g -O0 -D_GNU_SOURCE -DPCRE2_CODE_UNIT_WIDTH=8 -I$(mle_srcdir) $(CFLAGS) $(CPPFLAGS)
mle_ldflags:=$(LDFLAGS)
mle_dynamic_libs:=-lpcre2-8 -llua5.5
mle_static_libs:=$(mle_vendor_libdir)/libpcre2-8.a $(mle_vendor_libdir)/liblua5.5.a
mle_ldlibs:=-lm $(LDLIBS)
mle_sources:=$(filter-out %.inc.c,$(wildcard $(mle_srcdir)/*.c))
mle_objects:=$(patsubst $(mle_srcdir)/%.c,$(mle_objdir)/%.o,$(mle_sources))
mle_objects_no_main:=$(filter-out $(mle_objdir)/main.o,$(mle_objects))
mle_unit_test_sources:=$(wildcard tests/unit/test_*.c)
mle_unit_test_objects:=$(patsubst tests/unit/%.c,$(mle_testdir)/%.o,$(mle_unit_test_sources))
mle_depfiles:=$(mle_objects:.o=.d) $(mle_unit_test_objects:.o=.d)
mle_unit_test_all:=$(mle_testdir)/test$(mle_exeext)
mle_vendor_deps:=
mle_static_var:=
mle_git_sha:=$(shell test -d .git && command -v git >/dev/null && git rev-parse --short HEAD)
# Optional release filename tag, for example cosmo. Empty uses uname.
mle_dist_tag?=

ifdef mle_static
  mle_static_var:=-static
endif

ifdef mle_vendor
  mle_ldlibs:=$(mle_static_libs) $(mle_ldlibs)
  mle_cflags:=-Ivendor/pcre2/src -Ivendor -Ivendor/uthash/src $(mle_cflags)
  mle_vendor_deps:=$(mle_static_libs)
else
  mle_ldlibs:=$(mle_dynamic_libs) $(mle_ldlibs)
endif

ifneq ($(mle_git_sha),)
  mle_cflags:=-DMLE_VERSION_APPEND=-$(mle_git_sha) $(mle_cflags)
endif

.PHONY: all mle test unit release sloc install uninstall uscript clean clean_quick vendor

all: $(mle_bin)

mle: $(mle_bin)

$(mle_dist) $(mle_objdir) $(mle_testdir):
	mkdir -p $@

$(mle_bin): $(mle_vendor_deps) $(mle_objects) $(mle_srcdir)/termbox2.h | $(mle_dist)
	$(CC) $(mle_static_var) $(mle_cflags) $(mle_objects) $(mle_ldflags) $(mle_ldlibs) -o $@

$(mle_objdir)/%.o: $(mle_srcdir)/%.c | $(mle_objdir)
	$(CC) -MMD -MP -c $(mle_cflags) $< -o $@

$(mle_static_libs):
	$(MAKE) -C vendor CC="$(CC)" AR="$(AR)"
	@test -f $(mle_vendor_libdir)/libpcre2-8.a
	@test -f $(mle_vendor_libdir)/liblua5.5.a

$(mle_testdir)/%.o: tests/unit/%.c | $(mle_testdir)
	$(CC) -MMD -MP -DTEST_NAME=$(basename $(notdir $<)) -c $(mle_cflags) $< -o $@

$(mle_unit_test_all): $(mle_objects_no_main) $(mle_unit_test_objects) tests/unit/test.c $(mle_vendor_deps) | $(mle_testdir)
	$(CC) $(mle_cflags) -rdynamic tests/unit/test.c $(mle_objects_no_main) $(mle_unit_test_objects) $(mle_ldflags) $(mle_ldlibs) -ldl -o $@

unit: $(mle_unit_test_all)
	@set -eu; \
	for src in $(mle_unit_test_sources); do \
		name=$$(basename "$$src" .c); \
		echo "UNIT $$name"; \
		$(mle_unit_test_all) "$$name"; \
	done

test: $(mle_bin) unit
	$(mle_bin) -v && MLE=$(abspath $(mle_bin)) $(MAKE) -C tests

-include $(mle_depfiles)

# Named `release`, not `dist`: a target called dist fights the dist/ directory.
release: $(mle_bin)
	@set -eu; \
	tag="$(mle_dist_tag)"; \
	if [ -z "$$tag" ]; then tag="$$(uname -s | tr A-Z a-z)_$$(uname -m)"; fi; \
	ver="$$($(mle_bin) -N -v | awk '{print $$NF}')"; \
	name="mle-code_$${ver}_$${tag}$(mle_exeext)"; \
	cp -f $(mle_bin) $(mle_dist)/$$name; \
	echo "$(mle_dist)/$$name"; \
	cd $(mle_dist) && \
	if command -v sha256sum >/dev/null 2>&1; then sha256sum mle-code_* > SHA256SUMS; \
	elif command -v shasum >/dev/null 2>&1; then shasum -a 256 mle-code_* > SHA256SUMS; \
	fi

sloc:
	find $(mle_srcdir) -maxdepth 1 \
		'(' -name '*.c' -or -name '*.h' ')' \
		-not -name 'termbox2.h' \
		-exec cat {} ';' | wc -l

install: $(mle_bin)
	install -v -d $(DESTDIR)$(prefix)/bin
	install -v -m 755 $(mle_bin) $(DESTDIR)$(prefix)/bin/mle$(mle_exeext)

uninstall:
	rm -v $(DESTDIR)$(prefix)/bin/mle$(mle_exeext)

uscript:
	php $(mle_srcdir)/uscript.inc.php >$(mle_srcdir)/uscript.inc.c

clean_quick:
	rm -rf $(mle_objdir) $(mle_testdir) $(mle_dist)
	rm -f mle$(mle_exeext) *.o *.bak *.out *.bak.* a.out
	rm -f $(mle_srcdir)/*.o $(mle_srcdir)/*.bak $(mle_srcdir)/*.out $(mle_srcdir)/*.bak.* $(mle_srcdir)/a.out
	rm -f tests/unit/*.o tests/unit/test$(mle_exeext)
	rm -f $(patsubst tests/unit/%.c,tests/unit/%,$(mle_unit_test_sources))

clean: clean_quick
	$(MAKE) -C vendor clean
	rm -rf $(mle_cache)

.NOTPARALLEL:
