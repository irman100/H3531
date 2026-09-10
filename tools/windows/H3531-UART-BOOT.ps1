param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$BootWaitSeconds = 120,
    [switch]$NoAutoBoot
)

$ErrorActionPreference = "Stop"
$Prompt = "hisilicon #"
$StopMarker = "PHY 0x02: OUI = 0x01F0, Model = 0x0F, Rev = 0x01"
$LogPath = Join-Path $PSScriptRoot "H3531-UART-last.log"
$script:OriginalInputMode = $null
$script:OriginalOutputMode = $null
$script:ConsoleInputHandle = [IntPtr]::Zero
$script:ConsoleOutputHandle = [IntPtr]::Zero
$script:OldTreatControlCAsInput = $null

try {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class H3531ConsoleMode {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GetStdHandle(int nStdHandle);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetConsoleMode(IntPtr hConsoleHandle, out uint lpMode);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetConsoleMode(IntPtr hConsoleHandle, uint dwMode);
}
"@
} catch {}

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

function Enable-TerminalConsoleMode {
    try {
        $script:OldTreatControlCAsInput = [Console]::TreatControlCAsInput
        [Console]::TreatControlCAsInput = $true
    } catch {}
    try {
        $script:ConsoleInputHandle = [H3531ConsoleMode]::GetStdHandle(-10)
        $mode = [uint32]0
        if ([H3531ConsoleMode]::GetConsoleMode($script:ConsoleInputHandle, [ref]$mode)) {
            $script:OriginalInputMode = $mode
            $newMode = [uint32]($mode -band 0xFFFFFFFE)
            [void][H3531ConsoleMode]::SetConsoleMode($script:ConsoleInputHandle, $newMode)
        }
    } catch {}
    try {
        $script:ConsoleOutputHandle = [H3531ConsoleMode]::GetStdHandle(-11)
        $modeOut = [uint32]0
        if ([H3531ConsoleMode]::GetConsoleMode($script:ConsoleOutputHandle, [ref]$modeOut)) {
            $script:OriginalOutputMode = $modeOut
            [void][H3531ConsoleMode]::SetConsoleMode($script:ConsoleOutputHandle, [uint32]($modeOut -bor 0x0004))
        }
    } catch {}
}

function Restore-TerminalConsoleMode {
    try {
        if ($null -ne $script:OriginalInputMode -and $script:ConsoleInputHandle -ne [IntPtr]::Zero) {
            [void][H3531ConsoleMode]::SetConsoleMode($script:ConsoleInputHandle, [uint32]$script:OriginalInputMode)
        }
    } catch {}
    try {
        if ($null -ne $script:OriginalOutputMode -and $script:ConsoleOutputHandle -ne [IntPtr]::Zero) {
            [void][H3531ConsoleMode]::SetConsoleMode($script:ConsoleOutputHandle, [uint32]$script:OriginalOutputMode)
        }
    } catch {}
    try {
        if ($null -ne $script:OldTreatControlCAsInput) {
            [Console]::TreatControlCAsInput = $script:OldTreatControlCAsInput
        }
    } catch {}
}

function New-H3531Serial([string]$Name, [int]$Speed) {
    $s = New-Object System.IO.Ports.SerialPort $Name,$Speed,([System.IO.Ports.Parity]::None),8,([System.IO.Ports.StopBits]::One)
    $s.Handshake = [System.IO.Ports.Handshake]::None
    $s.ReadTimeout = 100
    $s.WriteTimeout = 2000
    $s.DtrEnable = $false
    $s.RtsEnable = $false
    $s.Encoding = [System.Text.Encoding]::ASCII
    return $s
}

function Write-SerialBytes($Serial, [byte[]]$Bytes) {
    if ($Bytes -and $Bytes.Length -gt 0) { $Serial.Write($Bytes, 0, $Bytes.Length) }
}

function Write-SerialText($Serial, [string]$Text) {
    if ($null -eq $Text -or $Text.Length -eq 0) { return }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    Write-SerialBytes $Serial $bytes
}

function Send-ControlByte($Serial, [int]$Code) {
    Write-SerialBytes $Serial ([byte[]]@([byte]$Code))
}

function Send-EscapeSequence($Serial, [string]$Sequence) {
    Write-SerialText $Serial ([string][char]27 + $Sequence)
}

