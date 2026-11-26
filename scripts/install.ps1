# Better File Sizes installer.
# Installs the Everything 1.5 plugin. Everything 1.5.0.1350a or newer is required.
param(
    [string]$Stage,
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$Repo = 'Nurdich/better-file-sizes-standalone'
$AssetName = 'BetterFileSizes-windows.zip'
$RawScript = "https://raw.githubusercontent.com/$Repo/main/scripts/install.ps1"
$Readme = "https://github.com/$Repo/blob/main/README.en.md#one-click-install"

function Write-Line([string]$Text) {
    Write-Host $Text
}

function Test-Package([string]$Root) {
    foreach ($arch in @('x64', 'x86')) {
        $plugin = Join-Path $Root "$arch\Everything-plugins\BetterFileSizesEverything.dll"
        $mod = Join-Path $Root "$arch\Everything-plugins\BetterFileSizes\BetterFileSizes.dll"
        if (-not (Test-Path -LiteralPath $plugin) -or -not (Test-Path -LiteralPath $mod)) {
            return $false
        }
    }
    return $true
}

function Get-PeMachine([string]$Path) {
    $fs = [IO.File]::OpenRead($Path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $pe = $br.ReadInt32()
        $fs.Position = $pe + 4
        return $br.ReadUInt16()
    } finally {
        $fs.Dispose()
    }
}

function Get-ArchName([string]$Exe) {
    $machine = Get-PeMachine $Exe
    if ($machine -eq 0x8664) { return 'x64' }
    if ($machine -eq 0x014C) { return 'x86' }
    throw "Unrecognized executable architecture: $Exe"
}

function Test-PluginCapable([string]$Exe) {
    $v = [Diagnostics.FileVersionInfo]::GetVersionInfo($Exe)
    if ($v.FileMajorPart -gt 1) { return $true }
    if ($v.FileMajorPart -lt 1 -or $v.FileMinorPart -lt 5) { return $false }
    if ($v.FileMinorPart -gt 5 -or $v.FileBuildPart -gt 0) { return $true }
    if ($v.FilePrivatePart -eq 0) { return $true }
    return $v.FilePrivatePart -ge 1350
}

function Add-Candidate([System.Collections.Generic.List[string]]$List, [string]$Path) {
    if (-not $Path) { return }
    $Path = $Path.Trim().Trim('"')
    if ($Path -match '^(.*\.exe)') { $Path = $Matches[1] }
    if ((Test-Path -LiteralPath $Path) -and -not $List.Contains($Path)) {
        $List.Add($Path)
    }
}

function Find-Everything {
    $list = New-Object 'System.Collections.Generic.List[string]'
    Get-CimInstance Win32_Process -Filter "Name = 'Everything.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { Add-Candidate $list $_.ExecutablePath }

    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($key in $keys) {
        Get-ItemProperty $key -ErrorAction SilentlyContinue | ForEach-Object {
            if ($_.DisplayName -notlike 'Everything*') { return }
            Add-Candidate $list $_.DisplayIcon
            if ($_.InstallLocation) {
                Add-Candidate $list (Join-Path $_.InstallLocation 'Everything.exe')
            }
        }
    }

    foreach ($dir in @(
        "$env:ProgramFiles\Everything 1.5a",
        "$env:ProgramFiles\Everything",
        ${env:ProgramFiles(x86)} + '\Everything 1.5a',
        ${env:ProgramFiles(x86)} + '\Everything'
    )) {
        if ($dir) { Add-Candidate $list (Join-Path $dir 'Everything.exe') }
    }

    $best = $null
    foreach ($exe in $list) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
        $item = [pscustomobject]@{
            Exe     = $exe
            Dir     = Split-Path -Parent $exe
            Arch    = Get-ArchName $exe
            Version = $info.FileVersion
            Plugin  = Test-PluginCapable $exe
        }
        if ($null -eq $best -or ($item.Plugin -and -not $best.Plugin)) {
            $best = $item
        }
    }
    return $best
}

