# pg_lion — build with: make PG_CONFIG=<path to pg_config>
# (the default is .local/pg/bin/pg_config when that exists, else pg_config on PATH)
MODULE_big = pg_lion
OBJS = src/lion_container.o src/lion_sparse.o src/lion_wal.o src/lion_pages.o src/lion_dir.o src/lion_posting.o src/lion_am.o \
       src/lion_build.o src/lion_scan.o \
       src/lion_insert.o src/lion_vacuum.o src/lion_funcs.o src/lion_count.o src/lion_customscan.o \
       src/lion_multikey.o
PGFILEDESC = "pg_lion - roaring bitmap inverted index"

EXTENSION = pg_lion pg_lion_citext
DATA = pg_lion--0.1.sql pg_lion_citext--0.1.sql

REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test
ISOLATION = $(patsubst test/isolation/%.spec,%,$(sort $(wildcard test/isolation/*.spec)))
ISOLATION_OPTS = --inputdir=test/isolation --outputdir=test/isolation

PG_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc
EXTRA_CLEAN = test/unit/container_test test/unit/sparse_test test/results test/isolation/results

PG_CONFIG ?= $(if $(wildcard .local/pg/bin/pg_config),.local/pg/bin/pg_config,pg_config)
PGXS := $(shell $(PG_CONFIG) --pgxs)

# The isolation specs that park a backend on an injection point need a server
# configured with --enable-injection-points (PostgreSQL 17 or later) and the
# injection_points test module installed.  Where that module is missing they
# are left out, and the run says so; INJECTION_POINTS=1/0 overrides the guess.
INJECTION_SPECS = $(patsubst test/isolation/%.spec,%,$(shell grep -l injection_points test/isolation/*.spec))
INJECTION_POINTS ?= $(if $(wildcard $(shell $(PG_CONFIG) --sharedir)/extension/injection_points.control),1,0)
ifneq ($(INJECTION_POINTS),1)
ISOLATION := $(filter-out $(INJECTION_SPECS),$(ISOLATION))
$(info pg_lion: no injection_points module in this installation; skipping isolation specs: $(INJECTION_SPECS))
endif
include $(PGXS)

# Standalone unit tests for the container library (no server needed).
UNIT_CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -DFRONTEND -Isrc \
              -I$(shell $(PG_CONFIG) --includedir-server) -I$(shell $(PG_CONFIG) --includedir)
# pkglibdir first: the Debian/Ubuntu packages put the server's own
# libpgcommon.a and libpgport.a there, while libdir holds libpq-dev's copies,
# which are of whatever major libpq-dev is at.  Source builds have them in
# libdir only.
UNIT_LDFLAGS = -L$(shell $(PG_CONFIG) --pkglibdir) -L$(shell $(PG_CONFIG) --libdir) -lpgcommon -lpgport -lm

test/unit/container_test: test/unit/container_test.c src/lion_container.c src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/container_test.c src/lion_container.c $(UNIT_LDFLAGS)

test/unit/sparse_test: test/unit/sparse_test.c src/lion_sparse.c src/lion_container.c \
                       src/lion_sparse.h src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/sparse_test.c src/lion_sparse.c src/lion_container.c $(UNIT_LDFLAGS)

.PHONY: unit
unit: test/unit/container_test test/unit/sparse_test
	./test/unit/container_test
	./test/unit/sparse_test

# header deps (the PostgreSQL build we compile against was not configured with --enable-depend)
$(OBJS): src/lion.h src/lion_compat.h src/lion_container.h src/lion_sparse.h src/lion_tid.h
src/lion_count.o src/lion_customscan.o src/lion_am.o: src/lion_count.h

# Crash-recovery and hot-standby tests (test/recovery/README.md).  These need a
# whole PostgreSQL *installation* to initdb their own private clusters into,
# not just a pg_config, and they install the extension into it, so the prefix
# is named separately rather than derived from PG_CONFIG:
#
#   make recovery-check PG_CONFIG=<prefix>/bin/pg_config RECOVERY_PREFIX=<prefix>
#
# With RECOVERY_PREFIX unset, run.sh uses the worktree install
# (../pg_roaring_index-partial/.local/pg) - deliberately NOT the prefix
# PG_CONFIG points at, so a bare "make recovery-check" cannot install into the
# dev cluster's tree.  Nothing here touches the dev cluster either way: run.sh
# creates its own clusters on a private socket directory and port and removes
# them on exit.
RECOVERY_PREFIX ?=
EXTRA_CLEAN += test/recovery/log

# The whole suite with the custom WAL resource manager registered (DESIGN.md
# §25).  The resource manager can only be registered from
# shared_preload_libraries, and a preloaded library is loaded once at
# postmaster start, so this RESTARTS the dev cluster with the option on the
# command line, runs installcheck, and restarts it back into generic mode.
# Nothing is written into postgresql.conf, so an interrupted run leaves no
# trace.
#
#   make installcheck-rmgr
#   make installcheck-rmgr WAL_CONSISTENCY=1   ... and replay every page and
#                                                  compare it, which is the
#                                                  proof that redo reproduces
#                                                  them
.PHONY: installcheck-rmgr
installcheck-rmgr:
	WAL_CONSISTENCY=$(if $(WAL_CONSISTENCY),$(WAL_CONSISTENCY),0) \
		PG_CONFIG="$(PG_CONFIG)" ./test/rmgr-check.sh

.PHONY: recovery-check
recovery-check:
	./test/recovery/run.sh $(if $(RECOVERY_PREFIX),--prefix "$(RECOVERY_PREFIX)") \
		$(if $(RECOVERY_MODE),--mode $(RECOVERY_MODE)) \
		$(if $(WAL_CONSISTENCY),--conf "wal_consistency_checking = 'pg_lion'")

# The same harness with the custom WAL resource manager registered and every
# page of every lion record replayed and compared (DESIGN.md §25).  This is
# the proof that redo reproduces the writer's pages: the standby replays
# everything phase 1 and phase 2 write, and a mismatch is a FATAL in the
# startup process.
.PHONY: recovery-check-rmgr
recovery-check-rmgr:
	./test/recovery/run.sh $(if $(RECOVERY_PREFIX),--prefix "$(RECOVERY_PREFIX)") \
		--mode rmgr --conf "wal_consistency_checking = 'pg_lion'" \
		--conf "wal_keep_size = 2GB" --conf "max_wal_size = 2GB"

# The PGXN release archive: pg_lion-<version>.zip, made by git archive from
# the committed HEAD, so it holds exactly the tracked files named here - what
# PGXS needs, the documentation and the test suites - and nothing from the
# working tree (.local, benchmark results, build products, .deps).  The
# version is META.json's; the extension's own version is the control files'
# default_version, and the two map as DESIGN.md section 23 says.
DIST_VERSION = $(shell sed -n 's/^   "version": "\(.*\)",$$/\1/p' META.json)
DIST_NAME = pg_lion-$(DIST_VERSION)
DIST_FILES = META.json README.md LICENSE DESIGN.md Makefile dev.sh \
             $(addsuffix .control,$(EXTENSION)) $(DATA) src \
             test/sql test/expected test/isolation test/unit test/recovery test/rmgr-check.sh

.PHONY: dist
dist:
	@test -n "$(DIST_VERSION)" || { echo "no version in META.json" >&2; exit 1; }
	@git diff --quiet HEAD -- $(DIST_FILES) || \
		echo "warning: uncommitted changes are not in $(DIST_NAME).zip, which is built from HEAD" >&2
	git archive --format=zip --prefix=$(DIST_NAME)/ -o $(DIST_NAME).zip HEAD -- $(DIST_FILES)