function Get-XtermModifier([ConsoleKeyInfo]$KeyInfo) {
    $m = 1
    if (($KeyInfo.Modifiers -band [ConsoleModifiers]::Shift) -ne 0)   { $m += 1 }
    if (($KeyInfo.Modifiers -band [ConsoleModifiers]::Alt) -ne 0)     { $m += 2 }
    if (($KeyInfo.Modifiers -band [ConsoleModifiers]::Control) -ne 0) { $m += 4 }
    return $m
}

function Show-TerminalHelp {
    Write-Host ""
    Write-Host "--- H3531 LOCAL TERMINAL KEYS ---" -ForegroundColor Cyan
    Write-Host "All normal keys are forwarded to UART." -ForegroundColor Gray
    Write-Host "Ctrl+A..Ctrl+Z are forwarded as control bytes (Ctrl+C = 0x03)." -ForegroundColor Gray
    Write-Host "Arrows/Home/End/Insert/Delete/PgUp/PgDn and F1..F12 send ANSI/xterm sequences." -ForegroundColor Gray
    Write-Host "Alt+key sends ESC followed by the key, as a normal terminal does." -ForegroundColor Gray
    Write-Host "Ctrl+Alt+H : show this LOCAL help (not sent to board)." -ForegroundColor Yellow
    Write-Host "Ctrl+Alt+Q : close the H3531 terminal cleanly (not sent to board)." -ForegroundColor Yellow
    Write-Host "---------------------------------" -ForegroundColor Cyan
}

function Send-OneConsoleKey($Serial, [ConsoleKeyInfo]$k) {
    $ctrl  = (($k.Modifiers -band [ConsoleModifiers]::Control) -ne 0)
    $alt   = (($k.Modifiers -band [ConsoleModifiers]::Alt) -ne 0)

    if ($ctrl -and $alt -and $k.Key -eq [ConsoleKey]::H) { Show-TerminalHelp; return $true }
    if ($ctrl -and $alt -and $k.Key -eq [ConsoleKey]::Q) { return $false }

    $mod = Get-XtermModifier $k
    switch ($k.Key) {
        'UpArrow'    { if ($mod -eq 1) { Send-EscapeSequence $Serial "[A" } else { Send-EscapeSequence $Serial ("[1;$mod" + "A") }; return $true }
        'DownArrow'  { if ($mod -eq 1) { Send-EscapeSequence $Serial "[B" } else { Send-EscapeSequence $Serial ("[1;$mod" + "B") }; return $true }
        'RightArrow' { if ($mod -eq 1) { Send-EscapeSequence $Serial "[C" } else { Send-EscapeSequence $Serial ("[1;$mod" + "C") }; return $true }
        'LeftArrow'  { if ($mod -eq 1) { Send-EscapeSequence $Serial "[D" } else { Send-EscapeSequence $Serial ("[1;$mod" + "D") }; return $true }
        'Home'       { if ($mod -eq 1) { Send-EscapeSequence $Serial "[H" } else { Send-EscapeSequence $Serial ("[1;$mod" + "H") }; return $true }
        'End'        { if ($mod -eq 1) { Send-EscapeSequence $Serial "[F" } else { Send-EscapeSequence $Serial ("[1;$mod" + "F") }; return $true }
        'Insert'     { Send-EscapeSequence $Serial "[2~"; return $true }
        'Delete'     { Send-EscapeSequence $Serial "[3~"; return $true }
        'PageUp'     { Send-EscapeSequence $Serial "[5~"; return $true }
        'PageDown'   { Send-EscapeSequence $Serial "[6~"; return $true }
        'F1'         { Send-EscapeSequence $Serial "OP"; return $true }
        'F2'         { Send-EscapeSequence $Serial "OQ"; return $true }
        'F3'         { Send-EscapeSequence $Serial "OR"; return $true }
        'F4'         { Send-EscapeSequence $Serial "OS"; return $true }
        'F5'         { Send-EscapeSequence $Serial "[15~"; return $true }
        'F6'         { Send-EscapeSequence $Serial "[17~"; return $true }
        'F7'         { Send-EscapeSequence $Serial "[18~"; return $true }
        'F8'         { Send-EscapeSequence $Serial "[19~"; return $true }
        'F9'         { Send-EscapeSequence $Serial "[20~"; return $true }
        'F10'        { Send-EscapeSequence $Serial "[21~"; return $true }
        'F11'        { Send-EscapeSequence $Serial "[23~"; return $true }
        'F12'        { Send-EscapeSequence $Serial "[24~"; return $true }
        'Enter'      { Send-ControlByte $Serial 13; return $true }
        'Backspace'  { Send-ControlByte $Serial 8; return $true }
        'Tab'        { Send-ControlByte $Serial 9; return $true }
        'Escape'     { Send-ControlByte $Serial 27; return $true }
    }

    if ($ctrl -and $k.Key -ge [ConsoleKey]::A -and $k.Key -le [ConsoleKey]::Z) {
        $code = ([int]$k.Key - [int][ConsoleKey]::A) + 1
        if ($alt) { Send-ControlByte $Serial 27 }
        Send-ControlByte $Serial $code
        return $true
    }

    if ($ctrl) {
        switch ($k.Key) {
            'Spacebar' { if ($alt) { Send-ControlByte $Serial 27 }; Send-ControlByte $Serial 0; return $true }
            'Oem4'     { if ($alt) { Send-ControlByte $Serial 27 }; Send-ControlByte $Serial 27; return $true }
            'Oem5'     { if ($alt) { Send-ControlByte $Serial 27 }; Send-ControlByte $Serial 28; return $true }
            'Oem6'     { if ($alt) { Send-ControlByte $Serial 27 }; Send-ControlByte $Serial 29; return $true }
        }
    }

    if ($k.KeyChar -ne [char]0) {
        if ($alt) { Send-ControlByte $Serial 27 }
        Write-SerialText $Serial ([string]$k.KeyChar)
    }
    return $true
}

