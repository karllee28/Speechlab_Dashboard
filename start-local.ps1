$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$serverDir = Join-Path $root 'server'
$certDir = Join-Path $serverDir 'certs'

$lanIp = Get-NetIPAddress -AddressFamily IPv4 -PrefixOrigin Dhcp |
    Where-Object {
        $_.IPAddress -notlike '127.*' -and
        $_.IPAddress -notlike '169.254.*' -and
        $_.InterfaceAlias -match 'Wi-Fi|Wireless|Ethernet'
    } |
    Select-Object -First 1 -ExpandProperty IPAddress

if (-not $lanIp) {
    $lanIp = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
        Select-Object -First 1 -ExpandProperty IPAddress
}

if (-not $lanIp) {
    throw 'Could not determine a LAN IPv4 address.'
}

if (-not (Test-Path (Join-Path $certDir 'lan-cert.pem')) -or
    -not (Test-Path (Join-Path $certDir 'lan-key.pem'))) {
    throw "Missing LAN certificate files in $certDir"
}

Push-Location $serverDir
try {
    if (-not (Test-Path (Join-Path $serverDir 'node_modules'))) {
        Write-Host 'Installing Node.js dependencies...'
        npm install
    }
} finally {
    Pop-Location
}

$env:PORT = '3000'
$env:ANNOUNCED_IP = $lanIp
$env:DASHBOARD_URL = "https://127.0.0.1:$($env:PORT)"
$env:TLS_CERT = Join-Path $certDir 'lan-cert.pem'
$env:TLS_KEY = Join-Path $certDir 'lan-key.pem'
$env:TLS_REJECT_UNAUTHORIZED = '0'

Write-Host ''
Write-Host "SpeechLab LAN address: https://${lanIp}:$($env:PORT)" -ForegroundColor Green
Write-Host 'The certificate is self-signed. Open the URL and accept the browser warning once.' -ForegroundColor Yellow
Write-Host ''

$serverCommand = "Set-Location '$serverDir'; node server.js"
$bridgeCommand = "Set-Location '$serverDir'; node esp_webrtc_bridge.js"

Start-Process powershell.exe -WorkingDirectory $serverDir -ArgumentList @('-NoExit', '-ExecutionPolicy', 'Bypass', '-Command', $serverCommand)
Start-Process powershell.exe -WorkingDirectory $serverDir -ArgumentList @('-NoExit', '-ExecutionPolicy', 'Bypass', '-Command', $bridgeCommand)

Write-Host 'Server and bridge windows started.' -ForegroundColor Green
Write-Host "Open: https://${lanIp}:$($env:PORT)"
