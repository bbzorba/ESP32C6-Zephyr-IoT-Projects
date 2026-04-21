$openocd = "C:\Users\bzorba.B1-ES\.espressif\tools\openocd-esp32\v0.12.0-esp32-20250422\openocd-esp32\bin\openocd.exe"
$cfg = "D:\baris\personal\personal_projects\ESP32\ESP32C6-Zephyr-IoT-Projects\external\zephyr\boards\espressif\esp32c6_devkitc\support\openocd.cfg"
$fixup = "D:\baris\personal\personal_projects\ESP32\ESP32C6-Zephyr-IoT-Projects\.vscode\openocd_fixup.tcl"
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

$gdb = "C:\Users\bzorba.B1-ES\.zephyr_ide\toolchains\zephyr-sdk-0.17.3\riscv64-zephyr-elf\bin\riscv64-zephyr-elf-gdb.exe"
$elf = "D:\baris\personal\personal_projects\ESP32\ESP32C6-Zephyr-IoT-Projects\applications\blink_LED\build\zephyr\zephyr.elf"

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
