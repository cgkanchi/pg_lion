# pg_lion — build with: make PG_CONFIG=.local/pg/bin/pg_config
MODULE_big = pg_lion
OBJS = src/lion_container.o src/lion_sparse.o src/lion_pages.o src/lion_dir.o src/lion_am.o \
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

PG_CONFIG ?= .local/pg/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Standalone unit tests for the container library (no server needed).
UNIT_CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -DFRONTEND -Isrc \
              -I$(shell $(PG_CONFIG) --includedir-server) -I$(shell $(PG_CONFIG) --includedir)
UNIT_LDFLAGS = -L$(shell $(PG_CONFIG) --libdir) -lpgcommon -lpgport -lm

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
$(OBJS): src/lion.h src/lion_container.h src/lion_sparse.h src/lion_tid.h
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

.PHONY: recovery-check
recovery-check:
	./test/recovery/run.sh $(if $(RECOVERY_PREFIX),--prefix "$(RECOVERY_PREFIX)")
