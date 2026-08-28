param(
    [string]$ReleasePath,
    [string]$OutputPath,
    [string]$Version,
    [ValidateSet("win", "win64", "winarm")]
    [string]$BuildTarget = "win64",
    [string]$XrayPath
)

$ErrorActionPreference = "Stop"

$scriptPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$homePath = Resolve-Path (Join-Path $scriptPath "..")
$repoPath = Resolve-Path (Join-Path $homePath "..")

if (-not $ReleasePath) {
    $ReleasePath = Join-Path $repoPath "out\Release"
}
$ReleasePath = (Resolve-Path $ReleasePath).Path

if (-not $OutputPath) {
    $OutputPath = Join-Path $ReleasePath "xray-package"
}

if (-not $Version) {
    $versionFile = Join-Path $scriptPath "version"
    $versionLine = Get-Content $versionFile | Where-Object {
        $_ -match "^AppVersionStr\s+"
    } | Select-Object -First 1
    $Version = ($versionLine -split "\s+")[1]
}

if (-not $XrayPath) {
    $XrayPath = Join-Path $ReleasePath "xray.exe"
}

$telegramExe = Join-Path $ReleasePath "Telegram.exe"
$updaterExe = Join-Path $ReleasePath "Updater.exe"
$xrayExe = Resolve-Path $XrayPath

foreach ($path in @($telegramExe, $xrayExe.Path)) {
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$releaseFull = [System.IO.Path]::GetFullPath($ReleasePath)
if (-not $outputFull.StartsWith($releaseFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must stay inside ReleasePath for safe cleanup."
}

if (Test-Path $outputFull) {
    Remove-Item -LiteralPath $outputFull -Recurse -Force
}
New-Item -ItemType Directory -Path $outputFull | Out-Null

$staging = Join-Path $outputFull "Telegram"
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item -LiteralPath $telegramExe -Destination $staging
if (Test-Path $updaterExe -PathType Leaf) {
    Copy-Item -LiteralPath $updaterExe -Destination $staging
}
Copy-Item -LiteralPath $xrayExe.Path -Destination (Join-Path $staging "xray.exe")

if ($BuildTarget -eq "win64") {
    $platform = "x64"
} elseif ($BuildTarget -eq "winarm") {
    $platform = "arm64"
} else {
    $platform = "x86"
}

$d3d = Join-Path $ReleasePath "modules\$platform\d3d\d3dcompiler_47.dll"
if (Test-Path $d3d -PathType Leaf) {
    $d3dTarget = Join-Path $staging "modules\$platform\d3d"
    New-Item -ItemType Directory -Path $d3dTarget | Out-Null
    Copy-Item -LiteralPath $d3d -Destination $d3dTarget
}

$portableMarker = Join-Path $staging "TelegramForcePortable"
New-Item -ItemType Directory -Path $portableMarker | Out-Null
Set-Content `
    -LiteralPath (Join-Path $portableMarker ".portable") `
    -Value "This folder keeps Telegram Desktop in portable mode." `
    -NoNewline

$portableZip = Join-Path $outputFull "tportable-xray-$BuildTarget.$Version.zip"
Compress-Archive -Path $staging -DestinationPath $portableZip -CompressionLevel Optimal

$standalone = Join-Path $outputFull "replace-installed"
New-Item -ItemType Directory -Path $standalone | Out-Null
Copy-Item -Path (Join-Path $staging "*") -Destination $standalone -Recurse
Remove-Item `
    -LiteralPath (Join-Path $standalone "TelegramForcePortable") `
    -Recurse `
    -Force

$setupScript = Join-Path $scriptPath "setup.iss"
$iscc = Get-Command "iscc.exe" -ErrorAction SilentlyContinue
if (-not $iscc) {
    $isccPaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 5\ISCC.exe"
    )
    $iscc = $isccPaths | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
}
if ($iscc) {
    & $iscc `
        "/dMyAppVersion=$Version" `
        "/dMyAppVersionZero=$Version" `
        "/dMyAppVersionFull=xray-$Version" `
        "/dReleasePath=$ReleasePath" `
        "/dMyBuildTarget=$BuildTarget" `
        "/dNoSign=1" `
        $setupScript
    $setupName = if ($BuildTarget -eq "win64") {
        "tsetup-x64.xray-$Version.exe"
    } elseif ($BuildTarget -eq "winarm") {
        "tsetup-arm64.xray-$Version.exe"
    } else {
        "tsetup.xray-$Version.exe"
    }
    $setupPath = Join-Path $ReleasePath $setupName
    if (Test-Path $setupPath -PathType Leaf) {
        Move-Item -LiteralPath $setupPath -Destination $outputFull
    }
}

Write-Host "Package output: $outputFull"
Write-Host "Portable zip: $portableZip"
Write-Host "Installed-folder replacement: $standalone"
