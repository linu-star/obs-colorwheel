param(
  [string]$Out = 'obs-colorwheel.dll'
)
$ErrorActionPreference = 'Stop'

# Sources live here; toolchain, OBS headers and import libs are reused from the
# obs-curves tree so both plugins build against exactly the same OBS 32.2.2.
$root   = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel\native'
$shared = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-curves\native'
$bin    = "$shared\w64devkit\w64devkit\bin"
$obs    = "$shared\obs-src\obs-studio-32.2.2\libobs"

$env:TMP = "$root\tmp"; $env:TEMP = "$root\tmp"
New-Item -ItemType Directory -Force "$root\tmp" | Out-Null
Set-Location $root

& "$bin\gcc.exe" -c -O2 -o "$root\grade_engine.o" "$root\grade_engine.c"
if ($LASTEXITCODE -ne 0) { throw "grade_engine compile failed" }

& "$bin\gcc.exe" -c -O2 "$root\obs-colorwheel.c" -o "$root\obs-colorwheel.o" `
  "-I$obs" "-I$obs\graphics" "-I$obs\util" "-I$obs\media-io" "-I$obs\audio-monitoring"
if ($LASTEXITCODE -ne 0) { throw "filter compile failed" }

& "$bin\g++.exe" -c -O2 -std=c++17 -o "$root\editor_d2d.o" "$root\editor_d2d.cpp"
if ($LASTEXITCODE -ne 0) { throw "editor compile failed" }

& "$bin\g++.exe" -shared -o "$root\$Out" `
  "$root\obs-colorwheel.o" "$root\grade_engine.o" "$root\editor_d2d.o" `
  "-L$shared\import" -lobs -luser32 -lgdi32 -ld3d11 -ldxgi -ld2d1 -ldwrite -lole32 -luuid `
  -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "link failed" }

$dll = Get-Item "$root\$Out"
$hash = (Get-FileHash $dll.FullName -Algorithm SHA256).Hash
"{0}  {1} bytes  SHA256 {2}" -f $dll.Name, $dll.Length, $hash
