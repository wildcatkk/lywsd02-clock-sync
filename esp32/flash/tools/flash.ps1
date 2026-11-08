# Writes the full firmware image to the board, then hands it the Wi-Fi network over
# the same USB cable (Improv Serial), so no phone is needed.
#
#   flash.bat                 detect the port, write, set up Wi-Fi
#   flash.bat -Port COM5      use this port
#   flash.bat -NoFlash        only set up Wi-Fi on a board that already runs it
#   flash.bat -NoWifi         only write

param(
    [string]$Port,
    [switch]$NoFlash,
    [switch]$NoWifi
)

. (Join-Path $PSScriptRoot 'common.ps1')

$CMD_WIFI = 1
$CMD_STATE = 2
$CMD_INFO = 3
$STATE_PROVISIONED = 4
$ERROR_CONNECT = 3

$Port = Select-Port $Port

# ---------------------------------------------------------------- write

if (-not $NoFlash) {
    Say detecting
    $probe = & $Esptool --port $Port chip-id 2>&1 | Out-String
    $found = [regex]::Match($probe, 'Detecting chip type\.\.\. (ESP32[-A-Z0-9]*)')
    if (-not $found.Success) {
        Write-Host $probe
        Fail noChip
    }
    $chipName = $found.Groups[1].Value
    $chip = $chipName.ToLower().Replace('-', '')
    Say chip $chipName

    $image = Get-ChildItem (Join-Path $PackageRoot 'firmware') -Filter "*-$chip-full.bin" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $image) { Fail noImage $chipName }

    # The full image starts at 0 and covers the settings area too, so this is
    # also a factory reset.
    Say flashing $image.Name
    & $Esptool --port $Port --chip $chip --baud 460800 write-flash 0x0 $image.FullName
    if ($LASTEXITCODE -ne 0) { Fail flashFailed }
    Say flashed
    Start-Sleep -Seconds 2
}

if ($NoWifi) {
    Say done
    exit 0
}

# ---------------------------------------------------------------- Wi-Fi

$serial = Open-Port $Port 30
if (-not $serial) { Fail portGone }

try {
    $state = $null
    $deadline = (Get-Date).AddSeconds(25)
    while (-not $state -and (Get-Date) -lt $deadline) {
        Send-ImprovRpc $serial $CMD_STATE
        $until = (Get-Date).AddMilliseconds(1500)
        while ($true) {
            $packet = Read-ImprovPacket $serial $until
            if (-not $packet) { break }
            if ($packet.Type -eq $Improv.State) { $state = $packet.Data[0] }
            if ($packet.Type -eq $Improv.Result -and $packet.Data[0] -eq $CMD_STATE) {
                $currentUrl = (Get-ImprovStrings $packet.Data)[0]
                break
            }
            if ($state -and $state -ne $STATE_PROVISIONED) { break }
        }
    }
    if (-not $state) { Fail noAnswer }

    Send-ImprovRpc $serial $CMD_INFO
    $hostname = ''
    $until = (Get-Date).AddSeconds(3)
    while ($packet = Read-ImprovPacket $serial $until) {
        if ($packet.Type -eq $Improv.Result -and $packet.Data[0] -eq $CMD_INFO) {
            $info = Get-ImprovStrings $packet.Data
            $hostname = $info[3]
            Say device $info[0] $info[1] $hostname
            break
        }
    }

    if ($state -eq $STATE_PROVISIONED) {
        Say online $currentUrl
        if ((Read-Host (Text askChange)) -notmatch '^[yY]') {
            Say done
            exit 0
        }
    }

    # wifi.txt next to flash.bat: "ssid=..." and "password=...", exactly as typed.
    $ssid = $null
    $password = ''
    $fromFile = $false
    $wifiTxt = Join-Path $PackageRoot 'wifi.txt'
    if (Test-Path $wifiTxt) {
        foreach ($line in [System.IO.File]::ReadAllLines($wifiTxt, [System.Text.Encoding]::UTF8)) {
            if ($line -match '^\s*#' -or $line -notmatch '=') { continue }
            $key, $value = $line -split '=', 2
            switch ($key.Trim().ToLower()) {
                'ssid' { $ssid = $value }
                'password' { $password = $value }
            }
        }
        if ($ssid) {
            $fromFile = $true
            Say fromFile
        } else {
            Say badFile
        }
    }
    if (-not $ssid) {
        $ssid = Read-Host (Text askSsid)
        if (-not $ssid) {
            Say skipWifi $hostname
            exit 0
        }
        $secure = Read-Host (Text askPassword) -AsSecureString
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }

    $ssidBytes = [System.Text.Encoding]::UTF8.GetBytes($ssid)
    $passBytes = [System.Text.Encoding]::UTF8.GetBytes($password)
    $password = $null
    if ($ssidBytes.Length -gt 32) { Fail badSsid }
    if (($passBytes.Length -gt 0 -and $passBytes.Length -lt 8) -or $passBytes.Length -gt 64) { Fail badPassword }

    $payload = [byte[]](@([byte]$ssidBytes.Length) + $ssidBytes + @([byte]$passBytes.Length) + $passBytes)
    Say joining
    Send-ImprovRpc $serial $CMD_WIFI $payload
    [Array]::Clear($payload, 0, $payload.Length)
    [Array]::Clear($passBytes, 0, $passBytes.Length)

    $url = $null
    $failed = $false
    $until = (Get-Date).AddSeconds(60)
    while (-not $url -and -not $failed) {
        $packet = Read-ImprovPacket $serial $until
        if (-not $packet) { $failed = $true; break }
        if ($packet.Type -eq $Improv.Error -and $packet.Data[0] -eq $ERROR_CONNECT) { $failed = $true }
        if ($packet.Type -eq $Improv.Result -and $packet.Data[0] -eq $CMD_WIFI) {
            $url = (Get-ImprovStrings $packet.Data)[0]
        }
    }
    if ($failed) { Fail joinFailed }

    Say joined
    Write-Host "  $url" -ForegroundColor Green
    if ($hostname) { Write-Host "  http://$hostname.local:8787/" -ForegroundColor Green }
    if ($fromFile) { Say wifiTxtNote }
    Start-Process $url
    Say done
} finally {
    $serial.Close()
}