function Pump-ConsoleKeys($Serial) {
    try {
        while ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if (-not (Send-OneConsoleKey $Serial $k)) { return $false }
        }
    } catch {}
    return $true
}

function Drain-Serial($Serial, [int]$Milliseconds = 250) {
    $until = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $until) {
        $x = $Serial.ReadExisting()
        if ($x) { Log $x -NoNewline }
        if (-not (Pump-ConsoleKeys $Serial)) { throw "Terminal closed by user." }
        Start-Sleep -Milliseconds 10
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
        if (-not (Pump-ConsoleKeys $Serial)) { throw "Terminal closed by user." }
        Start-Sleep -Milliseconds 10
    }
    throw "Timeout waiting for U-Boot prompt '$Prompt'."
}

function Send-CtrlCBurst($Serial, [int]$Count = 24) {
    if ($Count -lt 1) { return }
    $b = New-Object byte[] $Count
    for ($i=0; $i -lt $Count; $i++) { $b[$i] = 3 }
    try { Write-SerialBytes $Serial $b } catch {}
}

function Acquire-UbootPrompt($Serial, [int]$TimeoutSeconds = 120) {
    Write-Host ""
    Write-Host "AUTO INTERRUPT ARMED." -ForegroundColor Green
    Write-Host "Exact physical-board trigger:" -ForegroundColor DarkGray
    Write-Host "  $StopMarker" -ForegroundColor DarkGray
    Write-Host "At that marker the helper starts a sustained UART Ctrl+C burst immediately." -ForegroundColor Yellow
    Write-Host "The keyboard is live at the same time. Ctrl+Alt+H = local help." -ForegroundColor Gray

    try {
        Drain-Serial $Serial 80
        Send-ControlByte $Serial 13
        $r = Wait-UbootPrompt $Serial 1
        if ($r.Contains($Prompt)) {
            Write-Host "`n[H3531] U-Boot prompt already available." -ForegroundColor Green
            return
        }
    } catch {}

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $tail = ""
    $markerSeen = $false
    $bannerShown = $false

    while ([DateTime]::UtcNow -lt $deadline) {
        if ($markerSeen) { Send-CtrlCBurst $Serial 8 }

        $x = $Serial.ReadExisting()
        if ($x) {
            $tail += $x
            if ($tail.Length -gt 4096) { $tail = $tail.Substring($tail.Length - 4096) }

            if (-not $markerSeen -and $tail.Contains($StopMarker)) {
                $markerSeen = $true
                Send-CtrlCBurst $Serial 32
            }

            $promptSeen = $tail.Contains($Prompt)
            Log $x -NoNewline

            if ($markerSeen -and -not $bannerShown) {
                $bannerShown = $true
                Write-Host "`n[H3531] STOP MARKER detected -> sustained CTRL+C is active NOW." -ForegroundColor Yellow
            }

            if ($promptSeen) {
                Write-Host "`n[H3531] U-Boot prompt acquired." -ForegroundColor Green
                return
            }
        }

        if (-not (Pump-ConsoleKeys $Serial)) { throw "Terminal closed by user." }
        if (-not $markerSeen) { Start-Sleep -Milliseconds 4 }
    }

    throw "U-Boot prompt was not acquired within $TimeoutSeconds seconds."
}

