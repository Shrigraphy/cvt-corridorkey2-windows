# CorridorKey 2 for Resolve - current-user Windows OFX install
# Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$plugins = Join-Path $env:LOCALAPPDATA 'OFX\Plugins'
$name = 'CorridorKey2.ofx.bundle'
$target = Join-Path $plugins $name
$source = Join-Path $PSScriptRoot $name
if (!(Test-Path -LiteralPath $source -PathType Container)) {
    $source = Join-Path $PSScriptRoot "build\windows\$name"
}

if ((Test-Path -LiteralPath $target) -and (Get-Process -Name Resolve,Fusion -ErrorAction SilentlyContinue)) {
    throw 'Close Resolve and Fusion Studio before replacing or removing CorridorKey 2.'
}
if ($Uninstall) {
    if (Test-Path -LiteralPath $target -PathType Container) {
        $resolved = (Resolve-Path -LiteralPath $target).Path
        if ($resolved -ne $target) { throw 'Unexpected plugin location' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    return
}

foreach ($file in 'Contents\Win64\CorridorKey2.ofx','Contents\Resources\setup\bin\uv.exe','Contents\Resources\worker\ck2_worker.py') {
    if (!(Test-Path -LiteralPath (Join-Path $source $file) -PathType Leaf)) { throw "Bundle is missing $file. Extract the complete ZIP or build it first." }
}
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
$id = [guid]::NewGuid().ToString('n')
$stage = Join-Path $plugins "corridorkey-stage-$id"
$backup = Join-Path $plugins "corridorkey-backup-$id"
Copy-Item -LiteralPath $source -Destination $stage -Recurse
if (Test-Path -LiteralPath $target) { [IO.Directory]::Move($target, $backup) }
try { [IO.Directory]::Move($stage, $target) }
catch {
    if (Test-Path -LiteralPath $backup) { [IO.Directory]::Move($backup, $target) }
    throw
}
if (Test-Path -LiteralPath $backup) {
    $resolved = (Resolve-Path -LiteralPath $backup).Path
    if ($resolved -ne $backup) { throw 'Unexpected backup location' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$paths = @([Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH', 'User'),
           [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH', 'Machine'), $plugins) -split ';' |
    Where-Object { $_ } | Select-Object -Unique
[Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', ($paths -join ';'), 'User')
Write-Host "Installed $target. Reopen Resolve or Fusion Studio to load it."
