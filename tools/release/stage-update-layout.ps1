param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Repository,
    [string]$BuildDir = 'build-retail',
    [string]$OutDir = 'dist-update',
    [string]$Launcher = '',
    [string]$Stub = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Launcher) { $Launcher = "$BuildDir/updater/launcher/PartiesLauncher.exe" }
if (-not $Stub) { $Stub = "$BuildDir/updater/boot/Parties.exe" }

function Write-Outputs([string]$Staged, [string]$Version = '') {
    $lines = @("staged=$Staged", "version=$Version")
    if ($env:GITHUB_OUTPUT) { $lines | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8 }
    $lines | ForEach-Object { Write-Host $_ }
}

function Skip([string]$Reason) {
    Write-Host "::warning::$Reason, so this release has no update layout and is not published to Beta."
    Write-Outputs 'false'
    exit 0
}

if ($Tag -notmatch '^v(\d+\.\d+\.\d+\.\d+)$') { Skip "$Tag is not a vA.B.C.D tag" }
$version = $Matches[1]
if (-not (Test-Path $Launcher)) { Skip "$Launcher was not built" }

$client = "$BuildDir/client/parties_client.exe"
$tool = "$BuildDir/updater/tools/parties_manifest_tool.exe"
$assets = "$OutDir/assets"
New-Item -ItemType Directory -Force $assets | Out-Null

# The retail client is a GUI program, so PowerShell would not wait for it.
$buildInfo = "$OutDir/build-info.json"
$run = Start-Process -FilePath $client -ArgumentList '--write-build-info', "`"$buildInfo`"" -Wait -PassThru -NoNewWindow
if ($run.ExitCode -ne 0 -or -not (Test-Path $buildInfo)) {
    throw "parties_client.exe --write-build-info failed with exit code $($run.ExitCode)"
}

$launcherAsset = "$assets/PartiesLauncher-$version.exe"
$appAsset = "$assets/parties_client-$version.exe"
Copy-Item $Launcher $launcherAsset
Copy-Item $client $appAsset
if (Test-Path $Stub) {
    Copy-Item $Stub "$assets/Parties-$version.exe"
} else {
    Write-Host "::warning::$Stub was not built; the release has no Parties-$version.exe."
}

& $tool entry --build-info $buildInfo `
    --file "PartiesLauncher.exe=$launcherAsset" --file "parties_client.exe=$appAsset" `
    --url-base "https://github.com/$Repository/releases/download/$Tag/" `
    --expect-version $version --out "$OutDir/entry.json"
if ($LASTEXITCODE -ne 0) { throw "parties_manifest_tool entry failed with exit code $LASTEXITCODE" }
Get-Content "$OutDir/build-info.json", "$OutDir/entry.json" | Write-Host
Write-Outputs 'true' $version
