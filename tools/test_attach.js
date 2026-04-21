const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ocd = 'C:/Users/bzorba.B1-ES/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250422/openocd-esp32/bin/openocd.exe';
const helpers = 'C:/Users/bzorba.B1-ES/.vscode/extensions/marus25.cortex-debug-1.12.1/support/openocd-helpers.tcl';
const board = 'D:/baris/personal/personal_projects/ESP32/ESP32C6-Zephyr-IoT-Projects/external/zephyr/boards/espressif/esp32c6_devkitc/support/openocd.cfg';
const elf   = 'D:/baris/personal/personal_projects/ESP32/ESP32C6-Zephyr-IoT-Projects/applications/blink_LED/build/zephyr/zephyr.elf';
const gdb   = 'C:/Users/bzorba.B1-ES/.zephyr_ide/toolchains/zephyr-sdk-0.17.3/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb.exe';

const tmpfixup = path.join(os.tmpdir(), 'fixup_test.tcl');
const tmpgdb   = path.join(os.tmpdir(), 'gdbcmds_attach.txt');

// Override gdb-attach to just halt (no reset). Set appimage_offset to match flash.text LMA.
fs.writeFileSync(tmpfixup, `
esp appimage_offset 0x10000
foreach t [target names] {
    $t configure -event gdb-attach {
        halt
        gdb breakpoint_override hard
    }
    $t configure -event gdb-detach {
        echo "Detaching: resuming"
        resume
    }
}
`);

const ocdArgs = [
    '-c', 'gdb_port 50000',
    '-c', 'tcl_port 50001',
    '-c', 'telnet_port 50002',
    '-f', helpers,
    '-f', board,
    '-f', tmpfixup
];

const ocdProc = spawn(ocd, ocdArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
let ocdOut = '';
ocdProc.stderr.on('data', d => { ocdOut += d.toString(); process.stdout.write('[OCD] ' + d); });
ocdProc.on('close', code => { console.log('[OCD EXIT]', code); });

setTimeout(() => {
    // GDB commands: connect, check PC, set tbreak at main, continue, wait
    fs.writeFileSync(tmpgdb, [
        'set pagination off',
        'target extended-remote :50000',
        'info registers pc',
        'set $blink_count = 0',
        'tbreak main',
        'continue',
        'info registers pc',
        'quit'
    ].join('\n') + '\n');

    const gdbProc = spawn(gdb, ['--batch', '-x', tmpgdb, elf], { stdio: 'pipe' });
    let gdbOut = '';
    gdbProc.stdout.on('data', d => { gdbOut += d; process.stdout.write('[GDB] ' + d); });
    gdbProc.stderr.on('data', d => { gdbOut += d; process.stdout.write('[GDB ERR] ' + d); });
    gdbProc.on('close', code => {
        console.log('[GDB EXIT]', code);
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    });

    // Timeout if GDB hangs (main never reached)
    setTimeout(() => {
        console.log('[TIMEOUT] GDB did not complete. Killing both.');
        try { gdbProc.kill(); } catch(e) {}
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    }, 15000);
}, 3000);
