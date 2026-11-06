# 当前构建反射导出

`reflection_export.py` 是独立的开发工具，只使用 Python 标准库和 Windows 只读进程 API。
不注入 DLL、不附加调试器、不执行游戏函数、不写进程内存；不会改变加载器或已发布 SDK。

支持以下实测 Steam Win64 构建；先核对正在运行进程对应的 EXE 哈希，再选择地址配置。
未知哈希直接拒绝，不回退到旧地址。

| 构建 | EXE SHA256 | 名称块 / 对象数组 RVA |
| --- | --- | --- |
| `CL-301524_JKS-17219` | `7b4fcd286a350780faf0824d73aa150b21cbf1bccde883e6a96386cbe09973b4` | `0xdd11ad0` / `0xddb3c50` |
| `CL-302256_JKS-17280`（2026-11-04） | `4415933ce33476f14c837abb61a1b60af8b78cd81783896501913a7aac69e423` | `0xdd86ad0` / `0xde28c60` |

新版地址分别通过名称块内容和对象数组槽位独立定位，并通过 Vector、Actor、GetPlayerPawn、
EAxis 及采集前后一致性校验。定位证据在 `build/game-api-audit-20261104/`，
首份完整清单在 `build/reflection/update-20261104-PBZ_Fujizhilin_WP/`；其地图名来自引擎，
即使用户界面显示主菜单，也不应将该值改写为旧版菜单地图名。
每份清单记录实际选用的构建和配置，生成 SDK 时继续禁止混合两个构建的数据。

## 使用

需要 Windows x64、64 位 Python 3.10+。按项目约束启动游戏：

```powershell
& './scripts/影之刃零-窗口模式1280x720.cmd'
```

确认实际为 1280×720 客户区的普通窗口模式（可用 `scripts/PBZ-Windowed.ps1 -Status` 检查），
已运行或尺寸异常时先执行 `scripts/影之刃零-恢复1280x720.cmd`，等待主菜单或存档地图加载稳定，再在仓库根目录执行：

```powershell
./scripts/export-reflection.ps1 -Python 'C:\path\to\python.exe' -Label PBZ_Xigu_WP
```

脚本自动选择 `platform/steam/PBZ.exe`，不会误选 `steam/PBZ.exe` 启动包装进程。
也可传 `-GamePid 12345 -Output build/reflection/my-capture`，输出目录必须尚不存在。
Label 是调用方提供的场景标注，应根据游戏 UI/加载器日志填写，不由导出器自动推断。
工具还会读取实际 `PBZSettingsLocal` 的分辨率和窗口模式，条件不满足则拒绝导出。

Python 直接入口：

```powershell
python scripts/reflection_export.py --pid 12345 --label main-menu --output build/reflection/my-capture
python tests/reflection_test.py
```

退出码：`0` 为校验通过且无解析失败；`2` 为校验通过、清单已生成但存在未解析项；
`1` 为构建、读取或一致性校验失败。自动化应保留 `2` 的输出并检查失败报告，不把它当作无条件通过。

## 输出

| 文件 | 内容 |
| --- | --- |
| `reflection.json` | 完整反射记录及采集清单，后续声明生成器的输入 |
| `inventory.csv` | 每个类型/函数一行，便于搜索、筛选、统计 |
| `summary.json` | 构建、解析配置、采集时间、场景标注、校验结果、数量统计 |
| `failures.json` | 失败阶段、所属类型、成员名、地址、错误原因及无法解析属性的原始字段 |

记录包含类、结构、枚举和独立函数（包括委托签名）；默认对象 CDO 不作为声明。
成员包括继承关系、声明属性、函数成员列表、参数顺序、返回/输入输出/引用标志、
容器元素与键值类型、对象/结构/枚举/委托引用、枚举名称及有符号 64 位值。
函数 properties 也可能包含蓝图局部字段；以 `parameter`、`return` 区分参数和返回值。
继承成员只记录在声明它的父类型上，不在每个派生类中重复展开。

