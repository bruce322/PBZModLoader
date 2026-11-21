param([string]$Version='')
$ErrorActionPreference='Stop'
function Remove-Tree([string]$path) {
    if (!(Test-Path -LiteralPath $path)) { return }
    $files=@(Get-ChildItem -LiteralPath $path -Recurse -Force -File -ErrorAction SilentlyContinue)
    foreach($f in $files) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
    $dirs=@(Get-ChildItem -LiteralPath $path -Recurse -Force -Directory -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending)
    foreach($d in $dirs) { Remove-Item -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}
if (!$Version) { $Version=(Get-Content -LiteralPath "$PSScriptRoot/VERSION" -Raw).Trim() }
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9-]+)?$') { throw 'Invalid release version' }
$files=@(
    'PBZModLoader-安装说明.txt',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/mods.ini',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/bootstrap.js',
    'ProjectPBZ/Content/JavaScript/PBZModLoader/manager.js'
)
$release=Join-Path $PSScriptRoot "releases/PBZModLoader-$Version-win64"
$zip="$release.zip"
$manifestPath="$zip.manifest.json"
if ((Test-Path -LiteralPath $release) -or (Test-Path -LiteralPath $zip)) { throw 'Release already exists; choose a new version or archive the existing release first.' }
foreach ($relative in $files) {
    if (!(Test-Path -LiteralPath (Join-Path "$PSScriptRoot/runtime" $relative) -PathType Leaf)) { throw "Missing runtime file: $relative" }
}
foreach($proxy in @('version','hid','winmm','dsound','dxgi')) {
    if (!(Test-Path -LiteralPath "$PSScriptRoot/build/$proxy.dll")) { throw "Build $proxy.dll first." }
}
$builtHeader=Get-Content -LiteralPath "$PSScriptRoot/build/loader-version.hpp" -Raw
if ($builtHeader -notmatch ('PBZ_LOADER_VERSION "' + [regex]::Escape($Version) + '"')) { throw "build/*.dll were not built for version $Version; rebuild with -Version $Version" }
$guideFirst=Get-Content -LiteralPath "$PSScriptRoot/runtime/PBZModLoader-安装说明.txt" -TotalCount 1
if ($guideFirst -notmatch [regex]::Escape("PBZModLoader $Version")) { throw "Installer guide title does not mention version $Version" }
$verify=$null
try {
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
        $entries=@($archive.Entries)
        $actualFiles=@($entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\','/') })
        if (Compare-Object ($files | Sort-Object) ($actualFiles | Sort-Object)) { throw 'ZIP file allowlist mismatch' }
        $expectedDirs=New-Object System.Collections.Generic.List[string]
        foreach($relative in $files) {
            $parts=$relative.Split('/')
            for($i=1;$i -lt $parts.Count;$i++) {
                $dir=(($parts[0..($i-1)]) -join '/')+'/'
                if(!$expectedDirs.Contains($dir)) { $expectedDirs.Add($dir) }
            }
        }
        foreach($relative in $directories) { if(!$expectedDirs.Contains($relative)) { $expectedDirs.Add($relative) } }
        $actualDirs=@($entries | Where-Object { -not $_.Name } | ForEach-Object { $_.FullName.Replace('\','/') })
        foreach($dir in $actualDirs) { if($expectedDirs -notcontains $dir) { throw "Unexpected ZIP directory: $dir" } }
        foreach($dir in $directories) { if($actualDirs -notcontains $dir) { throw "Missing ZIP directory: $dir" } }
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
    $manifest=[ordered]@{version=$Version;buildVersion=$Version;zipSha256=(Get-FileHash -LiteralPath $zip).Hash.ToLower();directories=$directories;files=$hashes;verified=$true}
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 5),(New-Object System.Text.UTF8Encoding($false)))
    Write-Output "PASS ZIP root layout, exact allowlist ($($files.Count) files), extraction and SHA-256 checks"
    Write-Output $zip
} catch {
    Remove-Tree $release
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $manifestPath -Force -ErrorAction SilentlyContinue
    throw
} finally {
    if ($verify) { Remove-Tree $verify }
}
