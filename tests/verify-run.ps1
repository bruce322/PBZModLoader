param(
    [Parameter(Mandatory=$true)][string]$Log,
    [int]$ExpectedLoaded=0,
    [switch]$RequireExit
)
$ErrorActionPreference='Stop'
$lines = @(Get-Content -LiteralPath $Log -Encoding utf8 | ForEach-Object { $_ -replace '^\[\d\d:\d\d:\d\d\.\d{3}\] ','' })
$dll = @($lines -eq 'backend=dll; startup=exe-entry; debugger-api=none').Count -eq 1
$checks = [ordered]@{
    bootstrap = if ($dll) { @($lines -match '^native hooks installed;').Count -eq 1 } else { @($lines -match '^detached debugger=false;').Count -eq 1 }
    singleMainStart = @($lines -match '^main-start count=').Count -eq 1 -and @($lines -eq 'main-start count=1').Count -eq 1
    originalReturned = @($lines -eq '[PBZMOD] original MainStart returned').Count -eq 1
    loaded = @($lines -eq "[PBZMOD] ready loaded=$ExpectedLoaded failed=0").Count -eq 1
    noReportedErrors = @($lines -match 'ERROR|MODs skipped:|\[PBZMOD\] failed |configuration failed:|log-overflow|script (bytes|text) rejected:|MOD (byte|text) validation failed:|original MainStart failed:').Count -eq 0
}
if ($RequireExit) {
    if ($dll) {
        $observed = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Log).Path) 'exit-observer.json'
        $entry = @($lines -match '^entry-restored; pid=')[0]
        if (!$entry) { throw 'Missing entry-restored; pid= line in the loader log.' }
        $gameId = [int]($entry -replace '^entry-restored; pid=','')
        $observer = if (Test-Path -LiteralPath $observed) { Get-Content -Raw -LiteralPath $observed | ConvertFrom-Json } else { $null }
        $checks['normalExit'] = $null -ne $observer -and $null -ne $observer.exitCode -and $observer.exitCode -eq 0 -and $observer.gamePid -eq $gameId
        $checks['steamAncestry'] = $null -ne $observer -and $observer.parentExe -match '\\steam\\PBZ.exe$' -and $observer.grandparentExe -match '\\steam.exe$'
        $checks['processShutdown'] = @($lines -match '^process-detach shutdown=true').Count -eq 1
    } else { $checks['normalExit'] = @($lines -eq 'game-exit code=0').Count -eq 1 }
}
$result = [ordered]@{
    passed = @($checks.Values | Where-Object { $_ -ne $true }).Count -eq 0
    log = (Resolve-Path -LiteralPath $Log).Path
    checks = $checks
}
$json = $result | ConvertTo-Json -Depth 5
$json | Set-Content -LiteralPath (Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Log).Path) 'verification.json') -Encoding utf8
$json
if (!$result.passed) { exit 1 }
