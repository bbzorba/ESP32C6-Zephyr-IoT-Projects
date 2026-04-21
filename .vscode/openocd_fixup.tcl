# openocd_fixup.tcl — ESP32-C6 OpenOCD fixup
# =========================================================================
# PURPOSE
#
#   The Espressif esp_common.cfg already registers a gdb-attach event that:
#     - calls 'reset halt'
#     - calls 'gdb breakpoint_override hard'
#
#   This file does NOT override those events (doing so caused double
#   reset-halt which left the CPU in ROM at 0x40000000 unable to reach
#   application code).
#
#   Instead, this file only ensures gdb-detach resumes the target cleanly
#   so the firmware keeps running after the debug session ends.
#
# ESP32-C6 HARDWARE BREAKPOINT LIMIT: 4
#   cortex-debug is configured with "hardwareBreakpoints": {"limit": 4}
#   in launch.json to prevent GDB from inserting more than 4 breakpoints.
#
# Note: 'esp appimage_offset' cannot be called at init time (requires target
#   halted). The "Application image is invalid!" warning from OpenOCD is
#   benign — we do not flash via GDB (loadFiles:[]) and hardware breakpoints
#   do not require flash address mapping.
# =========================================================================

foreach t [target names] {
    $t configure -event gdb-detach {
        echo "Debugger detached: resuming target"
        resume
    }
}
