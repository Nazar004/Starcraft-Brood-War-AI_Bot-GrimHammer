[CmdletBinding()]
param(
    [string]$BwapiDir = $env:BWAPI_DIR,
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$PlatformToolset,
    [string]$MSBuildPath
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BwapiDir -or -not (Test-Path -LiteralPath (Join-Path $BwapiDir 'include/BWAPI.h'))) {
    throw 'Specify -BwapiDir pointing to a compatible BWAPI SDK containing include/BWAPI.h.'
}
foreach ($header in @('svnrev.h', 'Debug.h', 'starcraftver.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BwapiDir "include/$header"))) {
        throw "BWAPILIB needs SDK header include/$header. See the submission README.md."
    }
}
$versionHeader = Join-Path $BwapiDir 'include/starcraftver.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw
if ($versionText -notmatch '#define\s+BWAPI_VER\s+"4\.4\.0"') {
    throw 'This AIIDE package requires the official BWAPI 4.4.0 SDK.'
}
if (-not $MSBuildPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $MSBuildPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
    if (-not $MSBuildPath) {
        $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
        if ($command) { $MSBuildPath = $command.Source }
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath)) {
    throw 'MSBuild was not found. Install Visual Studio C++ tools or pass -MSBuildPath.'
}
if (-not $PlatformToolset) {
    # Read the installed default toolset, rather than assume a particular VS edition.
    $vsRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MSBuildPath)))
    $toolsDirectory = Join-Path $vsRoot 'VC/Tools/MSVC'
    if (Test-Path -LiteralPath $toolsDirectory) {
        $version = Get-ChildItem -LiteralPath $toolsDirectory -Directory |
            Where-Object { $_.Name -match '^14\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if ($version) {
            $minorVersion = ([version]$version.Name).Minor
            $PlatformToolset = if ($minorVersion -ge 50) { 'v145' }
                elseif ($minorVersion -ge 30) { 'v143' }
                elseif ($minorVersion -ge 20) { 'v142' }
                else { 'v141' }
        }
    }
    if (-not $PlatformToolset) { $PlatformToolset = 'v143' }
}
$arguments = @('/t:Build', "/p:Configuration=$Configuration", '/p:Platform=Win32', "/p:PlatformToolset=$PlatformToolset", "/p:BWAPI_DIR=$BwapiDir", '/m:1', '/v:minimal', '/nologo')
Push-Location $repoRoot
try {
    $bwapiProject = Join-Path $BwapiDir 'BWAPILIB/BWAPILIB.vcxproj'
    if (-not (Test-Path -LiteralPath $bwapiProject)) {
        throw "Official BWAPI SDK project missing: $bwapiProject"
    }
    $officialLibraryOutput = Join-Path $repoRoot "build/$Configuration"
    $officialLibraryIntermediate = Join-Path $repoRoot "build/obj/OfficialBWAPILIB/$Configuration"
    New-Item -ItemType Directory -Force -Path $officialLibraryOutput, $officialLibraryIntermediate | Out-Null
    $officialArguments = @('/t:Build', "/p:Configuration=$Configuration", '/p:Platform=Win32',
        "/p:PlatformToolset=$PlatformToolset", "/p:BWAPI_DIR=$BwapiDir",
        "/p:OutDir=$officialLibraryOutput\", "/p:IntDir=$officialLibraryIntermediate\",
        '/m:1', '/v:minimal', '/nologo')
    & $MSBuildPath $bwapiProject @officialArguments
    if ($LASTEXITCODE -ne 0) { throw "Official BWAPILIB build failed with exit code $LASTEXITCODE." }
    $officialLib = Join-Path $officialLibraryOutput 'BWAPILIB.lib'
    if (-not (Test-Path -LiteralPath $officialLib)) { throw "Official BWAPILIB output missing: $officialLib" }
    & $MSBuildPath 'Steamhammer/VisualStudio/GrimHammer.sln' @arguments
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
    $dllName = if ($Configuration -eq 'Debug') { 'Steamhammer_d.dll' } else { 'Steamhammer.dll' }
    $dllPath = Join-Path $repoRoot "build/$Configuration/$dllName"
    if (-not (Test-Path -LiteralPath $dllPath)) { throw "Expected output is missing: $dllPath" }
    Write-Host "Built: $dllPath"
} finally { Pop-Location }
