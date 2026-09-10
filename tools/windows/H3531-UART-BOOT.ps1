param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$BootWaitSeconds = 120
)

$ErrorActionPreference = "Stop"
$Prompt = "hisilicon #"
$StopMarker = "PHY 0x02: OUI = 0x01F0, Model = 0x0F, Rev = 0x01"
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

function Send-ConsoleKeys($Serial) {
    try {
        while ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if (($k.Modifiers -band [ConsoleModifiers]::Control) -and $k.Key -eq [ConsoleKey]::Oem6) { return $false }
            switch ($k.Key) {
                'Enter'     { $Serial.Write("`r") }
                'Backspace' { $Serial.Write([char]8) }
                'Tab'       { $Serial.Write("`t") }
                'Escape'    { $Serial.Write([char]27) }
                default { if ($k.KeyChar -ne [char]0) { $Serial.Write([string]$k.KeyChar) } }
            }
        }
    } catch {}
    return $true
}

function Drain-Serial($Serial, [int]$Milliseconds = 250) {
    $until = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $until) {
        $x = $Serial.ReadExisting()
        if ($x) { Log $x -NoNewline }
        if (-not (Send-ConsoleKeys $Serial)) { throw "Terminal stopped by user." }
        Start-Sleep -Milliseconds 15
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
            if ($tail.Length -gt 2048) { $tail = $tail.Substring($tail.Length - 2048) }
            if ($tail.Contains($Prompt)) { return $all.ToString() }
        }
        if (-not (Send-ConsoleKeys $Serial)) { throw "Terminal stopped by user." }
        Start-Sleep -Milliseconds 15
    }
    throw "Timeout waiting for U-Boot prompt '$Prompt'."
}

function Acquire-UbootPrompt($Serial, [int]$TimeoutSeconds = 120) {
    Write-Host ""
    Write-Host "AUTO INTERRUPT ARMED." -ForegroundColor Green
    Write-Host "This window is also a LIVE SERIAL TERMINAL." -ForegroundColor Green
    Write-Host "You may press SPACE yourself at any time to stop autoboot." -ForegroundColor Yellow
    Write-Host "Automatic stop marker:" -ForegroundColor DarkGray
    Write-Host "  $StopMarker" -ForegroundColor DarkGray
    Write-Host "Ctrl+] exits the terminal." -ForegroundColor DarkGray

    try {
        Drain-Serial $Serial 100
        $Serial.Write("`r")
        $r = Wait-UbootPrompt $Serial 1
        if ($r.Contains($Prompt)) {
            Write-Host "`n[H3531] U-Boot prompt already available." -ForegroundColor Green
            return
        }
    } catch {}

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $tail = ""
    $markerSeen = $false
    $nextStopKey = [DateTime]::MaxValue

    while ([DateTime]::UtcNow -lt $deadline) {
        $x = $Serial.ReadExisting()
        if ($x) {
            Log $x -NoNewline
            $tail += $x
            if ($tail.Length -gt 4096) { $tail = $tail.Substring($tail.Length - 4096) }
            if ($tail.Contains($Prompt)) {
                Write-Host "`n[H3531] U-Boot prompt acquired." -ForegroundColor Green
                return
            }
            if (-not $markerSeen -and $tail.Contains($StopMarker)) {
                $markerSeen = $true
                Write-Host "`n[H3531] STOP MARKER detected -> interrupting autoboot NOW." -ForegroundColor Yellow
                try { $Serial.Write(" ") } catch {}
                $nextStopKey = [DateTime]::UtcNow.AddMilliseconds(35)
            }
        }
        if ($markerSeen -and [DateTime]::UtcNow -ge $nextStopKey) {
            try { $Serial.Write(" ") } catch {}
            $nextStopKey = [DateTime]::UtcNow.AddMilliseconds(35)
        }
        if (-not (Send-ConsoleKeys $Serial)) { throw "Terminal stopped by user." }
        Start-Sleep -Milliseconds 10
    }
    throw "U-Boot prompt was not acquired within $TimeoutSeconds seconds. Terminal will remain available for manual control."
}

function Assert-UbootSuccess([string]$Response, [switch]$RequireBytesRead) {
    $bad = @("Unknown command","Unable to use","Unable to read","not valid on device","Wrong Ramdisk Image Format","corrupt or invalid")
    foreach ($needle in $bad) { if ($Response.Contains($needle)) { throw "U-Boot reported an error: $needle" } }
    if ($RequireBytesRead -and $Response -notmatch '\d+\s+bytes read') { throw "fatload returned without a 'bytes read' confirmation." }
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

function Live-SerialTerminal([string]$Name,[int]$Speed,$Serial,[string]$Banner) {
    Write-Host ""
    Write-Host $Banner -ForegroundColor Green
    Write-Host "LIVE SERIAL TERMINAL is active." -ForegroundColor Green
    Write-Host "Keyboard is sent directly to the board. Ctrl+] exits. Log: $LogPath" -ForegroundColor DarkGray
    while ($true) {
        try {
            if (-not $Serial -or -not $Serial.IsOpen) {
                $Serial = New-H3531Serial $Name $Speed
                $Serial.Open()
                Write-Host "[UART] Reconnected to $Name" -ForegroundColor Yellow
            }
            $x = $Serial.ReadExisting()
            if ($x) { Log $x -NoNewline }
            if (-not (Send-ConsoleKeys $Serial)) {
                Write-Host "`n[H3531] Terminal closed by user (Ctrl+])." -ForegroundColor Yellow
                return
            }
            Start-Sleep -Milliseconds 15
        }
        catch {
            $msg = $_.Exception.Message
            Write-Host "`n[UART] Connection interrupted: $msg" -ForegroundColor Yellow
            try { Add-Content -Path $LogPath -Value ("[UART ERROR] " + $msg) -Encoding UTF8 } catch {}
            try { if ($Serial -and $Serial.IsOpen) { $Serial.Close() } } catch {}
            $Serial = $null
            Write-Host "[UART] Waiting 2 seconds and trying $Name again..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
}

$serial = $null
$Port = Select-SerialPort $Port
Write-Host "H3531 UART RAM auto-boot helper 0.5"
Write-Host "Port: $Port @ $Baud"
Write-Host "Expected FAT root files: zImage.img and H3531.IMG"
Write-Host "No saveenv. No SPI flash writes."
Write-Host "Exact board stop marker is enabled."
Write-Host "The same window remains a permanent serial terminal."

try {
    $serial = New-H3531Serial $Port $Baud
    $serial.Open()
    try {
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
        Live-SerialTerminal $Port $Baud $serial "Linux boot started."
    }
    catch {
        Write-Host ""
        Write-Host "[H3531] AUTO-BOOT DID NOT COMPLETE:" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
        try { Add-Content -Path $LogPath -Value ("AUTOBOOT ERROR: " + $_.Exception.ToString()) -Encoding UTF8 } catch {}
        Write-Host ""
        Write-Host "Falling back to MANUAL LIVE TERMINAL instead of closing." -ForegroundColor Yellow
        Write-Host "You can now press SPACE, Enter, or type U-Boot commands yourself." -ForegroundColor Yellow
        Live-SerialTerminal $Port $Baud $serial "Manual fallback mode."
    }
}
finally {
    try { if ($serial -and $serial.IsOpen) { $serial.Close() } } catch {}
}