`path` 是完整 Outer 链形成的稳定标识，包含包路径。嵌套成员标识用于清单关联，
不是承诺可以直接传给游戏资源加载函数的路径。
`source=native` 表示 `/Script/` 包中的记录；`asset-or-runtime` 包含蓝图资产和临时运行时类型，
不等于全部都是持久蓝图资产。`objectIndex`、错误中的地址仅用于单次采集诊断。

未解析字段保留在所属类型中并标记 `unresolved`，不补成 `any`，不默默丢弃。
空 UEnum 指针不等于属性存储缺失：底层整数属性可解析且尺寸匹配时，保留 `enum=null`、
`underlying` 和 `enumMetadataMissing=true`。SDK 对已验证的非 64 位整数存储声明为 `number`，
不猜测枚举名称/取值集。非法非空引用、底层解析失败或尺寸不匹配仍按失败处理。
`declaredProperties` 不含函数字段；`functionFields` 包含参数、返回及局部字段；
`propertyKinds` 还包含容器的嵌套描述符，不能直接当作顶层成员总数。

## 范围与一致性

覆盖对象数组中已注册的原生反射类型和当时已加载的蓝图反射类型。
不主动加载资产、不读取未加载蓝图、不恢复未反射 C++ 成员、不解析 PuerTS 静态绑定表，
也不验证 PuerTS 可调用性。此阶段输出元数据，不修改 `sdk/types/*.d.ts`。

布局取自当前构建的实际内存，包括该构建的对象数组布局、FField Next/Owner 顺序、
ArrayProperty Inner 偏移及枚举分离的名称/数值数组。不能直接复用到其他 UE 5.7 游戏。
校验包括名称池、对象索引、链表循环、引用解析、参数数量/缓冲区边界/返回偏移、
Vector 字段、Actor 继承、GetPlayerPawn 签名、EAxis 枚举。

采集前后检查对象数组头、反射对象槽位/身份以及继承/成员链头；检测到切图或 GC/运行时
反射重建会拒绝发布清单并输出 fatal 报告。等场景稳定后使用新目录重试。
这是只读实时采集，不是冻结引擎的原子快照；上述检查不能排除所有并发修改。

## 扩展

更换游戏构建应重新定位并验证布局，添加新的哈希配置和对应回归样本，不能仅修改哈希跳过限制。
新增属性类型时保留显式失败处理。后续可以在此原始清单之上增加资产加载补全和 PuerTS 类型转换。

## 资产覆盖差异采集

`asset-inventory-probe.js` 是临时开发 MOD：调用 `GetAllAssets(..., true)` 读取当前磁盘资产登记表，
统计全部资产类别，详细导出类名含 Blueprint/Class/Struct/Enum 的类型候选及生成类/父类标签。
它不主动加载资产、不创建 Actor，也不采集函数签名。`IsSearchAllAssets=false` 时尤其不能将该快照
宣称为全部可发现资产；报告不能衡量普通 JS 模块或未反射 C++ 的覆盖率。

1. 备份游戏安装目录的 `ProjectPBZ/Content/JavaScript/PBZModLoader/mods.ini`，临时禁用其他 MOD。
2. 将探针复制为 `ProjectPBZ/Content/JavaScript/PBZModLoader/mods/asset-inventory-dev/main.js`。
3. 必须通过 `scripts/影之刃零-窗口模式1280x720.cmd` 启动；已运行时用恢复脚本。
   `powershell -NoProfile -File scripts/PBZ-Windowed.ps1 -Status` 提供只读客户区检查，
   返回 `valid=true` 才能开始测试。探针另外检查游戏设置的 1280×720、窗口模式 2。
4. 等待探针的 `end` 记录全部输出，将本次 `evidence/<run>/loader.log` 复制到新的本地采集目录。
   同时保留游戏 EXE 哈希、探针哈希、启动命令和采集前后窗口检查结果。
