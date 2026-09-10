param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$BootWaitSeconds = 90
)

$ErrorActionPreference = "Stop"
$Prompt = "hisilicon #"
$LogPath = Join-Path $PSScriptRoot "H3531-UART-last.log"
try { Set-Content -Path $LogPath -Value ("H3531 UART auto-boot log " + (Get-Date)) -Encoding UTF8 } catch {}

function Log([string]$Text, [switch]$NoNewline) {
    if ($NoNewline) { Write-Host -NoNewline $Text } else { Write-Host $Text }
    try { Add-Content -Path $LogPath -Value $Text -Encoding UTF8 } catch {}
}

function Select-SerialPort([string]$Requested) {
    if ($Requested) { return $Requested }
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($ports.Count -eq 0) { throw "No COM ports found." }
    if ($ports.Count -eq 1) { return $ports[0] }
    Write-Host "Available serial ports:"
    for ($i=0; $i -lt $ports.Count; $i++) { Write-Host "  [$i] $($ports[$i])" }
    $n = Read-Host "Select port number"
    if ($n -notmatch '^\d+$' -or [int]$n -ge $ports.Count) { throw "Invalid port selection." }
    return $ports[[int]$n]
}

function New-H3531Serial([string]$Name, [int]$Speed) {
    $s = New-Object System.IO.Ports.SerialPort $Name,$Speed,([System.IO.Ports.Parity]::None),8,([System.IO.Ports.StopBits]::One)
    $s.Handshake = [System.IO.Ports.Handshake]::None
    $s.ReadTimeout = 100
    $s.WriteTimeout = 2000
    $s.DtrEnable = $false
    $s.RtsEnable = $false
    return $s
}

function Drain-Serial($Serial, [int]$Milliseconds = 250) {
    $until = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $until) {
        $x = $Serial.ReadExisting()
        if ($x) { Log $x -NoNewline }
        Start-Sleep -Milliseconds 20
    }
}

