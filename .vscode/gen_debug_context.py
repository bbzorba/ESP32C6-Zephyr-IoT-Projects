#!/usr/bin/env python3
"""
gen_debug_context.py — Auto-generates Zephyr debug context from the Makefile.

Reads COMPILE_DIR and BOARD from the active (uncommented) lines in the
workspace Makefile, then resolves:
  - The board's openocd.cfg path inside external/zephyr/boards/
  - The Zephyr SDK version installed at ~/.zephyr_ide/toolchains/
  - The GDB toolchain prefix for the board's CPU architecture
  - Any SVD file present in .vscode/ that matches the board

Writes the results as  "zephyr.*"  keys into .vscode/settings.json so that
launch.json can reference them via ${config:zephyr.*} variable substitution.

Run automatically via the  "gen-debug-context"  VS Code preLaunchTask.
"""

import platform
import re
import shutil
import sys
from pathlib import Path

# ── Markers for the auto-generated block in settings.json ────────────────────
_BLOCK_START = '    // ─── Zephyr debug context (auto-generated — do not edit manually) ─────'
_BLOCK_END   = '    // ─────────────────────────────────────────────────────────────────────────'


# ── Board → GDB architecture mapping ─────────────────────────────────────────
def _board_to_gdb_arch(board: str) -> str:
    b = board.lower()
    # Strip soc qualifier for matching (e.g. "nrf52840dk/nrf52840" → "nrf52840dk")
    b_base = b.split('/')[0]
    # ESP32 RISC-V variants — Zephyr SDK ships riscv64-zephyr-elf for these
    if any(x in b for x in ['esp32c3', 'esp32h2', 'esp32c6', 'esp32c2']):
        return 'riscv64-zephyr-elf'
    if 'esp32s2' in b:
        return 'xtensa-espressif_esp32s2_zephyr-elf'
    if 'esp32s3' in b:
        return 'xtensa-espressif_esp32s3_zephyr-elf'
    if 'esp32' in b:
        return 'xtensa-espressif_esp32_zephyr-elf'
    if any(x in b_base for x in ['hifive', 'riscv', 'rv32', 'rv64', 'vexriscv', 'litex']):
        return 'riscv64-zephyr-elf'
    # Default: ARM Cortex-M (STM32, nRF, LPC, SAM, i.MX, …)
    return 'arm-zephyr-eabi'


# ── Helpers ───────────────────────────────────────────────────────────────────
def _active_makefile_var(makefile_text: str, var: str):
    """Return the value of the first non-commented  VAR ?= value  line."""
    for line in makefile_text.splitlines():
        stripped = line.strip()
        if stripped.startswith('#'):
            continue
        m = re.match(rf'^{re.escape(var)}\s*\??\s*=\s*(.+)', stripped)
        if m:
            return m.group(1).strip()
    return None


def _find_openocd_cfg(ws: Path, board: str):
    """
    Scan external/zephyr/boards/ for an openocd.cfg whose directory path
    contains the board name.  Returns the path relative to the boards dir
    using forward slashes, e.g.  "espressif/esp32c6_devkitc/support/openocd.cfg".

    Handles Zephyr 3.7+ board/soc naming (e.g. "esp32c6_devkitc/esp32c6/hpcore")
    by trying the first segment as the board directory name.
    """
    boards_dir = ws / 'external' / 'zephyr' / 'boards'
    # For "board/soc/core" naming, use just the board part for directory matching
    board_dir_name = board.split('/')[0]
    for cfg in sorted(boards_dir.rglob('openocd.cfg')):
        if board_dir_name in cfg.parts:
            return str(cfg.relative_to(boards_dir)).replace('\\', '/')
    return None


def _find_sdk(ws: Path):
    """
    Return the name of the latest Zephyr SDK directory found under
    ~/.zephyr_ide/toolchains/, e.g.  "zephyr-sdk-0.17.3".
    Returns None if not found.
    """
    sdk_base = Path.home() / '.zephyr_ide' / 'toolchains'
    if not sdk_base.exists():
        return None
    sdks = sorted(sdk_base.glob('zephyr-sdk-*'))
    return sdks[-1].name if sdks else None


