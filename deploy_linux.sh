#!/usr/bin/env bash
# Deploy real TSN on a Linux host (PC as master / RPi as edge slave).
# Requires root, linuxptp (ptp4l/phc2sys) and an interface with mpqn / tc.
#
#   sudo ./deploy_linux.sh --iface eth0 --role master
#   sudo ./deploy_linux.sh --iface eth0 --role slave --cycle-ns 4000000
#
# Options:
#   --iface <eth>       network interface (default eth0)
#   --role <master|slave>  gPTP role (default master)
#   --cycle-ns <ns>     TAS cycle time (default 4000000)
#   --qbv on|off        apply taprio (default on)
#   --vlan <id>         create VLAN device (optional)

IFACE="eth0"
ROLE="master"
CYCLE_NS=4000000
QBV="on"
VLAN=""

log() { echo -e "\033[1;36m[wtsn-linux]\033[0m $*"; }

usage() { grep '^#' "$0" | sed 's/^# //'; exit 0; }
[ $# -eq 0 ] && usage

while [ $# -gt 0 ]; do
    case "$1" in
        --iface) shift; IFACE="$1";;
        --role) shift; ROLE="$1";;
        --cycle-ns) shift; CYCLE_NS="$1";;
        --qbv) shift; QBV="$1";;
        --vlan) shift; VLAN="$1";;
        --help|-h) usage;;
        *) log "unknown: $1"; usage;;
    esac
    shift
done

[ "$(id -u)" = "0" ] || { log "run as root: sudo $0 ..."; exit 1; }
command -v tc >/dev/null || { log "missing 'tc' (iproute2)"; exit 1; }

# ---- 1) gPTP (802.1AS) ----
if command -v ptp4l >/dev/null; then
    pkill -f "ptp4l -i $IFACE" 2>/dev/null
    if [ "$ROLE" = "master" ]; then
        log "gPTP: $IFACE as grandmaster"
        ptp4l -i "$IFACE" -m -f /etc/linuxptp/gptp_master.cfg &
        # TODO: real config /etc/linuxptp/gptp_master.cfg may not exist
    else
        log "gPTP: $IFACE as slave"
        ptp4l -i "$IFACE" -m -s -f /etc/linuxptp/gptp_slave.cfg &
    fi
else
    log "linuxptp not installed - skipping gPTP (apt install linuxptp)"
fi

# ---- 2) TAS / Qbv (802.1Qbv) via taprio ----
if [ "$QBV" = "on" ]; then
    HALF=$((CYCLE_NS/2)); Q1=$((CYCLE_NS/4)); Q3=$((CYCLE_NS/4))
    log "TAS: taprio on $IFACE cycle ${CYCLE_NS}ns"
    tc qdisc replace dev "$IFACE" root handle 100 taprio \
        num_tc 4 map 0 1 2 3 4 5 6 7 \
        queues 1@0 1@1 1@2 1@3 \
        base-time 0 clockid CLOCK_TAI \
        sched-entry S 0x01 "$HALF" \
        sched-entry S 0x03 "$Q1"  \
        sched-entry S 0x04 "$Q3"  2>/dev/null \
        && log "taprio OK" || log "taprio failed (need kernel + mac >= Intel I210); try --qbv off"
fi

# ---- 3) QoS (802.1Q) ----
log "QoS: prio qdisc on $IFACE"
tc qdisc replace dev "$IFACE" root handle 1: prio  2>/dev/null

# ---- 4) VLAN (802.1Q) ----
if [ -n "$VLAN" ]; then
    log "VLAN $VLAN on $IFACE"
    ip link add link "$IFACE" name "vlan${VLAN}" type vlan id "$VLAN" 2>/dev/null || true
    ip link set "vlan${VLAN}" up
fi

log "Deploy done (real): iface=$IFACE role=$ROLE gPTP=$ROLE TAS-cycle=${CYCLE_NS}ns"