function Wait-UbootPrompt($Serial, [int]$TimeoutSeconds = 60) {
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $tail = ""
    $all = New-Object System.Text.StringBuilder
    while ([DateTime]::UtcNow -lt $until) {
        $x = $Serial.ReadExisting()
        if ($x) {
            Log $x -NoNewline
            [void]$all.Append($x)
            $tail += $x
            if ($tail.Length -gt 1024) { $tail = $tail.Substring($tail.Length - 1024) }
            if ($tail.Contains($Prompt)) { return $all.ToString() }
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Timeout waiting for U-Boot prompt '$Prompt'."
}

function Acquire-UbootPrompt($Serial, [int]$TimeoutSeconds = 90) {
    Write-Host ""
    Write-Host "AUTO INTERRUPT ARMED." -ForegroundColor Green
    Write-Host "No separate terminal is needed." -ForegroundColor Green
    Write-Host "If the board is already running, press RESET or power-cycle it now." -ForegroundColor Yellow
    Write-Host "The helper will send harmless SPACE characters during early boot to stop U-Boot autoboot." -ForegroundColor DarkGray

    try {
        Drain-Serial $Serial 150
        $Serial.Write("`r")
        $r = Wait-UbootPrompt $Serial 1
        if ($r.Contains($Prompt)) {
            Write-Host "`n[H3531] U-Boot prompt already available." -ForegroundColor Green
            return
        }
    } catch {}

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $tail = ""
    $nextSpace = [DateTime]::UtcNow
    $sawBoot = $false

    while ([DateTime]::UtcNow -lt $deadline) {
        $x = $Serial.ReadExisting()
        if ($x) {
            Log $x -NoNewline
            $tail += $x
            if ($tail.Length -gt 2048) { $tail = $tail.Substring($tail.Length - 2048) }

            if ($tail.Contains($Prompt)) {
                $Serial.Write("`r")
                [void](Wait-UbootPrompt $Serial 3)
                Write-Host "`n[H3531] Autoboot stopped; U-Boot prompt acquired." -ForegroundColor Green
                return
            }

            if ($tail -match '(?i)U-Boot|HiSilicon|hisilicon|autoboot|Hit any key|stop autoboot') {
                $sawBoot = $true
            }
            if ($tail -match '(?i)Starting kernel|Freeing init memory|login:') {
                throw "Factory OS started before U-Boot was interrupted. Leave this helper running and press RESET/power-cycle the board once more."
            }
        }

        if ([DateTime]::UtcNow -ge $nextSpace) {
            try { $Serial.Write(" ") } catch {}
            if ($sawBoot) {
                $nextSpace = [DateTime]::UtcNow.AddMilliseconds(70)
            } else {
                $nextSpace = [DateTime]::UtcNow.AddMilliseconds(180)
            }
        }
        Start-Sleep -Milliseconds 20
    }

    throw "U-Boot prompt was not seen within $TimeoutSeconds seconds. Start this helper, then reset/power-cycle the board while it says AUTO INTERRUPT ARMED."
}

function Assert-UbootSuccess([string]$Response, [switch]$RequireBytesRead) {
    $bad = @("Unknown command","Unable to use","Unable to read","not valid on device","Wrong Ramdisk Image Format","corrupt or invalid")
    foreach ($needle in $bad) { if ($Response.Contains($needle)) { throw "U-Boot reported an error: $needle" } }
    if ($RequireBytesRead -and $Response -notmatch '\d+\s+bytes read') {
        throw "fatload returned without a 'bytes read' confirmation."
    }
}

function Send-UbootCommand($Serial,[string]$Command,[int]$TimeoutSeconds=60,[switch]$RequireBytesRead) {
    Drain-Serial $Serial 100
    Write-Host "`n>>> $Command" -ForegroundColor Cyan
    try { Add-Content -Path $LogPath -Value (">>> " + $Command) -Encoding UTF8 } catch {}
    $Serial.Write("$Command`r")
    $response = Wait-UbootPrompt $Serial $TimeoutSeconds
    Assert-UbootSuccess $response -RequireBytesRead:$RequireBytesRead
    return $response
}

function Watch-LinuxSerial([string]$Name,[int]$Speed,$Serial) {
    Write-Host ""
    Write-Host "Linux boot started. UART viewer remains active." -ForegroundColor Green
    Write-Host "Ctrl+C stops the viewer. Log: $LogPath" -ForegroundColor DarkGray

    while ($true) {
        try {
            if (-not $Serial -or -not $Serial.IsOpen) {
                $Serial = New-H3531Serial $Name $Speed
                $Serial.Open()
                Write-Host "[UART] Reconnected to $Name" -ForegroundColor Yellow
            }
            $x = $Serial.ReadExisting()
            if ($x) { Log $x -NoNewline }
            Start-Sleep -Milliseconds 30
        }
        catch {
            $msg = $_.Exception.Message
            Write-Host "`n[UART] Read connection interrupted: $msg" -ForegroundColor Yellow
            try { Add-Content -Path $LogPath -Value ("[UART ERROR] " + $msg) -Encoding UTF8 } catch {}
            try { if ($Serial -and $Serial.IsOpen) { $Serial.Close() } } catch {}
            $Serial = $null
            Write-Host "[UART] Waiting 2 seconds and trying $Name again..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
}

$serial = $null
try {
    $Port = Select-SerialPort $Port
    Write-Host "H3531 UART RAM auto-boot helper 0.4"
    Write-Host "Port: $Port @ $Baud"
    Write-Host "Expected FAT root files: zImage.img and H3531.IMG"
    Write-Host "No saveenv. No SPI flash writes."
    Write-Host "This helper can stop U-Boot autoboot by itself."

    $serial = New-H3531Serial $Port $Baud
    $serial.Open()

    Acquire-UbootPrompt $serial $BootWaitSeconds

    [void](Send-UbootCommand $serial "usb start" 30)
    [void](Send-UbootCommand $serial "fatload usb 0:1 0x82000000 zImage.img" 120 -RequireBytesRead)
    [void](Send-UbootCommand $serial "fatload usb 0:1 0x83000000 H3531.IMG" 180 -RequireBytesRead)
    [void](Send-UbootCommand $serial "setenv initrd_high 0xffffffff" 10)
    [void](Send-UbootCommand $serial "setenv bootargs mem=130M console=ttyAMA0,115200 root=/dev/ram0 rootfstype=cramfs ro mtdparts=hi_sfc:512K(boot),4M(romfs),5632K(usr),1536K(web),3M(custom),256K(logo),1280K(mtd)" 10)

    Write-Host "`nImages loaded successfully." -ForegroundColor Green
    Write-Host ">>> bootm 0x82000000 0x83000000" -ForegroundColor Green
    try { Add-Content -Path $LogPath -Value ">>> bootm 0x82000000 0x83000000" -Encoding UTF8 } catch {}
    $serial.Write("bootm 0x82000000 0x83000000`r")

    Watch-LinuxSerial $Port $Baud $serial
}
catch {
    Write-Host ""
    Write-Host "H3531 BOOT HELPER ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    try { Add-Content -Path $LogPath -Value ("FATAL: " + $_.Exception.ToString()) -Encoding UTF8 } catch {}
    exit 1
}
finally {
    try { if ($serial -and $serial.IsOpen) { $serial.Close() } } catch {}
}
