AM_CFLAGS = $(WARNING_CFLAGS) $(CODE_COVERAGE_CFLAGS)
AM_LDFLAGS = $(CODE_COVERAGE_LIBS)

AM_CPPFLAGS = -I$(top_srcdir) $(JANSSON_CFLAGS) $(LUA_INCLUDE)

TESTS_ENVIRONMENT = \
    LUA_PATH="$(abs_top_srcdir)/rdl/?.lua;$(FLUX_PREFIX)/share/lua/$(LUA_VERSION)/?.lua;$(FLUX_PREFIX)/share/lua/$(LUA_VERSION)/fluxometer/?.lua;$(LUA_PATH);;" \
    LUA_CPATH="$(FLUX_PREFIX)/lib64/lua/$(LUA_VERSION)/?.so;$(FLUX_PREFIX)/lib/lua/$(LUA_VERSION)/?.so;$(LUA_CPATH);;" \
    TESTRDL_INPUT_FILE="$(abs_top_srcdir)/conf/hype.lua"

TESTS = trdl test-jansson.lua

check_PROGRAMS = trdl
check_LTLIBRARIES = janssontest.la
trdl_SOURCES = trdl.c
trdl_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/rdl
trdl_LDADD = $(top_builddir)/rdl/libflux-rdl.la \
    $(FLUX_CORE_LIBS) $(LUA_LIB) $(JANSSON_LIBS) $(CZMQ_LIBS)

janssontest_la_LDFLAGS = \
    -avoid-version -module -shared --disable-static -rpath /dev/null
janssontest_la_LIBADD = \
    $(top_builddir)/rdl/libflux-rdl.la \
    $(JANSSON_LIBS) $(LUA_LIB) $(FLUX_CORE_LIBS)
janssontest_la_SOURCES = \
    jansson-test.c

EXTRA_DIST = test-rdl.lua test-jansson.lua
