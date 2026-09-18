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

[ -f /etc/wtsn/env ] && cp -f /etc/wtsn/env "$DIR/etc/env" && chmod 600 "$DIR/etc/env"
[ -f /etc/mosquitto/conf.d/wtsn.conf ] && cp -f /etc/mosquitto/conf.d/wtsn.conf "$DIR/etc/"
[ -f /etc/mosquitto/passwd ] && cp -f /etc/mosquitto/passwd "$DIR/etc/" && chmod 600 "$DIR/etc/passwd"
[ -f /home/wtsn/wtsn-ai/config.json ] && cp -f /home/wtsn/wtsn-ai/config.json "$DIR/"
[ -f /home/wtsn/wtsn-ai/policy_state.json ] && cp -f /home/wtsn/wtsn-ai/policy_state.json "$DIR/"

find "$DEST" -mindepth 1 -maxdepth 1 -type d -mtime +14 -exec rm -rf {} + 2>/dev/null
say "backup done: $DIR"
