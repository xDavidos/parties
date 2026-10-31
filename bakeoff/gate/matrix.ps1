param(
    [Parameter(Mandatory)] [string]$Tag,
    [Parameter(Mandatory)] [string[]]$Variants,
    [string[]]$Only = @(),
    [string]$Env = ''
)
foreach ($pair in ($Env -split ';' | Where-Object { $_ })) {
    $k, $v = $pair -split '=', 2
    [Environment]::SetEnvironmentVariable($k, $v)
    "env $k=$v" | Out-Host
}
$ErrorActionPreference = 'Continue'
$Variants = $Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ }
$Only = $Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ }
if ($Variants | Where-Object { $_ -notin 'off', 'v1', 'v2', 'v3', 'v4', 'v5', 'v6', 'v7', 'v8' }) { throw "unknown variant in $Variants" }
Set-Location 'D:/IT/parties/.claude/worktrees/verify-stream-hillclimb'
$ps1 = '.claude/skills/verify-parties-stream/scripts/verify-stream.ps1'
$tmp = 'C:/Users/david/.claude/jobs/2a988a6f/tmp'
$log = "$tmp/matrix-$Tag.log"

$scenarios = @(
    @{ Name = 'A'; Codec = 'H.265'; Mbps = 12; Link = 10;   Delay = 10; Secs = 45 },
    @{ Name = 'C'; Codec = 'H.264'; Mbps = 4;  Link = 20;   Delay = 10; Secs = 45 },
    @{ Name = 'D'; Codec = 'H.265'; Mbps = 12; Link = 12.5; Delay = 10; Secs = 60 },
    @{ Name = 'E'; Codec = 'H.265'; Mbps = 12; Link = 8;    Delay = 40; Secs = 45 },
    @{ Name = 'B'; Codec = 'H.265'; Mbps = 12; Link = 40;   Delay = 10; Secs = 30 }
)
if ($Only.Count) { $scenarios = $scenarios | Where-Object { $Only -contains $_.Name } }

$dirs = @()
foreach ($v in $Variants) {
    foreach ($s in $scenarios) {
        $id = "bo-$Tag-$v-$($s.Name)"
        "=== $id $(Get-Date -Format HH:mm:ss)" | Tee-Object -FilePath $log -Append
        $env:PARTIES_PROTO_GATE = $v
        & pwsh -NoProfile -File $ps1 run -Run $id -PatternSize 3840x2160 -Noise -Codec $s.Codec `
            -BitrateMbps $s.Mbps -Seconds $s.Secs -ViewerLinkMbps $s.Link -ViewerLinkDelayMs $s.Delay *>> $log
        "exit $LASTEXITCODE for $id" | Tee-Object -FilePath $log -Append
        $dirs += "build/verify-evidence/$id"
    }
}
Remove-Item Env:PARTIES_PROTO_GATE -ErrorAction SilentlyContinue
& python -I tools/verify-stream/bakeoff.py @dirs --out "$tmp/bakeoff-$Tag.md"
