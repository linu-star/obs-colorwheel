param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root   = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel\native'
$shared = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-curves\native'
$bin    = "$shared\w64devkit\w64devkit\bin"
$env:TMP = "$root\tmp"; $env:TEMP = "$root\tmp"
New-Item -ItemType Directory -Force "$root\tmp" | Out-Null
Set-Location $root

$fail = 0

Write-Host "=== 1/3  math + mapping unit tests ===" -ForegroundColor Cyan
& "$bin\gcc.exe" -O2 -o "$root\grade_test.exe" "$root\grade_test.c" "$root\grade_engine.c" -static
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }
& "$root\grade_test.exe"
if ($LASTEXITCODE -ne 0) { $fail++ }

Write-Host "`n=== 2/3  filter/shader build ===" -ForegroundColor Cyan
if (-not $SkipBuild) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\build_dll.ps1" 2>&1 | Out-String | Write-Host
  if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }
}

Write-Host "`n=== 3/3  editor interaction test ===" -ForegroundColor Cyan
& "$bin\g++.exe" -O2 -std=c++17 -DCW_TEST -o "$root\ui_test.exe" `
  "$root\ui_test.cpp" "$root\editor_d2d.cpp" "$root\grade_engine.o" `
  -ld2d1 -ld3d11 -ldxgi -ldwrite -luser32 -lgdi32 -lole32 -luuid `
  -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }
Get-Process ui_test -ErrorAction SilentlyContinue | Stop-Process -Force
& "$root\ui_test.exe"
if ($LASTEXITCODE -ne 0) { $fail++ }

Write-Host ""
if ($fail -eq 0) {
  Write-Host "ALL TEST SUITES PASSED" -ForegroundColor Green
} else {
  Write-Host "$fail SUITE(S) FAILED" -ForegroundColor Red
}
exit $fail