5. 使用下列命令生成报告；输出目录必须尚不存在：

   ```powershell
   npm run sdk:coverage -- build/<capture>/loader.log sdk/type-index.json build/<capture>/coverage
   ```

6. 关闭本次测试游戏，恢复备份的 `mods.ini`，移走临时探针，并校验恢复文件哈希。

输出包括 `report.md`、`asset-coverage.json`、`summary.json` 和按类型路径去重的
`load-candidates.json`。`covered` 仅表示 SDK 含此类型路径；`not-in-sdk` 有登记表路径依据；
`candidate-unverified` 只是缺少生成类标签时的命名推测，不应直接称作已确认缺失。
蓝图资产与 GeneratedClass 可各占一条资产记录，因此报告另外提供独立类型路径计数。
日志按帧重组，缺帧、重复帧、错误记录、不完整结束及计数不一致都会拒绝生成报告。

## 按队列加载并采集签名

`prepare-type-batches.cjs <asset-coverage.json> <new-batches.json> [label-prefix]` 按
PBZCombatAsset、Game、HuaShira、其他目录排序，只处理有登记表依据的未覆盖类型。
首批包含少量类/结构/枚举用于验证加载路径，后续每批最多 512 个路径。

开发 MOD `load-types-probe.js` 每次只读取一个 `./commands/0001.js` 形式的命令模块，
使用 `UE.Object.Load` 加载指定类型对象并检查实际完整路径；不创建 Actor、切图、调用游戏逻辑或保存。
当前批次的对象引用保留到下一批开始；每帧限制调用数量与时间预算，单次同步加载仍可能超出预算。
每个路径记录 loaded/failed 结果，批次结束后等待控制器采集，不自动加载下一批。

`capture-type-batches.cjs <controller.json>` 的配置示例：

```json
{
  "pid": 12345,
  "python": "C:/path/to/python.exe",
  "batches": "build/type-batches/batches.json",
  "archive": "build/type-batches/checkpoints",
  "reflectionRoot": "build/reflection",
  "log": "D:/game/ProjectPBZ/Content/JavaScript/PBZModLoader/evidence/<run>/loader.log",
  "commandDir": "D:/game/ProjectPBZ/Content/JavaScript/PBZModLoader/mods/load-types-dev/commands"
}
```

按前述方式备份配置、隔离其他 MOD、安装探针，并使用项目窗口脚本启动。控制器在发布每批命令前
检查客户区与进程身份；批次加载完成并稳定后执行只读反射导出，失败最多重试三次。它还检查加载结果
完整性、每个成功加载路径是否存在于有效反射记录中、采集后的窗口尺寸，最后写入独立检查点。
检查点中的失败路径不能计作成功覆盖；进程重启后须创建新的运行配置/命令序列，不能沿用旧进程身份。
只应在同一进程且命令目录未重置的情况下恢复控制器。修改驱动脚本不会更新已运行的控制器进程。

SDK 合并使用原有两次采集和检查点指向的有效 `reflection.json`。生成器逐份读取输入以限制内存占用，
继续拒绝构建不符与签名冲突。已观察到、其余属性结构完全相同的 FName 大小写差异会单独记录，
声明提供可选的大小写别名；这不意味着两种拼写在每次运行中都可用。

结束后关闭测试游戏、恢复原 MOD 配置并校验哈希、移走临时 MOD。保留命令计划、加载日志、
窗口检查、原始反射和最终 SDK 差异报告，供重建与核查使用。

## 属性映射定向验证

`tests/property-mapping-probe.js` 是命令门控的临时 MOD。启动并通过 `PBZ-Windowed.ps1 -Status`
后，才向其 MOD 目录（`mods/<ID>/`）写入 `command.js`，导出 `{targets:[{owner,member,kind,typeKind}]}`。
目标由旧版 `generation-report.json` 的 `unresolvedProperties` 与 `type-index.json` 的类型种类生成。
探针检查全部目标的 getter/setter、读取可取得的类默认对象或临时结构体；不会写类默认对象。
可选 `write` 只用于已核对存储类型的临时结构字段，不批量向不同 FieldPath 子类型写同一路径。
`targets:[]` 可单独运行代表性字段路径回环、Optional 写入和空目标函数参数转换检查。

