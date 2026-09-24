# lion_hooktest: test-only module for test/hook-check.sh (make hookcheck).
# Built and installed by that script, never by the pg_lion build.
MODULES = lion_hooktest
EXTENSION = lion_hooktest
DATA = lion_hooktest--1.0.sql
PGFILEDESC = "lion_hooktest - test-only hook companion for pg_lion"

# The tests are chosen per server configuration by test/hook-check.sh, which
# passes REGRESS itself; this default is what runs with nothing preloaded.
REGRESS ?= hooks_setup hooks_lion_outer
REGRESS_OPTS = --inputdir=. --outputdir=.

PG_CPPFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
ifeq ($(WERROR),1)
PG_CPPFLAGS += -Werror
endif
EXTRA_CLEAN = results regression.diffs regression.out

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
