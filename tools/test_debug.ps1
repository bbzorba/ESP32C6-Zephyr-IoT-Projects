# ── Dynamic path discovery (reads Makefile; no hardcoded user/workspace paths) ─
$wsRoot  = (Resolve-Path "$PSScriptRoot\..").Path
$userHome = $HOME

function Get-MakeVar([string]$VarName) {
    $mk = Get-Content "$wsRoot\Makefile"
    foreach ($line in $mk) {
        if ($line.TrimStart().StartsWith('#')) { continue }
        if ($line -match "^$VarName\s*\??=\s*(.+)") { return $Matches[1].Trim() }
    }
    return $null
}

function Get-LatestSubdir([string]$Base, [string]$Pattern = '') {
    if (-not (Test-Path $Base)) { return $null }
    $dirs = Get-ChildItem $Base -Directory | Where-Object { $Pattern -eq '' -or $_.Name -match $Pattern } | Sort-Object Name
    if ($dirs) { return ($dirs | Select-Object -Last 1).FullName }
    return $null
}

function Find-BoardCfg([string]$Dir, [string]$BoardBase) {
    if (-not (Test-Path $Dir)) { return $null }
    foreach ($item in Get-ChildItem $Dir -Directory) {
        if ($item.Name -eq $BoardBase) {
            $cfgPath = Join-Path $item.FullName "support\openocd.cfg"
            if (Test-Path $cfgPath) { return $cfgPath }
        }
        $result = Find-BoardCfg $item.FullName $BoardBase
        if ($result) { return $result }
    }
    return $null
}

function Get-BoardArch([string]$Board) {
    $b = $Board.ToLower()
    if ($b -match 'esp32c[2-9]|esp32h2') { return 'riscv64-zephyr-elf' }
    if ($b -match 'esp32s2') { return 'xtensa-espressif_esp32s2_zephyr-elf' }
    if ($b -match 'esp32s3') { return 'xtensa-espressif_esp32s3_zephyr-elf' }
    if ($b -match 'esp32')   { return 'xtensa-espressif_esp32_zephyr-elf' }
    return 'arm-zephyr-eabi'
}

# OpenOCD: $HOME/.espressif/tools/openocd-esp32/<version>/openocd-esp32/bin/openocd.exe
$ocdBase = Join-Path $userHome '.espressif\tools\openocd-esp32'
$ocdVer  = Get-LatestSubdir $ocdBase
$openocd = if ($ocdVer) { Join-Path $ocdVer 'openocd-esp32\bin\openocd.exe' } else { 'openocd' }

# Board openocd.cfg — resolved from BOARD in Makefile
$boardName = Get-MakeVar 'BOARD'
if (-not $boardName) { $boardName = 'esp32c6_devkitc/esp32c6/hpcore' }
$boardBase = $boardName.Split('/')[0]
$cfg = Find-BoardCfg "$wsRoot\external\zephyr\boards" $boardBase

# GDB: $HOME/.zephyr_ide/toolchains/<sdk>/<arch>/bin/<arch>-gdb.exe
$tcBase = Join-Path $userHome '.zephyr_ide\toolchains'
$sdk    = Get-LatestSubdir $tcBase 'zephyr-sdk'
$arch   = Get-BoardArch $boardName
$gdb    = if ($sdk) { Join-Path $sdk "$arch\bin\$arch-gdb.exe" } else { $null }

# ELF: <COMPILE_DIR>/build/zephyr/zephyr.elf — resolved from COMPILE_DIR in Makefile
$compileDir = Get-MakeVar 'COMPILE_DIR'
if (-not $compileDir) { $compileDir = 'applications/blink_LED' }
$elf = Join-Path $wsRoot ($compileDir.Replace('/', '\') + '\build\zephyr\zephyr.elf')

# openocd_fixup.tcl — workspace-relative
$fixup = Join-Path $wsRoot '.vscode\openocd_fixup.tcl'
# ─────────────────────────────────────────────────────────────────────────────

$logfile = "$env:TEMP\openocd_gdb_test.log"
Remove-Item $logfile -ErrorAction SilentlyContinue

Write-Host "Starting OpenOCD..."
$proc = Start-Process -FilePath $openocd -ArgumentList @("-f", $cfg, "-f", $fixup) -RedirectStandardError $logfile -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

if ($proc.HasExited) {
    Write-Host "ERROR: OpenOCD exited immediately with code $($proc.ExitCode)"
    Get-Content $logfile
    exit 1
}
Write-Host "OpenOCD running (PID $($proc.Id)), connecting GDB..."

$gdbScript = @"
set pagination off
target remote :3333
info registers pc
quit
"@
$gdbScript | Out-File -Encoding ascii "$env:TEMP\gdb_test_cmds.txt"

Write-Host "=== GDB output ==="
& $gdb --batch -x "$env:TEMP\gdb_test_cmds.txt" $elf 2>&1

Write-Host "`n=== OpenOCD log (last 20 lines) ==="
Start-Sleep -Seconds 1
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Get-Content $logfile | Select-Object -Last 20
