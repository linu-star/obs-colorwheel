param(
  [string]$Version = '1.0.0',
  [switch]$SkipBuild,
  [switch]$Install
)
$ErrorActionPreference = 'Stop'

# Assemble a release folder that works on BOTH sides of the OBS 33 plugin
# layout change:
#
#   OBS 33+  plugins\obs-colorwheel\obs-colorwheel.dll        (new, preferred)
#   OBS <33  plugins\obs-colorwheel\bin\64bit\obs-colorwheel.dll  (legacy)
#   both     plugins\obs-colorwheel\data\*
#
# OBS 33 tries the new path first and skips the legacy copy once the module
# loads, so shipping both is safe and stops the plugin being flagged "Legacy".

$root = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel'
$native = "$root\native"
$dist = "$root\dist"
$stage = "$dist\obs-colorwheel"

if (-not $SkipBuild) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$native\build_dll.ps1"
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$dll = "$native\obs-colorwheel.dll"
if (-not (Test-Path $dll)) { throw "missing $dll" }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\bin\64bit", "$stage\data\effects", "$stage\data\locale" | Out-Null

Copy-Item $dll "$stage\obs-colorwheel.dll" -Force
Copy-Item $dll "$stage\bin\64bit\obs-colorwheel.dll" -Force
Copy-Item "$native\colorwheel.effect" "$stage\data\effects\" -Force
foreach ($ini in Get-ChildItem "$native\*.ini" -ErrorAction SilentlyContinue) {
  Copy-Item $ini.FullName "$stage\data\locale\" -Force
}
Copy-Item "$root\README.md" "$stage\README.md" -Force -ErrorAction SilentlyContinue

# Release zip
$zip = "$dist\obs-colorwheel-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

# Optional install into the live OBS tree (both structures)
if ($Install) {
  $target = 'C:\ProgramData\obs-studio\plugins\obs-colorwheel'
  if (Test-Path $target) { Remove-Item $target -Recurse -Force }
  New-Item -ItemType Directory -Force $target | Out-Null
  Copy-Item "$stage\*" $target -Recurse -Force
  "installed to $target"
}

$dllHash = (Get-FileHash $dll -Algorithm SHA256).Hash
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash
$files = (Get-ChildItem $stage -Recurse -File).Count
"stage : $stage  ($files files)"
"zip   : $zip  ($([math]::Round((Get-Item $zip).Length/1KB,1)) KB)"
"dll   : $([math]::Round((Get-Item $dll).Length/1KB,1)) KB  SHA256 $dllHash"
"zip   : SHA256 $zipHash"
