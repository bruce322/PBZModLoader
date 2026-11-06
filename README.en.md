[![PBZModLoader — Alt+F5 Hot Reload](docs/images/pbzmodloader-cover.png)](https://www.nexusmods.com/phantombladezero/mods/113)

# PBZModLoader

[简体中文](README.md) | **English**

PBZModLoader is a mod loader for **Phantom Blade Zero**. It provides a shared entry point for script mods, JavaScript / TypeScript development templates, and a game API type declaration SDK. It supports automatic mod discovery, enabling and disabling mods through configuration, and **Alt+F5 hot reload** for mods that implement the cleanup lifecycle.

**Downloads and releases: [Nexus Mods · PBZModLoader](https://www.nexusmods.com/phantombladezero/mods/113)**

### Mod development template

**Direct download: [Mod development template ZIP (template 0.2.0 / SDK 0.3.3)](https://github.com/zoneoflkf/PBZModLoader/releases/download/v1.0.0/PBZModLoader-TS-Template-0.2.0-SDK-0.3.3.zip)**

To create your own mod, start with the [minimal TypeScript mod template](templates/ts-mod/). See the [template development guide](templates/ts-mod/README.md) (Chinese) for details. The template includes an example entry point, build scripts, and the game API SDK, with UE / PuerTS completion and strict type checking. Copy the entire template directory to develop independently; you do not need to compile the loader's C++ project.

Install Node.js and npm on your development machine, then run `npm ci`, `npm run check`, and `npm run build` in your copy of the template. Edit `src/main.ts` to implement your mod. Copy the entire generated `dist/my-mod/` folder into the game's `ProjectPBZ/Content/JavaScript/PBZModLoader/mods/` directory for automatic discovery. Players only need the loader and the built mod; development tools are not required.

### Technical approach

- **Native proxy DLL bootstrap:** `version.dll` is the default entry point, with `hid.dll`, `winmm.dll`, `dsound.dll`, and `dxgi.dll` available as alternatives. Each proxy forwards system DLL exports and initializes the loader during game startup. Install only one entry point.
- **Signature scanning and validation:** instruction signatures in the game executable, strings, and call relationships locate `MainStart`, PuerTS startup, and logging interfaces without fixed addresses. Mod loading is skipped if a match is missing or ambiguous. Game updates may still require compatibility changes.
- **Zero-copy image view:** the loader scans and validates the already mapped EXE image instead of allocating and copying `SizeOfImage` bytes at startup. `ImageView` keeps readable section metadata for an in-process borrowed view; owned file copies remain available for tests.
- **Reuse of the game's scripting environment:** the game's existing PuerTS environment runs the bootstrap script and mod manager, allowing JS mods to use exposed UE APIs. The loader synchronizes mods into the script search directory and provides narrowly scoped validation compatibility for local loader scripts and generated content.
- **JavaScript runtime and TypeScript development support:** the loader loads JavaScript (ES2020/CommonJS) only and does not bundle a TypeScript compiler. TypeScript developers build `main.js` with the template before installation. The SDK generates declarations from game reflection data, while the standalone template provides completion and type checking. Hot reload cleans up old instances through `stop()` and isolates module caches.

This repository contains loader source code, runtime file templates, the SDK, and development tools. Players should download an installation package from the release page above. Locally built player packages are placed in `releases/`; only runtime files belong in the game installation directory.

## 1.0.0: First stable release

The former 0.3.7-preview package is now released as stable version 1.0.0. Future releases will no longer use the preview suffix by default.

The WeGame executable directory has been confirmed as `platform/wegame`. The package includes both `platform/steam/version.dll` and `platform/wegame/version.dll`, produced by the same build. If no DLL with the same name is already installed, users of either platform can extract the package directly into the game root without moving DLLs manually.

This release only changes the package layout and installation instructions, retaining the 0.3.6 DLL. **WeGame has been tested in-game: automatic loader startup and mod execution are confirmed working.**

## 0.3.6-preview: Compatibility with the November 4 game update

Added the MainStart instruction layout introduced by the Steam update, fixing the issue where all mods were skipped after logging `MainStart signature requires one validated match; found=0`.

The new FString argument uses `[rsp+60h]`; the old layout uses `[rbp-69h]`. Both complete instruction layouts are matched separately, and both still require a unique candidate, the MainStart string, and Start trampoline validation. Validation of the three PuerTS logging hooks is unchanged.

The updated game also validates module bytes and executed source code. The loader identifies two validation functions and five call sites, providing compatibility only for the bootstrap, manager, configuration, and generated mod directories under `PBZModLoader`. Supplied content must match the corresponding local file; executed source must be either the original text or PuerTS's CommonJS wrapper. Original game scripts still use the original validation functions. Out-of-bounds paths, mismatched content, or missing signatures are rejected.

PuerTS warnings and errors are now also written to `loader.log`, helping diagnose failures before the bootstrap script executes. There is no fixed-address fallback. To upgrade, exit the game, replace your selected proxy DLL, and update `bootstrap.js`. Your configuration and mods do not need changes.

The earlier 0.3.5-preview release only added MainStart matching and omitted script validation compatibility, which could break initialization of the original game menus. It has been withdrawn to `releases/archive` and should no longer be used.

`build.ps1 -Test -GameExe <game-executable>` supports both the previous baseline and this update. It covers both stack layouts, ambiguity across layouts, rejection of inconsistent stack arguments, completeness of the two script validation functions and five call sites, and the existing relocation, ASLR, and logging hook checks. Native tests also cover local script content, UTF-8, CommonJS wrappers, official script paths, and rejection of out-of-bounds paths.

## 0.3.4-preview: Alternative proxy DLL entry points

The default is `version.dll`. Release builds also provide `hid.dll`, `winmm.dll`, `dsound.dll`, and `dxgi.dll`. The default entry points are packaged at `platform/steam/version.dll` and `platform/wegame/version.dll`. The other four DLLs are in the top-level `备用入口/` directory (alternative entry points) and are not enabled automatically.

**Install only one loader entry point beside the actual game executable. Keep its original filename; do not install multiple entry points at once.**

If another mod already uses `version.dll`, merge only the package's `ProjectPBZ` directory, then copy an unused DLL from `备用入口/` beside the actual executable. Do not overwrite another mod's DLL with the default `version.dll`. When switching entry points, remove this loader's previous entry point. When upgrading, replace only the selected entry point and update runtime files, preserving your configuration and mods. When uninstalling, remove only the entry point selected for this loader.

No entry point setting is required in `mods.ini`. Renaming `version.dll` is not a substitute for using the corresponding dedicated proxy.

The four alternative entry points passed startup validation on the Steam build tested for this release. This does not guarantee their behavior on other platforms, during map transitions or extended sessions, or alongside other mods. Alternative entry points resolve filename conflicts, not game logic conflicts between mods. If multiple entry points from this version are installed accidentally, only one installs hooks; the others retain system function forwarding. This is a safeguard, and single-entry installation is still required.

Builds use fixed export lists in `src/version.def` and `src/proxies/*.def`. `scripts/generate-proxies.cjs` generates five sets of configurations and forwarding stubs without reading the build machine's System32 directory. Named exports are forwarded by name where possible; NONAME exports use ordinals. System functions are resolved on demand, so missing optional interfaces that are never used do not prevent startup. Calling an unavailable interface on an unknown system fails fast. These lists come from the exports observed on the locally tested system and do not guarantee undocumented ordinal ABIs across all historical Windows versions. Only proxies loaded statically during startup install the executable entry hook; proxies loaded through LoadLibrary at runtime only forward functions.

`build.ps1 -Test` checks static loading, entry point restoration, representative API forwarding, concurrent calls, and export counts for all five proxies, plus duplicate-entry and late-loading safeguards. Existing version proxy tests for multiple layouts, ABI behavior, and game signatures remain in place. `package.ps1` defaults to stable version `1.0.0` and checks all five DLLs, both platforms' default entry points, the ZIP allowlist, and extracted file hashes.

## 0.3.0-preview: Platform-independent bootstrap

The DLL no longer checks the Steam path, process filename, or a fixed executable hash. Place `version.dll` beside the actual game process executable. Starting from that directory, the loader searches upward for a `ProjectPBZ` directory containing `Content`, loader configuration, and `bootstrap.js`. Launcher processes, unknown builds, and incorrect installations do not pass full signature validation. The game must load the proxy DLL before its executable entry point runs.

`src/signatures.hpp` scans executable PE sections only. It uses wildcard instruction sequences together with the MainStart string, Start trampoline instructions, references to `JSLogger.cpp`, log levels, and a shared logging category address to locate hooks. Every match must be unique, references must point into valid PE sections, and all hooks must pass validation before game code is modified. Original trampoline and call bytes are read from actual match locations, removing the dependency on fixed RVAs or fixed relative offsets. Missing or ambiguous signatures cause mods to be skipped while the original game continues; there is no fallback to old fixed addresses.

These signatures originate from CL-301524_JKS-17219, with the MainStart layout from the 2026-11-04 Steam update added later. They tolerate address and relative-offset changes but are not a universal ABI for every version. Compiler, runtime structure, or protection changes may require new signatures. The WeGame executable directory is confirmed as `platform/wegame`; automatic loader startup and mod execution have been tested successfully on WeGame.

Additional checks:

```powershell
.\build.ps1 -Compiler C:\path\to\clang++.exe -Test -GameExe C:\path\to\PBZ.exe
.\package.ps1 -Version 1.0.0
```

`-GameExe` reads the file without launching or modifying the game. This regression test targets known Steam baseline files and checks scan results, address rearrangement, ASLR, call-offset changes, and rejection of ambiguous or damaged data.

The package contains `ProjectPBZ`, `platform`, `备用入口`, and installation instructions. Default DLLs are placed at `platform/steam/version.dll` and `platform/wegame/version.dll`. For a first installation on Steam or WeGame, extract directly into the game root and merge folders with matching names; the DLLs are already in their respective executable directories. If a DLL with the same name exists, identify its source first. Preserve your own configuration and mods when updating.

At the time of this preview, the new build had passed static matching, native/JS tests, and proxy host tests with multiple layouts; actual game startup, map transitions, and exit validation were still pending. See the 1.0.0 section above for the subsequent WeGame test result.

## Directory layout

- `src`: native DLL source code; no legacy launcher source.
- `runtime`: player runtime templates laid out relative to the game root; the JS manager is maintained here too.
- `templates/ts-mod`: standalone TypeScript mod development template.
- `sdk`: reflection-generated UE/PuerTS declarations, additions verified in-game, and build manifests for mod development only.
- `tests`: native, proxy DLL, JS, and runtime acceptance tools.
- `build`: compiled output and test artifacts, not distributed directly to players.
- `releases`: player directories and ZIP archives, packaged from an explicit file allowlist.
- `archive`: development directories from before the cleanup, old prototypes, and historical evidence backups; excluded from builds and releases.

## Development and packaging

To develop a TS mod, start with the [minimal development template](templates/ts-mod/README.md) (Chinese): copy `templates/ts-mod/`, run `npm ci`, `npm run check`, and `npm run build` in the copy, then install the resulting `dist/my-mod/` into the game's `ProjectPBZ/Content/JavaScript/PBZModLoader/mods/`. The template manages its own development dependencies and does not require compiling the loader DLL.

The template already references the [game API SDK](sdk/README.md) (Chinese) as a local development dependency. Use `import ... from 'ue'` / `'puerts'` for completion and type checking. SDK 0.3.3 was regenerated from Steam build CL-302256_JKS-17280 after the 2026-11-04 update. It contains 34,593 types, 49,386 functions, and 119,282 mapped properties, and retains additions verified in-game. Generation coverage, unresolved items, and call validation are tracked separately; see the SDK guide for details.

Existing mods can run `npm run sdk:pack` and install the npm package from `build/sdk/` as a development dependency; migration steps are in the SDK guide. After editing `sdk/`, maintainers should run `npm run sdk:sync` to update the template copy, then run `npm test`.

Developers can use the [read-only reflection export tool](scripts/REFLECTION.md) (Chinese) to collect native types and loaded Blueprints from the current build, producing a complete metadata manifest, counts, and parsing failure reports. Run `npm run sdk:generate -- <reflection.json> [<reflection.json> ...]` to convert these into PuerTS declarations, then synchronize the template, run tests, and package the result.

```powershell
npm ci
.\build.ps1 -Compiler C:\path\to\llvm-mingw\bin\clang++.exe -Test
.\package.ps1 -Version 1.0.0
```

Neither the main project nor release packages include preinstalled mods. Use the standalone template above to develop your own.

On each startup, the loader automatically discovers `mods/<ID>/main.js` and loads mods in ID order. TypeScript mods must be built to `main.js` first. Adding a mod requires no configuration changes. Set `ID=0` under `[Mods]` in `mods.ini` to disable it; delete the line or set it to `1` to enable it again. `Enabled` and `Version` under `[Loader]` are retained; `Count` can be omitted. Legacy `Count` / `Mod.N` entries are still loaded first in numerical order, followed by automatically discovered entries with duplicates removed. `[Mods]` switches take precedence over legacy `Enabled` values. Helper scripts are not loaded as separate mods.

The player's `mods.ini` and `mods/` reside in `ProjectPBZ/Content/JavaScript/PBZModLoader`, and `Entry` paths are relative to that directory. Starting with this release, an older `ProjectPBZ/PBZModLoader` directory is migrated automatically on first launch: configuration, `mods/`, `evidence/`, and status logs are moved into the new directory. If the new directory already has a `mods.ini`, the legacy configuration replaces it and the packaged default is saved as `mods.ini.before-migration`; conflicting files are not overwritten. Installations using the older `ProjectPBZ/Mods/PBZModLoader` level still need the manual move described in the installation instructions. `Entry` values in `mods.ini` do not need changing.

At startup, the DLL writes the manifest and loads `mods/<ID>/main.js` in place; it no longer copies MOD bodies for startup. Only hot reload creates a per-revision copy at `Content/JavaScript/PBZModLoader/generated-reloads/<revision>/generated-mods/` (a copy, not a hard link, so later edits cannot change an older revision); revision directories are cleaned up on the next startup and must not be edited directly. Hot reload is off by default; set `HotReload=1` under `[Loader]` in `mods.ini` and restart the game before using Alt+F5 (see below). `Content/JavaScript/PBZModLoader` also holds `bootstrap.js`, `manager.js`, `mods.ini`, `mods/`, `evidence/`, and the status log; unrelated files under the game's `Content/JavaScript` are untouched.

Native builds produce `version.dll`, `hid.dll`, `winmm.dll`, `dsound.dll`, and `dxgi.dll` under `build`. Use `-VerifyOnly -Test` to check existing DLLs without recompiling. `package.ps1` refuses to overwrite an existing version and checks the ZIP root layout, allowlist, and hashes of all extracted files. New code still requires actual Steam startup and map-transition acceptance testing before release; static and host tests do not replace in-game testing.

Check runtime logs with `tests/verify-run.ps1 -Log <loader.log>`. It expects zero loaded mods by default. After installing custom mods, use `-ExpectedLoaded <count>` to specify the expected number; `-RequireExit` additionally checks for exit evidence. This script checks startup, loaded mod count, and error logs. It no longer checks round-trip map transitions or GameInstance consistency; map transitions require separate in-game acceptance testing.

The first reorganized release, 0.2.0, used the same DLL that had already passed full Steam in-game testing, without recompiling or changing in-game code. Its SHA-256 is `0727d3c15b7453425df0538f30de9a6e6372e03ba9e41da4e77ee425ad0cb54b`. Provenance is recorded in `build/tested-build-info.json`; full game evidence is stored in `archive/installed-development-0.2/evidence/20261029-132016-33200-dll`.

Players do not need this source project, Node.js, a separately installed compiler, Python, a standalone launcher, or a test observer. Player installation, configuration, and uninstall instructions are in `runtime/PBZModLoader-安装说明.txt` (Chinese), included in the release ZIP.

### JavaScript runtime and TypeScript builds

The loader only discovers and loads `mods/<ID>/main.js` and its referenced JS dependencies. It no longer bundles a TypeScript compiler and does not read `main.ts`. A directory containing only `main.ts` is ignored; an explicit `.ts` Entry in `mods.ini` fails configuration with: `TypeScript MODs are not supported; build main.ts to main.js`.

Build TypeScript mods on the development machine with `templates/ts-mod`: run `npm ci`, `npm run check`, and `npm run build`, then copy `main.js` and its dependencies from `dist/<ID>/` into `mods/<ID>/`. The loader performs no startup compilation, does not read `tsconfig.json`, and produces no `compile.log`; type checking and builds happen before release. Replace any previously installed `main.ts` with the built `main.js`.

### Mod hot reload

Hot reload is off by default. Set `HotReload=1` under `[Loader]` in `mods.ini` and restart the game first (with the default `0`, Alt+F5 is not registered). Once enabled, save your JS files or dependencies under `ProjectPBZ/Content/JavaScript/PBZModLoader/mods/`, return to the game, and press **Alt+F5**. The shortcut only responds while the game window is in the foreground, does not repeat while held, and does not watch for file changes. The JS thread polls for reload requests once per second. For locally built mods, copy the updated build output into the installation directory before reloading. A notification appears in the upper-left corner for about four seconds, indicating success, failure, or a skipped reload, without intercepting mouse input.

Each request reloads the entire mod set, including unchanged mods. Separate version directories (copies) isolate CommonJS entry points and relative dependency caches. Before loading new entry points, the loader calls and awaits the exported `stop()` functions of all old instances, including Promise results. Mods must clear timers, unsubscribe from events, and release held resources in this function. Even mods without persistent resources should export an empty `stop()`. If the function is missing or cleanup throws, the loader retains the old instance and skips that mod, preventing duplicate registration by the new version. A pending `stop()` is awaited for up to five seconds; after that cleanup is treated as failed, the old instance is retained, the mod is skipped for the rest of the run, and no further reload starts until the current one finishes. Map transitions and game exit do not trigger `stop()`.

Adding or removing mods and changing `[Mods] ID=0` also take effect after Alt+F5. The loader calls `stop()` before removing or disabling a mod. A JS entry point exception may already have caused side effects while preventing access to its exports, so a failed instance is not automatically retried during that game session. Restart after fixing it. Mods should clean up resources created before an initialization failure themselves.

Temporary output is stored in `Content/JavaScript/PBZModLoader/generated-reloads/`. Version directories are retained until the next startup to support old instances and deferred `require` calls. Many reloads during a long session increase disk and module-cache usage. Only relative dependencies inside the mod directory are isolated by version; do not use absolute loader cache paths to reference modules across versions. Search logs for `hot reload published`, `stopped`, `reload complete`, and `reload blocked`.

### Diagnostic hooks and safe mode

The three PuerTS log hooks only copy `[PBZ` diagnostics into `loader.log`; they are not required to load mods. When their signatures are missing or ambiguous, the loader disables them automatically and mods still load. Control them under `[Loader]` in `mods.ini`:

- `LoggerHooks=1` (default): install diagnostic hooks when signatures match; `0` disables them.
- `SafeMode=1`: disables diagnostic hooks and Alt+F5 reload for the current run; default `0`.

For startup-crash troubleshooting, try `SafeMode=1` first. If `ProjectPBZ/Content/JavaScript/PBZModLoader/loader-initializing.marker` is left behind, the status log records `previous-run=unclean`; a successful initialization removes the marker. The status log also reports `logger-hooks` and `safe-mode`.

### Player-readable status

Each startup overwrites `ProjectPBZ/Content/JavaScript/PBZModLoader/loader-status.log` with a short summary: loader version, game path and size, entry DLL, total/enabled/disabled mods and their list, hook/diagnostic/safe-mode state, hot-reload switch, and the evidence log path. Reload results are appended to the same file. This file is a read-only summary; detailed reasons remain in `evidence/<run>/loader.log`.
