# ============================================================
#  WTSN Configurator - Windows launcher
#  MQTT broker (optional) + web GUI + browser
#  Mirrors run.sh for Linux.
#
#  Usage:
#    .\run.ps1            # broker (if mosquitto.exe on PATH) + GUI + browser
#    .\run.ps1 -Headless  # services only, no browser
# ============================================================
param(
    [switch]$Headless
)

$ErrorActionPreference = "Stop"
$projDir  = $PSScriptRoot
$guiPort  = 8000
$mqttPort = 1883
$guiLog   = Join-Path $env:TEMP "wtsn_webgui.log"

function Log($msg) { Write-Host "[wtsn] $msg" -ForegroundColor Cyan }

# ---------------- LAN IP (first non-loopback, non-link-local) ----------------
function Get-LanIp {
    $ip = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object {
            $_.IPAddress -notlike "127.*" -and
            $_.IPAddress -notlike "169.254.*"
        } |
        Select-Object -First 1 -ExpandProperty IPAddress
    if (-not $ip) { $ip = "127.0.0.1" }
    return $ip
}

$lanIp = Get-LanIp

# ---------------- 1) MQTT broker ----------------
function Start-Broker {
    $listening = Get-NetTCPConnection -LocalPort $script:mqttPort -State Listen -ErrorAction SilentlyContinue
    if ($listening) {
        Log "MQTT broker already listening on 0.0.0.0:$mqttPort"
        return
    }
    $mosq = Get-Command mosquitto.exe -ErrorAction SilentlyContinue
    if (-not $mosq) {
        Log "mosquitto.exe not found on PATH - no external broker started."
        Log "The web GUI still works in Simulation mode; for REAL mode start a broker manually."
        return
    }
    $conf = Join-Path $env:TEMP "wtsn-mosquitto.conf"
    Set-Content -Path $conf -Value "listener 1883 0.0.0.0`nallow_anonymous true`n" -Encoding ASCII
    Start-Process -FilePath $mosq.Source -ArgumentList @("-c", $conf) -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $listening = Get-NetTCPConnection -LocalPort $script:mqttPort -State Listen -ErrorAction SilentlyContinue
    if ($listening) {
        Log "MQTT broker started on 0.0.0.0:$mqttPort (ESP32 connects to $lanIp:$mqttPort)"
    } else {
        Log "WARNING: broker did not start listening on $mqttPort"
    }
}

# ---------------- 2) web GUI ----------------
function Stop-OldGui {
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*webgui.py*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
}

function Start-Gui {
    Stop-OldGui
    $env:WTSN_BROKER = "$lanIp:$mqttPort"
    Start-Process -FilePath "python" -WorkingDirectory $projDir `
        -ArgumentList "webgui.py" `
        -RedirectStandardOutput $guiLog `
        -RedirectStandardError (Join-Path $env:TEMP "wtsn_webgui.err.log") `
        -WindowStyle Hidden
}

function Wait-Gui {
    for ($i = 0; $i -lt 10; $i++) {
        $ok = $false
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:$guiPort/" -TimeoutSec 2 -UseBasicParsing | Out-Null
            $ok = $true
        } catch { }
        if ($ok) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

# ---------------- 3) GUI health monitor (self-healing, mirrors run.sh) ----------------
function Start-GuiMonitor {
    $monitorScript = Join-Path $env:TEMP "wtsn_gui_monitor.ps1"
    $header = @(
        "`$proj = '$projDir'",
        "`$log  = '$guiLog'",
        "`$lanIp = '$lanIp'",
        "`$mqttPort = $mqttPort"
    )
    $body = @'
$stale = 0
while ($true) {
    Start-Sleep -Seconds 4
    $alive = $false
    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:8000/" -TimeoutSec 3 -UseBasicParsing | Out-Null
        $alive = $true
    } catch { }
    if ($alive) { $stale = 0; continue }
    $stale++
    if ($stale -ge 2) {
        Write-Host "$(Get-Date -Format 'HH:mm:ss') [wtsn] RESTARTING GUI"
        Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like "*webgui.py*" } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 1
        $env:WTSN_BROKER = "$lanIp:$mqttPort"
        Start-Process -FilePath "python" -WorkingDirectory $proj -ArgumentList "webgui.py" `
            -RedirectStandardOutput $log `
            -RedirectStandardError "$log.err" -WindowStyle Hidden
        $stale = 0
    }
}
'@
    ($header + $body) | Set-Content -Path $monitorScript -Encoding ASCII
    Start-Process -FilePath "powershell" `
        -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $monitorScript) `
        -WindowStyle Hidden
}

# ---------------- main ----------------
Log "LAN IP detected: $lanIp"
Start-Broker
Start-Gui
if (Wait-Gui) {
    Log "web GUI OK on http://127.0.0.1:$guiPort"
} else {
    Log "ERROR: web GUI failed to start - see $guiLog"
    Get-Content $guiLog -TotalCount 20 -ErrorAction SilentlyContinue
    exit 1
}
Start-GuiMonitor
if (-not $Headless) {
    Start-Process "http://127.0.0.1:$guiPort"
    Log "Done. Broker=$lanIp:$mqttPort  GUI=http://127.0.0.1:$guiPort"
    Log "New board? Connect to WiFi 'WTSN-Setup' and open http://192.168.4.1/ to provision."
} else {
    Log "headless: services only (broker + GUI + monitor)."
}
