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

// ── Dynamic path discovery (reads Makefile; no hardcoded user/workspace paths) ─
const wsRoot = path.resolve(__dirname, '..');

function _readMakeVar(varName) {
    const mk = fs.readFileSync(path.join(wsRoot, 'Makefile'), 'utf8');
    for (const line of mk.split('\n')) {
        if (line.trim().startsWith('#')) continue;
        const m = line.match(new RegExp(`^${varName}\\s*\\??=\\s*(.+)`));
        if (m) return m[1].trim();
    }
    return null;
}

function _latestSubdir(base, pattern) {
    if (!fs.existsSync(base)) return null;
    const match = fs.readdirSync(base)
        .filter(d => !pattern || pattern.test(d)).sort().pop();
    return match ? path.join(base, match) : null;
}

function _findBoardCfg(dir, boardBase) {
    if (!fs.existsSync(dir)) return null;
    for (const e of fs.readdirSync(dir)) {
        const full = path.join(dir, e);
        if (!fs.statSync(full).isDirectory()) continue;
        if (e === boardBase) {
            const cfg = path.join(full, 'support', 'openocd.cfg');
            if (fs.existsSync(cfg)) return cfg.replace(/\\/g, '/');
        }
        const r = _findBoardCfg(full, boardBase);
        if (r) return r;
    }
    return null;
}

function _boardToArch(b) {
    b = b.toLowerCase();
    if (/esp32c[2-9]|esp32h2/.test(b)) return 'riscv64-zephyr-elf';
    if (/esp32s2/.test(b)) return 'xtensa-espressif_esp32s2_zephyr-elf';
    if (/esp32s3/.test(b)) return 'xtensa-espressif_esp32s3_zephyr-elf';
    if (/esp32/.test(b))   return 'xtensa-espressif_esp32_zephyr-elf';
    return 'arm-zephyr-eabi';
}

const isWin = process.platform === 'win32';
const home  = os.homedir();

// OpenOCD: ~/.espressif/tools/openocd-esp32/<version>/openocd-esp32/bin/openocd[.exe]
const _ocdVer = _latestSubdir(path.join(home, '.espressif', 'tools', 'openocd-esp32'));
const ocd     = _ocdVer
    ? path.join(_ocdVer, 'openocd-esp32', 'bin', isWin ? 'openocd.exe' : 'openocd').replace(/\\/g, '/')
    : 'openocd';

// OpenOCD helpers: ~/.vscode/extensions/marus25.cortex-debug-*/support/openocd-helpers.tcl
const _cdExt  = _latestSubdir(path.join(home, '.vscode', 'extensions'), /^marus25\.cortex-debug/);
const helpers = _cdExt
    ? path.join(_cdExt, 'support', 'openocd-helpers.tcl').replace(/\\/g, '/')
    : null;

// Board openocd.cfg — resolved from BOARD in Makefile
const boardName = _readMakeVar('BOARD') || 'esp32c6_devkitc/esp32c6/hpcore';
const board     = _findBoardCfg(path.join(wsRoot, 'external', 'zephyr', 'boards'), boardName.split('/')[0]);

// GDB: ~/.zephyr_ide/toolchains/<sdk>/<arch>/bin/<arch>-gdb[.exe]
const _sdk = _latestSubdir(path.join(home, '.zephyr_ide', 'toolchains'), /^zephyr-sdk/);
const arch = _boardToArch(boardName);
const gdb  = _sdk
    ? path.join(_sdk, arch, 'bin', `${arch}-gdb${isWin ? '.exe' : ''}`).replace(/\\/g, '/')
    : null;

// ELF: <COMPILE_DIR>/build/zephyr/zephyr.elf — resolved from COMPILE_DIR in Makefile
const compileDir = _readMakeVar('COMPILE_DIR') || 'applications/blink_LED';
const elf        = path.join(wsRoot, compileDir, 'build', 'zephyr', 'zephyr.elf').replace(/\\/g, '/');

// openocd_fixup.tcl — workspace-relative, no hardcoded path
const fixup  = path.join(wsRoot, '.vscode', 'openocd_fixup.tcl').replace(/\\/g, '/');
const tmpgdb = path.join(os.tmpdir(), 'gdbcmds_final.txt');
// ─────────────────────────────────────────────────────────────────────────────


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
