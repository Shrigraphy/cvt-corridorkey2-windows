# CorridorKey 2 for Resolve - NVIDIA CUDA model setup
# Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
param(
    [string]$Folder = "$env:USERPROFILE\CorridorKey",
    [string]$RuntimeFolder,
    [switch]$RuntimeOnly,
    [switch]$AcceptLicence
)

$ErrorActionPreference = 'Stop'
$Folder = [IO.Path]::GetFullPath($Folder)
$RuntimeFolder = if ($RuntimeFolder) { [IO.Path]::GetFullPath($RuntimeFolder) } else { $Folder }
$commit = '485f5e9ab759ead6138cc990776184b59d6ea971'
$code = Join-Path $Folder 'CorridorKey2.0'
$licence = Join-Path $Folder '.licence-accepted'
$python = Join-Path $RuntimeFolder '.venv\Scripts\python.exe'
$worker = Join-Path (Split-Path -Parent $PSScriptRoot) 'worker\ck2_worker.py'
$uv = Join-Path $PSScriptRoot 'bin\uv.exe'
if (!(Test-Path -LiteralPath $uv)) {
    $uv = (Get-Command uv -ErrorAction Stop).Source
}

function Invoke-Native([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed with exit code $LASTEXITCODE" }
}

function Confirm-Hashes([string]$directory, [string]$manifest) {
    foreach ($entry in Get-Content -LiteralPath (Join-Path $PSScriptRoot $manifest)) {
        if ($entry -notmatch '^(?<hash>[a-f0-9]{64})\s+(?<file>.+)$') { throw "Invalid manifest $manifest" }
        $path = Join-Path $directory $Matches.file
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ne $Matches.hash) {
            throw "Missing file or checksum mismatch in ${manifest}: $($Matches.file)"
        }
    }
}

if (!$RuntimeOnly -and !(Test-Path -LiteralPath $licence -PathType Leaf)) {
    Write-Host 'CorridorKey 2.0 by Corridor Digital uses the Corridor Key Licence (CC BY-NC-SA 4.0 with additional terms).'
    Write-Host 'Read it at https://github.com/nikopueringer/CorridorKey2.0/blob/main/LICENSE'
    if (!$AcceptLicence -and (Read-Host 'Type yes to accept the licence and continue') -cne 'yes') {
        throw 'The Corridor Key Licence must be accepted before downloading the model.'
    }
    New-Item -ItemType Directory -Path $Folder -Force | Out-Null
    [IO.File]::WriteAllText($licence, (Get-Date -Format o))
}

New-Item -ItemType Directory -Path $RuntimeFolder -Force | Out-Null
$env:UV_PYTHON_INSTALL_DIR = Join-Path $RuntimeFolder 'python'
$env:UV_CACHE_DIR = Join-Path $RuntimeFolder '.uv-cache'
$env:UV_NO_PROGRESS = '1'
if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
    Invoke-Native $uv @('venv', '--managed-python', '--python', '3.12', (Join-Path $RuntimeFolder '.venv'))
}
Write-Host 'Installing the pinned CUDA runtime'
$requirements = Join-Path $PSScriptRoot 'requirements-windows.txt'
$utils = Join-Path $PSScriptRoot 'wheels\utils3d-1.7-py3-none-any.whl'
Invoke-Native $uv @('pip', 'install', '--python', $python, '-r', $requirements, $utils)
Invoke-Native $python @('-I', '-B', '-c', 'import torch, OpenEXR, cv2, torchvision; assert torch.cuda.is_available(); print(torch.__version__, torch.cuda.get_device_name(0))')
if ($RuntimeOnly) { return }

if (!(Test-Path -LiteralPath $code -PathType Container)) {
    $stage = Join-Path $Folder ('setup-' + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $archive = Join-Path $stage 'code.zip'
    Invoke-Native 'curl.exe' @('--fail', '--location', '--retry', '3', '--silent', '--show-error',
        '--output', $archive, "https://codeload.github.com/nikopueringer/CorridorKey2.0/zip/$commit")
    Expand-Archive -LiteralPath $archive -DestinationPath $stage
    $source = Join-Path $stage "CorridorKey2.0-$commit"
    Confirm-Hashes $source 'corridorkey_files.sha256'
    Move-Item -LiteralPath $source -Destination $code
    Remove-Item -LiteralPath $archive
    Remove-Item -LiteralPath $stage
}
Confirm-Hashes $code 'corridorkey_files.sha256'

Write-Host 'Downloading and verifying the CorridorKey model and foundation weights'
$env:HF_HUB_DISABLE_TELEMETRY = '1'
$env:HF_HUB_DISABLE_IMPLICIT_TOKEN = '1'
$env:HF_HOME = Join-Path $Folder '.hf-cache'
Invoke-Native $python @('-B', '-u', (Join-Path $code 'scripts\download_weights.py'))
Confirm-Hashes $code 'weights.sha256'
Invoke-Native $python @('-B', '-u', $worker, 'check', $code)

$settings = Join-Path $env:USERPROFILE '.config\cvt-corridorkey2\settings.json'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $settings) | Out-Null
$config = if (Test-Path -LiteralPath $settings -PathType Leaf) {
    Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json
} else { [pscustomobject]@{} }
$config | Add-Member -NotePropertyName 'model_folder' -NotePropertyValue ($code.Replace('\','/')) -Force
$config | Add-Member -NotePropertyName 'python' -NotePropertyValue ($python.Replace('\','/')) -Force
$json = $config | ConvertTo-Json -Compress
[IO.File]::WriteAllText("$settings.tmp", $json)
if (Test-Path -LiteralPath $settings) { [IO.File]::Replace("$settings.tmp", $settings, $null) }
else { [IO.File]::Move("$settings.tmp", $settings) }
Write-Host 'CorridorKey 2 is ready. Reopen Resolve or create a new node to pick up the settings.'
