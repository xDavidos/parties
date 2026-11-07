# Setup installs the stub once and
# automatic updates never replace it, so these limits keep it auditable.
param([Parameter(Mandatory)][string]$BuildDir)
$ErrorActionPreference = 'Stop'

$repo = Resolve-Path "$PSScriptRoot/../.."
$exe = Join-Path $BuildDir 'updater/boot/Parties.exe'
$failures = [System.Collections.Generic.List[string]]::new()

$maxLines = 600
$sources = Get-ChildItem "$repo/updater/boot" -Recurse -File -Include *.cpp, *.h, *.rc
$lines = ($sources | ForEach-Object { @(Get-Content $_.FullName).Count } | Measure-Object -Sum).Sum
Write-Host "Stub sources: $lines lines, limit $maxLines"
if ($lines -gt $maxLines) { $failures.Add("stub sources have $lines lines, limit $maxLines") }

$allowedLibs = 'kernel32', 'user32', 'comctl32', 'shell32', 'bcrypt', 'parties_update_boot'
$ninja = Get-Content (Join-Path $BuildDir 'build.ninja')
$at = [array]::FindIndex($ninja, [Predicate[string]] { param($l) $l -match '^build updater[\\/]boot[\\/]Parties\.exe:' })
$linkLine = $null
for ($i = $at + 1; $at -ge 0 -and $i -lt $ninja.Count -and $ninja[$i] -ne ''; $i++) {
    if ($ninja[$i] -match '^\s+LINK_LIBRARIES = (.*)$') { $linkLine = $Matches[1] }
}
if ($null -eq $linkLine) {
    $failures.Add('no LINK_LIBRARIES line for Parties.exe in build.ninja')
} else {
    Write-Host "Link libraries: $linkLine"
    foreach ($lib in $linkLine -split '\s+' | Where-Object { $_ }) {
        $name = [IO.Path]::GetFileNameWithoutExtension($lib).ToLowerInvariant()
        if ($name -notin $allowedLibs) { $failures.Add("link line has $lib") }
    }
}

$allowedDlls = 'kernel32.dll', 'user32.dll', 'comctl32.dll', 'shell32.dll', 'bcrypt.dll'
$readobj = (Get-Command llvm-readobj -ErrorAction SilentlyContinue).Source
if (-not $readobj) { $readobj = 'C:\Program Files\LLVM\bin\llvm-readobj.exe' }
$dlls = & $readobj --coff-imports $exe | Select-String '^\s*Name: (.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
if ($LASTEXITCODE -ne 0 -or -not $dlls) { $failures.Add("could not read the imports of $exe") }
Write-Host "Imported DLLs: $($dlls -join ', ')"
foreach ($dll in $dlls) {
    if ($dll.ToLowerInvariant() -notin $allowedDlls) { $failures.Add("Parties.exe imports $dll") }
}

$testing = Select-String -Path (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^PARTIES_UPDATE_TESTING:BOOL=(ON|TRUE|1)$' -Quiet
if (-not $testing) {
    $bytes = [IO.File]::ReadAllBytes($exe)
    $text = [Text.Encoding]::Unicode.GetString($bytes) + [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)
    if ($text.Contains('--test-')) { $failures.Add('Parties.exe contains testing switches without PARTIES_UPDATE_TESTING') }
    else { Write-Host 'No testing switches in Parties.exe' }
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Error "check_boot_stub: $_" -ErrorAction Continue }
    exit 1
}
Write-Host 'Boot stub checks passed'
