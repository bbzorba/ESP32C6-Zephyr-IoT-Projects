#!/bin/sh
# Over-the-air update through mcumgr (UDP 1337), see
# applications/web_app/BOOTLOADER.md.
#   tools/ota.sh <smpmgr> <imgtool> <hub ip> <signed image> [bootloader]
# Application: upload to slot 1, mark for a trial boot, reset, then wait
# until the hub runs the new image and has confirmed it.
# Bootloader: upload the signed MCUboot to slot 1 (not marked), reset so the
# confirmed firmware installs it at 0x0, then reset once more to prove the
# hub still boots through it.
SMPMGR=$1
IMGTOOL=$2
IP=$3
IMAGE=$4
MODE=${5:-app}
# Long enough to cover the hub's Wi-Fi watchdog (reboot after 5 min offline)
WAIT_S=${OTA_WAIT_S:-360}

# smpmgr dumps every timed-out packet in hex; report failures ourselves
smp() { "$SMPMGR" --ip "$IP" --timeout 10 --loglevel CRITICAL "$@"; }

# Image slots, one "slot=N,...,confirmed=..,active=.." line each
slots() {
    smp image state-read 2>/dev/null | tr -d ' \n' | sed 's/ImageState(/\n/g' | grep '^slot='
}

# Slot 0 runs (optionally: the image with hash $1) and is confirmed
slot0_confirmed() {
    slots | grep '^slot=0,' | grep -q "${1:+hash=HashBytes('$1').*}confirmed=True,active=True"
}

slot1_free() { ! slots | grep -q '^slot=1,'; }

# Retries "$@" every 5 s until it succeeds or WAIT_S passes
wait_for() {
    end=$(( $(date +%s) + WAIT_S ))
    until "$@"; do
        [ "$(date +%s)" -ge "$end" ] && return 1
        sleep 5
    done
}

# UDP has no connection ("Connecting... OK" means nothing), so an echo is
# the only way to know the hub is listening
hub_up() { smp os echo ping >/dev/null 2>&1; }

fail() {
    for line in "$@"; do echo "ota: $line" >&2; done
    exit 1
}

wait_hub() {
    wait_for hub_up ||
        fail "no mcumgr answer from $IP (UDP 1337) for $WAIT_S s." \
             "is the hub on Wi-Fi? Check http://$IP/ or its serial log."
}

reset_hub() {
    smp os reset >/dev/null 2>&1
    sleep 10
    wait_hub
}

HASH=$(sh "$(dirname "$0")/image_hash.sh" "$IMGTOOL" "$IMAGE")
[ -n "$HASH" ] || fail "can't read the image hash of $IMAGE"

echo "ota: waiting for the hub at $IP"
wait_hub
if [ "$MODE" = bootloader ]; then
    slot0_confirmed ||
        fail "the running firmware isn't confirmed yet; only a confirmed one installs a bootloader"
fi

echo "ota: uploading $IMAGE"
smp image upload "$IMAGE" || fail "upload failed (hub stopped answering?)"

if [ "$MODE" = bootloader ]; then
    echo "ota: restarting the hub; it installs the bootloader once online"
    reset_hub
    # bl_update.c erases slot 1 only after the read-back of 0x0 matched
    wait_for slot1_free ||
        fail "the hub didn't install the bootloader within $WAIT_S s." \
             "see \"bl\" in http://$IP/api/status or the serial log."
    echo "ota: installed and verified, restarting to boot through it"
    reset_hub
    wait_for slot0_confirmed ||
        fail "the hub came back but its firmware isn't confirmed"
    echo "ota: done, the hub boots through the new bootloader"
    exit 0
fi

smp image state-write "$HASH" >/dev/null ||
    fail "the hub refused to mark the image for test (already running it?)"
smp os reset >/dev/null 2>&1

echo "ota: hub restarting, waiting until it runs and confirms ${HASH%"${HASH#????????}"}"
sleep 10
wait_for slot0_confirmed "$HASH" ||
    fail "the new image wasn't confirmed within $WAIT_S s;" \
         "MCUboot restores the previous one at the next reset."
echo "ota: done, the hub runs the new image and has confirmed it"
