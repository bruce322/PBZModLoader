param([string]$Compiler='clang++',[switch]$Test,[switch]$VerifyOnly,[string]$GameExe)
$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    $proxies=@('version','hid','winmm','dsound','dxgi')
    node scripts/generate-proxies.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Proxy generation failed' }
    foreach ($proxy in $proxies) {
    $def=if($proxy -eq 'version'){'src/version.def'}else{"src/proxies/$proxy.def"}
    if (!$VerifyOnly) {
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static -shared "-Ibuild/proxies/$proxy" src/proxy.cpp "build/proxies/$proxy/proxy.S" $def -lbcrypt -luser32 '-Wl,--exclude-all-symbols' -o "build/$proxy.dll"
        if ($LASTEXITCODE -ne 0) { throw 'DLL build failed' }
    }
    if (!(Test-Path "build/$proxy.dll")) { throw "Missing build/$proxy.dll" }
    }
    if ($Test) {
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/native.cpp -lbcrypt -o build/native-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Native test build failed' }
        & ./build/native-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
        if ($GameExe) {
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/signatures.cpp -lbcrypt -o build/signature-test.exe
            if ($LASTEXITCODE -ne 0) { throw 'Signature test build failed' }
            & ./build/signature-test.exe $GameExe
            if ($LASTEXITCODE -ne 0) { throw 'Signature tests failed' }
        }
        node --test tests/manager.test.cjs
        if ($LASTEXITCODE -ne 0) { throw 'JS tests failed' }
        foreach ($layout in @('platform/steam','platform/wegame','ProjectPBZ/Binaries/Win64')) {
        $fixtureRoot=Join-Path $PSScriptRoot ('build/proxy-fixture/'+$layout.Replace('/','-'))
        $fixture=Join-Path $fixtureRoot $layout
        $runtime=Join-Path $fixtureRoot 'ProjectPBZ'
        New-Item -ItemType Directory -Force "$runtime/Content/JavaScript/PBZModLoader" | Out-Null
        Set-Content "$runtime/Content/JavaScript/PBZModLoader/mods.ini" '[Loader]'
        Set-Content "$runtime/Content/JavaScript/PBZModLoader/bootstrap.js" '// fixture'
        New-Item -ItemType Directory -Force $fixture | Out-Null
        & $Compiler -std=c++17 -O2 -Wall -Wextra -static tests/proxy-host.cpp -lversion -o "$fixture/PBZ.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Proxy host build failed' }
        Copy-Item -LiteralPath build/version.dll -Destination "$fixture/version.dll" -Force
        & "$fixture/PBZ.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Proxy tests failed' }
        $logs=@(Get-ChildItem "$runtime/Content/JavaScript/PBZModLoader/evidence" -Filter loader.log -Recurse | Sort-Object LastWriteTime -Descending)
        if (!$logs.Count -or !(Select-String -LiteralPath $logs[0].FullName -SimpleMatch 'MainStart signature requires one validated match; found=0')) { throw 'Missing signature rejection evidence' }
        Write-Output "PASS platform layout: $layout, entry restored and unsupported host skipped"
        }
        $readobj=Join-Path (Split-Path (Get-Command $Compiler).Source) 'llvm-readobj.exe'
        $imports=& $readobj --coff-imports build/version.dll
        if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
        if ($imports -match 'Symbol: (DebugActiveProcess|DebugActiveProcessStop|WaitForDebugEvent|ContinueDebugEvent|DebugSetProcessKillOnExit|CheckRemoteDebuggerPresent|IsDebuggerPresent|ReadProcessMemory|WriteProcessMemory|CreateRemoteThread)\b') { throw 'Unexpected debugger or remote process API' }
        $exports=& $readobj --coff-exports build/version.dll
        if ($LASTEXITCODE -ne 0 -or @($exports -match '^Export \{').Count -ne 17) { throw 'Unexpected exports' }
        Write-Output 'PASS DLL import audit and 17 exports'
        foreach ($proxy in $proxies) {
            $fixture=Join-Path $PSScriptRoot "build/alternate-fixture/$proxy/platform/steam"
            $runtime=Join-Path $PSScriptRoot "build/alternate-fixture/$proxy/ProjectPBZ"
            New-Item -ItemType Directory -Force $fixture,"$runtime/Content/JavaScript/PBZModLoader" | Out-Null
            Set-Content "$runtime/Content/JavaScript/PBZModLoader/mods.ini" '[Loader]'
            Set-Content "$runtime/Content/JavaScript/PBZModLoader/bootstrap.js" '// fixture'
            Copy-Item "build/$proxy.dll" "$fixture/$proxy.dll" -Force
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static "-DPBZ_TEST_$($proxy.ToUpper())" tests/alternate-proxy-host.cpp "-l$proxy" -o "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw "Host build failed: $proxy" }
            & "$fixture/PBZ.exe"
            if ($LASTEXITCODE -ne 0) { throw "Host failed: $proxy" }
            $logs=@(Get-ChildItem "$runtime/Content/JavaScript/PBZModLoader/evidence" -Filter loader.log -Recurse | Sort-Object LastWriteTime -Descending)
            if (!$logs.Count -or !(Select-String -LiteralPath $logs[0].FullName -SimpleMatch 'MainStart signature requires one validated match; found=0')) { throw "Missing rejection evidence: $proxy" }
            $imports=& $readobj --coff-imports "build/$proxy.dll"
            if ($LASTEXITCODE -ne 0 -or $imports -match 'Symbol: (DebugActiveProcess|WriteProcessMemory|CreateRemoteThread)\b') { throw "Import audit failed: $proxy" }
            $exports=& $readobj --coff-exports "build/$proxy.dll"
            $manifest=@(Get-Content "build/proxies/$proxy/exports.json" -Raw | ConvertFrom-Json)
            $expected=$manifest.Count
            if ($LASTEXITCODE -ne 0 -or @($exports -match '^Export \{').Count -ne $expected) { throw "Export audit failed: $proxy" }
            $exportText=$exports -join "`n"
            foreach($entry in $manifest) {
                $exportName=if($entry.noname){''}else{[regex]::Escape($entry.name)}
                if($exportText -notmatch "Ordinal: $($entry.ordinal)\r?\n  Name: $exportName\r?\n") {throw "Export name/ordinal mismatch: $proxy $($entry.name)"}
            }
        }
        foreach($mode in @('DUPLICATE','LATE')) {
            $fixture=Join-Path $PSScriptRoot "build/alternate-fixture/$mode/platform/steam"
            $runtime=Join-Path $PSScriptRoot "build/alternate-fixture/$mode/ProjectPBZ"
            New-Item -ItemType Directory -Force $fixture,"$runtime/Content/JavaScript/PBZModLoader" | Out-Null
            Set-Content "$runtime/Content/JavaScript/PBZModLoader/mods.ini" '[Loader]'
            Set-Content "$runtime/Content/JavaScript/PBZModLoader/bootstrap.js" '// fixture'
            Copy-Item build/version.dll,build/hid.dll $fixture -Force
            $flags=@(if($mode -eq 'DUPLICATE'){'-DPBZ_TEST_HID';'-lhid'}else{'-DPBZ_TEST_VERSION'})
            & $Compiler -std=c++17 -O2 -Wall -Wextra -static "-DPBZ_TEST_$mode" @flags tests/alternate-proxy-host.cpp -lversion -o "$fixture/PBZ.exe"
            if($LASTEXITCODE -ne 0){throw "Host build failed: $mode"}
            & "$fixture/PBZ.exe"
            if($LASTEXITCODE -ne 0){throw "Host failed: $mode"}
            $logs=@(Get-ChildItem "$runtime/Content/JavaScript/PBZModLoader/evidence" -Filter loader.log -Recurse | Sort-Object LastWriteTime -Descending)
            $lines=Get-Content $logs[0].FullName
            if(@($lines -match 'backend=dll;').Count -ne 1 -or @($lines -match 'entry-restored;').Count -ne 1){throw "Duplicate initialization: $mode"}
            Write-Output "PASS $mode entry protection"
        }
    }
} finally { Pop-Location }
