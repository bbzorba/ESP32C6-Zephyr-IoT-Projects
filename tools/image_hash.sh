#!/bin/sh
# Prints the SHA256 hash of a signed MCUboot image (the value MCUboot and
# mcumgr use to identify it), e.g. for `smpmgr image state-write <hash>`.
#   tools/image_hash.sh <imgtool> <zephyr.signed.bin>
"$1" dumpinfo "$2" | awk '
    /type: SHA256 \(0x10\)/ { found = 1; next }
    found && /type:/        { exit }
    found && /0x/ && !/len:/ {
        line = $0
        sub(/.*data:/, "", line)
        gsub(/0x| /, "", line)
        hash = hash line
    }
    END { print toupper(hash) }'
