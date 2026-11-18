$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$wd = 'D:\Chronica\qnx\blackbox'
$serial = Join-Path $wd 'serial_bb.log'
$errlog = Join-Path $wd 'qemu-stderr.log'
Remove-Item $serial, $errlog -ErrorAction SilentlyContinue
$args = @('-cpu','max','-smp','1','-m','256','-kernel',(Join-Path $wd 'blackbox.ifs'),'-display','none','-serial',("file:" + $serial),'-monitor','none','-no-reboot')
$p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -RedirectStandardError $errlog -WindowStyle Hidden
$deadline = 300
$elapsed = 0
while ($elapsed -lt $deadline) {
    Start-Sleep -Seconds 5
    $elapsed += 5
    if ($p.HasExited) { break }
    if ((Test-Path $serial) -and (Select-String -Path $serial -Pattern 'BLACKBOX RUN END' -Quiet)) { Start-Sleep -Seconds 2; break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output 'QEMU: killed (timeout or after END marker)' } else { Write-Output ('QEMU: exited, code ' + $p.ExitCode) }
Write-Output '--- SERIAL LOG ---'
if (Test-Path $serial) { Get-Content $serial } else { Write-Output '(no serial log)' }