function Assert-UbootSuccess([string]$Response, [switch]$RequireBytesRead) {
    $bad = @("Unknown command","Unable to use","Unable to read","not valid on device","Wrong Ramdisk Image Format","corrupt or invalid")
    foreach ($needle in $bad) {
        if ($Response.Contains($needle)) { throw "U-Boot reported an error: $needle" }
    }
    if ($RequireBytesRead -and $Response -notmatch '\d+\s+bytes read') {
        throw "fatload returned without a 'bytes read' confirmation."
    }
}

function Send-UbootCommand($Serial,[string]$Command,[int]$TimeoutSeconds=60,[switch]$RequireBytesRead) {
    Drain-Serial $Serial 80
    Write-Host "`n>>> $Command" -ForegroundColor Cyan
    try { Add-Content -Path $LogPath -Value (">>> " + $Command) -Encoding UTF8 } catch {}
    Write-SerialText $Serial ($Command + "`r")
    $response = Wait-UbootPrompt $Serial $TimeoutSeconds
    Assert-UbootSuccess $response -RequireBytesRead:$RequireBytesRead
    return $response
}

function Live-SerialTerminal([string]$Name,[int]$Speed,$Serial,[string]$Banner) {
    Write-Host ""
    Write-Host $Banner -ForegroundColor Green
    Write-Host "H3531 FULL-DUPLEX UART TERMINAL 0.7" -ForegroundColor Green
    Write-Host "All ordinary terminal keys, including Ctrl+C, are forwarded to the board." -ForegroundColor Yellow
    Write-Host "Ctrl+Alt+H = local help. Ctrl+Alt+Q = close terminal." -ForegroundColor DarkGray
    Write-Host "Log: $LogPath" -ForegroundColor DarkGray

    while ($true) {
        try {
            if (-not $Serial -or -not $Serial.IsOpen) {
                $Serial = New-H3531Serial $Name $Speed
                $Serial.Open()
                Write-Host "[UART] Reconnected to $Name" -ForegroundColor Yellow
            }
            $x = $Serial.ReadExisting()
            if ($x) { Log $x -NoNewline }
            if (-not (Pump-ConsoleKeys $Serial)) {
                Write-Host "`n[H3531] Terminal closed locally with Ctrl+Alt+Q." -ForegroundColor Yellow
                return
            }
            Start-Sleep -Milliseconds 8
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
try {
    $Port = Select-SerialPort $Port
    try { Set-Content -Path $LogPath -Value ("H3531 UART terminal 0.7 log " + (Get-Date)) -Encoding UTF8 } catch {}

    Write-Host "H3531 UART AUTO-BOOT + FULL TERMINAL 0.7"
    Write-Host "Port: $Port @ $Baud"
    Write-Host "No saveenv. No SPI flash writes."
    if (-not $NoAutoBoot) { Write-Host "Auto boot: zImage.img + H3531.IMG from FAT usb 0:1" }
    else { Write-Host "Auto boot disabled: terminal-only mode." }

    $serial = New-H3531Serial $Port $Baud
    $serial.Open()
    Enable-TerminalConsoleMode

    if ($NoAutoBoot) {
        Live-SerialTerminal $Port $Baud $serial "Terminal-only mode."
    } else {
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
            Write-SerialText $serial "bootm 0x82000000 0x83000000`r"
            Live-SerialTerminal $Port $Baud $serial "Linux boot started."
        }
        catch {
            Write-Host ""
            Write-Host "[H3531] AUTO-BOOT DID NOT COMPLETE:" -ForegroundColor Red
            Write-Host $_.Exception.Message -ForegroundColor Red
            try { Add-Content -Path $LogPath -Value ("AUTOBOOT ERROR: " + $_.Exception.ToString()) -Encoding UTF8 } catch {}
            Write-Host ""
            Write-Host "Automation is stopping, but THE TERMINAL IS NOT." -ForegroundColor Yellow
            Write-Host "You have direct manual UART control now." -ForegroundColor Yellow
            Live-SerialTerminal $Port $Baud $serial "Manual fallback mode."
        }
    }
}
catch {
    Write-Host ""
    Write-Host "[H3531] TERMINAL ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    try { Add-Content -Path $LogPath -Value ("FATAL: " + $_.Exception.ToString()) -Encoding UTF8 } catch {}
    try {
        if ($serial -and $serial.IsOpen) { Live-SerialTerminal $Port $Baud $serial "Recovered manual terminal mode." }
    } catch {}
}
finally {
    try { if ($serial -and $serial.IsOpen) { $serial.Close() } } catch {}
    Restore-TerminalConsoleMode
}
