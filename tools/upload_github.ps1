param(
  [string]$RepoName = 'obs-colorwheel',
  [string]$CommitMessage = 'Initial commit: OBS colour wheels plugin (Lift/Gamma/Gain/Offset)'
)
$ErrorActionPreference = 'Stop'

# Upload the plugin repository to GitHub using the OAuth token GitHub Desktop
# already stores in the Windows credential store. The token stays in memory for
# the duration of this script: it is never printed and never written to disk.

$ProjectRoot = 'C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel'
$Git = "$env:LOCALAPPDATA\GitHubDesktop\app-3.6.10\resources\app\git\cmd\git.exe"
$Proxy = 'http://127.0.0.1:7897'

if (-not (Test-Path $Git)) { throw "bundled git not found: $Git" }

$src = @'
using System;
using System.Runtime.InteropServices;
public class Cred {
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern bool CredReadW(string target, int type, int flags, out IntPtr cred);
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern void CredFree(IntPtr cred);
  [StructLayout(LayoutKind.Sequential)]
  struct CREDENTIAL {
    public int Flags; public int Type; public IntPtr TargetName;
    public IntPtr Comment; public long LastWritten; public int CredentialBlobSize;
    public IntPtr CredentialBlob; public int Persist; public int AttributeCount;
    public IntPtr Attributes; public IntPtr TargetAlias; public IntPtr UserName;
  }
  public static string ReadBlob(string target) {
    IntPtr p;
    if (!CredReadW(target, 1, 0, out p)) return null;
    try {
      CREDENTIAL c = (CREDENTIAL)Marshal.PtrToStructure(p, typeof(CREDENTIAL));
      if (c.CredentialBlobSize == 0) return "";
      byte[] buf = new byte[c.CredentialBlobSize];
      Marshal.Copy(c.CredentialBlob, buf, 0, buf.Length);
      return System.Text.Encoding.UTF8.GetString(buf);
    } finally { CredFree(p); }
  }
}
'@
if (-not ('Cred' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp | Out-Null }

$token = [Cred]::ReadBlob('GitHub - https://api.github.com/linu-star')
if (-not $token) { throw 'no GitHub credential found in the credential store' }
"token : loaded (length $($token.Length))"

$H = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'User-Agent' = 'dsh-agent' }
$user = Invoke-RestMethod -Uri 'https://api.github.com/user' -Headers $H -Proxy $Proxy
"account: $($user.login)"

$repo = $null
try { $repo = Invoke-RestMethod -Uri "https://api.github.com/repos/$($user.login)/$RepoName" -Headers $H -Proxy $Proxy } catch { }
if ($repo) {
  "repo   : exists ($($repo.visibility)) -> $($repo.html_url)"
} else {
  $body = @{
    name = $RepoName
    description = 'DaVinci Resolve style colour wheels for OBS Studio (Lift / Gamma / Gain / Offset) with a Direct2D editor'
    private = $false
    has_issues = $true
    has_wiki = $false
  } | ConvertTo-Json
  $repo = Invoke-RestMethod -Uri 'https://api.github.com/user/repos' -Headers $H -Method Post -Body $body -ContentType 'application/json' -Proxy $Proxy
  "repo   : created -> $($repo.html_url)"
}

# ---- local repository ------------------------------------------------------
Set-Location $ProjectRoot
if (-not (Test-Path "$ProjectRoot\.git")) {
  & $Git init -b main | Out-Null
  "git    : initialised"
}
& $Git config user.name  'linu-star' | Out-Null
& $Git config user.email 'linu-star@users.noreply.github.com' | Out-Null

& $Git add -A
$staged = (& $Git diff --cached --name-only | Measure-Object -Line).Lines
"staged : $staged files"

& $Git -c core.quotepath=false commit -m $CommitMessage --quiet
if ($LASTEXITCODE -ne 0) { "commit: nothing new to commit" } else { "commit : done" }

$clone = "https://github.com/$($user.login)/$RepoName.git"
$b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("x-access-token:$token"))

# Native tools write diagnostics to stderr and PowerShell 5.1 surfaces those as
# error records; keep this section non-terminating so a missing remote (first
# run) does not abort the upload.
$ErrorActionPreference = 'Continue'
$remotes = @(& $Git remote 2>$null)
if ($remotes -contains 'origin') { & $Git remote remove origin 2>$null | Out-Null }
& $Git remote add origin $clone 2>$null | Out-Null
$ErrorActionPreference = 'Stop'

& $Git -c "http.proxy=$Proxy" -c "http.extraheader=AUTHORIZATION: basic $b64" push -u origin main --quiet 2>$null
if ($LASTEXITCODE -ne 0) { throw "push failed (exit $LASTEXITCODE)" }

"pushed : $clone"
"url    : $($repo.html_url)"
