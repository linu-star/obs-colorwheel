param([switch]$KeepRunning)
$ErrorActionPreference = 'Stop'
$cfg    = "$env:APPDATA\obs-studio\plugin_config\obs-websocket\config.json"
$backup = "$env:TEMP\obs-ws-config.backup.json"
$obsDir = 'C:\Program Files\obs-studio\bin\64bit'
$node   = 'C:\Users\qiusen\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\node\bin\node.exe'
$script = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel\tools\ws_pixel_test.cjs'

function Wait-Port([int]$port, [int]$tries = 60) {
  for ($i = 0; $i -lt $tries; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $iar = $c.BeginConnect('127.0.0.1', $port, $null, $null)
      if ($iar.AsyncWaitHandle.WaitOne(300) -and $c.Connected) { $c.Close(); return $true }
      $c.Close()
    } catch { }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Stop-ObsGracefully {
  Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object {
    if (-not $_.HasExited) {
      [void]$_.CloseMainWindow()
      if (-not $_.WaitForExit(8000)) { $_.Kill(); $_.WaitForExit(3000) }
    }
  }
}

Copy-Item $cfg $backup -Force
$c = Get-Content $cfg -Raw | ConvertFrom-Json
$port = $c.server_port
$pass = $c.server_password
$c.server_enabled = $true
$c | ConvertTo-Json -Depth 5 | Set-Content $cfg -Encoding UTF8

$exit = 1
try {
  Stop-ObsGracefully
  Start-Sleep -Seconds 1

  # OBS keeps a .sentinel directory while running and removes it on a clean
  # exit.  A leftover one makes the next start report "crash or unclean
  # shutdown" and block behind a modal prompt before plugins load.
  $sentinel = "$env:APPDATA\obs-studio\.sentinel"
  if (Test-Path $sentinel) { Remove-Item $sentinel -Recurse -Force -ErrorAction SilentlyContinue }

  Write-Host "launching OBS (websocket :$port) ..." -ForegroundColor Cyan
  Start-Process -FilePath "$obsDir\obs64.exe" -WorkingDirectory $obsDir `
    -ArgumentList '--disable-shutdown-check' `
    -RedirectStandardOutput "$env:TEMP\obs-pixel.out" -RedirectStandardError "$env:TEMP\obs-pixel.err" -WindowStyle Hidden

  if (-not (Wait-Port $port)) { throw "obs-websocket did not come up on port $port" }
  Start-Sleep -Seconds 2

  Write-Host "`n=== pixel test ===" -ForegroundColor Cyan
  & $node $script $port $pass
  $exit = $LASTEXITCODE
}
finally {
  if (-not $KeepRunning) { Stop-ObsGracefully }
  if (Test-Path $backup) {
    Copy-Item $backup $cfg -Force
    Remove-Item $backup -Force
    Write-Host "`nobs-websocket config restored (server_enabled=false)" -ForegroundColor DarkGray
  }
}
exit $exit
