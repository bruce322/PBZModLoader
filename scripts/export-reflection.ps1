param(
    [int]$GamePid,
    [string]$Python='python',
    [Parameter(Mandatory=$true)][string]$Label,
    [string]$Output
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (!$GamePid) {
    $games=@(Get-CimInstance Win32_Process -Filter "Name='PBZ.exe'" |
        Where-Object { $_.ExecutablePath -match '\\platform\\(steam|wegame)\\PBZ\.exe$' })
    if ($games.Count -ne 1) { throw 'Expected one running Steam game process. Start the game in windowed 1280x720, or specify -GamePid.' }
    $GamePid=$games[0].ProcessId
}
if (!$Output) { $Output=Join-Path $repo ('build/reflection/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
& $Python (Join-Path $PSScriptRoot 'reflection_export.py') --pid $GamePid --output $Output --label $Label
$result=$LASTEXITCODE
if ($result -eq 2) { Write-Host "Inventory exported with unresolved entries. Read $Output/failures.json" }
elseif ($result -ne 0) { Write-Host "Capture rejected. Read $Output/failures.json when present." }
else { Write-Host "Inventory exported: $Output" }
exit $result