def _find_gdb(arch: str) -> str:
    """
    Locate the GDB binary for the given toolchain arch across all installed
    Zephyr SDK versions, newest first.  Supports both layouts:
      - SDK 0.x:  <sdk>/<arch>/bin/<arch>-gdb[.exe]
      - SDK 1.x+: <sdk>/gnu/<arch>/bin/<arch>-gdb[.exe]
    Returns the full forward-slash path, or empty string if not found.
    """
    exe = f'{arch}-gdb.exe' if platform.system() == 'Windows' else f'{arch}-gdb'
    sdk_base = Path.home() / '.zephyr_ide' / 'toolchains'
    if not sdk_base.exists():
        return ''
    # Sort newest-first so the latest working SDK wins
    for sdk in sorted(sdk_base.glob('zephyr-sdk-*'), reverse=True):
        for candidate in [
            sdk / arch / 'bin' / exe,           # 0.x layout
            sdk / 'gnu' / arch / 'bin' / exe,   # 1.x layout
        ]:
            if candidate.exists():
                return str(candidate).replace('\\', '/')
    return ''


def _find_openocd(ws: Path) -> str:
    """
    Auto-discover the Espressif OpenOCD binary from ~/.espressif/tools/openocd-esp32/.
    Tries the latest installed version; falls back to bare 'openocd' (assumes PATH).
    """
    exe = 'openocd.exe' if platform.system() == 'Windows' else 'openocd'
    base = Path.home() / '.espressif' / 'tools' / 'openocd-esp32'
    if base.exists():
        candidates = sorted(base.glob(f'*/openocd-esp32/bin/{exe}'))
        if candidates:
            return str(candidates[-1]).replace('\\', '/')
    return 'openocd'


def _find_svd(ws: Path, board: str):
    """
    Look for a .svd file in .vscode/ that best matches the board name.
    Returns a workspace-relative path like  ".vscode/ESP32C6.svd",
    or an empty string if nothing found.
    """
    svd_files = list((ws / '.vscode').glob('*.svd'))
    if not svd_files:
        return ''
    if len(svd_files) == 1:
        return f'.vscode/{svd_files[0].name}'
    # Try to find the best match by board name
    board_key = re.sub(r'[^a-z0-9]', '', board.lower())
    for svd in svd_files:
        stem_key = re.sub(r'[^a-z0-9]', '', svd.stem.lower())
        if board_key in stem_key or stem_key in board_key:
            return f'.vscode/{svd.name}'
    return f'.vscode/{svd_files[0].name}'  # fallback: first one


def _copy_elf(ws: Path, compile_dir: str) -> bool:
    """
    Copy the built ELF to .vscode/zephyr.elf so launch.json can always
    reference a fixed path regardless of the active project.
    Returns True on success.
    """
    src = ws / compile_dir / 'build' / 'zephyr' / 'zephyr.elf'
    dst = ws / '.vscode' / 'zephyr.elf'
    if src.exists():
        shutil.copy2(src, dst)
        return True
    return False


def _write_board_cfg(ws: Path, openocd_board_path: str):
    """
    Write .vscode/board_cfg.tcl that sources the correct board openocd.cfg.
    launch.json always loads this fixed file; only its content changes per board.
    Uses an absolute forward-slash path so it works regardless of OpenOCD cwd.
    """
    abs_cfg = ws / 'external' / 'zephyr' / 'boards' / Path(openocd_board_path)
    abs_cfg_str = str(abs_cfg).replace('\\', '/')
    content = (
        '# Auto-generated by gen_debug_context.py — do not edit manually\n'
        f'source "{abs_cfg_str}"\n'
    )
    (ws / '.vscode' / 'board_cfg.tcl').write_text(content, encoding='utf-8')