日志按帧输出，使用 `tests/verify-property-mapping.cjs <bindings.log> <followup.log> <prior-generation-report.json> <report.json>`
核验完整性与结果。中途退出、缺帧或缺少结束标志的运行不计入成功证据。
字段路径读写已验证为字符串，空字符串可被读出但不能作为写入参数；Optional/UTF-8 样本
虽有访问器，读写后仍返回 undefined，因此不能用“访问器存在”推断值映射已支持。
ANSI 仅有绑定证据，仍保留未解析声明。原始采集目录为 `build/property-mapping-20261030/`。

## 绑定支持与异常退出复核

`tests/binding-support-probe.js` 按 `commands/0001.js`、`0002.js` 顺序接收开发命令。
每个命令导出 `label` 和 `run({UE,puerts,step,describe,hold})`；每个原生操作必须用
`step` 包装，记录操作前后边界。先备份 MOD 配置、隔离其他 MOD，按上文验证实际
1280×720 普通窗口后再发布命令；不要将已发布命令带入下一次游戏进程。
最后关闭游戏、恢复配置并校验哈希、移走安装及生成目录内的临时探针。

`tests/verify-binding-support.cjs <log> <generation-report.json> <out.json>` 仅接受完整
连续命令和成对操作日志，拒绝中途退出、错序、重复及缺失的操作结果。
0.3.2 的完整命令、窗口检查和日志位于 `build/binding-support-20261030/`，发布摘要为
`sdk/binding-support-verification.json`。`loader-first.log` 含崩溃尾部，不可计入通过；
`loader-final.log` 是独立进程中完成的复测。

不要重放 `0005.js` 中的 Kismet 嵌套属性名实验；该调用已观察到进程退出。
复测使用 `safe-fieldpath.js`：只在新建临时对象上直接赋值并重新读取嵌套字段。
采集范围内没有直接持有 FieldPath 属性的 UObject 类，不能用无关对象或错误字段名
替代该函数的成功写入验证。FieldPath 声明中的目标种类来自反射；游戏转换器接受了
部分不匹配种类的路径，不代表这些值可供引擎后续使用。

大整数枚举证据保存在 `scripts/sdk-supplemental.json` 的 `bindingVerification` 中，
绑定构建、EXE 哈希、完整成员名、原始整数和运行时精确整数。只有实测为 number 且
`BigInt(Number(raw))` 与原始整数一致的条目可以进入声明；不能将四舍五入后的值用于补全。

## FText 与 UObject 工厂复核

`tests/text-mapping-probe.js` 导出 `run()`，应在窗口检查后通过临时开发命令调用。
它用 `UE.TextBlock.StaticClass()` 和单参数 `UE.NewObject` 创建临时文本控件，
检查原生文本转换、方法/属性赋值，以及 BuiltinText 数组和 Map 值的读写回环。
控件不加入视口，不据此宣称字体或画面渲染正常；没有修改已有游戏对象。

`node tests/verify-text-mapping.cjs <loader.log> <new-report.json>` 要求完整、唯一的观测记录，
确认窗口设置、实例类型、原生对象表示及实际文本读回；缺失、重复、错误值或普通字符串
替代原生值均不计为通过。成功证据用于 `TextProperty` / `BuiltinText` 的 `UE.$FText`
声明，以及单参数 `NewObject(Class)` 声明；不猜测构造函数和工厂的其他参数形式。
本轮 `Class.Load('/Script/UMG.TextBlock')` 返回空值；原生类应从对应类型的 `StaticClass()`
取得类对象，不能将资产类路径加载经验直接套用到所有原生 `/Script/` 路径。
