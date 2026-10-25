<#
.SYNOPSIS
  Builds Release x64, runs plugin_test, and assembles dist\dbz-kakarot-save-syncer-<version>.zip.

.DESCRIPTION
  The zip holds dbz-kakarot-save-syncer.asi, xinput1_3.dll (Ultimate ASI Loader, from
  third_party\UltimateASILoader), its licence as UltimateASILoader-LICENSE.txt, INSTALL.txt
  (generated from tools\INSTALL.txt) and LICENSE.txt.
  The version comes from the VERSION file at the repository root. Windows PowerShell 5.1 or later.

  plugin_test runs only when both the standard exe and the fixtures folder are known (from
  -TestExe/-TestFixtures or the KAKAROT_EXE/KAKAROT_FIXTURES environment variables); otherwise
  it is skipped with a warning and the zip is still built.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package.ps1
  powershell -ExecutionPolicy Bypass -File tools\package.ps1 -TestExe "<game>\AT\Binaries\Win64\AT-Win64-Shipping.exe" -TestFixtures "<backups folder>"
#>
[CmdletBinding()]
param(
    # MSBuild.exe; found with vswhere when not given.
    [string]$MSBuild = '',
    # Passed to plugin_test.exe as --exe and --fixtures; default from KAKAROT_EXE / KAKAROT_FIXTURES.
    [string]$TestExe = $env:KAKAROT_EXE,
    [string]$TestFixtures = $env:KAKAROT_FIXTURES
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot
$Name = 'dbz-kakarot-save-syncer'

function Find-VsTool([string]$pattern) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $found = & $vswhere -latest -prerelease -products * -find $pattern | Select-Object -First 1
    if ($found -and (Test-Path $found)) { return $found }
    return $null
}

# --- version
$versionFile = Join-Path $Repo 'VERSION'
if (-not (Test-Path $versionFile)) { throw "VERSION file not found at $versionFile" }
$Version = (Get-Content $versionFile -TotalCount 1).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must be MAJOR.MINOR.PATCH, got '$Version'" }
Write-Host "Version $Version"

# --- loader: verify before spending time on the build
$loader = Join-Path $Repo 'third_party\UltimateASILoader\xinput1_3.dll'
if (-not (Test-Path $loader)) { throw "Ultimate ASI Loader not found at $loader" }
$loaderInfo = (Get-Item $loader).VersionInfo
if (($loaderInfo.FileDescription -ne 'Ultimate ASI Loader') -or ($loaderInfo.ProductName -notlike 'Ultimate-ASI-Loader*')) {
    throw "$loader does not identify as Ultimate ASI Loader (FileDescription '$($loaderInfo.FileDescription)', ProductName '$($loaderInfo.ProductName)')"
}
$loaderVersion = $loaderInfo.ProductVersion
$loaderSha256 = (Get-FileHash $loader -Algorithm SHA256).Hash
Write-Host "Ultimate ASI Loader: product version $loaderVersion ($($loaderInfo.ProductName))"
Write-Host "Ultimate ASI Loader: SHA-256 $loaderSha256"

# --- build
if (-not $MSBuild) { $MSBuild = Find-VsTool 'MSBuild\**\Bin\amd64\MSBuild.exe' }
if (-not $MSBuild) { $MSBuild = Find-VsTool 'MSBuild\**\Bin\MSBuild.exe' }
if ((-not $MSBuild) -or (-not (Test-Path $MSBuild))) { throw 'MSBuild.exe not found; pass -MSBuild <path>' }
$solution = Join-Path $Repo "$Name.slnx"
Write-Host "Building Release x64 with $MSBuild"
& $MSBuild $solution /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed (MSBuild exit code $LASTEXITCODE)" }

$bin = Join-Path $Repo 'x64\Release'
$asi = Join-Path $bin "$Name.asi"
if (-not (Test-Path $asi)) { throw "Build output not found: $asi" }

# --- the .asi must not need the Visual C++ redistributable
$dumpbin = Find-VsTool 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe'
if ($dumpbin) {
    $deps = @(& $dumpbin /nologo /dependents $asi | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
    Write-Host ".asi dependencies: $($deps -join ', ')"
    $crt = @($deps | Where-Object { $_ -match '^(msvcp|vcruntime|ucrtbase|api-ms-win-crt-)' })
    if ($crt.Count -gt 0) { throw ".asi depends on the C++ runtime DLLs: $($crt -join ', ')" }
} else {
    Write-Warning 'dumpbin.exe not found; the .asi dependency check was skipped'
}

# --- tests
$test = Join-Path $bin 'plugin_test.exe'
if ($TestExe -and $TestFixtures) {
    Write-Host 'Running plugin_test.exe'
    & $test --exe $TestExe --fixtures $TestFixtures
    if ($LASTEXITCODE -ne 0) { throw "plugin_test.exe failed (exit code $LASTEXITCODE)" }
} else {
    Write-Warning 'plugin_test.exe was SKIPPED: set KAKAROT_EXE and KAKAROT_FIXTURES (or pass -TestExe and -TestFixtures) to run it'
}

# --- stage
$dist = Join-Path $Repo 'dist'
$stage = Join-Path $dist "$Name-$Version"
$zip = Join-Path $dist "$Name-$Version.zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item $asi (Join-Path $stage "$Name.asi")
Copy-Item $loader (Join-Path $stage 'xinput1_3.dll')
Copy-Item (Join-Path $Repo 'LICENSE.txt') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $Repo 'third_party\UltimateASILoader\LICENSE.txt') (Join-Path $stage 'UltimateASILoader-LICENSE.txt')

$install = Get-Content (Join-Path $PSScriptRoot 'INSTALL.txt')
$install = $install | ForEach-Object {
    $_.Replace('@VERSION@', $Version).Replace('@LOADER_VERSION@', $loaderVersion).Replace('@LOADER_SHA256@', $loaderSha256)
}
[System.IO.File]::WriteAllText((Join-Path $stage 'INSTALL.txt'), (($install -join "`r`n") + "`r`n"), [System.Text.Encoding]::ASCII)

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force

# --- report
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    Write-Host ''
    Write-Host "Created $zip"
    $archive.Entries | Sort-Object FullName | ForEach-Object { '  {0,10:N0}  {1}' -f $_.Length, $_.FullName } | Write-Host
} finally {
    $archive.Dispose()
}
Write-Host "  .asi SHA-256          $((Get-FileHash $asi -Algorithm SHA256).Hash)"
Write-Host "  xinput1_3.dll SHA-256 $loaderSha256 (Ultimate ASI Loader $loaderVersion)"