function Test-CanWrite([string]$Dir) {
    try {
        New-Item -ItemType Directory -Force -Path $Dir | Out-Null
        $probe = Join-Path $Dir ('.bfs-write-' + [guid]::NewGuid().ToString('n'))
        [IO.File]::WriteAllText($probe, 'ok')
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch {
        return $false
    }
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Copy-Ini([string]$Source, [string]$Dest, [string]$FolderSizes) {
    $dir = Split-Path -Parent $Dest
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (Test-Path -LiteralPath $Dest) { return }
    $text = [IO.File]::ReadAllText($Source)
    if ($FolderSizes) {
        $text = [regex]::Replace($text, 'calculateFolderSizes=\w+', "calculateFolderSizes=$FolderSizes")
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [IO.File]::WriteAllText($Dest, $text, $utf8)
}

function Install-Plugin([string]$Package, $Everything) {
    $src = Join-Path $Package $Everything.Arch
    $plugins = Join-Path $Everything.Dir 'plugins'
    $modDir = Join-Path $plugins 'BetterFileSizes'
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item -Force (Join-Path $src 'Everything-plugins\BetterFileSizesEverything.dll') $plugins
    Copy-Item -Force (Join-Path $src 'Everything-plugins\BetterFileSizes\BetterFileSizes.dll') $modDir
    $bundledIni = Join-Path $src 'Everything-plugins\BetterFileSizes\BetterFileSizes.ini'
    $pluginIni = Join-Path $modDir 'BetterFileSizes.ini'
    if (-not (Test-Path -LiteralPath $pluginIni)) {
        Copy-Item $bundledIni $pluginIni
    }
    $userIni = Join-Path $env:LOCALAPPDATA 'BetterFileSizes\BetterFileSizes.ini'
    Copy-Ini $bundledIni $userIni 'everything'
    Write-Line "Installed the Everything plugin: $plugins"
    Write-Line 'Quit Everything completely and open it again. Then enable Better File Sizes under Tools, Options, Plugins.'
    Write-Line 'Folder sizes also need Index folder size turned on in the Everything index options.'
}

function Get-LatestPackage {
    Write-Line 'Downloading the latest release...'
    $headers = @{ 'User-Agent' = 'BetterFileSizes-installer'; 'Accept' = 'application/vnd.github+json' }
    $rel = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repo/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1
    if (-not $asset) { throw "The latest release does not include $AssetName." }
    $root = Join-Path $env:TEMP 'BetterFileSizes-setup'
    if (Test-Path $root) { Remove-Item -Recurse -Force $root }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $zip = Join-Path $root $AssetName
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $zip
    Expand-Archive -LiteralPath $zip -DestinationPath $root -Force
    Remove-Item $zip -Force
    if (-not (Test-Package $root)) { throw 'The release package is incomplete.' }
    return $root
}

function Resolve-Package {
    if ($Stage -and (Test-Package $Stage)) { return $Stage }
    $dirs = @()
    if ($PSScriptRoot) {
        $dirs += $PSScriptRoot
        $parent = Split-Path -Parent $PSScriptRoot
        if ($parent) { $dirs += $parent }
    }
    foreach ($dir in $dirs) {
        if (Test-Package $dir) { return $dir }
    }
    return Get-LatestPackage
}

function Restart-Elevated([string]$Package) {
    if (Test-IsAdmin) { throw 'The plugin folder is not writable.' }
    Write-Line 'Installing the plugin needs administrator permission. A confirmation window will open.'
    $self = Join-Path $env:TEMP 'BetterFileSizes-install.ps1'
    if ($PSCommandPath -and (Test-Path -LiteralPath $PSCommandPath)) {
        Copy-Item -Force $PSCommandPath $self
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri $RawScript -OutFile $self
    }
    $arg = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', $self,
        '-Stage', $Package,
        '-Pause'
    )
    try {
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $arg -Verb RunAs -Wait -PassThru
    } catch {
        throw 'Administrator permission was not granted.'
    }
    if ($proc.ExitCode -ne 0) { throw "Install did not finish (exit code $($proc.ExitCode))." }
    exit 0
}

Write-Line 'Better File Sizes setup'
Write-Line "Instructions: $Readme"
Write-Line ''

$package = Resolve-Package
$everything = Find-Everything
if (-not $everything -or -not $everything.Plugin) {
    throw 'Everything 1.5 with plugin support was not found (1.5.0.1350a or newer is required).'
}

Write-Line "Found Everything $($everything.Version): $($everything.Exe)"
$plugins = Join-Path $everything.Dir 'plugins'
if (-not (Test-CanWrite $plugins)) {
    Restart-Elevated $package
}
Install-Plugin $package $everything

if ($Pause) {
    Write-Line ''
    Read-Host 'Press Enter to close'
}
