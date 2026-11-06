param([string]$Version='1.0.0')
$ErrorActionPreference='Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9-]+)?$') { throw 'Invalid release version' }
$files=@(
    'PBZModLoader-安装说明.txt',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/mods.ini',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/bootstrap.js',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/manager.js'
)
$release=Join-Path $PSScriptRoot "releases/PBZModLoader-$Version-win64"
$zip="$release.zip"
if ((Test-Path -LiteralPath $release) -or (Test-Path -LiteralPath $zip)) { throw 'Release already exists; choose a new version or archive the existing release first.' }
foreach ($relative in $files) {
    if (!(Test-Path -LiteralPath (Join-Path "$PSScriptRoot/runtime" $relative) -PathType Leaf)) { throw "Missing runtime file: $relative" }
}
foreach($proxy in @('version','hid','winmm','dsound','dxgi')) {
    if (!(Test-Path -LiteralPath "$PSScriptRoot/build/$proxy.dll")) { throw "Build $proxy.dll first." }
}
New-Item -ItemType Directory -Path $release | Out-Null
foreach ($relative in $files) {
    $destination=Join-Path $release $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path "$PSScriptRoot/runtime" $relative) -Destination $destination
}
foreach ($platform in @('steam','wegame')) {
    $dllRelative="platform/$platform/version.dll"
    $dllDestination=Join-Path $release $dllRelative
    New-Item -ItemType Directory -Force (Split-Path $dllDestination) | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot/build/version.dll" -Destination $dllDestination
    $files += $dllRelative
}
foreach($proxy in @('hid','winmm','dsound','dxgi')) {
    $relative="备用入口/$proxy.dll"
    $destination=Join-Path $release $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot/build/$proxy.dll" -Destination $destination
    $files += $relative
}
$directories=@('ProjectPBZ/Content/JavaScript/PBZModLoader/mods/')
foreach ($relative in $directories) {
    New-Item -ItemType Directory -Force (Join-Path $release $relative) | Out-Null
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($release,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)
$archive=[IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries=@($archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\','/') })
    if (Compare-Object ($files | Sort-Object) ($entries | Sort-Object)) { throw 'ZIP entry allowlist mismatch' }
    foreach ($relative in $directories) {
        if (!($archive.Entries | Where-Object { $_.FullName.Replace('\','/') -eq $relative })) { throw "Missing ZIP directory: $relative" }
    }
} finally { $archive.Dispose() }
$verify=Join-Path $PSScriptRoot ('build/zip-check-'+[guid]::NewGuid().ToString('N'))
[IO.Compression.ZipFile]::ExtractToDirectory($zip,$verify)
foreach ($relative in $directories) {
    if (!(Test-Path -LiteralPath (Join-Path $verify $relative) -PathType Container)) { throw "Missing extracted directory: $relative" }
}
$hashes=[ordered]@{}
foreach ($relative in $files) {
    $expected=(Get-FileHash -LiteralPath (Join-Path $release $relative)).Hash
    $actual=(Get-FileHash -LiteralPath (Join-Path $verify $relative)).Hash
    if ($actual -ne $expected) { throw "Extracted content mismatch: $relative" }
    $hashes[$relative]=$actual.ToLower()
}
[ordered]@{version=$Version;zipSha256=(Get-FileHash -LiteralPath $zip).Hash.ToLower();directories=$directories;files=$hashes;verified=$true} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$zip.manifest.json" -Encoding utf8
Write-Output "PASS ZIP root layout, exact allowlist ($($files.Count) files), extraction and SHA-256 checks"
Write-Output $zip
