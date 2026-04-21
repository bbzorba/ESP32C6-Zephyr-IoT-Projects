// Simulate the exact cortex-debug F5 sequence with the fixed launch.json:
// 1. target extended-remote  -> gdb-attach fires (halt 1000)
// 2. overrideLaunchCommands:
//    a. monitor reset halt   -> reset to ROM, halted
//    b. monitor esp appimage_offset 0x0  -> configure flash bank
// 3. runToEntryPoint:"main"  -> tbreak main + continue
//
// This tests whether hardware breakpoints fire at main() after a proper reset.

const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ocd = 'C:/Users/bzorba.B1-ES/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250422/openocd-esp32/bin/openocd.exe';
const helpers = 'C:/Users/bzorba.B1-ES/.vscode/extensions/marus25.cortex-debug-1.12.1/support/openocd-helpers.tcl';
const board = 'D:/baris/personal/personal_projects/ESP32/ESP32C6-Zephyr-IoT-Projects/external/zephyr/boards/espressif/esp32c6_devkitc/support/openocd.cfg';
const fixup = 'D:/baris/personal/personal_projects/ESP32/ESP32C6-Zephyr-IoT-Projects/.vscode/openocd_fixup.tcl';
const elf   = 'D:/baris/personal/personal_projects/ESP32/ESP32C6-Zephyr-IoT-Projects/applications/blink_LED/build/zephyr/zephyr.elf';
const gdb   = 'C:/Users/bzorba.B1-ES/.zephyr_ide/toolchains/zephyr-sdk-0.17.3/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb.exe';
const tmpgdb = path.join(os.tmpdir(), 'gdbcmds_final.txt');

const ocdArgs = [
    '-c', 'gdb_port 50000',
    '-c', 'tcl_port 50001',
    '-c', 'telnet_port 50002',
    '-f', helpers,
    '-f', board,
    '-f', fixup
];

console.log('[TEST] Starting OpenOCD...');
const ocdProc = spawn(ocd, ocdArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
let ocdStarted = false;
ocdProc.stderr.on('data', d => {
    const s = d.toString();
    process.stdout.write('[OCD] ' + s);
    if (s.includes('Listening on port 50000')) ocdStarted = true;
});
ocdProc.on('close', code => { console.log('[OCD EXIT]', code); });
ocdProc.on('error', err => { console.log('[OCD ERROR]', err.message); });

setTimeout(() => {
    // Exact cortex-debug sequence:
    // 1. target extended-remote  (init)
    // 2. monitor reset halt       (overrideLaunchCommands[0])
    // 3. monitor esp appimage_offset 0x0  (overrideLaunchCommands[1])
    // 4. tbreak main              (runToEntryPoint: "main")
    // 5. continue
    // 6. Check where we halted
    fs.writeFileSync(tmpgdb, [
        'set pagination off',
        'set remotetimeout 30',
        'target extended-remote :50000',
        'monitor reset halt',
        'monitor esp appimage_offset 0x0',
        'tbreak main',
        'continue',
        'info registers pc',
        'where 3',
        'quit'
    ].join('\n') + '\n');

    console.log('[TEST] Connecting GDB...');
    const gdbProc = spawn(gdb, ['--batch', '-x', tmpgdb, elf], { stdio: 'pipe' });
    let gdbOut = '';
    gdbProc.stdout.on('data', d => { gdbOut += d; process.stdout.write('[GDB] ' + d); });
    gdbProc.stderr.on('data', d => { gdbOut += d; process.stdout.write('[GDB ERR] ' + d); });
    gdbProc.on('close', code => {
        console.log('[GDB EXIT]', code);
        if (code === 0 && (gdbOut.includes('main') || gdbOut.includes('42000002'))) {
            console.log('\n=== SUCCESS: Stopped at main() ===');
        } else if (code === 0) {
            console.log('\n=== PARTIAL: GDB completed but may not have reached main() ===');
        } else {
            console.log('\n=== FAILED ===');
        }
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    });

    setTimeout(() => {
        console.log('[TIMEOUT] main() not reached in 20s. Killing.');
        try { gdbProc.kill(); } catch(e) {}
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    }, 20000);
}, 3000);
