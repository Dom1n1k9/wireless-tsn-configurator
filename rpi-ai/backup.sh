#!/usr/bin/env bash
# WTSN edge backup: hot sqlite copies of the DBs + key configs,
# stored under /home/wtsn/backups/<stamp>/, 14 days retention.
set -u
AI=/home/wtsn/wtsn-ai
DEST=/home/wtsn/backups
LOG="$AI/backup.log"
TS() { date +'%F %T'; }
say() { echo "[$(TS)] $*" | tee -a "$LOG"; }

STAMP=$(date +%Y%m%d-%H%M%S)
DIR="$DEST/$STAMP"
mkdir -p "$DIR/etc"

python3 - "$DIR" >>"$LOG" 2>&1 <<'EOF'
import os, sqlite3, sys
dest = sys.argv[1]
dbdir = "/home/wtsn/wtsn-configurator/build"
for name in ("wtsn_gui.db", "wtsn_sim.db"):
    src = os.path.join(dbdir, name)
    if not os.path.isfile(src):
        continue
    con = sqlite3.connect("file:%s?mode=ro" % src, uri=True)
    bak = sqlite3.connect(os.path.join(dest, name))
    with bak:
        con.backup(bak)
    con.close()
    bak.close()
    print("backed up", name)
EOF

copy() {  # copy <src> <dest> [chmod] -- log a warning instead of failing silently
    if [ -f "$1" ]; then
        if cp -f "$1" "$2"; then
            [ -n "${3:-}" ] && chmod "$3" "$2"
        else
            say "WARNING: could not copy $1"
        fi
    fi
}
# Runs as root (see wtsn-backup.service) so the root-owned credential files are
# included; the result dir is handed back to the wtsn user below.
copy /etc/wtsn/env "$DIR/etc/env" 600
copy /etc/mosquitto/conf.d/wtsn.conf "$DIR/etc/"
copy /etc/mosquitto/passwd "$DIR/etc/passwd" 600
copy /home/wtsn/wtsn-ai/config.json "$DIR/"
copy /home/wtsn/wtsn-ai/policy_state.json "$DIR/"

[ "$(id -u)" = 0 ] && chown -R wtsn:wtsn "$DIR"

find "$DEST" -mindepth 1 -maxdepth 1 -type d -mtime +14 -exec rm -rf {} + 2>/dev/null
say "backup done: $DIR"
