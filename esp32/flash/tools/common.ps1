# Shared by flash.ps1 and monitor.ps1: messages, finding the board's serial port,
# and the Improv Serial packets the firmware understands.

$ErrorActionPreference = 'Continue'
$PackageRoot = Split-Path -Parent $PSScriptRoot
$Esptool = Join-Path $PSScriptRoot 'esptool.exe'

$Lang = switch -Regex ((Get-UICulture).Name) {
    '^zh-(TW|HK|MO)|Hant' { 'zh-TW'; break }
    '^zh' { 'zh-CN'; break }
    default { 'en' }
}

$Messages = @{
    'zh-CN' = @{
        noPort        = '没找到 ESP32 的串口。请用能传数据的 USB 线插好板子；还是没有就按住 BOOT 键再插上。'
        pickPort      = '找到多个串口：'
        pickPrompt    = '输入要用的序号'
        usingPort     = '使用串口 {0}（{1}）'
        detecting     = '正在识别芯片…'
        chip          = '芯片：{0}'
        noChip        = '识别不了芯片。按住板子上的 BOOT 键重新插上 USB，松开后再运行一次。'
        noImage       = '固件包里没有 {0} 用的完整镜像。'
        flashing      = '正在写入 {0}，大约 1 分钟，期间不要拔线…'
        flashFailed   = '写入失败。按住板子上的 BOOT 键重新插上 USB，松开后再运行一次。'
        flashed       = '写入完成，等设备启动…'
        portGone      = '设备重启后串口一直没回来。拔下再插上 USB，然后运行 flash.bat -NoFlash 继续配网。'
        noAnswer      = '设备没有回应串口配网。可以用手机连设备开出的热点来配网。'
        device        = '设备：{0} {1}，主机名 {2}'
        online        = '设备已经连上网络：{0}'
        askChange     = '要改连别的 WiFi 吗？[y/N]'
        fromFile      = '使用 wifi.txt 里的 WiFi 设置。'
        badFile       = 'wifi.txt 里没有填 ssid=，改为手动输入。'
        askSsid       = 'WiFi 名称（直接回车跳过，之后用手机配网）'
        askPassword   = 'WiFi 密码（开放网络直接回车）'
        badSsid       = 'WiFi 名称不能超过 32 字节。'
        badPassword   = '密码要 8 到 64 位，开放网络留空。'
        joining       = '正在连接 WiFi，最多等 1 分钟…'
        joined        = '已连上网络。在同一个网络里的电脑或手机上打开：'
        joinFailed    = '没能连上这个 WiFi。请检查名称和密码，并确认是 2.4 GHz 网络；改好后运行 flash.bat -NoFlash 再试。'
        skipWifi      = '跳过 WiFi 设置。设备会开出热点 {0}（最多等 3 分钟），手机连上后按提示配网。'
        wifiTxtNote   = '提醒：wifi.txt 里存着 WiFi 密码，用完可以删掉。'
        done          = '完成。'
        monitorStart  = '串口 {0}，日志同时保存到 {1}'
        monitorHelp   = '可以输入命令后回车，例如 info、heap、selftest、reboot。按 Ctrl+C 退出。'
        monitorWait   = '[串口断开，等设备回来…]'
        monitorBack   = '[串口已重新连上]'
        monitorEnd    = '已退出，日志在 {0}'
    }
    'zh-TW' = @{
        noPort        = '找不到 ESP32 的序列埠。請用能傳資料的 USB 線插好板子；還是沒有就按住 BOOT 鍵再插上。'
        pickPort      = '找到多個序列埠：'
        pickPrompt    = '輸入要用的序號'
        usingPort     = '使用序列埠 {0}（{1}）'
        detecting     = '正在辨識晶片…'
        chip          = '晶片：{0}'
        noChip        = '辨識不了晶片。按住板子上的 BOOT 鍵重新插上 USB，放開後再執行一次。'
        noImage       = '韌體包裡沒有 {0} 用的完整映像檔。'
        flashing      = '正在寫入 {0}，大約 1 分鐘，期間不要拔線…'
        flashFailed   = '寫入失敗。按住板子上的 BOOT 鍵重新插上 USB，放開後再執行一次。'
        flashed       = '寫入完成，等裝置啟動…'
        portGone      = '裝置重新啟動後序列埠一直沒回來。拔下再插上 USB，然後執行 flash.bat -NoFlash 繼續設定網路。'
        noAnswer      = '裝置沒有回應序列埠設定。可以用手機連裝置開出的熱點來設定網路。'
        device        = '裝置：{0} {1}，主機名稱 {2}'
        online        = '裝置已經連上網路：{0}'
        askChange     = '要改連別的 Wi-Fi 嗎？[y/N]'
        fromFile      = '使用 wifi.txt 裡的 Wi-Fi 設定。'
        badFile       = 'wifi.txt 裡沒有填 ssid=，改為手動輸入。'
        askSsid       = 'Wi-Fi 名稱（直接按 Enter 跳過，之後用手機設定）'
        askPassword   = 'Wi-Fi 密碼（開放網路直接按 Enter）'
        badSsid       = 'Wi-Fi 名稱不能超過 32 位元組。'
        badPassword   = '密碼要 8 到 64 位，開放網路留空。'
        joining       = '正在連接 Wi-Fi，最多等 1 分鐘…'
        joined        = '已連上網路。在同一個網路裡的電腦或手機上開啟：'
        joinFailed    = '沒能連上這個 Wi-Fi。請檢查名稱和密碼，並確認是 2.4 GHz 網路；改好後執行 flash.bat -NoFlash 再試。'
        skipWifi      = '跳過 Wi-Fi 設定。裝置會開出熱點 {0}（最多等 3 分鐘），手機連上後依提示設定。'
        wifiTxtNote   = '提醒：wifi.txt 裡存著 Wi-Fi 密碼，用完可以刪掉。'
        done          = '完成。'
        monitorStart  = '序列埠 {0}，記錄同時存到 {1}'
        monitorHelp   = '可以輸入指令後按 Enter，例如 info、heap、selftest、reboot。按 Ctrl+C 離開。'
        monitorWait   = '[序列埠中斷，等裝置回來…]'
        monitorBack   = '[序列埠已重新連上]'
        monitorEnd    = '已離開，記錄在 {0}'
    }
    'en' = @{
        noPort        = 'No ESP32 serial port found. Plug the board in with a USB cable that carries data; if it still does not show up, hold BOOT while plugging it in.'
        pickPort      = 'Several serial ports found:'
        pickPrompt    = 'Number of the port to use'
        usingPort     = 'Using {0} ({1})'
        detecting     = 'Detecting the chip...'
        chip          = 'Chip: {0}'
        noChip        = 'Could not detect the chip. Hold BOOT on the board, plug USB in again, release it and run this again.'
        noImage       = 'This package has no full image for {0}.'
        flashing      = 'Writing {0}, about a minute. Do not unplug...'
        flashFailed   = 'Writing failed. Hold BOOT on the board, plug USB in again, release it and run this again.'
        flashed       = 'Written. Waiting for the device to start...'
        portGone      = 'The serial port did not come back after the restart. Replug USB, then run flash.bat -NoFlash to set up Wi-Fi.'
        noAnswer      = 'The device did not answer serial setup. Use a phone on the hotspot it opens instead.'
        device        = 'Device: {0} {1}, hostname {2}'
        online        = 'The device is already online: {0}'
        askChange     = 'Switch it to another Wi-Fi network? [y/N]'
        fromFile      = 'Using the Wi-Fi settings in wifi.txt.'
        badFile       = 'wifi.txt has no ssid= line filled in; asking instead.'
        askSsid       = 'Wi-Fi name (press Enter to skip and use a phone later)'
        askPassword   = 'Wi-Fi password (press Enter for an open network)'
        badSsid       = 'The Wi-Fi name can be at most 32 bytes.'
        badPassword   = 'The password needs 8 to 64 characters; leave it empty for an open network.'
        joining       = 'Joining Wi-Fi, up to a minute...'
        joined        = 'Online. Open this on a computer or phone in the same network:'
        joinFailed    = 'Could not join that Wi-Fi. Check the name and password and that it is a 2.4 GHz network, then run flash.bat -NoFlash again.'
        skipWifi      = 'Skipping Wi-Fi. The device opens a hotspot named {0} (within 3 minutes); join it with a phone and follow the page.'
        wifiTxtNote   = 'Note: wifi.txt holds your Wi-Fi password; delete it when you are done.'
        done          = 'Done.'
        monitorStart  = 'Port {0}, also saving to {1}'
        monitorHelp   = 'Type a command and press Enter, e.g. info, heap, selftest, reboot. Ctrl+C quits.'
        monitorWait   = '[Port lost, waiting for the device...]'
        monitorBack   = '[Port back]'
        monitorEnd    = 'Stopped. The log is in {0}'
    }
}

