cache?=../.cache/vendor

lua_cflags:=-std=c99 -Wall -Wextra -g -O3 -DLUA_USE_POSIX $(CFLAGS)
lua_sources:=$(filter-out lua/lua.c lua/onelua.c,$(wildcard lua/*.c))
lua_objects:=$(patsubst lua/%.c,$(cache)/lua/%.o,$(lua_sources))

pcre2_cflags:=-std=c99 -Wall -Wextra -g -O3 -DPCRE2_CODE_UNIT_WIDTH=8 -DSUPPORT_JIT -DSUPPORT_UNICODE -DHAVE_CONFIG_H $(CFLAGS)
pcre2_names:= \
	pcre2_auto_possess.o pcre2_chartables.o pcre2_chkdint.o pcre2_compile.o \
	pcre2_compile_class.o pcre2_config.o pcre2_context.o pcre2_convert.o \
	pcre2_dfa_match.o pcre2_error.o pcre2_extuni.o pcre2_find_bracket.o \
	pcre2_jit_compile.o pcre2_maketables.o pcre2_match_data.o pcre2_match.o \
	pcre2_newline.o pcre2_ord2utf.o pcre2_pattern_info.o pcre2_script_run.o \
	pcre2_serialize.o pcre2_string_utils.o pcre2_study.o pcre2_substitute.o \
	pcre2_substring.o pcre2_tables.o pcre2_ucd.o pcre2_valid_utf.o \
	pcre2_xclass.o
pcre2_objects:=$(addprefix $(cache)/pcre2/,$(pcre2_names))

.PHONY: all clean

all: uthash/src/utlist.h $(cache)/libpcre2-8.a $(cache)/liblua5.5.a

uthash/src/utlist.h:
	command -v git && git submodule update --init --recursive

$(cache) $(cache)/lua $(cache)/pcre2:
	mkdir -p $@

$(cache)/liblua5.5.a: $(lua_objects) | $(cache)
	$(AR) rcs $@ $(lua_objects)

$(cache)/lua/%.o: lua/%.c | $(cache)/lua
	$(CC) -c $(lua_cflags) $< -o $@

pcre2/src/pcre2.h: pcre2/src/pcre2.h.generic
	cp -f $< $@

pcre2/src/config.h: pcre2/src/config.h.generic
	cp -f $< $@

pcre2/src/pcre2_chartables.c: pcre2/src/pcre2_chartables.c.dist
	cp -f $< $@

$(cache)/libpcre2-8.a: $(pcre2_objects) | $(cache)
	$(AR) rcs $@ $(pcre2_objects)

$(cache)/pcre2/%.o: pcre2/src/%.c pcre2/src/pcre2.h pcre2/src/config.h pcre2/src/pcre2_chartables.c | $(cache)/pcre2
	$(CC) -c $(pcre2_cflags) $< -o $@

clean:
	rm -f pcre2/libpcre2-8.a lua/liblua5.5.a
	rm -f pcre2/src/*.o lua/*.o
	rm -f pcre2/src/pcre2.h pcre2/src/config.h pcre2/src/pcre2_chartables.c
	rm -rf $(cache)
