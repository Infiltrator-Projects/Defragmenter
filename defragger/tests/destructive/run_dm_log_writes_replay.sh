#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  sudo tests/destructive/run_dm_log_writes_replay.sh \
    --data-device /dev/LOOP_OR_SCRATCH_DATA \
    --log-device /dev/LOOP_OR_SCRATCH_LOG \
    --image prepared-filesystem.img \
    --worker build/linux-defragger-fat-worker \
    --operation defrag \
    --check-command 'build/linux-defragger-fat-worker analyze {device}' \
    --confirm-data /dev/LOOP_OR_SCRATCH_DATA \
    --confirm-log /dev/LOOP_OR_SCRATCH_LOG \
    [--check-mode flush|fua] [-- extra worker arguments...]

Both block devices are DESTROYED. The image is copied through dm-log-writes,
marked as the baseline, the requested Defragmenter operation is executed, then
replay-log reconstructs the baseline and checks every selected durability
boundary. {device} in --check-command is replaced by the replay data device.

This harness deliberately does not model persistence of Defragmenter's external
recovery-journal filesystem. Existing transaction/fault-injection tests cover
that coupling; this script adds block-layer ordering evidence for the source
filesystem itself.
EOF
    exit 2
}

DATA=
LOG=
IMAGE=
WORKER=
OPERATION=
CHECK_COMMAND=
CHECK_MODE=flush
CONFIRM_DATA=
CONFIRM_LOG=
EXTRA=()

while (($#)); do
    case "$1" in
        --data-device) DATA=${2:-}; shift 2 ;;
        --log-device) LOG=${2:-}; shift 2 ;;
        --image) IMAGE=${2:-}; shift 2 ;;
        --worker) WORKER=${2:-}; shift 2 ;;
        --operation) OPERATION=${2:-}; shift 2 ;;
        --check-command) CHECK_COMMAND=${2:-}; shift 2 ;;
        --check-mode) CHECK_MODE=${2:-}; shift 2 ;;
        --confirm-data) CONFIRM_DATA=${2:-}; shift 2 ;;
        --confirm-log) CONFIRM_LOG=${2:-}; shift 2 ;;
        --) shift; EXTRA=("$@"); break ;;
        *) usage ;;
    esac
done

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
[[ -n $DATA && -n $LOG && -n $IMAGE && -n $WORKER && -n $OPERATION && -n $CHECK_COMMAND ]] || usage
[[ $CHECK_MODE == flush || $CHECK_MODE == fua ]] || usage
[[ $CONFIRM_DATA == "$DATA" && $CONFIRM_LOG == "$LOG" ]] || {
    echo "destructive confirmation must exactly match both device paths" >&2
    exit 1
}
[[ $DATA != "$LOG" ]] || { echo "data and log devices must differ" >&2; exit 1; }
[[ -b $DATA && -b $LOG ]] || { echo "data/log targets must be block devices" >&2; exit 1; }
[[ -f $IMAGE ]] || { echo "baseline image not found: $IMAGE" >&2; exit 1; }
[[ -x $WORKER ]] || { echo "worker is not executable: $WORKER" >&2; exit 1; }

for command in dmsetup replay-log blockdev dd findmnt stat sync; do
    command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }
done

if findmnt -rn -S "$DATA" >/dev/null 2>&1 || findmnt -rn -S "$LOG" >/dev/null 2>&1; then
    echo "refusing mounted data/log device" >&2
    exit 1
fi

image_bytes=$(stat -c '%s' "$IMAGE")
data_bytes=$(blockdev --getsize64 "$DATA")
log_bytes=$(blockdev --getsize64 "$LOG")
(( image_bytes > 0 && image_bytes <= data_bytes )) || {
    echo "baseline image does not fit data device" >&2
    exit 1
}
(( log_bytes >= image_bytes * 3 )) || {
    echo "log device must be at least three times the fixture image size" >&2
    exit 1
}

modprobe dm-log-writes 2>/dev/null || modprobe dm_log_writes 2>/dev/null || true
dmsetup targets | grep -q '^log-writes' || {
    echo "kernel does not expose the dm-log-writes target" >&2
    exit 1
}

name="defragger-log-writes-$$"
mapper="/dev/mapper/$name"
journal=$(mktemp "${TMPDIR:-/tmp}/defragger-crash-replay.XXXXXX.journal")
checker=$(mktemp "${TMPDIR:-/tmp}/defragger-crash-check.XXXXXX")
created=0

cleanup() {
    set +e
    if (( created )); then dmsetup remove "$name" >/dev/null 2>&1; fi
    rm -f "$journal" "$checker"
}
trap cleanup EXIT INT TERM

if command -v blkdiscard >/dev/null && blkdiscard -f "$LOG" 2>/dev/null; then
    :
else
    dd if=/dev/zero of="$LOG" bs=1M count=16 conv=fsync status=none
fi

sectors=$(blockdev --getsz "$DATA")
dmsetup create "$name" --table "0 $sectors log-writes $DATA $LOG"
created=1

dd if="$IMAGE" of="$mapper" bs=4M conv=fsync,notrunc status=none
dmsetup message "$name" 0 mark baseline

"$WORKER" "$OPERATION" "$mapper" \
    --write --confirm "$mapper" --journal "$journal" "${EXTRA[@]}"
sync
dmsetup message "$name" 0 mark operation-complete
dmsetup remove "$name"
created=0

replay-log --log "$LOG" --replay "$DATA" --end-mark baseline

resolved_check=${CHECK_COMMAND//\{device\}/$DATA}
printf '%s\n' '#!/bin/sh' "exec sh -c $(printf '%q' "$resolved_check")" >"$checker"
chmod 700 "$checker"

replay-log \
    --log "$LOG" \
    --replay "$DATA" \
    --start-mark baseline \
    --end-mark operation-complete \
    --fsck "$checker" \
    --check "$CHECK_MODE"

echo "dm-log-writes replay completed successfully at every $CHECK_MODE boundary"
