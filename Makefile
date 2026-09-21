# roaring_index — build with: make PG_CONFIG=.local/pg/bin/pg_config
MODULE_big = roaring_index
OBJS = src/rbi_container.o src/rbi_sparse.o src/rbi_pages.o src/rbi_am.o src/rbi_build.o src/rbi_scan.o \
       src/rbi_insert.o src/rbi_vacuum.o src/rbi_funcs.o src/rbi_count.o src/rbi_customscan.o
PGFILEDESC = "roaring_index - roaring bitmap inverted index"

EXTENSION = roaring_index roaring_index_citext
DATA = roaring_index--0.1.sql roaring_index_citext--0.1.sql

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

test/unit/container_test: test/unit/container_test.c src/rbi_container.c src/rbi_container.h src/rbi_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/container_test.c src/rbi_container.c $(UNIT_LDFLAGS)

test/unit/sparse_test: test/unit/sparse_test.c src/rbi_sparse.c src/rbi_container.c \
                       src/rbi_sparse.h src/rbi_container.h src/rbi_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/sparse_test.c src/rbi_sparse.c src/rbi_container.c $(UNIT_LDFLAGS)

.PHONY: unit
unit: test/unit/container_test test/unit/sparse_test
	./test/unit/container_test
	./test/unit/sparse_test

# header deps (the PostgreSQL build we compile against was not configured with --enable-depend)
$(OBJS): src/rbi.h src/rbi_container.h src/rbi_sparse.h src/rbi_tid.h
src/rbi_count.o src/rbi_customscan.o src/rbi_am.o: src/rbi_count.h
