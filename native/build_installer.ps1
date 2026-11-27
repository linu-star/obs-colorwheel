param(
  [string]$Version = '1.0.0',
  [string]$OutDir = 'D:\Users\Desktop\obs'
)
$ErrorActionPreference = 'Stop'

# Build a self-extracting installer with IExpress (ships with Windows; NSIS and
# Inno Setup are not installed on this machine).
#
# The payload is flattened: subdirectories are recreated by install.cmd, because
# IExpress source groups are easier to keep flat.

$root = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel'
$dist = "$root\dist\obs-colorwheel"
$stage = "$root\dist\_installer"
$exeName = "obs-colorwheel-$Version-setup.exe"
$exePath = Join-Path $OutDir $exeName

if (-not (Test-Path "$dist\obs-colorwheel.dll")) { throw "run package.ps1 first" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item "$dist\obs-colorwheel.dll" "$stage\obs-colorwheel.dll" -Force
Copy-Item "$dist\bin\64bit\obs-colorwheel.dll" "$stage\cw_legacy.dll" -Force
Copy-Item "$dist\data\effects\colorwheel.effect" "$stage\colorwheel.effect" -Force
Copy-Item "$dist\data\locale\en-US.ini" "$stage\en-US.ini" -Force
Copy-Item "$dist\data\locale\zh-CN.ini" "$stage\zh-CN.ini" -Force
Copy-Item "$root\assets\installer-logo.bmp" "$stage\" -Force -ErrorAction SilentlyContinue

$cmd = @'
@echo off
chcp 65001 >nul
setlocal
set "SRC=%~dp0"
set "DST=C:\ProgramData\obs-studio\plugins\obs-colorwheel"

echo ==========================================
echo   obs-colorwheel __VERSION__  -  Color Wheels for OBS
echo ==========================================
echo.

tasklist /fi "imagename eq obs64.exe" 2>nul | find /i "obs64.exe" >nul
if not errorlevel 1 (
  echo [X] OBS is running. Close OBS, then run this installer again.
  echo.
  pause
  exit /b 1
)

if exist "%DST%\obs-colorwheel.dll" (
  echo [i] Existing installation found, upgrading in place.
)

mkdir "%DST%\bin\64bit" 2>nul
mkdir "%DST%\data\effects" 2>nul
mkdir "%DST%\data\locale" 2>nul

copy /y "%SRC%obs-colorwheel.dll" "%DST%\" >nul
copy /y "%SRC%cw_legacy.dll" "%DST%\bin\64bit\obs-colorwheel.dll" >nul
copy /y "%SRC%colorwheel.effect" "%DST%\data\effects\" >nul
copy /y "%SRC%en-US.ini" "%DST%\data\locale\" >nul
copy /y "%SRC%zh-CN.ini" "%DST%\data\locale\" >nul
if exist "%SRC%README.md" copy /y "%SRC%README.md" "%DST%\" >nul

if exist "%DST%\obs-colorwheel.dll" if exist "%DST%\bin\64bit\obs-colorwheel.dll" (
  echo.
  echo [OK] Installed to: %DST%
  echo      Layout: new (OBS 33+) + legacy (OBS 32 and older)
  echo      Restart OBS, then add filter "Color Wheels" on a source.
) else (
  echo [X] Installation failed.
  exit /b 1
)
echo.
pause
'@ -replace '__VERSION__', $Version
[System.IO.File]::WriteAllText("$stage\install.cmd", $cmd, (New-Object System.Text.UTF8Encoding($false)))

$files = @('install.cmd','obs-colorwheel.dll','cw_legacy.dll','colorwheel.effect','en-US.ini','zh-CN.ini')
$strings = ($files | ForEach-Object -Begin { $i = 0 } -Process {
  "FILE$i=`"$_`""
  $i++
}) -join "`r`n"
$srcList = ($files | ForEach-Object -Begin { $i = 0 } -Process {
  "%FILE$i%="
  $i++
}) -join "`r`n"

$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=1
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$exePath
FriendlyName=obs-colorwheel $Version
AppLaunched=install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles
[Strings]
$strings
[SourceFiles]
SourceFiles0=$stage\
[SourceFiles0]
$srcList
"@
$sedPath = "$stage\build.sed"
[System.IO.File]::WriteAllText($sedPath, $sed, [System.Text.Encoding]::Default)

if (Test-Path $exePath) { Remove-Item $exePath -Force }
& "$env:SystemRoot\System32\iexpress.exe" /N /Q $sedPath
# iexpress is a GUI-subsystem binary: $LASTEXITCODE is unreliable (often $null),
# so judge by the artefact it was asked to produce.
Start-Sleep -Seconds 2
if (-not (Test-Path $exePath)) { throw "iexpress produced no installer" }
$h = (Get-FileHash $exePath -Algorithm SHA256).Hash
"installer : $exePath"
"size      : $([math]::Round((Get-Item $exePath).Length/1KB,1)) KB"
"SHA256    : $h"