function Text([string]$key) {
    $template = $Messages[$Lang][$key]
    if ($args.Count) { return $template -f $args }
    return $template
}

function Say([string]$key) {
    Write-Host (Text $key @args)
}

function Fail([string]$key) {
    Write-Host (Text $key @args) -ForegroundColor Red
    exit 1
}

# Espressif USB, WCH CH340, Silicon Labs CP210x and FTDI cover the boards people buy.
function Get-SerialPorts {
    Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object {
        [pscustomobject]@{
            Port  = [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value
            Name  = $_.Name
            Board = $_.PNPDeviceID -match 'VID_(303A|1A86|10C4|0403)'
        }
    } | Sort-Object Port
}

function Select-Port([string]$Wanted) {
    $ports = @(Get-SerialPorts)
    if ($Wanted) {
        $match = $ports | Where-Object { $_.Port -eq $Wanted } | Select-Object -First 1
        $name = if ($match) { $match.Name } else { $Wanted }
        Say usingPort $Wanted $name
        return $Wanted
    }
    $boards = @($ports | Where-Object { $_.Board })
    if ($boards.Count -eq 1) {
        Say usingPort $boards[0].Port $boards[0].Name
        return $boards[0].Port
    }
    $choices = if ($boards.Count) { $boards } else { $ports }
    if (-not $choices.Count) { Fail noPort }
    Say pickPort
    for ($i = 0; $i -lt $choices.Count; $i++) {
        Write-Host ("  {0}. {1}" -f ($i + 1), $choices[$i].Name)
    }
    $pick = [int](Read-Host (Text pickPrompt)) - 1
    if ($pick -lt 0 -or $pick -ge $choices.Count) { Fail noPort }
    return $choices[$pick].Port
}

# DTR and RTS stay low: on these boards they drive reset and the boot pin.
function Open-Port([string]$Name, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Name) {
            $port = New-Object System.IO.Ports.SerialPort $Name, 115200
            $port.DtrEnable = $false
            $port.RtsEnable = $false
            $port.ReadTimeout = 200
            $port.Encoding = [System.Text.Encoding]::UTF8
            try {
                $port.Open()
                return $port
            } catch {
                $port.Dispose()
            }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# ---------------------------------------------------------------- Improv Serial

$Improv = @{ State = 1; Error = 2; Rpc = 3; Result = 4 }
$Script:Received = New-Object System.Collections.Generic.List[byte]

function New-ImprovPacket([byte]$Type, [byte[]]$Data) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    $bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('IMPROV'))
    $bytes.Add(1)
    $bytes.Add($Type)
    $bytes.Add([byte]$Data.Length)
    $bytes.AddRange($Data)
    $sum = 0
    foreach ($b in $bytes) { $sum = ($sum + $b) -band 0xFF }
    $bytes.Add([byte]$sum)
    return , $bytes.ToArray()
}

