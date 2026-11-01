#!/bin/sh
# Over-the-air update of an MCUboot application through mcumgr (UDP 1337).
#   tools/ota.sh <smpmgr> <imgtool> <hub ip> <zephyr.signed.bin>
# Waits until the hub answers, uploads the image to slot 1, marks it for a
# trial boot, resets, then waits until the hub runs the image and has
# confirmed it (see applications/web_app/BOOTLOADER.md).
SMPMGR=$1
IMGTOOL=$2
IP=$3
IMAGE=$4
# Long enough to cover the hub's Wi-Fi watchdog (reboot after 5 min offline)
WAIT_S=${OTA_WAIT_S:-360}

# smpmgr dumps every timed-out packet in hex; report failures ourselves
smp() { "$SMPMGR" --ip "$IP" --timeout 10 --loglevel CRITICAL "$@"; }

# UDP has no connection ("Connecting... OK" means nothing), so an echo is
# the only way to know the hub is listening
wait_hub() {
    end=$(( $(date +%s) + WAIT_S ))
    until smp os echo ping >/dev/null 2>&1; do
        if [ "$(date +%s)" -ge "$end" ]; then
            echo "ota: no mcumgr answer from $IP (UDP 1337) for $WAIT_S s." >&2
            echo "ota: is the hub on Wi-Fi? Check http://$IP/ or its serial log." >&2
            exit 1
        fi
        sleep 5
    done
}

# Slot 0 runs our image and it is confirmed
running_confirmed() {
    smp image state-read 2>/dev/null | tr -d ' \n' | sed 's/ImageState(/\n/g' |
        grep '^slot=0,' | grep -q "hash=HashBytes('$HASH').*confirmed=True,active=True"
}

HASH=$(sh "$(dirname "$0")/image_hash.sh" "$IMGTOOL" "$IMAGE")
if [ -z "$HASH" ]; then
    echo "ota: can't read the image hash of $IMAGE" >&2
    exit 1
fi

echo "ota: waiting for the hub at $IP"
wait_hub

echo "ota: uploading $IMAGE"
if ! smp image upload "$IMAGE"; then
    echo "ota: upload failed (hub stopped answering?)" >&2
    exit 1
fi
if ! smp image state-write "$HASH" >/dev/null; then
    echo "ota: the hub refused to mark the image for test (already running it?)" >&2
    exit 1
fi
smp os reset >/dev/null 2>&1

echo "ota: hub restarting, waiting until it runs and confirms ${HASH%"${HASH#????????}"}"
sleep 10
end=$(( $(date +%s) + WAIT_S ))
until running_confirmed; do
    if [ "$(date +%s)" -ge "$end" ]; then
        echo "ota: the new image wasn't confirmed within $WAIT_S s;" >&2
        echo "ota: MCUboot restores the previous one at the next reset." >&2
        exit 1
    fi
    sleep 5
done
echo "ota: done, the hub runs the new image and has confirmed it"