def _update_clangd(settings_path: Path, query_driver: str):
    """
    Ensure clangd.arguments in settings.json has the correct --query-driver
    pointing to the local toolchain install.  Creates the block if absent.
    The --compile-commands-dir always points to .vscode/ (populated by this
    script via the compile_commands.json copy step).
    """
    clangd_block = (
        '    "clangd.arguments": [\n'
        '        "--compile-commands-dir=${workspaceFolder}/.vscode",\n'
        '        "--background-index",\n'
        '        "--completion-style=detailed",\n'
        '        "--header-insertion=never",\n'
        f'        "--query-driver={query_driver}"\n'
        '    ]'
    )

    content = settings_path.read_text(encoding='utf-8')

    if '"clangd.arguments"' in content:
        content = re.sub(
            r'"clangd\.arguments"\s*:\s*\[[^\]]*\]',
            clangd_block.lstrip(),
            content,
            flags=re.DOTALL,
        )
    else:
        last_brace = content.rfind('}')
        before = content[:last_brace].rstrip()
        if before and not before.endswith(','):
            before += ','
        content = before + '\n\n' + clangd_block + '\n}\n'

    settings_path.write_text(content, encoding='utf-8')


def _update_settings(settings_path: Path, values: dict):
    """
    Update the auto-generated  zephyr.*  block in settings.json.
    Creates the block before the closing  }  on first run; replaces it on
    subsequent runs.  All other content (including user comments) is preserved.

    IMPORTANT: Only content strictly between _BLOCK_START and _BLOCK_END is
    replaced.  Any user-defined keys must be placed OUTSIDE these markers.
    """
    content = settings_path.read_text(encoding='utf-8')

    lines = [f'    "{k}": "{v}"' for k, v in values.items()]
    block_body = ',\n'.join(lines) + ','
    new_block = f'{_BLOCK_START}\n{block_body}\n{_BLOCK_END}'

    if _BLOCK_START in content and _BLOCK_END in content:
        # Replace only what is strictly between the two markers (inclusive)
        start_idx = content.index(_BLOCK_START)
        end_idx   = content.index(_BLOCK_END) + len(_BLOCK_END)
        content   = content[:start_idx] + new_block + content[end_idx:]
    elif _BLOCK_START in content:
        # Malformed: start marker without end — replace to end of object
        start_idx = content.index(_BLOCK_START)
        last_brace = content.rfind('}')
        after = content[last_brace:]          # keep the closing brace + anything after
        content = content[:start_idx] + new_block + '\n' + after
    else:
        # First run: insert before the final closing brace
        last_brace = content.rfind('}')
        before = content[:last_brace].rstrip()
        if before and not before.endswith(','):
            before += ','
        content = before + '\n\n' + new_block + '\n' + content[last_brace:]

    settings_path.write_text(content, encoding='utf-8')


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    ws = Path(__file__).parent.parent  # .vscode/ -> workspace root

    # 1. Parse Makefile
    makefile_path = ws / 'Makefile'
    if not makefile_path.exists():
        print(f'ERROR: Makefile not found at {makefile_path}', file=sys.stderr)
        sys.exit(1)
    makefile = makefile_path.read_text(encoding='utf-8')

    compile_dir = _active_makefile_var(makefile, 'COMPILE_DIR')
    board       = _active_makefile_var(makefile, 'BOARD') or 'esp32c6_devkitc/esp32c6/hpcore'

    if not compile_dir:
        print(
            'ERROR: No active COMPILE_DIR found in Makefile.\n'
            '       Uncomment exactly one  COMPILE_DIR ?= ...  line.',
            file=sys.stderr,
        )
        sys.exit(1)

    print(f'[gen_debug_context]')
    print(f'  COMPILE_DIR  : {compile_dir}')
    print(f'  BOARD        : {board}')

    # 2. Find board's openocd.cfg
    openocd_board_path = _find_openocd_cfg(ws, board)
    if openocd_board_path:
        print(f'  OpenOCD cfg  : external/zephyr/boards/{openocd_board_path}')
    else:
        print(
            f'  WARNING: No openocd.cfg found for board "{board}" under external/zephyr/boards/.\n'
            f'           Set "zephyr.openocdBoardPath" manually in settings.json.',
        )
        openocd_board_path = f'(openocd.cfg not found for {board})'

    # 3. Find SDK version (informational only)
    sdk_version = _find_sdk(ws)
    if sdk_version:
        print(f'  SDK version  : {sdk_version}')
    else:
        sdk_version = 'zephyr-sdk-0.17.3'  # reasonable fallback
        print(
            f'  WARNING: No Zephyr SDK found at ~/.zephyr_ide/toolchains/.\n'
            f'           Falling back to "{sdk_version}". Install the Zephyr IDE extension.',
        )

    # 4. GDB architecture prefix and resolved binary path
    gdb_arch = _board_to_gdb_arch(board)
    print(f'  GDB arch     : {gdb_arch}')
    gdb_path = _find_gdb(gdb_arch)
    if gdb_path:
        print(f'  GDB binary   : {gdb_path}')
    else:
        print(
            f'  WARNING: GDB binary not found for arch "{gdb_arch}".\n'
            f'           Check that the Zephyr SDK is installed at ~/.zephyr_ide/toolchains/.'
        )

    # 5. SVD file
    svd_file = _find_svd(ws, board)
    if svd_file:
        print(f'  SVD file     : {svd_file}')
    else:
        print(f'  SVD file     : (none found — peripheral viewer unavailable)')

    # 6. OpenOCD binary path
    openocd_path = _find_openocd(ws)
    if openocd_path == 'openocd':
        print(
            f'  WARNING: OpenOCD not found at ~/.espressif/tools/openocd-esp32/.\n'
            f'           Falling back to bare "openocd" (must be on PATH).'
        )
    else:
        print(f'  OpenOCD path : {openocd_path}')

    # 7. Update settings.json
    settings_path = ws / '.vscode' / 'settings.json'
    _update_settings(settings_path, {
        'zephyr.compileDir':       compile_dir,
        'zephyr.board':            board,
        'zephyr.openocdBoardPath': openocd_board_path,
        'zephyr.sdkVersion':       sdk_version,
        'zephyr.gdbArch':          gdb_arch,
        'zephyr.gdbPath':          gdb_path,
        'zephyr.svdFile':          svd_file,
        'cortex-debug.openocdPath': openocd_path,
        'C_Cpp.default.compileCommands':
            f'${{workspaceFolder}}/{compile_dir}/build/compile_commands.json',
    })
    print('  settings.json updated  ✓')

    # 8. Copy ELF to .vscode/zephyr.elf (fixed path used by launch.json)
    if _copy_elf(ws, compile_dir):
        print('  zephyr.elf copied  ✓')
    else:
        print('  WARNING: zephyr.elf not found — run build first')

    # 9. Write .vscode/board_cfg.tcl (fixed path used by launch.json)
    if openocd_board_path and not openocd_board_path.startswith('('):
        _write_board_cfg(ws, openocd_board_path)
        print('  board_cfg.tcl written  ✓')
    else:
        print('  WARNING: board_cfg.tcl not written — board openocd.cfg not found')

    # 10. Copy compile_commands.json to .vscode/ so clangd always finds it
    src_cc = ws / compile_dir / 'build' / 'compile_commands.json'
    dst_cc = ws / '.vscode' / 'compile_commands.json'
    if src_cc.exists():
        shutil.copy2(src_cc, dst_cc)
        print('  compile_commands.json copied  ✓')
    else:
        print('  compile_commands.json: not found (run build first)')

    # 11. Update clangd.arguments query-driver with the resolved toolchain path
    sdk_base = str(Path.home() / '.zephyr_ide' / 'toolchains').replace('\\', '/')
    query_driver = f'{sdk_base}/**/*'
    _update_clangd(settings_path, query_driver)
    print(f'  clangd query-driver  : {query_driver}')


if __name__ == '__main__':
    main()
