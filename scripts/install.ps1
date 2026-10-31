<#
.SYNOPSIS
  Installs dxgi.dll + dxgi_filter.ini into a game folder, asking which GPU the
  game should be allowed to use.

.PARAMETER Target
  Folder that contains the game's exe (for Unreal games, the one with
  *-Win64-Shipping.exe). Prompted for if omitted.

.PARAMETER Dll
  Path to dxgi.dll. Defaults to a dxgi.dll next to this script.

.PARAMETER Uninstall
  Remove dxgi.dll, dxgi_filter.ini and dxgi_filter.log from -Target.

.EXAMPLE
  .\install.ps1 -Target "D:\SteamLibrary\steamapps\common\Game\Game\Binaries\Win64"
#>
[CmdletBinding()]
param(
    [string]$Target,
    [string]$Dll = (Join-Path $PSScriptRoot 'dxgi.dll'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

if (-not $Target) { $Target = Read-Host 'Folder containing the game exe' }
$Target = $Target.Trim('"')
if (-not (Test-Path -LiteralPath $Target -PathType Container)) { throw "Not a folder: $Target" }

if ($Uninstall) {
    foreach ($f in 'dxgi.dll', 'dxgi_filter.ini', 'dxgi_filter.log') {
        $p = Join-Path $Target $f
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "Removed $p" }
    }
    return
}

if (-not (Test-Path -LiteralPath $Dll)) {
    throw "dxgi.dll not found at '$Dll'. Download it from this repo's Releases page (built by CI), or build it from source."
}

# Ask which GPU the game should use.
$gpus = @(Get-CimInstance Win32_VideoController | Where-Object { $_.Name -and $_.Name -notmatch 'Basic Render|Basic Display' })
if ($gpus.Count -eq 0) { throw 'No GPUs found.' }

Write-Host "`nGPUs in this PC:"
for ($i = 0; $i -lt $gpus.Count; $i++) { Write-Host ("  [{0}] {1}" -f $i, $gpus[$i].Name) }
$choice = Read-Host "`nWhich one should the game use? (number)"
if ($choice -notmatch '^\d+$' -or [int]$choice -ge $gpus.Count) { throw 'Invalid choice.' }
$name = $gpus[[int]$choice].Name

if (Test-Path -LiteralPath (Join-Path $Target 'dxgi.dll')) {
    $a = Read-Host 'A dxgi.dll already exists here (possibly ReShade or another mod). Overwrite? (y/N)'
    if ($a -notmatch '^[yY]') { Write-Host 'Aborted.'; return }
}

Copy-Item -LiteralPath $Dll -Destination (Join-Path $Target 'dxgi.dll') -Force

$ini = @"
[Filter]
KeepNameContains=$name
HideDuplicateWithoutOutputs=1
Log=1
"@
Set-Content -LiteralPath (Join-Path $Target 'dxgi_filter.ini') -Value $ini -Encoding ASCII

Write-Host "`nInstalled. The game will only see: $name"
Write-Host "Check '$Target\dxgi_filter.log' after launching. To remove: .\install.ps1 -Target '$Target' -Uninstall"
