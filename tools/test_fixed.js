// Simulate the fixed cortex-debug flow:
// overrideLaunchCommands:[] + no runToEntryPoint
// gdb-attach: halt 1000 (from esp_common.cfg, no reset)
// Then just check PC and backtrace

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
const tmpgdb = path.join(os.tmpdir(), 'gdbcmds_fixed.txt');

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
let ocdOut = '';
ocdProc.stderr.on('data', d => { ocdOut += d.toString(); process.stdout.write('[OCD] ' + d); });
ocdProc.on('close', code => { console.log('[OCD EXIT]', code); });
ocdProc.on('error', err => { console.log('[OCD ERROR]', err.message); });

setTimeout(() => {
    // Simulate cortex-debug fixed flow:
    // 1. target extended-remote (triggers gdb-attach -> halt 1000)
    // 2. overrideLaunchCommands:[] -> nothing
    // 3. No runToEntryPoint -> just inspect state
    fs.writeFileSync(tmpgdb, [
        'set pagination off',
        'target extended-remote :50000',
        'info registers pc',
        'backtrace 5',
        'quit'
    ].join('\n') + '\n');

    console.log('[TEST] Connecting GDB...');
    const gdbProc = spawn(gdb, ['--batch', '-x', tmpgdb, elf], { stdio: 'pipe' });
    let gdbOut = '';
    gdbProc.stdout.on('data', d => { gdbOut += d; process.stdout.write('[GDB] ' + d); });
    gdbProc.stderr.on('data', d => { gdbOut += d; process.stdout.write('[GDB ERR] ' + d); });
    gdbProc.on('close', code => {
        console.log('[GDB EXIT]', code);
        if (code === 0) {
            console.log('\n=== SUCCESS: GDB connected and chip was inspected ===');
        } else {
            console.log('\n=== FAILED: GDB exited with error ===');
        }
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    });

    setTimeout(() => {
        console.log('[TIMEOUT] GDB did not complete in 10s. Killing.');
        try { gdbProc.kill(); } catch(e) {}
        setTimeout(() => { try { ocdProc.kill(); } catch(e) {} }, 500);
    }, 10000);
}, 3000);
