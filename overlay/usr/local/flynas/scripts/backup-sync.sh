#!/bin/sh
# backup-sync.sh — encrypted offsite backup, run as root (detached
# by `flynas-helper backupsync`).
#
# Reads a job spec written by the API from RUN_DIR/backup-job.json,
# key=value lines (one volume= line per mounted volume):
#   bucket_id=1
#   endpoint=s3.example.com or /local/dir
#   bucket=mybucket
#   access_key=... / secret_key=... / password=...
#   volume=tank
#
# Per volume: cryfs-batch incremental backup of the live mountpoint
# into a local block store -> rclone sync of the blocks to the S3
# target. An endpoint starting with "/" is treated as a local
# directory (rclone local backend) so the flow can be tested offline.
#
# NOTE: the backup reads the live filesystem, not a snapshot.
# Mounting a snapshot PFS of a multi-volume HAMMER2 panics the
# 6.4 kernel (hammer2_base_delete during flush, seen 2026-06-12),
# so transient backup snapshots are disabled until that is fixed.
#
# Status for GET /api/backup/status is kept in RUN_DIR/backup-status.json.

FLYNAS=/usr/local/flynas
RUN_DIR=$FLYNAS/run
JOB=$RUN_DIR/backup-job.json
STATUS=$RUN_DIR/backup-status.json
BACKUP_DIR=$FLYNAS/backup
CRYFS_BATCH=$FLYNAS/bin/cryfs-batch
RCLONE=/usr/local/bin/rclone
LOCK=$RUN_DIR/backup.lock

# write_status <state> <phase> <volume> <detail>
write_status() {
    _detail=$(printf '%s' "$4" | tr -d '"\\' | tr '\n' ' ' | cut -c1-300)
    printf '{"state":"%s","phase":"%s","volume":"%s","detail":"%s","updated_at":"%s"}\n' \
        "$1" "$2" "$3" "$_detail" "$(date -u '+%Y-%m-%d %H:%M:%S')" > "$STATUS.tmp"
    chown www:flynas "$STATUS.tmp" 2>/dev/null
    mv "$STATUS.tmp" "$STATUS"
}

# jget <key> — value for key from the job file (rest of line)
jget() {
    sed -n "s/^$1=//p" "$JOB" | head -1
}

fail() {
    write_status error "$1" "$2" "$3"
    rm -f "$PWFILE" "$JOB"
    rmdir "$LOCK" 2>/dev/null
    exit 1
}

[ -f "$JOB" ] || exit 1
mkdir "$LOCK" 2>/dev/null || exit 1   # already running

mkdir -p "$RUN_DIR" "$BACKUP_DIR"

BUCKET_ID=$(jget bucket_id)
ENDPOINT=$(jget endpoint)
BUCKET=$(jget bucket)
ACCESS_KEY=$(jget access_key)
SECRET_KEY=$(jget secret_key)

PWFILE=$RUN_DIR/backup-pw.$$
umask 077
jget password > "$PWFILE"
[ -s "$PWFILE" ] || fail init "" "no cryfs password in job"

VOLS=$(sed -n 's/^volume=//p' "$JOB")
[ -n "$VOLS" ] || fail init "" "no volumes in job"

rm -f "$JOB"

echo "$VOLS" | while read -r VOL; do
    [ -n "$VOL" ] || continue

    SRC=/data/$VOL
    [ -d "$SRC" ] || fail source "$VOL" "volume not mounted"

    BLOCKS=$BACKUP_DIR/blocks/$BUCKET_ID/$VOL
    CFGDIR=$BACKUP_DIR/cfg/$BUCKET_ID
    CFG=$CFGDIR/$VOL.json
    mkdir -p "$BLOCKS" "$CFGDIR"

    if [ ! -f "$CFG" ]; then
        write_status running init "$VOL" ""
        $CRYFS_BATCH init --remote "$BLOCKS" --config "$CFG" \
            --password-file "$PWFILE" >/dev/null 2>&1 \
            || fail init "$VOL" "cryfs-batch init failed"
    fi

    write_status running encrypt "$VOL" ""
    OUT=$($CRYFS_BATCH backup --source "$SRC" --remote "$BLOCKS" \
        --config "$CFG" --password-file "$PWFILE" --quiet 2>&1) \
        || fail encrypt "$VOL" "cryfs-batch: $OUT"

    write_status running upload "$VOL" ""
    case "$ENDPOINT" in
    /*)
        # Local-directory endpoint (offline testing)
        mkdir -p "$ENDPOINT/$BUCKET/$VOL"
        OUT=$($RCLONE sync "$BLOCKS" "$ENDPOINT/$BUCKET/$VOL" 2>&1) \
            || fail upload "$VOL" "rclone: $OUT"
        ;;
    *)
        OUT=$($RCLONE sync "$BLOCKS" \
            ":s3,provider=Other,endpoint='$ENDPOINT',access_key_id='$ACCESS_KEY',secret_access_key='$SECRET_KEY':$BUCKET/$VOL" 2>&1) \
            || fail upload "$VOL" "rclone: $OUT"
        ;;
    esac
done || exit 1

# Restore browsing runs as www directly against the block store
chown -R www:flynas "$BACKUP_DIR"
chmod -R g+rX "$BACKUP_DIR"

rm -f "$PWFILE"
write_status done done "" ""
rmdir "$LOCK" 2>/dev/null
exit 0