function Send-ImprovRpc($Port, [byte]$Command, [byte[]]$Payload = @()) {
    $data = [byte[]](@($Command, [byte]$Payload.Length) + $Payload)
    $packet = New-ImprovPacket $Improv.Rpc $data
    $Port.Write($packet, 0, $packet.Length)
}

function Find-Header {
    $header = [byte[]][char[]]'IMPROV'
    for ($i = 0; $i -le $Script:Received.Count - 6; $i++) {
        $hit = $true
        for ($k = 0; $k -lt 6; $k++) {
            if ($Script:Received[$i + $k] -ne $header[$k]) { $hit = $false; break }
        }
        if ($hit) { return $i }
    }
    return -1
}

# The next well-formed packet, or $null once the deadline passes. Log text in
# between is skipped.
function Read-ImprovPacket($Port, [datetime]$Deadline) {
    while ($true) {
        $at = Find-Header
        if ($at -ge 0 -and $Script:Received.Count -ge $at + 9) {
            $length = $Script:Received[$at + 8]
            if ($Script:Received.Count -ge $at + 10 + $length) {
                $packet = $Script:Received.GetRange($at, 10 + $length).ToArray()
                $Script:Received.RemoveRange(0, $at + 10 + $length)
                $sum = 0
                for ($k = 0; $k -lt $packet.Length - 1; $k++) { $sum = ($sum + $packet[$k]) -band 0xFF }
                if ($sum -eq $packet[-1] -and $packet[6] -eq 1) {
                    $data = if ($length) { [byte[]]$packet[9..(8 + $length)] } else { [byte[]]@() }
                    return [pscustomobject]@{ Type = $packet[7]; Data = $data }
                }
                continue
            }
        } elseif ($at -lt 0 -and $Script:Received.Count -gt 5) {
            $Script:Received.RemoveRange(0, $Script:Received.Count - 5)
        }
        if ((Get-Date) -gt $Deadline) { return $null }
        $count = 0
        try { $count = $Port.BytesToRead } catch { }
        if ($count -gt 0) {
            $chunk = New-Object byte[] $count
            $got = $Port.Read($chunk, 0, $count)
            for ($k = 0; $k -lt $got; $k++) { $Script:Received.Add($chunk[$k]) }
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
}

# An RPC result is: command, length, then length-prefixed UTF-8 strings.
function Get-ImprovStrings([byte[]]$Data) {
    $strings = @()
    $at = 2
    while ($at -lt $Data.Length) {
        $length = $Data[$at]
        $strings += [System.Text.Encoding]::UTF8.GetString($Data, $at + 1, $length)
        $at += 1 + $length
    }
    return , $strings
}
