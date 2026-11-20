[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $EngineRoot,
    [Parameter(Mandatory = $true)]
    [string] $PatchFile,
    [switch] $Apply
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-FullPath([string] $Path) {
    return [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
}
function Invoke-Git([string[]] $Arguments) {
    $output = & git @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ("git {0} failed:{1}{2}" -f ($Arguments -join ' '), [Environment]::NewLine, ($output -join [Environment]::NewLine))
    }
    return @($output)
}

$EngineRoot = Resolve-FullPath $EngineRoot
$PatchFile = Resolve-FullPath $PatchFile
$BuildVersionFile = Join-Path $EngineRoot 'Engine\Build\Build.version'
if (-not (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Source\Runtime\Renderer') -PathType Container)) {
    throw "EngineRoot is not an Unreal Engine source root: $EngineRoot"
}
if (-not (Test-Path -LiteralPath $BuildVersionFile -PathType Leaf)) {
    throw "Cannot find Engine\Build\Build.version under EngineRoot: $EngineRoot"
}
if ([System.IO.Path]::GetExtension($PatchFile) -ne '.patch') {
    throw "Only real unified-diff .patch files are accepted. Markdown design/patch notes are not executable: $PatchFile"
}
$version = Get-Content -LiteralPath $BuildVersionFile -Raw | ConvertFrom-Json
if ([int]$version.MajorVersion -ne 4 -or [int]$version.MinorVersion -ne 27) {
    throw ("Wrong Engine version. Expected UE 4.27.x; found {0}.{1}." -f $version.MajorVersion, $version.MinorVersion)
}
$firstLine = Get-Content -LiteralPath $PatchFile -TotalCount 1
if ($firstLine -notmatch '^diff --git ') { throw 'Patch does not begin with a Git unified-diff header.' }

Push-Location $EngineRoot
try {
    Invoke-Git @('apply', '--check', '--whitespace=error', $PatchFile) | Out-Null
    $numstat = Invoke-Git @('apply', '--numstat', $PatchFile)
    $relativePaths = New-Object System.Collections.Generic.List[string]
    foreach ($line in $numstat) {
        if ([string]::IsNullOrWhiteSpace([string]$line)) { continue }
        $parts = ([string]$line) -split [char]9, 3
        if ($parts.Count -ne 3) { throw "Unsupported patch entry; refusing to guess its target: $line" }
        $relative = $parts[2]
        if ($relative -match '[\r\n]' -or [System.IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') {
            throw "Unsafe path in patch: $relative"
        }
        $target = [System.IO.Path]::GetFullPath((Join-Path $EngineRoot $relative))
        $prefix = $EngineRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        if (-not $target.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Patch target escapes EngineRoot: $relative"
        }
        $relativePaths.Add($relative)
    }
    if ($relativePaths.Count -eq 0) { throw 'Patch contains no file changes.' }

    Write-Host ''
    Write-Host 'UE 4.27 patch preflight passed.' -ForegroundColor Green
    Write-Host "Engine root : $EngineRoot"
    Write-Host "Patch       : $PatchFile"
    Write-Host 'Targets:'
    foreach ($relative in $relativePaths) {
        $target = Join-Path $EngineRoot $relative
        if (Test-Path -LiteralPath $target -PathType Leaf) { Write-Host "  MODIFY  $relative" }
        else { Write-Host "  CREATE  $relative" }
    }
    if (-not $Apply) {
        Write-Host ''
        Write-Host 'DRY RUN ONLY: no files were changed.' -ForegroundColor Yellow
        Write-Host 'Review the target list, then rerun with -Apply to create backups and apply.'
        return
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupRoot = Join-Path $EngineRoot (".software-rt-patch-backups\$stamp")
    $manifest = New-Object System.Collections.Generic.List[string]
    foreach ($relative in $relativePaths) {
        $target = Join-Path $EngineRoot $relative
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $backup = Join-Path $backupRoot $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
            Copy-Item -LiteralPath $target -Destination $backup
            $manifest.Add(('BACKUP' + [char]9 + $relative))
        } else {
            $manifest.Add(('NEW' + [char]9 + $relative))
        }
    }
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    Set-Content -LiteralPath (Join-Path $backupRoot 'manifest.txt') -Value $manifest -Encoding UTF8
    try {
        Invoke-Git @('apply', '--whitespace=error', $PatchFile) | Out-Null
    } catch {
        Write-Warning "Patch application failed. Backups (where applicable) are in: $backupRoot"
        throw
    }
    Write-Host ''
    Write-Host 'Patch applied.' -ForegroundColor Green
    Write-Host "Backups/manifest: $backupRoot"
    Write-Host 'This script does not compile the Engine or prove runtime correctness.'
}
finally {
    Pop-Location
}
