#!/bin/sh
set -eu

BINDIR="/usr/local/bin"
UNITDIR="/usr/lib/systemd/system"
SYSUSERSDIR="/usr/lib/sysusers.d"
TMPFILESDIR="/usr/lib/tmpfiles.d"
CONFDIR="/etc/chromatindb"
DATADIR="/var/lib/chromatindb"
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    echo "Usage: install.sh [--uninstall] <chromatindb>" >&2
    exit 1
}

die() {
    echo "error: $1" >&2
    exit 1
}

do_uninstall() {
    if [ "$(id -u)" -ne 0 ]; then
        die "must be run as root"
    fi

    systemctl stop chromatindb.service 2>/dev/null || true
    systemctl disable chromatindb.service 2>/dev/null || true

    rm -f "$UNITDIR/chromatindb.service"
    rm -f "$SYSUSERSDIR/chromatindb.conf"
    rm -f "$TMPFILESDIR/chromatindb.conf"
    rm -f "$BINDIR/chromatindb"

    systemctl daemon-reload
}

do_install() {
    if [ "$(id -u)" -ne 0 ]; then
        die "must be run as root"
    fi

    NODE_BIN="$1"

    if [ ! -f "$NODE_BIN" ] || [ ! -x "$NODE_BIN" ]; then
        die "$NODE_BIN is not an executable file"
    fi

    # Install binaries
    install -m 0755 "$NODE_BIN" "$BINDIR/chromatindb"

    # Install sysusers.d and create user
    install -m 0644 "$SCRIPTDIR/sysusers.d/chromatindb.conf" "$SYSUSERSDIR/chromatindb.conf"
    systemd-sysusers

    # Install tmpfiles.d and create directories
    install -m 0644 "$SCRIPTDIR/tmpfiles.d/chromatindb.conf" "$TMPFILESDIR/chromatindb.conf"
    systemd-tmpfiles --create chromatindb.conf

    # Install configs (preserve existing)
    if [ ! -f "$CONFDIR/node.json" ]; then
        install -m 0644 "$SCRIPTDIR/config/node.json" "$CONFDIR/node.json"
    fi
    # Install systemd units
    install -m 0644 "$SCRIPTDIR/systemd/chromatindb.service" "$UNITDIR/chromatindb.service"
    systemctl daemon-reload

    # Generate identity keys if missing
    if [ ! -f "$DATADIR/node.key" ]; then
        "$BINDIR/chromatindb" keygen --data-dir "$DATADIR"
        chown chromatindb:chromatindb "$DATADIR/node.key" "$DATADIR/node.pub"
    fi
}

case "${1:-}" in
    --uninstall)
        do_uninstall
        ;;
    "")
        usage
        ;;
    *)
        if [ $# -ne 1 ]; then
            usage
        fi
        do_install "$1"
        ;;
esac
