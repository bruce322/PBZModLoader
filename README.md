[![PBZModLoader — Alt+F5 Hot Reload](docs/images/pbzmodloader-cover.png)](https://www.nexusmods.com/phantombladezero/mods/113)

# PBZModLoader 主工程

**简体中文** | [English](README.en.md)

PBZModLoader 是《影之刃零》（Phantom Blade Zero）的 MOD 加载器，为玩家提供统一的脚本 MOD 加载入口，也为开发者提供 JavaScript / TypeScript 开发模板和游戏 API 类型声明 SDK。支持自动发现 MOD、配置启用或禁用，以及通过 `Alt+F5` 热重载支持清理生命周期的 MOD。

**下载与发布页面：[Nexus Mods · PBZModLoader](https://www.nexusmods.com/phantombladezero/mods/113)**

### MOD 开发模板

**直接下载：[MOD 开发模板 ZIP（模板 0.2.0 / SDK 0.3.3）](https://github.com/zoneoflkf/PBZModLoader/releases/download/v1.0.0/PBZModLoader-TS-Template-0.2.0-SDK-0.3.3.zip)**

想制作自己的 MOD，可从 [TypeScript MOD 最小模板](templates/ts-mod/) 开始，详细步骤见 [模板开发说明](templates/ts-mod/README.md)。模板包含示例入口、构建脚本和游戏 API SDK，提供 UE / PuerTS 类型补全与严格类型检查；复制整个模板目录即可独立开发，无需编译加载器 C++ 工程。

开发电脑安装 Node.js 和 npm 后，在模板副本中依次执行 `npm ci`、`npm run check`、`npm run build`。修改 `src/main.ts` 编写逻辑，将构建生成的 `dist/my-mod/` 整个文件夹放入游戏的 `ProjectPBZ/PBZModLoader/mods/`，即可由加载器自动发现。玩家只需安装加载器和构建后的 MOD，无需安装开发工具。

### 技术路线

- **原生 DLL 代理引导**：以 `version.dll` 为默认入口，并提供 `hid.dll`、`winmm.dll`、`dsound.dll`、`dxgi.dll` 备用入口。代理转发系统 DLL 导出，在游戏启动阶段接入加载器；安装时只使用一个入口。
- **特征扫描与校验**：扫描游戏可执行文件中的指令特征，结合字符串和调用关系定位 `MainStart`、PuerTS 启动及日志接口，不依赖固定地址。匹配缺失或存在歧义时跳过 MOD 加载；游戏更新仍可能需要适配。
- **零拷贝镜像视图**：启动时直接扫描、校验已映射的 EXE 镜像，不再按 `SizeOfImage` 分配并复制整镜像。`ImageView` 为当前进程保留可读节元数据并使用非拥有视图；测试仍可使用自有文件副本。
- **复用游戏内置脚本环境**：通过游戏已有的 PuerTS 执行引导脚本和 MOD 管理器，使构建后的 JS MOD 能够使用游戏暴露的 UE API。加载器将 MOD 同步到脚本搜索目录，并对本地加载器脚本及生成内容进行限定范围的校验适配。
- **JavaScript 运行时与 TypeScript 开发支持**：运行时只加载 JavaScript（ES2020/CommonJS），不内置 TS 编译器；TS 开发者在模板中构建出 `main.js` 后再安装。SDK 基于游戏反射信息生成类型声明，配合独立模板提供补全与类型检查；热重载通过 `stop()` 清理旧实例并隔离模块缓存。

本仓库包含加载器源码、运行文件模板、SDK 和开发工具。玩家请从上方发布页面获取安装包；本地构建的玩家运行包位于 `releases/`，游戏安装目录只需保留运行文件。

## 1.0.0：首个正式发布版

将原 0.3.7-preview 的打包内容作为 1.0.0 正式发布，后续发布不再默认使用 preview 后缀。

已确认 WeGame 主程序位于 `platform/wegame`。安装包同时包含
`platform/steam/version.dll` 和 `platform/wegame/version.dll`，两份文件来自同一构建。
无同名 DLL 冲突时，两个平台均可直接解压到游戏根目录，无需手动搬移 DLL。
本次仅调整打包布局和安装说明，沿用 0.3.6 的 DLL；WeGame 平台已实测可用，已确认加载器自动加载与 MOD 运行。

## 0.3.6-preview：11 月 4 日游戏更新兼容

新增此次 Steam 更新的 MainStart 指令布局，修复启动日志出现
`MainStart signature requires one validated match; found=0` 后跳过全部 MOD 的问题。
新版 FString 参数使用 `[rsp+60h]`，旧版使用 `[rbp-69h]`；两种完整指令布局分别匹配，
仍共同要求唯一候选、MainStart 字符串及 Start 跳板验证，三个 PuerTS 日志挂钩校验保持不变。
新版游戏还增加了模块字节与执行源码校验。加载器识别两个校验函数及五处调用，
仅为 `PBZModLoader` 下的引导、管理器、配置及生成的 MOD 目录提供适配：
传入内容须与对应本地文件一致，执行源码只允许原文或 PuerTS 的 CommonJS 包装。
原游戏脚本仍由原校验函数处理；路径越界、内容不一致或特征缺失时拒绝。
PuerTS 警告和错误现在也写入 loader.log，便于定位引导脚本执行前的失败。
不使用固定地址回退。升级时退出游戏，替换当前选用的代理 DLL，并同步更新 bootstrap.js、manager.js；配置与 MOD 无需改动。

先前的 0.3.5-preview 只补充 MainStart 指令匹配，遗漏了脚本校验适配，会导致原游戏菜单初始化异常，
已撤回到 releases/archive，不应继续使用。

`build.ps1 -Test -GameExe <游戏EXE>` 支持旧版基准和本次更新，覆盖两种栈布局、跨布局歧义、
栈参数不一致拒绝、两个脚本校验函数和五处调用的完整性，以及原有重定位、ASLR 和日志挂钩检查。
原生测试另覆盖本地脚本内容、UTF-8、CommonJS 包装、官方脚本路径及越界拒绝。

## 0.3.4-preview：备用 DLL 入口

默认使用 `version.dll`，正式构建同时提供 `hid.dll`、`winmm.dll`、`dsound.dll`、`dxgi.dll`。
安装包默认入口位于 `platform/steam/version.dll` 和 `platform/wegame/version.dll`；其余四个放在顶层 `备用入口/`，不会自动启用。
**本加载器只选择一个入口安装到实际游戏 EXE 旁，保持原文件名，不要同时安装多个。**

如果其他 MOD 已占用 `version.dll`，只合并包内 `ProjectPBZ`，再从 `备用入口/` 选择一个未被占用的
DLL 复制到实际 EXE 旁；不要解压默认 `version.dll` 覆盖其他 MOD。更换入口时移走本加载器旧入口；
升级时只替换已选择的入口并更新运行文件，保留配置和 MOD。卸载时也只移除本加载器选用的入口。
无需在 `mods.ini` 配置入口，不能靠重命名 `version.dll` 代替对应的专用代理。

四个入口已在当前 Steam 构建完成启动验证；其他平台、切图、长期运行及与其他 MOD 共存不在此保证范围内。
备用入口解决文件名冲突，不解决多个 MOD 的游戏逻辑冲突。误装多个本版本入口时只有一个安装钩子，
其余保留系统函数转发；这只是保护机制，仍应按单入口安装。

构建使用 `src/version.def` 和 `src/proxies/*.def` 中固定的导出清单，`scripts/generate-proxies.cjs`
生成五组配置与转发桩，不读取构建机的 System32。导出名优先按名称转发，NONAME 按序号转发；
系统函数按需解析，未使用的可选接口缺失不会阻止启动。未知系统上实际调用不存在的接口时快速失败。
这些清单源自本机实测系统的导出面；并不保证所有历史 Windows 的未公开序号 ABI。
只有启动期静态加载的代理会安装 EXE 入口钩子，运行中 LoadLibrary 加载时仅提供函数转发。

`build.ps1 -Test` 验证五种代理的静态加载、入口恢复、代表性 API 转发、并发调用、导出数量，
以及重复入口与晚加载保护；原有 version 多布局、ABI 和游戏特征测试继续保留。
`package.ps1` 默认生成正式版 `1.0.0`，检查全部五种 DLL、两个平台的默认入口、ZIP 白名单及解压哈希。

## 0.3.0-preview：平台无关引导

DLL 不再检查 Steam 路径、进程文件名或固定 EXE 哈希。将 version.dll 放在实际游戏进程的 EXE 旁；
从该 EXE 目录向上寻找含 Content、加载器配置和 bootstrap.js 的 ProjectPBZ。
启动壳进程、未知构建和错误安装不会通过完整特征校验。必须由游戏在 EXE 入口执行前加载代理 DLL。

src/signatures.hpp 仅扫描可执行 PE 节，使用带通配符的指令序列，结合 MainStart 字符串、
Start 跳板指令、JSLogger.cpp 源文件引用、日志等级和共享日志类别地址定位挂钩。
所有匹配必须唯一，引用须位于有效 PE 节，所有挂钩验证完成后才修改游戏代码。
跳板及调用原始字节从实际匹配位置取得，不再依赖固定 RVA 或固定相对位移。
特征不匹配或歧义时跳过 MOD，继续原游戏；不会回退到旧固定地址。

这组特征来自 CL-301524_JKS-17219，并补充了 2026-11-04 Steam 更新的 MainStart 布局，允许地址和相对位移变化，但不是任意版本的通用 ABI。
编译器、运行时结构或保护机制变化仍可能需要新特征。WeGame 主程序目录已确认为
platform/wegame；WeGame 平台已通过加载器自动加载与 MOD 运行实测。

新增检查：
```powershell
.\build.ps1 -Compiler C:\path\to\clang++.exe -Test -GameExe C:\path\to\PBZ.exe
.\package.ps1 -Version 1.0.0
```
-GameExe 只读文件，不启动或修改游戏；该回归测试针对已知 Steam 基准文件，
验证扫描结果、地址重排、ASLR、调用位移变化及歧义/损坏拒绝。
安装包包含 ProjectPBZ、platform、备用入口和安装说明，默认 DLL 分别位于
platform/steam/version.dll 和 platform/wegame/version.dll。
Steam 和 WeGame 首次安装均可直接解压到游戏根目录，合并同名文件夹，DLL 随包放到对应主程序目录。
遇到已有同名 DLL 应先确认来源，更新时保留自己的配置和 MOD。
当前新版通过静态匹配、原生/JS 测试及多布局代理宿主测试，尚待实际游戏启动、切图和退出验收。

## 目录

- src：原生 DLL 源码，无旧启动器源码。
- runtime：玩家文件模板，按游戏根目录布局；JS 管理器也在此维护。
- templates/ts-mod：独立的 TypeScript MOD 开发模板。
- sdk：反射生成的 UE/PuerTS 类型声明、实测补充及构建清单，仅供 MOD 开发引用。
- tests：原生、代理 DLL、JS 和运行验收工具。
- build：编译输出及测试产物，不直接给玩家。
- releases：玩家目录及 ZIP；只从明确的文件白名单打包。
- archive：整理前的开发目录、旧原型与历史证据备份，不参与构建或发布。

## 开发与打包

开发自己的 TS MOD 可从 [最小开发模板](templates/ts-mod/README.md) 开始：复制 `templates/ts-mod/`，
在副本中执行 `npm ci`、`npm run check`、`npm run build`，将输出的 `dist/my-mod/` 安装到
游戏的 `ProjectPBZ/PBZModLoader/mods/`。模板独立维护开发依赖，不需要编译加载器 DLL。

模板已通过本地开发依赖引用 [游戏 API SDK](sdk/README.md)，直接 `import ... from 'ue'` / `'puerts'`
即可获得补全和类型检查。SDK 0.3.3 基于 2026-11-04 更新的 Steam 构建 CL-302256_JKS-17280 重新采集，
包含 34,593 个类型、49,386 个函数和 119,282 个已映射属性，并保留实测补充接口；
生成覆盖、未解析项与调用验证分别记录，详见 SDK 说明。
已有 MOD 可运行 `npm run sdk:pack`，将 `build/sdk/` 中的 npm 包安装为开发依赖；迁移步骤见 SDK 说明。
维护者修改 `sdk/` 后运行 `npm run sdk:sync` 更新模板副本，再执行 `npm test`。

开发者可使用[只读反射导出工具](scripts/REFLECTION.md)，采集当前构建的原生类型与已加载蓝图，
输出完整元数据清单、数量统计和解析失败报告。使用 `npm run sdk:generate -- <reflection.json> [<reflection.json> ...]`
可转换为 PuerTS 类型声明；随后同步模板、运行测试并打包。

```powershell
npm ci
.\build.ps1 -Compiler C:\path\to\llvm-mingw\bin\clang++.exe -Test
.\package.ps1 -Version 1.0.0
```

主工程和发布包均不预装 MOD；开发自己的 MOD 请使用上述独立模板。
每次启动自动发现 `mods/<ID>/main.js`，按 ID 排序加载；TypeScript MOD 需先构建为 `main.js`。新增 MOD 无需修改配置。
在 mods.ini 的 `[Mods]` 下设置 `ID=0` 可禁用，删除该行或设为 `1` 恢复。
`[Loader]` 的 Enabled、Version 保留；Count 可省略。旧 Count / Mod.N 配置仍优先按编号加载，
自动发现结果随后追加并去重；`[Mods]` 开关优先于旧项 Enabled。辅助脚本不会作为独立 MOD 加载。
玩家的 mods.ini 与 mods/ 均位于 ProjectPBZ/PBZModLoader，Entry 相对于该目录。
旧版带 Mods 层的目录需整体上移到 ProjectPBZ/PBZModLoader，并同步更新 DLL；
具体迁移步骤见安装说明，mods.ini 中的 Entry 无需修改。
启动时 DLL 只生成清单，并在 Content/JavaScript/PBZModLoader/generated-mods 建立指向 mods/ 的硬链接视图
（同卷零字节；不支持硬链接时回退复制），不复制 MOD 正文。该缓存每次启动重建，不应直接编辑。
热重载默认关闭；在 mods.ini 的 [Loader] 设置 HotReload=1 并重启游戏后，按 Alt+F5 可触发，详见下文。
Content/JavaScript 下只需随包安装 bootstrap.js 和 manager.js，MOD 源文件无需放在那里。
原生编译输出 build 下的 version.dll、hid.dll、winmm.dll、dsound.dll、dxgi.dll。
-VerifyOnly -Test 可检查已有 DLL 而不重新编译它们。
package.ps1 拒绝覆盖已有版本，并检查 ZIP 根目录、白名单及解压后全部文件哈希。
发布前仍需对新代码进行实际 Steam 启动和切图验收；静态及宿主测试不能替代游戏实测。

运行日志可用 `tests/verify-run.ps1 -Log <loader.log>` 检查，默认预期加载 0 个 MOD；
安装自定义 MOD 后使用 `-ExpectedLoaded <数量>` 指定预期数量，`-RequireExit` 可额外检查退出证据。
该脚本检查启动、加载数量与错误日志，不再检查地图往返或 GameInstance 一致性；切图行为需单独实测验收。

首个整理版 0.2.0 使用此前通过完整 Steam 实测的同一个 DLL，未重新编译或修改游戏内代码。
其 SHA-256 为 0727d3c15b7453425df0538f30de9a6e6372e03ba9e41da4e77ee425ad0cb54b。
来源记录为 build/tested-build-info.json；完整游戏证据保存在 archive/installed-development-0.2/evidence/20261029-132016-33200-dll。

玩家不需要本主工程、Node.js、额外安装的编译器、Python、独立启动器或测试观察器。
玩家安装、配置及卸载说明在 runtime/PBZModLoader-安装说明.txt，并随 ZIP 发布。

### JavaScript 运行时与 TypeScript 构建

加载器只发现和加载 `mods/<ID>/main.js` 及其引用的 JS 依赖，不再内置 TypeScript 编译器，也不读取 `main.ts`。只包含 `main.ts` 的目录会被忽略；若在 `mods.ini` 的 Entry 中显式写入 `.ts`，配置加载会直接报错：`TypeScript MODs are not supported; build main.ts to main.js`。

TypeScript MOD 应使用 `templates/ts-mod` 在开发机执行 `npm ci`、`npm run check`、`npm run build`，把生成的 `dist/<ID>/` 中的 `main.js` 及依赖复制到 `mods/<ID>/`。加载器不做启动编译、不读取 `tsconfig.json`，也没有 `compile.log`；类型检查与构建都在发布前完成。旧版直接安装的 `main.ts` 需替换为构建后的 `main.js`。

### MOD 热重载

热重载默认关闭。先在 `mods.ini` 的 `[Loader]` 段设置 `HotReload=1` 并重启游戏（默认 0 时 Alt+F5 不会注册）。启用后，保存 `ProjectPBZ/PBZModLoader/mods/` 下的 JS 或依赖文件，切回游戏按 **Alt+F5**。仅游戏窗口在前台时响应，按住不连发，不监听文件变化。JS 线程每秒接收一次重载请求。本地构建的 MOD 需先把新产物覆盖到安装目录。完成后左上角显示约 4 秒提示，区分成功、失败或跳过；提示不拦截鼠标。

每次快捷键重载整组 MOD（包括未修改的 MOD），以独立版本目录（副本）隔离 CommonJS 入口及相对依赖缓存。加载新入口前，先调用并等待所有旧实例导出的 `stop()`，支持 Promise；MOD 须在其中清除定时器、取消事件订阅并释放持有的资源。无持久资源的 MOD 也应导出空 `stop()`。缺少该函数或清理抛错时保留旧实例并跳过该 MOD，新版本不会重复注册。`stop()` 超过 5 秒未完成时判定清理失败，保留旧实例并跳过该 MOD（日志 `reload blocked ... stop() timed out`），本次运行不再重试；期间不会启动另一轮重载。切图和游戏退出不触发 stop。

新增、删除及 `[Mods] ID=0` 的变化按 Alt+F5 后同样生效；删除或禁用前也会调用 stop。JS 入口执行异常可能已经产生副作用且无法取得其导出，因此失败实例在本次游戏运行中不再自动重试，修复后需重启。MOD 应自行清理初始化失败前产生的资源。

临时产物在 `Content/JavaScript/PBZModLoader/generated-reloads/`，版本目录保留至下一次启动清理，以支持旧实例和延迟 require；长时间大量重载会增加磁盘和模块缓存占用。仅 MOD 目录内的相对依赖随版本隔离，勿用加载器缓存绝对路径跨版本引用。日志查找 `hot reload published`、`stopped`、`reload complete` 和 `reload blocked`。

### 诊断钩子与安全模式

加载器的 3 个 PuerTS 日志钩子只用于把 `[PBZ` 日志写进 `loader.log`，不是加载 MOD 所必需。找不到匹配签名时会自动关闭，MOD 仍正常加载。可在 `mods.ini` 的 `[Loader]` 段显式控制：

- `LoggerHooks=1`（默认）：签名完整时安装诊断钩子；`0` 关闭。
- `SafeMode=1`：本次运行关闭诊断钩子和 Alt+F5 热重载，用于排查崩溃；默认 `0`。

排查启动崩溃时可先设 `SafeMode=1`。若 `ProjectPBZ/PBZModLoader/loader-initializing.marker` 残留，状态日志会写 `previous-run=unclean`；加载器成功初始化后会删除该标记。状态日志同时列出 `logger-hooks` 与 `safe-mode` 状态。

### 玩家可读状态

每次启动会在 `ProjectPBZ/PBZModLoader/loader-status.log` 覆盖写入一份简表：加载器版本、游戏路径与大小、入口 DLL、MOD 总数/启用/禁用及列表、挂钩/诊断/安全模式状态、热重载开关和 evidence 日志路径；重载结果会追加到同一文件。该文件是只读摘要，详细原因仍看 `evidence/<本次运行>/loader.log`。
