#!/bin/bash
# One-time setup on an Ubuntu or Debian machine (WSL2 included): PostgreSQL 18
# and pgvector from apt.postgresql.org, VectorChord's .deb from its GitHub
# release, pg_lion built from this checkout, a Python venv, a sudo rule so cold
# runs can drop the page cache, and the cluster (pg.sh init).  WITH_DISKANN=1
# also installs pgvectorscale from Timescale's apt repository, for
# KINDS="... diskann".
# Asks for sudo.  Safe to run again.
set -euo pipefail
. "$(dirname "$0")/env.sh"
VCHORD_VERSION=${VCHORD_VERSION:-1.1.1}

say() { printf '\n== %s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }

say "checks"
case "$REPO/" in /mnt/[a-z]/*) warn "the checkout is on a Windows drive ($REPO): build and run from the Linux filesystem (e.g. ~/pg_lion), /mnt/c is 10x slower"; exit 1 ;; esac
case "$BENCH_DIR/" in /mnt/[a-z]/*) warn "BENCH_DIR is on a Windows drive ($BENCH_DIR): use a Linux path"; exit 1 ;; esac
mkdir -p "$BENCH_DIR"
mem_gb=$(awk '/MemTotal/ {print int($2 / 1048576)}' /proc/meminfo)
free_gb=$(df -BG --output=avail "$BENCH_DIR" | tail -1 | tr -dc 0-9)
echo "RAM visible to Linux: ${mem_gb} GB; free disk at $BENCH_DIR: ${free_gb} GB"
if grep -qi microsoft /proc/version && [ "$mem_gb" -lt 20 ]; then
	warn "WSL is capped at ${mem_gb} GB (its default is half the PC's RAM). For the 1M-row run, give it more:"
	warn "  in %UserProfile%\\.wslconfig put  [wsl2]  memory=24GB  swap=8GB , then 'wsl --shutdown' from Windows."
	warn "  16 GB works, but more of the warm runs will read from disk."
fi
need=$(( NSHARDS * 60 / 26 + 10 ))
[ "$free_gb" -lt "$need" ] && warn "about ${need} GB of disk is needed for $NSHARDS shards; ${free_gb} GB free"

say "packages"
sudo apt-get update -q
sudo apt-get install -y -q curl ca-certificates gnupg lsb-release build-essential python3-venv postgresql-common
# no auto-created 'main' cluster: the benchmark runs its own
sudo mkdir -p /etc/postgresql-common/createcluster.d
echo 'create_main_cluster = false' | sudo tee /etc/postgresql-common/createcluster.d/lionbench-no-main.conf >/dev/null
[ -e /etc/apt/sources.list.d/pgdg.list ] || [ -e /etc/apt/sources.list.d/pgdg.sources ] ||
	sudo /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
sudo apt-get install -y -q "postgresql-$PG_MAJOR" "postgresql-server-dev-$PG_MAJOR" "postgresql-$PG_MAJOR-pgvector"

say "VectorChord $VCHORD_VERSION"
if [ -e "$("$PG_BIN/pg_config" --pkglibdir)/vchord.so" ]; then
	echo "already installed"
else
	deb="postgresql-$PG_MAJOR-vchord_${VCHORD_VERSION}-1_$(dpkg --print-architecture).deb"
	curl -fL --retry 3 -o "/tmp/$deb" "https://github.com/tensorchord/VectorChord/releases/download/$VCHORD_VERSION/$deb" ||
		{ warn "could not download $deb (VCHORD_VERSION picks another release)"; exit 1; }
	sudo apt-get install -y -q "/tmp/$deb"
fi

say "pgvectorscale (diskann)"
if [ -e "$("$PG_BIN/pg_config" --sharedir)/extension/vectorscale.control" ]; then
	echo "already installed"
elif [ "${WITH_DISKANN:-0}" != 1 ]; then
	echo "skipped (WITH_DISKANN=1 to install it)"
else
	# Timescale's apt repository, set up by hand rather than through its
	# curl-to-bash script
	. /etc/os-release
	sudo install -d -m 0755 /etc/apt/keyrings
	if curl -fsSL https://packagecloud.io/timescale/timescaledb/gpgkey | sudo gpg --dearmor --yes -o /etc/apt/keyrings/timescaledb.gpg &&
	   echo "deb [signed-by=/etc/apt/keyrings/timescaledb.gpg] https://packagecloud.io/timescale/timescaledb/$ID/ $VERSION_CODENAME main" |
	     sudo tee /etc/apt/sources.list.d/timescaledb.list >/dev/null &&
	   sudo apt-get update -q && sudo apt-get install -y -q "pgvectorscale-postgresql-$PG_MAJOR"; then
		:
	else
		warn "pgvectorscale did not install; diskann cannot run"
		sudo rm -f /etc/apt/sources.list.d/timescaledb.list
	fi
fi

say "pg_lion from $REPO"
make -C "$REPO" -j"$(nproc)" PG_CONFIG="$PG_BIN/pg_config" >/dev/null
sudo make -C "$REPO" install PG_CONFIG="$PG_BIN/pg_config" >/dev/null
echo "installed pg_lion $(git -C "$REPO" rev-parse --short HEAD)"

say "Python"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install -q --upgrade pip
"$VENV/bin/pip" install -q numpy pyarrow 'psycopg[binary]'

say "page-cache drop for cold runs"
# a root-owned helper and a sudo rule for it alone, so a cold run (hours in)
# needs no password
sudo tee /usr/local/sbin/lionbench-drop-caches >/dev/null <<'SH'
#!/bin/sh
sync
echo 3 > /proc/sys/vm/drop_caches
SH
sudo chmod 0755 /usr/local/sbin/lionbench-drop-caches
echo "$(id -un) ALL=(root) NOPASSWD: /usr/local/sbin/lionbench-drop-caches" |
	sudo tee /etc/sudoers.d/lionbench >/dev/null
sudo chmod 0440 /etc/sudoers.d/lionbench
sudo visudo -cf /etc/sudoers.d/lionbench >/dev/null

say "cluster"
"$LOCAL/pg.sh" init

say "done: next, bench/vector/local/run.sh"
