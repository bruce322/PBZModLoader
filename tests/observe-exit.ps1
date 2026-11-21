# Optional acceptance-test observer only. It neither launches nor debugs the game.
param([Parameter(Mandatory=$true)][int]$GamePid,[Parameter(Mandatory=$true)][string]$Evidence)
$ErrorActionPreference='Stop'
$gameProcess=Get-Process -Id $GamePid
$null=$gameProcess.Handle # Retain the process handle so ExitCode remains available.
$gameInfo=Get-CimInstance Win32_Process -Filter "ProcessId=$GamePid"
$parent=Get-CimInstance Win32_Process -Filter "ProcessId=$($gameInfo.ParentProcessId)"
$grandparent=Get-CimInstance Win32_Process -Filter "ProcessId=$($parent.ParentProcessId)"
$result=[ordered]@{
    gamePid=$GamePid
    gameExe=$gameInfo.ExecutablePath
    parentExe=$parent.ExecutablePath
    grandparentExe=$grandparent.ExecutablePath
    proxyModules=@($gameProcess.Modules | Where-Object { $_.ModuleName -match '^(version|hid|winmm|dsound|dxgi)\.dll$' } | ForEach-Object { $_.FileName })
    observedAt=(Get-Date).ToString('o')
    exitCode=$null
}
$destination=Join-Path (Resolve-Path -LiteralPath $Evidence).Path 'exit-observer.json'
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $destination -Encoding utf8
$gameProcess.WaitForExit()
$result.exitCode=$gameProcess.ExitCode
$result['exitedAt']=(Get-Date).ToString('o')
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $destination -Encoding utf8
$result | ConvertTo-Json -Depth 4
$gameProcess.Dispose()
