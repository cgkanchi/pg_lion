# pg_lion — build with: make PG_CONFIG=<path to pg_config>
# (the default is .local/pg/bin/pg_config when that exists, else pg_config on PATH)
MODULE_big = pg_lion
# The page layer (lion.h): pages, the meta page, the relation state, entries,
# and writing container chains.
PAGES_OBJS = src/lion_pages.o src/lion_meta.o src/lion_state.o src/lion_entry.o \
       src/lion_posting_put.o
# The count engine: its interface is src/lion_count.h, and its files share the
# private header src/lion_count_int.h.
COUNT_OBJS = src/lion_set.o src/lion_set_copy.o src/lion_cursor.o src/lion_expr.o src/lion_vis.o \
       src/lion_count.o src/lion_count_groups.o src/lion_count_decode.o src/lion_count_shared.o \
       src/lion_rangesrc.o src/lion_range.o src/lion_count_sql.o src/lion_facets.o
# The LionCount custom scan: its planner half (lion_plan_*) and its executor
# half (lion_exec_*), which share the private header src/lion_customscan.h.
CUSTOMSCAN_OBJS = src/lion_plan_match.o src/lion_plan_partition.o src/lion_plan_target.o \
       src/lion_plan_cost.o src/lion_plan_fkjoin_cost.o src/lion_plan_fkjoin.o \
       src/lion_plan_count.o src/lion_plan_gsets.o src/lion_plan_hooks.o src/lion_plan_units.o \
       src/lion_plan_private.o \
       src/lion_exec_begin.o src/lion_exec_locate.o src/lion_exec_count.o \
       src/lion_exec_fkjoin.o src/lion_exec_run.o src/lion_exec_explain.o
# The SQL-callable helpers: lion_funcs.c and lion_index_verify()'s files, which
# share the private header src/lion_funcs.h.
FUNCS_OBJS = src/lion_funcs.o src/lion_verify.o src/lion_verify_dir.o src/lion_verify_heap.o \
       src/lion_verify_summary.o
# lionvec, the nearest-neighbour index of pgvector's `vector` and `halfvec`
# (DESIGN.md §44): the AM, its scan, and its arithmetic, which is unit-tested
# standalone.
VEC_OBJS = src/lion_vec.o src/lion_vec_scan.o src/lion_vec_loc.o src/lion_vec_math.o
# Reading a relation's blocks in an order its reader predicts: a read stream
# over them from 17 on, asks of the kernel before (DESIGN.md §44.4, "Read
# streams"); the ring's bookkeeping is unit-tested standalone.
READAHEAD_OBJS = src/lion_readahead.o src/lion_readahead_ring.o
OBJS = src/lion_container.o src/lion_sparse.o src/lion_positions.o src/lion_wal.o src/lion_waldelta.o $(PAGES_OBJS) src/lion_dir.o src/lion_dir_delete.o src/lion_posting.o src/lion_postree.o src/lion_posbuild.o src/lion_posfilter.o src/lion_bm25.o src/lion_bm25_scan.o src/lion_reader.o \
       src/lion_am.o src/lion_amcost.o src/lion_build.o src/lion_spool.o src/lion_scan.o \
       src/lion_insert.o src/lion_vacuum.o $(FUNCS_OBJS) $(COUNT_OBJS) $(CUSTOMSCAN_OBJS) \
       src/lion_multikey.o src/lion_jsonb.o src/lion_fkjoin.o src/lion_ordered.o src/lion_selfuncs.o \
       src/lion_costs.o src/lion_arena.o $(VEC_OBJS) $(READAHEAD_OBJS)
PGFILEDESC = "pg_lion - roaring bitmap inverted index"

EXTENSION = pg_lion pg_lion_citext pg_lion_vector
DATA = pg_lion--0.1.sql pg_lion_citext--0.1.sql pg_lion_vector--0.1.sql

REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test
ISOLATION = $(patsubst test/isolation/%.spec,%,$(sort $(wildcard test/isolation/*.spec)))
ISOLATION_OPTS = --inputdir=test/isolation --outputdir=test/isolation

PG_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc
# `make WERROR=1` makes every warning an error, here and in the unit tests; CI
# builds that way on every supported major.  The two -Wno- flags stay: the
# PostgreSQL headers themselves trip both.
ifeq ($(WERROR),1)
PG_CFLAGS += -Werror
endif
# `make LION_NO_SIMD=1` leaves out the container library's AVX2 and POPCNT
# kernels, which x86-64 GCC and clang builds pick between at run time
# (DESIGN.md §3, "Whole-bitset kernels"), and runs the portable one
# everywhere.  The unit tests always build a copy that way too.
ifeq ($(LION_NO_SIMD),1)
PG_CFLAGS += -DLION_NO_SIMD
endif
# lionvec's arithmetic is compiled without fused multiply-adds, which GCC
# and clang would otherwise make of a multiply and an add wherever the target
# has them - aarch64, an x86-64 baseline such as x86-64-v3 - so that it rounds
# as written everywhere and its kernels agree to the bit (DESIGN.md §44.3,
# "Kernels"); the unit tests compile it so too.
VEC_MATH_CFLAGS = -ffp-contract=off
src/lion_vec_math.o: override CFLAGS += $(VEC_MATH_CFLAGS)
src/lion_vec_math.bc: override BITCODE_CFLAGS += $(VEC_MATH_CFLAGS)
EXTRA_CLEAN = test/unit/container_test test/unit/container_test_nosimd test/unit/sparse_test test/unit/positions_test \
              test/unit/waldelta_test test/unit/entry_test test/unit/vec_test test/unit/vec_test_nosimd \
              test/unit/readahead_test test/results test/isolation/results
# Every object and dependency file in src, not only those OBJS names now, so
# that a source removed or renamed leaves no object behind.
EXTRA_CLEAN += src/*.o src/*.d src/*.bc

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
# lionvec's tests (test/sql/vector*.sql, test/isolation/lionvec_*.spec) need
# pgvector installed; where it is missing they are left out, and the run says
# so; PGVECTOR=1/0 overrides the guess.
VECTOR_TESTS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/vector*.sql)))
VECTOR_SPECS = $(patsubst test/isolation/%.spec,%,$(sort $(wildcard test/isolation/lionvec_*.spec)))
PGVECTOR ?= $(if $(wildcard $(shell $(PG_CONFIG) --sharedir)/extension/vector.control),1,0)
ifneq ($(PGVECTOR),1)
REGRESS := $(filter-out $(VECTOR_TESTS),$(REGRESS))
ISOLATION := $(filter-out $(VECTOR_SPECS),$(ISOLATION))
$(info pg_lion: no pgvector in this installation; skipping tests: $(VECTOR_TESTS) $(VECTOR_SPECS))
endif
include $(PGXS)

# Standalone unit tests for the container library (no server needed).
UNIT_CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -DFRONTEND -Isrc \
              -I$(shell $(PG_CONFIG) --includedir-server) -I$(shell $(PG_CONFIG) --includedir)
ifeq ($(WERROR),1)
UNIT_CFLAGS += -Werror
endif
ifeq ($(LION_NO_SIMD),1)
UNIT_CFLAGS += -DLION_NO_SIMD
endif
# pkglibdir first: the Debian/Ubuntu packages put the server's own
# libpgcommon.a and libpgport.a there, while libdir holds libpq-dev's copies,
# which are of whatever major libpq-dev is at.  Source builds have them in
# libdir only.
UNIT_LDFLAGS = -L$(shell $(PG_CONFIG) --pkglibdir) -L$(shell $(PG_CONFIG) --libdir) -lpgcommon -lpgport -lm

# `make unit SANITIZE=1` builds the unit tests with AddressSanitizer and
# UndefinedBehaviorSanitizer and makes every report fatal, which CI does on
# every major.  The unit tests are frontend programs that link nothing but
# libpgcommon and libpgport, so the sanitizers need no help from the server,
# and they are what feeds the container and sparse code malformed input.  The
# binaries are always rebuilt under SANITIZE=1: a plain `make unit` leaves
# binaries that are up to date by their sources, and running those would
# silently test nothing extra.  (The reverse is harmless: a plain `make unit`
# after a sanitized one runs the sanitized binaries until a source changes.)
ifeq ($(SANITIZE),1)
UNIT_CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
UNIT_LDFLAGS += -fsanitize=address,undefined
test/unit/container_test test/unit/container_test_nosimd test/unit/sparse_test test/unit/positions_test \
    test/unit/waldelta_test test/unit/entry_test test/unit/vec_test test/unit/vec_test_nosimd \
    test/unit/readahead_test test/unit/arena_test: .lion-force-unit
.PHONY: .lion-force-unit
endif

test/unit/container_test: test/unit/container_test.c src/lion_container.c src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/container_test.c src/lion_container.c $(UNIT_LDFLAGS)

# The same tests with -DLION_NO_SIMD: the build without the x86-64 kernels,
# which must refuse to force them and run the portable one.  On an x86-64
# host also -mpopcnt, which makes that kernel the fused loop other
# architectures run, where x86 built for its baseline counts through
# pg_popcount() - which container_test covers when it forces the portable
# kernel.
UNIT_NOSIMD_CFLAGS = -DLION_NO_SIMD $(if $(filter x86_64 amd64,$(shell uname -m)),-mpopcnt)
test/unit/container_test_nosimd: test/unit/container_test.c src/lion_container.c src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) $(UNIT_NOSIMD_CFLAGS) -o $@ test/unit/container_test.c src/lion_container.c $(UNIT_LDFLAGS)

test/unit/sparse_test: test/unit/sparse_test.c src/lion_sparse.c src/lion_container.c \
                       src/lion_sparse.h src/lion_positions.h src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/sparse_test.c src/lion_sparse.c src/lion_container.c $(UNIT_LDFLAGS)

test/unit/positions_test: test/unit/positions_test.c src/lion_positions.c \
                          src/lion_positions.h src/lion_container.h src/lion_tid.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/positions_test.c src/lion_positions.c $(UNIT_LDFLAGS)

# The WAL's item encoding (DESIGN.md §25, LION_OP_DELTA and LION_OP_SPLICE):
# what the writer logs and what the redo rebuilds from it, round-tripped.
test/unit/waldelta_test: test/unit/waldelta_test.c src/lion_waldelta.c src/lion_waldelta.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/waldelta_test.c src/lion_waldelta.c $(UNIT_LDFLAGS)

test/unit/entry_test: test/unit/entry_test.c src/lion_entryfmt.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/entry_test.c $(UNIT_LDFLAGS)

# lionvec's arithmetic (DESIGN.md §44.3 - §44.4): the rotation, the codes, the
# kernels and how often the estimates' bounds hold; and the same without the
# x86-64 kernels, as container_test_nosimd is built.
test/unit/vec_test: test/unit/vec_test.c src/lion_vec_math.c src/lion_vec_math.h
	$(CC) $(UNIT_CFLAGS) $(VEC_MATH_CFLAGS) -o $@ test/unit/vec_test.c src/lion_vec_math.c $(UNIT_LDFLAGS)

test/unit/vec_test_nosimd: test/unit/vec_test.c src/lion_vec_math.c src/lion_vec_math.h
	$(CC) $(UNIT_CFLAGS) $(VEC_MATH_CFLAGS) $(UNIT_NOSIMD_CFLAGS) -o $@ test/unit/vec_test.c src/lion_vec_math.c $(UNIT_LDFLAGS)

# The read stream ring's bookkeeping (DESIGN.md §44.4, "Read streams"),
# against a model of core's read stream.
test/unit/readahead_test: test/unit/readahead_test.c src/lion_readahead_ring.c src/lion_readahead_ring.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/readahead_test.c src/lion_readahead_ring.c $(UNIT_LDFLAGS)

# The arena a LionOrdered set spills to (DESIGN.md §30.4, "Spilling"): the
# size classes, the free lists, the regions over a temporary file, reset and
# release, against a shadow model.
test/unit/arena_test: test/unit/arena_test.c src/lion_arena.c src/lion_arena.h
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/arena_test.c src/lion_arena.c $(UNIT_LDFLAGS)

.PHONY: unit
unit: test/unit/container_test test/unit/container_test_nosimd test/unit/sparse_test test/unit/positions_test \
      test/unit/waldelta_test test/unit/entry_test test/unit/vec_test test/unit/vec_test_nosimd \
      test/unit/readahead_test test/unit/arena_test
	./test/unit/container_test
	./test/unit/container_test_nosimd
	./test/unit/sparse_test
	./test/unit/positions_test
	./test/unit/waldelta_test
	./test/unit/entry_test
	./test/unit/vec_test
	./test/unit/vec_test_nosimd
	./test/unit/readahead_test
	./test/unit/arena_test

# Header dependencies, made by the compiler: each object's .d file lists every
# header its source includes, so no hand-kept list can miss one and leave a
# stale object with an old struct layout after a header change.  A server
# configured with --enable-depend already does this in its own rules (into
# .deps/); elsewhere - the distributions' packages are not - it is done here.
# -MP keeps a deleted header from breaking the build.
ifneq ($(autodepend),yes)
override CFLAGS += -MMD -MP
-include $(OBJS:.o=.d)
endif

# Crash-recovery and hot-standby tests (test/recovery/README.md).  These need a
# whole PostgreSQL *installation* to initdb their own private clusters into,
# not just a pg_config, and they install the extension into it, so the prefix
# is named separately rather than derived from PG_CONFIG:
#
#   make recovery-check PG_CONFIG=<prefix>/bin/pg_config RECOVERY_PREFIX=<prefix>
#
# RECOVERY_PREFIX is REQUIRED: run.sh refuses to start without it rather than
# guess, and it is deliberately not derived from PG_CONFIG, so a bare "make
# recovery-check" can never install into whatever installation the dev cluster
# happens to use.  As root (a container, a CI image) also set
# RECOVERY_RUN_AS=<unprivileged user>, because initdb and postgres refuse to
# run as root.  Nothing here touches the dev cluster: run.sh creates its own
# clusters in a private mktemp -d directory, with the socket directory inside
# it, and removes that directory on exit.
RECOVERY_PREFIX ?=
EXTRA_CLEAN += test/recovery/log

# What the expected outputs assume of the server, established by installcheck
# itself rather than left to whichever cluster it runs against.  The session
# settings go to every connection the suite makes, regression and isolation
# alike, through PGOPTIONS: plans and their EXPLAIN shapes depend on them, and
# a test that needs another value SETs it, as before.  ./dev.sh's cluster sets
# the same values server-wide.  What a session cannot set is checked first:
# the contrib modules some tests read pages and WAL with, whose absence
# otherwise shows up as a dozen unrelated diffs.
LION_TEST_OPTIONS = -c work_mem=64MB -c maintenance_work_mem=1GB \
                    -c max_parallel_workers_per_gather=0 \
                    -c max_parallel_maintenance_workers=0 -c jit=off
LION_TEST_EXTENSIONS = citext pageinspect pg_buffercache pg_walinspect
installcheck: export PGOPTIONS := $(PGOPTIONS) $(LION_TEST_OPTIONS)
installcheck: lion-test-env
.PHONY: lion-test-env
lion-test-env:
	@missing=$$($(bindir)/psql -X -At -d postgres -c "SELECT string_agg(e, ' ') FROM unnest(string_to_array('$(LION_TEST_EXTENSIONS)', ' ')) e WHERE e NOT IN (SELECT name FROM pg_available_extensions)") || exit 1; \
	if [ -n "$$missing" ]; then \
		echo "pg_lion: the server has no $$missing extension, which the tests need (install PostgreSQL's contrib modules)" >&2; \
		exit 1; \
	fi

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

# pg_lion beside another extension that chains the same planner hook and
# registers its own WAL resource manager, in both load orders (DESIGN.md §23),
# the table-AM refusals that need a table AM other than the heap (§2),
# counts on a pool of sixteen buffers, which the pin budgets must fit (§15),
# and, on PostgreSQL 16, that no lion index is read while
# old_snapshot_threshold is set (§9).
# Builds and installs the test-only test/modules/lion_hooktest into the
# installation PG_CONFIG names, and restarts the dev cluster as
# installcheck-rmgr does.
#
#   make hookcheck
#   make hookcheck HOOKCHECK_FULL=1   ... and the whole suite in both preload
#                                         orders
.PHONY: hookcheck
hookcheck:
	HOOKCHECK_FULL=$(if $(HOOKCHECK_FULL),$(HOOKCHECK_FULL),0) WERROR=$(if $(WERROR),$(WERROR),0) \
		PG_CONFIG="$(PG_CONFIG)" ./test/hook-check.sh

.PHONY: recovery-check
recovery-check:
	./test/recovery/run.sh $(if $(RECOVERY_PREFIX),--prefix "$(RECOVERY_PREFIX)") \
		$(if $(RECOVERY_MODE),--mode $(RECOVERY_MODE)) \
		$(if $(WAL_CONSISTENCY),--conf "wal_consistency_checking = '$(if $(filter rmgr,$(RECOVERY_MODE)),pg_lion,generic)'")

# The same harness with the custom WAL resource manager registered and every
# page of every lion record replayed and compared (DESIGN.md §25).  This is
# the proof that redo reproduces the writer's pages: the standby replays
# everything phase 1 and phase 2 write, and a mismatch is a FATAL in the
# startup process.
.PHONY: recovery-check-rmgr
recovery-check-rmgr:
	./test/recovery/run.sh $(if $(RECOVERY_PREFIX),--prefix "$(RECOVERY_PREFIX)") \
		--mode rmgr --conf "wal_consistency_checking = 'pg_lion'"

# The concurrent write + VACUUM + count soak (test/soak/README.md): the dev
# cluster restarted into each WAL mode in turn, as installcheck-rmgr does,
# with a hot standby in rmgr mode, and put back afterwards.
#
#   make soak                                  10 minutes a mode
#   make soak SOAK_DURATION=120 SOAK_MODES=rmgr
#   make soak SOAK_RUN_AS=<user>               as root: the cluster's user
.PHONY: soak
soak:
	PG_CONFIG="$(PG_CONFIG)" ./test/soak/modes.sh

# The PGXN release archive: pg_lion-<version>.zip, made by git archive from
# the committed HEAD, so it holds exactly the tracked files named here - what
# PGXS needs, the documentation and the test suites - and nothing from the
# working tree (.local, benchmark results, build products, .deps).  The
# version is META.json's; the extension's own version is the control files'
# default_version, and the two map as DESIGN.md section 23 says.
DIST_VERSION = $(shell sed -n 's/^   "version": "\(.*\)",$$/\1/p' META.json)
DIST_NAME = pg_lion-$(DIST_VERSION)
DIST_FILES = META.json README.md LICENSE DESIGN.md docs Makefile dev.sh \
             $(addsuffix .control,$(EXTENSION)) $(DATA) src \
             test/sql test/expected test/isolation test/unit test/recovery test/rmgr-check.sh \
             test/hook-check.sh test/modules

.PHONY: dist
dist:
	@test -n "$(DIST_VERSION)" || { echo "no version in META.json" >&2; exit 1; }
	@git diff --quiet HEAD -- $(DIST_FILES) || \
		echo "warning: uncommitted changes are not in $(DIST_NAME).zip, which is built from HEAD" >&2
	git archive --format=zip --prefix=$(DIST_NAME)/ -o $(DIST_NAME).zip HEAD -- $(DIST_FILES)
