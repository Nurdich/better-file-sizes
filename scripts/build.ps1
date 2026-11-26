# Build Better File Sizes with MinGW and pack dist/BetterFileSizes-windows.zip.
# The sources use GCC extensions, so this script does not use MSVC.
param(
    [ValidateSet('all', 'x64', 'x86')]
    [string]$Arch = 'all',
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$OutRoot = Join-Path $Root 'out'
$DistRoot = Join-Path $Root 'dist'
$Products = @(
    'BetterFileSizes.dll',
    'BetterFileSizesEverything.dll'
)

function Write-Line([string]$Text) {
    Write-Host $Text
}

function Find-MsysRoot {
    $hints = New-Object System.Collections.Generic.List[string]
    foreach ($path in @($env:MSYS2_ROOT, 'C:\msys64', 'C:\msys32')) {
        if ($path) { $hints.Add($path) }
    }
    if ($env:SCOOP) { $hints.Add((Join-Path $env:SCOOP 'apps\msys2\current')) }
    $hints.Add((Join-Path $env:USERPROFILE 'scoop\apps\msys2\current'))
    $scoop = Get-Command scoop, scoop.ps1, scoop.cmd -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($scoop) {
        $scoopRoot = Split-Path (Split-Path $scoop.Source)
        $hints.Add((Join-Path $scoopRoot 'apps\msys2\current'))
    }
    foreach ($path in $hints) {
        if (Test-Path (Join-Path $path 'mingw64\bin\g++.exe')) { return $path }
    }
    return $null
}

function Find-QtBin {
    if (-not (Test-Path 'C:\Qt\Tools')) { return $null }
    $dirs = Get-ChildItem 'C:\Qt\Tools' -Directory -Filter 'mingw*_64' | Sort-Object Name -Descending
    foreach ($dir in $dirs) {
        $gpp = Join-Path $dir.FullName 'bin\g++.exe'
        if (Test-Path -LiteralPath $gpp) { return (Join-Path $dir.FullName 'bin') }
    }
    return $null
}

function New-Toolset([string]$Bin, [string]$CxxName, [string]$CcName, [string]$RcName) {
    $cxx = Join-Path $Bin $CxxName
    $cc = Join-Path $Bin $CcName
    $rc = Join-Path $Bin $RcName
    if (-not (Test-Path -LiteralPath $cxx)) { return $null }
    if (-not (Test-Path -LiteralPath $cc)) { $cc = $cxx }
    if (-not (Test-Path -LiteralPath $rc)) { return $null }
    return [pscustomobject]@{
        Bin = $Bin
        Cxx = $cxx.Replace('\', '/')
        Cc  = $cc.Replace('\', '/')
        Rc  = $rc.Replace('\', '/')
    }
}

function Find-Toolset([string]$Name) {
    $prefix = if ($Name -eq 'x64') { 'x86_64-w64-mingw32' } else { 'i686-w64-mingw32' }
    $onPath = Get-Command "$prefix-g++-posix", "$prefix-g++" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) {
        $bin = Split-Path $onPath.Source
        $leaf = Split-Path $onPath.Source -Leaf
        $ccLeaf = $leaf -replace 'g\+\+', 'gcc'
        $rcLeaf = $leaf -replace 'g\+\+(-posix)?.*', 'windres$1.exe'
        if ($rcLeaf -notmatch '\.exe$') { $rcLeaf = 'windres.exe' }
        $set = New-Toolset $bin $leaf $ccLeaf 'windres.exe'
        if ($set) { return $set }
    }

    $bins = @()
    $msys = Find-MsysRoot
    if ($msys) {
        $folder = if ($Name -eq 'x64') { 'mingw64' } else { 'mingw32' }
        $bins += Join-Path $msys "$folder\bin"
    }
    if ($Name -eq 'x64') {
        $qt = Find-QtBin
        if ($qt) { $bins += $qt }
    }
    foreach ($bin in $bins) {
        $set = New-Toolset $bin 'g++.exe' 'gcc.exe' 'windres.exe'
        if ($set) { return $set }
    }
    return $null
}

function Invoke-Logged([string]$Cmake, [string[]]$ArgumentList, [string]$Log) {
    $quoted = foreach ($arg in $ArgumentList) {
        if ($arg -match '[\s"]') { '"' + ($arg -replace '"', '\"') + '"' } else { $arg }
    }
    $cmakeQuoted = if ($Cmake -match '[\s"]') { '"' + $Cmake + '"' } else { $Cmake }
    $line = $cmakeQuoted + ' ' + ($quoted -join ' ')
    cmd /c "$line > `"$Log`" 2>&1"
    return $LASTEXITCODE
}

function Find-Product([string]$BuildDir, [string]$Name) {
    $hits = @(Get-ChildItem -Path $BuildDir -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
    if ($hits.Count -eq 0) { throw "Build did not produce $Name in $BuildDir." }
    return $hits[0].FullName
}

function Build-One([string]$Cmake, $Toolset, [string]$Name) {
    $buildDir = Join-Path $OutRoot "mingw-$Name"
    if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $oldPath = $env:PATH
    $env:PATH = $Toolset.Bin + ';' + $oldPath
    try {
        Write-Line "Configuring $Name with $($Toolset.Cxx)"
        $configureLog = Join-Path $buildDir 'configure.log'
        $configure = @(
            '-S', $Root,
            '-B', $buildDir,
            '-G', 'Ninja',
            "-DCMAKE_BUILD_TYPE=$Config",
            "-DCMAKE_C_COMPILER=$($Toolset.Cc)",
            "-DCMAKE_CXX_COMPILER=$($Toolset.Cxx)",
            "-DCMAKE_RC_COMPILER=$($Toolset.Rc)"
        )
        if ((Invoke-Logged $Cmake $configure $configureLog) -ne 0) {
            Get-Content -LiteralPath $configureLog -Tail 30 | ForEach-Object { Write-Line $_ }
            throw "CMake configure failed for $Name. Log: $configureLog"
        }
        Write-Line "Building $Name..."
        $buildLog = Join-Path $buildDir 'build.log'
        if ((Invoke-Logged $Cmake @('--build', $buildDir, '--parallel') $buildLog) -ne 0) {
            Get-Content -LiteralPath $buildLog -Tail 40 | ForEach-Object { Write-Line $_ }
            throw "Build failed for $Name. Log: $buildLog"
        }
    } finally {
        $env:PATH = $oldPath
    }
    $found = @{}
    foreach ($product in $Products) {
        $found[$product] = Find-Product $buildDir $product
    }
    return $found
}

function Copy-Package([hashtable]$Built) {
    $pkg = Join-Path $DistRoot 'pkg'
    if (Test-Path -LiteralPath $DistRoot) { Remove-Item -LiteralPath $DistRoot -Recurse -Force }
    $ini = Join-Path $Root 'config\BetterFileSizes.ini'
    foreach ($name in $Built.Keys) {
        $src = $Built[$name]
        $dst = Join-Path $pkg $name
        New-Item -ItemType Directory -Force -Path (Join-Path $dst 'Everything-plugins\BetterFileSizes') | Out-Null
        Copy-Item -Force $src['BetterFileSizesEverything.dll'] (Join-Path $dst 'Everything-plugins\BetterFileSizesEverything.dll')
        Copy-Item -Force $src['BetterFileSizes.dll'] (Join-Path $dst 'Everything-plugins\BetterFileSizes\BetterFileSizes.dll')
        Copy-Item -Force $ini (Join-Path $dst 'Everything-plugins\BetterFileSizes\BetterFileSizes.ini')
    }
    $scripts = Join-Path $pkg 'scripts'
    New-Item -ItemType Directory -Force -Path $scripts | Out-Null
    Copy-Item -Force (Join-Path $PSScriptRoot 'install.ps1') (Join-Path $scripts 'install.ps1')
    $zip = Join-Path $DistRoot 'BetterFileSizes-windows.zip'
    Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zip
    return $zip
}

$Cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $Cmake) {
    throw 'cmake was not found on PATH.'
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw 'ninja was not found on PATH.'
}

$names = if ($Arch -eq 'all') { @('x64', 'x86') } else { @($Arch) }
$toolsets = @{}
foreach ($name in $names) {
    $toolsets[$name] = Find-Toolset $name
    if (-not $toolsets[$name]) {
        throw "No MinGW $name compiler found. Install MSYS2 mingw-w64 for that architecture, or put $name g++ on PATH."
    }
}

Write-Line "Source: $Root"
$built = @{}
foreach ($name in $names) {
    $built[$name] = Build-One $Cmake $toolsets[$name] $name
}

if ($names.Count -eq 2) {
    $zip = Copy-Package $built
    Write-Line "Package: $zip"
} else {
    Write-Line "Built $Arch under $(Join-Path $OutRoot "mingw-$Arch"). Both x64 and x86 are required for the release zip."
}
