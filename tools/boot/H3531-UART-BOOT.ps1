param(
    [string]$Port = "",
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

function Select-SerialPort {
    param([string]$Requested)
    if ($Requested) { return $Requested }
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($ports.Count -eq 0) { throw "No COM ports found." }
    if ($ports.Count -eq 1) { return $ports[0] }
    Write-Host "Available serial ports:"
    for ($i = 0; $i -lt $ports.Count; $i++) {
        Write-Host "  [$i] $($ports[$i])"
    }
    $n = Read-Host "Select port number"
    if ($n -notmatch '^\d+$' -or [int]$n -ge $ports.Count) { throw "Invalid port selection." }
    return $ports[[int]$n]
}

function Drain-Serial {
    param([System.IO.Ports.SerialPort]$Serial, [int]$Milliseconds = 250)
    $until = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $until) {
        $s = $Serial.ReadExisting()
        if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 25
    }
}

function Send-UbootCommand {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [string]$Command,
        [int]$WaitSeconds = 3
    )
    Write-Host "`n>>> $Command" -ForegroundColor Cyan
    $Serial.Write("$Command`r")
    $until = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    while ([DateTime]::UtcNow -lt $until) {
        $s = $Serial.ReadExisting()
        if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 30
    }
}

$Port = Select-SerialPort $Port
Write-Host "H3531 UART RAM boot helper"
Write-Host "Port: $Port @ $Baud"
Write-Host "Expected FAT root files: zImage.img and H3531.IMG"
Write-Host "The helper does NOT run saveenv and does NOT write SPI flash."
Write-Host "Board must be at the 'hisilicon #' U-Boot prompt."

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

try {
    $serial.Open()
    $serial.Write("`r")
    Drain-Serial $serial 400

    Send-UbootCommand $serial "usb start" 5
    Send-UbootCommand $serial "fatload usb 0:1 0x82000000 zImage.img" 8
    Send-UbootCommand $serial "fatload usb 0:1 0x83000000 H3531.IMG" 12
    Send-UbootCommand $serial "setenv initrd_high 0xffffffff" 1
    Send-UbootCommand $serial "setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)" 1

    Write-Host "`n>>> bootm 0x82000000 0x83000000" -ForegroundColor Green
    $serial.Write("bootm 0x82000000 0x83000000`r")
    Write-Host "Boot command sent. Showing serial output; press Ctrl+C to stop viewing."
    while ($true) {
        $s = $serial.ReadExisting()
        if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 40
    }
}
finally {
    if ($serial -and $serial.IsOpen) { $serial.Close() }
}
