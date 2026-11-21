param([string]$Compiler='clang++',[switch]$Test,[switch]$VerifyOnly,[string]$GameExe,[string]$Version='')
$ErrorActionPreference='Stop'
function Remove-Tree([string]$path) {
    if (!(Test-Path -LiteralPath $path)) { return }
    $files=@(Get-ChildItem -LiteralPath $path -Recurse -Force -File -ErrorAction SilentlyContinue)
    foreach($f in $files) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
    $dirs=@(Get-ChildItem -LiteralPath $path -Recurse -Force -Directory -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending)
    foreach($d in $dirs) { Remove-Item -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    if (!$Version) { $Version=(Get-Content -LiteralPath 'VERSION' -Raw).Trim() }
    if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9-]+)?$') { throw "Invalid loader version: $Version" }
    $headerPath='build/loader-version.hpp'
    if ($VerifyOnly) {
        if (!(Test-Path -LiteralPath $headerPath)) { throw '-VerifyOnly needs an existing build; run a normal build first' }
        $headerText=Get-Content -LiteralPath $headerPath -Raw
        if ($headerText -notmatch ('PBZ_LOADER_VERSION "' + [regex]::Escape($Version) + '"')) { throw "Existing build is not version $Version; rebuild with -Version $Version" }
    } else {
        Set-Content -LiteralPath $headerPath -Encoding ascii -Value @('#pragma once', ('#define PBZ_LOADER_VERSION "' + $Version + '"'))
    }
    Write-Output "PBZModLoader version $Version"
    $proxies=@('version','hid','winmm','dsound','dxgi')
    node scripts/generate-proxies.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Proxy generation failed' }
    foreach ($proxy in $proxies) {
    $def=if($proxy -eq 'version'){'src/version.def'}else{"src/proxies/$proxy.def"}
    if (!$VerifyOnly) {
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static -shared "-Ibuild/proxies/$proxy" -Ibuild src/proxy.cpp "build/proxies/$proxy/proxy.S" $def -lbcrypt -luser32 '-Wl,--exclude-all-symbols' -o "build/$proxy.dll"
        if ($LASTEXITCODE -ne 0) { throw 'DLL build failed' }
    }
    if (!(Test-Path "build/$proxy.dll")) { throw "Missing build/$proxy.dll" }
    }
    if ($Test) {
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/native.cpp -lbcrypt -o build/native-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
        & ./build/native-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/evidence-log.cpp -lbcrypt -o build/evidence-log-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Evidence log test build failed' }
        & ./build/evidence-log-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Evidence log tests failed' }
        if ($GameExe) {
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/signatures.cpp -lbcrypt -o build/signature-test.exe
            if ($LASTEXITCODE -ne 0) { throw 'Signature test build failed' }
            & ./build/signature-test.exe $GameExe
            if ($LASTEXITCODE -ne 0) { throw 'Signature tests failed' }
        }
        node tests/manager.test.cjs
        if ($LASTEXITCODE -ne 0) { throw 'JS tests failed' }
        $readobj=Join-Path (Split-Path (Get-Command $Compiler).Source) 'llvm-readobj.exe'
        if (!(Test-Path -LiteralPath $readobj)) { throw "llvm-readobj.exe not found next to the compiler: $readobj" }
        function Get-EvidenceLog([string]$loader) {
            $logs=@(Get-ChildItem -LiteralPath (Join-Path $loader 'evidence') -Filter loader.log -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
            if (!$logs.Count) { throw "Missing evidence log under $loader" }
            return $logs[0]
        }
        function Write-FixtureJs([string]$loader) {
            Set-Content -LiteralPath (Join-Path $loader 'bootstrap.js') -Value '// fixture'
            Set-Content -LiteralPath (Join-Path $loader 'manager.js') -Value '// fixture'
        }
        foreach ($layout in @('platform/steam','platform/wegame','ProjectPBZ/Binaries/Win64')) {
            $label=($layout -replace '/','-')
            $fixtureRoot=Join-Path $PSScriptRoot ('build/proxy-fixture/'+$label)
            $fixture=Join-Path $fixtureRoot $layout
            $runtime=Join-Path $fixtureRoot 'ProjectPBZ'
            $loader=Join-Path $runtime 'Content/JavaScript/PBZModLoader'
            Remove-Tree $fixtureRoot
            New-Item -ItemType Directory -Force $loader | Out-Null
            Set-Content -LiteralPath (Join-Path $loader 'mods.ini') -Value @('[Loader]','Version=1','Enabled=1','HotReload=1')
            Write-FixtureJs $loader
            New-Item -ItemType Directory -Force $fixture | Out-Null
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/proxy-host.cpp -lversion -o "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Proxy host build failed' }
            Copy-Item -LiteralPath build/version.dll -Destination "$fixture/version.dll" -Force
            $started=Get-Date
            & "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Proxy tests failed' }
            $log=Get-EvidenceLog $loader
            if ($log.LastWriteTime -lt $started.AddSeconds(-5)) { throw "Stale evidence for layout $layout" }
            if (!(Select-String -LiteralPath $log.FullName -SimpleMatch 'MainStart signature requires one validated match; found=0')) { throw 'Missing signature rejection evidence' }
            $statusPath=Join-Path $loader 'loader-status.log'
            if (!(Test-Path -LiteralPath $statusPath)) { throw 'Missing loader-status.log' }
            $statusLine=Get-Content -LiteralPath $statusPath -TotalCount 1
            if ($statusLine -ne "PBZModLoader $Version (JS only)") { throw "Unexpected loader status version: $statusLine" }
            $statusText=Get-Content -LiteralPath $statusPath -Raw
            if ($statusText -notmatch '(?m)^hot-reload=on') { throw 'HotReload=1 was not honoured' }
            if (Test-Path -LiteralPath (Join-Path $loader 'loader-initializing.marker')) { throw 'Handled initialization failure left the initializing marker' }
            & "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Second proxy test run failed' }
            $statusText=Get-Content -LiteralPath $statusPath -Raw
            if ($statusText -match '(?m)^previous-run=unclean') { throw 'Handled initialization failure marked the next run unclean' }
            Write-Output "PASS platform layout: $layout fresh evidence, hot reload honoured, marker cleaned"
        }
        $diagRoot=Join-Path $env:TEMP ('pbzmodloader-diagnostic-'+[guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force $diagRoot | Out-Null
        try {
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/proxy-host.cpp -lversion -o "$diagRoot/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Diagnostic host build failed' }
            Copy-Item -LiteralPath build/version.dll -Destination "$diagRoot/version.dll" -Force
            & "$diagRoot/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Diagnostic host failed' }
            $diagFile=Join-Path $diagRoot 'PBZModLoader-diagnostic.log'
            if (!(Test-Path -LiteralPath $diagFile)) { throw 'Missing startup diagnostic when ProjectPBZ is absent' }
            $diagText=Get-Content -LiteralPath $diagFile -Raw
            if ($diagText -notmatch 'error=ProjectPBZ runtime not found above game executable') { throw 'Startup diagnostic lacks discovery error' }
            if ($diagText -notmatch 'checked\[0\]=') { throw 'Startup diagnostic lacks searched directories' }
            Write-Output 'PASS startup diagnostic when ProjectPBZ is absent'
        } finally {
            Remove-Item -LiteralPath "$diagRoot/PBZ.exe","$diagRoot/version.dll","$diagRoot/PBZModLoader-diagnostic.log" -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $diagRoot -Force -ErrorAction SilentlyContinue
        }
        $staleRoot=Join-Path $env:TEMP ('pbzmodloader-stale-diagnostic-'+[guid]::NewGuid().ToString('N'))
        $tempDiag=Join-Path $env:TEMP 'PBZModLoader-diagnostic.log'
        New-Item -ItemType Directory -Force $staleRoot | Out-Null
        try {
            New-Item -ItemType Directory -Force (Join-Path $staleRoot 'ProjectPBZ/Content/JavaScript/PBZModLoader') | Out-Null
            Set-Content -LiteralPath (Join-Path $staleRoot 'ProjectPBZ/Content/JavaScript/PBZModLoader/mods.ini') -Value @('[Loader]','Version=1') -Encoding ASCII
            Set-Content -LiteralPath (Join-Path $staleRoot 'ProjectPBZ/Content/JavaScript/PBZModLoader/bootstrap.js') -Value '' -Encoding ASCII
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/proxy-host.cpp -lversion -o "$staleRoot/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Stale diagnostic host build failed' }
            Copy-Item -LiteralPath build/version.dll -Destination "$staleRoot/version.dll" -Force
            Set-Content -LiteralPath "$staleRoot/PBZModLoader-diagnostic.log" -Value 'stale' -Encoding ASCII
            Set-Content -LiteralPath $tempDiag -Value 'stale' -Encoding ASCII
            & "$staleRoot/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Stale diagnostic host failed' }
            $staleStatus=Join-Path $staleRoot 'ProjectPBZ/Content/JavaScript/PBZModLoader/loader-status.log'
            if (!(Test-Path -LiteralPath $staleStatus)) { throw 'Late initialization failure did not write loader-status.log' }
            if ((Get-Content -LiteralPath $staleStatus -Raw) -notmatch '(?m)^status=failed') { throw 'Late initialization failure did not report status=failed' }
            if (Test-Path -LiteralPath "$staleRoot/PBZModLoader-diagnostic.log") { throw 'Handled late failure left a stale startup diagnostic' }
            if (Test-Path -LiteralPath $tempDiag) { throw 'Handled late failure left the %TEMP% diagnostic behind' }
            Write-Output 'PASS handled late failure clears stale startup diagnostics'
        } finally {
            Remove-Tree $staleRoot
            Remove-Item -LiteralPath $tempDiag -Force -ErrorAction SilentlyContinue
        }
        $imports=& $readobj --coff-imports build/version.dll
        if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
        if ($imports -match 'Symbol: (DebugActiveProcess|DebugActiveProcessStop|WaitForDebugEvent|ContinueDebugEvent|DebugSetProcessKillOnExit|CheckRemoteDebuggerPresent|IsDebuggerPresent|ReadProcessMemory|WriteProcessMemory|CreateRemoteThread)\b') { throw 'Unexpected debugger or remote process API' }
        $exports=& $readobj --coff-exports build/version.dll
        $manifest=@((Get-Content -LiteralPath 'build/proxies/version/exports.json' -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
        $expected=$manifest.Count
        if ($LASTEXITCODE -ne 0 -or @($exports -match '^Export \{').Count -ne $expected) { throw "Unexpected version.dll exports: expected $expected" }
        $exportText=$exports -join [Environment]::NewLine
        foreach($entry in $manifest) {
            $exportName=if($entry.noname){''}else{[regex]::Escape($entry.name)}
            if($exportText -notmatch "Ordinal: $($entry.ordinal)\r?\n  Name: $exportName\r?\n") { throw "Export name/ordinal mismatch: version.dll $($entry.name)" }
        }
        Write-Output "PASS DLL import audit and $expected version.dll exports"
        foreach ($proxy in $proxies) {
            $fixtureRoot=Join-Path $PSScriptRoot "build/alternate-fixture/$proxy"
            $fixture=Join-Path $fixtureRoot 'platform/steam'
            $runtime=Join-Path $fixtureRoot 'ProjectPBZ'
            $loader=Join-Path $runtime 'Content/JavaScript/PBZModLoader'
            Remove-Tree $fixtureRoot
            New-Item -ItemType Directory -Force $fixture,$loader | Out-Null
            Set-Content -LiteralPath (Join-Path $loader 'mods.ini') -Value @('[Loader]','Version=1')
            Write-FixtureJs $loader
            Copy-Item "build/$proxy.dll" "$fixture/$proxy.dll" -Force
            $started=Get-Date
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static "-DPBZ_TEST_$($proxy.ToUpper())" tests/alternate-proxy-host.cpp "-l$proxy" -o "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw "Host build failed: $proxy" }
            & "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw "Host failed: $proxy" }
            $log=Get-EvidenceLog $loader
            if ($log.LastWriteTime -lt $started.AddSeconds(-5)) { throw "Stale evidence: $proxy" }
            if (!(Select-String -LiteralPath $log.FullName -SimpleMatch 'MainStart signature requires one validated match; found=0')) { throw "Missing rejection evidence: $proxy" }
            $imports=& $readobj --coff-imports "build/$proxy.dll"
            if ($LASTEXITCODE -ne 0 -or $imports -match 'Symbol: (DebugActiveProcess|WriteProcessMemory|CreateRemoteThread)\b') { throw "Import audit failed: $proxy" }
            $exports=& $readobj --coff-exports "build/$proxy.dll"
            $manifest=@((Get-Content "build/proxies/$proxy/exports.json" -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
            $expected=$manifest.Count
            if ($LASTEXITCODE -ne 0 -or @($exports -match '^Export \{').Count -ne $expected) { throw "Export audit failed: $proxy" }
            $exportText=$exports -join [Environment]::NewLine
            foreach($entry in $manifest) {
                $exportName=if($entry.noname){''}else{[regex]::Escape($entry.name)}
                if($exportText -notmatch "Ordinal: $($entry.ordinal)\r?\n  Name: $exportName\r?\n") {throw "Export name/ordinal mismatch: $proxy $($entry.name)"}
            }
        }
        foreach($mode in @('DUPLICATE','LATE')) {
            $fixtureRoot=Join-Path $PSScriptRoot "build/alternate-fixture/$mode"
            $fixture=Join-Path $fixtureRoot 'platform/steam'
            $runtime=Join-Path $fixtureRoot 'ProjectPBZ'
            $loader=Join-Path $runtime 'Content/JavaScript/PBZModLoader'
            Remove-Tree $fixtureRoot
            New-Item -ItemType Directory -Force $fixture,$loader | Out-Null
            Set-Content -LiteralPath (Join-Path $loader 'mods.ini') -Value @('[Loader]','Version=1')
            Write-FixtureJs $loader
            Copy-Item build/version.dll,build/hid.dll $fixture -Force
            $flags=@(if($mode -eq 'DUPLICATE'){'-DPBZ_TEST_HID';'-lhid'}else{'-DPBZ_TEST_VERSION'})
            $started=Get-Date
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static "-DPBZ_TEST_$mode" @flags tests/alternate-proxy-host.cpp -lversion -o "$fixture/PBZ.exe"
            if($LASTEXITCODE -ne 0){throw "Host build failed: $mode"}
            & "$fixture/PBZ.exe"
            if($LASTEXITCODE -ne 0){throw "Host failed: $mode"}
            $log=Get-EvidenceLog $loader
            if ($log.LastWriteTime -lt $started.AddSeconds(-5)) { throw "Stale evidence: $mode" }
            $lines=Get-Content -LiteralPath $log.FullName
            if(@($lines -match 'backend=dll;').Count -ne 1 -or @($lines -match 'entry-restored;').Count -ne 1){throw "Duplicate initialization: $mode"}
            Write-Output "PASS $mode entry protection"
        }
    }
} finally { Pop-Location }
