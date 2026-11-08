# Shows what the board prints, saves it under logs\, and sends typed commands.
#
#   monitor.bat               detect the port, restart the board, watch
#   monitor.bat -Port COM5    use this port
#   monitor.bat -NoReset      watch without restarting the board

param(
    [string]$Port,
    [switch]$NoReset
)

. (Join-Path $PSScriptRoot 'common.ps1')

$Port = Select-Port $Port

$logDir = Join-Path $PackageRoot 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$logPath = Join-Path $logDir ('monitor-{0}.txt' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$log = New-Object System.IO.StreamWriter($logPath, $false, (New-Object System.Text.UTF8Encoding $false))
$log.AutoFlush = $true

$serial = Open-Port $Port 10
if (-not $serial) { Fail noPort }
Say monitorStart $Port $logPath
Say monitorHelp

# usbser.sys only passes an RTS change on together with a DTR write.
if (-not $NoReset) {
    $serial.RtsEnable = $true
    $serial.DtrEnable = $false
    Start-Sleep -Milliseconds 200
    $serial.RtsEnable = $false
    $serial.DtrEnable = $false
}

[Console]::TreatControlCAsInput = $true
$line = New-Object System.Text.StringBuilder
$quit = $false

function Test-Quit {
    while ([Console]::KeyAvailable) {
        $key = [Console]::ReadKey($true)
        if ($key.Key -eq 'C' -and ($key.Modifiers -band [ConsoleModifiers]::Control)) { return $true }
    }
    return $false
}

try {
    while (-not $quit) {
        try {
            $text = $serial.ReadExisting()
            if ($text) {
                [Console]::Write($text)
                $log.Write($text)
            }
        } catch {
            try { $serial.Close() } catch { }
            Write-Host ''
            Say monitorWait
            $serial = $null
            while (-not $serial -and -not $quit) {
                $serial = Open-Port $Port 2
                $quit = Test-Quit
            }
            if ($serial) { Say monitorBack }
            continue
        }

        $idle = $true
        while ([Console]::KeyAvailable) {
            $idle = $false
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'C' -and ($key.Modifiers -band [ConsoleModifiers]::Control)) {
                $quit = $true
                break
            }
            if ($key.Key -eq 'Enter') {
                [Console]::WriteLine()
                $command = $line.ToString()
                [void]$line.Clear()
                $log.WriteLine("> $command")
                try { $serial.Write("$command`r") } catch { }
            } elseif ($key.Key -eq 'Backspace') {
                if ($line.Length) {
                    [void]$line.Remove($line.Length - 1, 1)
                    [Console]::Write("`b `b")
                }
            } elseif ($key.KeyChar -and -not [char]::IsControl($key.KeyChar)) {
                [void]$line.Append($key.KeyChar)
                [Console]::Write($key.KeyChar)
            }
        }
        if ($idle) { Start-Sleep -Milliseconds 30 }
    }
} finally {
    [Console]::TreatControlCAsInput = $false
    if ($serial) { $serial.Close() }
    $log.Close()
    Write-Host ''
    Say monitorEnd $logPath
}
