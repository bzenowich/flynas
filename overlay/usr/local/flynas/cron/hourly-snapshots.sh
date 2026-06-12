#!/bin/sh
# hourly-snapshots.sh — run hourly as root from /etc/crontab:
#   0 * * * * root /bin/sh /usr/local/flynas/cron/hourly-snapshots.sh
#
# For every mounted FlyNAS volume (when the snapshot schedule is
# enabled): create an hourly snapshot, promote the first run of the
# day/week/year to daily/weekly/yearly, then prune:
#   hourly  - keep 24h
#   daily   - keep 7 days
#   weekly  - keep 52 weeks
#   yearly  - keep forever
# Snapshot PFSs and the snapshots DB table are kept in lockstep.

DB=/usr/local/flynas/flynas.db

sql() {
    /usr/local/bin/sqlite3 -cmd '.timeout 5000' "$DB" "$1"
}

ENABLED=$(sql "SELECT json_extract(value, '\$.enabled')
    FROM config WHERE key = 'snapshot_schedule';")
[ "$ENABLED" = "0" ] && exit 0   # default on when key absent

STAMP=$(date +%Y%m%d%H%M%S)

snap() {  # snap <vol_id> <volname> <retention> <letter>
    NAME="snap-$4-$STAMP"
    hammer2 snapshot "/data/$2" "$NAME" >/dev/null 2>&1 || return 1
    sql "INSERT INTO snapshots (volume_id, name, retention)
        VALUES ($1, '$NAME', '$3');"
}

prune() {  # prune <vol_id> <volname> <retention> <keep-sql-interval>
    sql "SELECT name FROM snapshots
        WHERE volume_id = $1 AND retention = '$3'
          AND created_at < datetime('now', '$4');" |
    while read -r NAME; do
        hammer2 -s "/data/$2" pfs-delete "$NAME" >/dev/null 2>&1
        sql "DELETE FROM snapshots WHERE volume_id = $1 AND name = '$NAME';"
    done
}

sql "SELECT id, name FROM volumes;" | tr '|' ' ' |
while read -r VID VNAME; do
    [ -n "$VNAME" ] || continue
    # skip unmounted volumes
    df "/data/$VNAME" 2>/dev/null | grep -q "/data/$VNAME$" || continue

    snap "$VID" "$VNAME" hourly h

    HAVE_DAILY=$(sql "SELECT COUNT(*) FROM snapshots
        WHERE volume_id = $VID AND retention = 'daily'
          AND date(created_at) = date('now');")
    [ "$HAVE_DAILY" = "0" ] && snap "$VID" "$VNAME" daily d

    HAVE_WEEKLY=$(sql "SELECT COUNT(*) FROM snapshots
        WHERE volume_id = $VID AND retention = 'weekly'
          AND strftime('%Y%W', created_at) = strftime('%Y%W', 'now');")
    [ "$HAVE_WEEKLY" = "0" ] && snap "$VID" "$VNAME" weekly w

    HAVE_YEARLY=$(sql "SELECT COUNT(*) FROM snapshots
        WHERE volume_id = $VID AND retention = 'yearly'
          AND strftime('%Y', created_at) = strftime('%Y', 'now');")
    [ "$HAVE_YEARLY" = "0" ] && snap "$VID" "$VNAME" yearly y

    prune "$VID" "$VNAME" hourly '-24 hours'
    prune "$VID" "$VNAME" daily '-7 days'
    prune "$VID" "$VNAME" weekly '-52 weeks'
done

exit 0
