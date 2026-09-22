# ASCII-ONLY on purpose: this machine's PowerShell 5.1 reads script files with the
# GBK codepage, so non-ASCII text here breaks string parsing.
#
# v2 changes vs v1:
#   - re-resolve the port before EVERY esptool operation (uploading the stub makes
#     USB re-enumerate / the COM number change, so a cached name goes stale)
#   - ONE single write_flash call per attempt (the merged image already contains
#     bootloader + partition table + app, so no separate erase is needed)
#   - only consider ports that are actually Present (Get-PnpDevice), not the
#     phantom COM entries that SerialPort::GetPortNames() also reports
$ErrorActionPreference = 'Continue'
$mk  = "C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source"
$esp = "$mk\piocore\packages\tool-esptoolpy\esptool.py"
$bin = "C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_device_kbd\merged_firmware\Left_MAKCM_MCU_MCU_KBD_PT_V1_0.bin"
$rightMac = "7c:0c:5f:a7:4a:60"
$skipPort = "COM5"                     # CH343 log port, never an ESP download port

function Log($m) { Write-Output ("[{0}] {1}" -f (Get-Date).ToString("HH:mm:ss.fff"), $m) }

function Get-PresentPorts {
    $out = @()
    foreach ($d in (Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue)) {
        if (-not $d.Present) { continue }
        $m = [regex]::Match([string]$d.FriendlyName, '\((COM\d+)\)')
        if ($m.Success) { $out += $m.Groups[1].Value }
    }
    return ($out | Select-Object -Unique)
}

function EspProbe([string]$port) {
    foreach ($before in @('no_reset','default_reset')) {
        $o = & python $esp --chip esp32s3 --port $port --before $before --connect-attempts 1 --after no_reset flash_id 2>&1 | Out-String
        if ($o -match 'Chip is ESP32-S3') {
            $mm = [regex]::Match($o, 'MAC:\s*([0-9a-fA-F:]+)')
            return $mm.Groups[1].Value
        }
    }
    return $null
}

Log "=== left-board flasher v2 ==="
if (-not (Test-Path $bin)) { Log "!! bin missing, abort"; exit 1 }
Log ("target bin SHA16 = " + (Get-FileHash $bin -Algorithm SHA256).Hash.Substring(0,16))
Log ("present ports now: " + ((Get-PresentPorts) -join ', '))
Log ">>> if the board is not in download mode: HOLD BOOT and replug the left-board USB <<<"

$deadline = (Get-Date).AddSeconds(300)
$done = $false
$round = 0

while ((Get-Date) -lt $deadline -and -not $done) {
    $round++
    $ports = @(Get-PresentPorts | Where-Object { $_ -ne $skipPort })
    if ($ports.Count -eq 0) { Start-Sleep -Milliseconds 200; continue }

    $target = $null
    foreach ($p in $ports) {
        $mac = EspProbe $p
        if (-not $mac) { continue }
        Log "round $round : $p answered, MAC=$mac"
        if ($mac -eq $rightMac) { Log "  !! RIGHT board - refusing to flash left FW"; continue }
        $target = $p
        break
    }
    if (-not $target) { Start-Sleep -Milliseconds 250; continue }

    Log "round $round : flashing LEFT board on $target (single write_flash) ..."
    $o = & python $esp --chip esp32s3 --port $target --baud 460800 --before no_reset --after hard_reset write_flash -z 0x0 $bin 2>&1 | Out-String
    ($o -split "`n" | Where-Object { $_ -match 'Wrote|Hash|Compressed|Leaving|fatal|rror' }) |
        ForEach-Object { Log ("    " + $_.Trim()) }

    if ($o -match 'Hash of data verified') {
        Log "*** FLASH OK - hash verified ***"
        $done = $true
    } else {
        Log "round $round failed (port likely re-enumerated) - retrying ..."
        Start-Sleep -Milliseconds 400
    }
}

if (-not $done) { Log "timeout; could not complete the flash"; exit 2 }

Log "waiting for the left board to boot the new firmware ..."
Start-Sleep -Seconds 6
Log ("present ports after boot: " + ((Get-PresentPorts) -join ', '))
Log "next: verify functionally over COM5 with crc=3+plen frames"
Log "=== done ==="
