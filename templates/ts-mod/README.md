# PBZ TypeScript MOD 最小模板

适用于只自动发现和加载 `mods/<ID>/main.js` 的 PBZModLoader。
复制整个模板文件夹即可独立开发，不需要加载器的 C++ 工程或游戏源码。

## 开始开发

开发电脑需要可运行本模板 TypeScript 7.0.2 的 Node.js 和 npm；玩家无需安装它们。
在复制后的模板目录执行：

```powershell
npm ci
npm run check
npm run build
```

- `check`：完整、严格的 TS 类型检查，不生成文件。
- `build`：先清空本项目的 `dist/`，再检查并生成 ES2020/CommonJS；检查失败时不输出可发布文件。
- `clean`：只清空本项目的 `dist/`。不要把自己的文件存进该目录。

编辑 `src/main.ts` 即可开始。示例立即读取 GameInstance 和地图名，之后每 2 秒读取一次，
总共打印 5 次后清理定时器。初次读取失败时由加载器记录加载失败；后续读取出错时停止定时器并记录错误。
切图时不缓存 World 对象，每次重新获取 GameInstance。

## 项目结构

```text
src/main.ts          MOD 入口
vendor/game-api/     SDK 0.3.3：反射生成并附实测补充的 UE / PuerTS 类型，本地开发依赖
types/custom.d.ts    自己新增的已验证 API 声明（可选）
scripts/clean.cjs    开发机上的输出清理工具
tsconfig.json       严格检查、ES2020/CommonJS 输出
dist/my-mod/main.js  构建产物，发布给玩家
```

开始自己的 MOD 时，将 `tsconfig.json` 中的 `outDir` 改为 `dist/你的MOD_ID`，
并同步修改 `src/main.ts` 的 `MOD_ID` 日志标签。真正决定 MOD ID 的是安装后的文件夹名。
ID 只使用英文字母、数字、下划线、连字符，并选择不与其他 MOD 重复的名字。
保留入口文件名 `main.ts`；可在 `src/` 下添加模块，用 `import ... from './模块名'` 引用，编译时会一起输出。

## 安装和发布

1. 执行 `npm run build`，确认退出成功。
2. 退出游戏，把 `dist/my-mod/` 整个文件夹复制到游戏的 `ProjectPBZ/PBZModLoader/mods/` 下。
3. 确认最终路径是 `ProjectPBZ/PBZModLoader/mods/my-mod/main.js`，启动游戏。

发布时打包整个 `my-mod/` 文件夹；不要携带 `node_modules`、`src`、`types`、`vendor` 或开发脚本。
模板不会自动复制 JSON、图片等资源，也不会打包 npm 运行时依赖；需要这些能力时再扩展构建过程。
不要把 `dist/` 这一层安装到 `mods/`，也不要把产物写进 `Content/JavaScript` 的加载器缓存。
更新已有 MOD 时，退出游戏后用新构建的整个目录替换旧目录，避免遗留脚本。
发布时只保留构建后的 `main.js`；加载器不会读取或加载 `main.ts`。

无需添加配置即可自动发现。若被禁用，在 `ProjectPBZ/PBZModLoader/mods.ini` 的已有 `[Mods]` 段中
删除 `my-mod=0` 或改成 `my-mod=1`。禁用时设为 `0`，卸载时退出游戏后移除对应 MOD 文件夹。

## 调试与运行边界

- 在 `ProjectPBZ/PBZModLoader/evidence/<本次运行>/loader.log` 中查找 `[PBZMOD] my-mod`。
  示例保留 `[PBZMOD]` 前缀，使日志能进入加载器捕获范围。
- 加载器通过执行入口的顶层代码启动 MOD，不会寻找或调用导出的 `start()`。
  热重载前会调用并等待旧实例导出的 `stop()`；在其中清除全部定时器和事件订阅，支持返回 Promise。
  切图和游戏退出不会自动调用此回调。
- 修改后重新构建并覆盖安装目录中的 JS，切回游戏按 Alt+F5 重载，无需重启游戏；完成后左上角显示提示。
  仅游戏窗口在前台时响应，不监听文件变化，按住不连发；每次重载整组 MOD，包括未修改的 MOD。
  新增、删除 MOD 和 `mods.ini` 的 `[Mods]` 开关变化也会生效，删除和禁用前同样需要执行 `stop()`。
  加载器先等待所有旧实例的清理，再执行新入口；无持久资源也应保留空 `stop()`。
  缺少 `stop()` 或清理失败会保留旧实例并阻止该 MOD 重载；未结束的 Promise 会阻塞后续重载。
  入口执行失败后，为避免部分注册重复执行，该 MOD 本次运行不再重试，修复后需重启游戏。
  初始化代码应自行清理抛错前已创建的资源。
- 热重载隔离入口和 MOD 目录内相对依赖的 CommonJS 缓存，不要用加载器缓存的绝对路径引用模块。
  `Content/JavaScript/PBZModLoader/generated-mods/` 和 `generated-reloads/` 均为自动缓存，勿直接编辑。
  重载版本保留至下次启动，供旧实例及延迟 `require` 使用；长期大量重载会增加磁盘和模块缓存占用。
  排查时在日志中搜索 `hot reload published`、`stopped`、`reload complete` 和 `reload blocked`。
- 游戏提供 `ue`、`puerts` 模块，不能在普通 Node.js 中直接运行 `main.js`。
  开发构建脚本可以用 Node API，MOD 源码不要假定存在 `fs`、`process`、浏览器 `window` 等 API。
  配置刻意不引入 Node/DOM 全局类型；也不提供任意字符串 `require()` 来绕过检查。
- `@pbz-mod-loader/game-api` 通过 `tsconfig.json` 的 `types` 加载，直接 `import ... from 'ue'` 或 `'puerts'` 即有补全。
  不要在源码中导入 SDK 包本身，它只有类型，没有运行时代码。
  SDK 0.3.3 基于 Steam Win64 `CL-302256_JKS-17280`（2026-11-04 更新）重新采集，
  包含 34,593 个类型、49,386 个函数和 119,282 个已映射属性，并保留实测补充层。
  生成声明不代表每项 API 都经过调用验证，也不覆盖所有未加载蓝图；WeGame 平台已通过加载器与 MOD 运行实测；这不代表 SDK 的全部 API 均已在 WeGame 上逐项验证，其他游戏构建仍需单独验证。
  [SDK 说明与迁移方法](vendor/game-api/README.md)列出了映射约定、蓝图路径用法及限制。
  `type-index.json` 可按反射完整路径定位声明，`generation-report.json` 记录生成覆盖和未解析项，
  `api-surface.json` 记录实测证据；这些文件均位于 `vendor/game-api/`。
  新增调用前核实实际可调用性，不要直接修改生成声明；自己的补充放在 `types/custom.d.ts`。
- 加载器只加载 JavaScript，不内置 TypeScript 编译器；本模板统一在本地完成类型检查并发布构建后的 `main.js`。
  本模板统一使用本地检查并发布 JS 的流程。

## UMG / UI 文本注意事项

以下经验来自 F12 功能开关窗口的游戏内排查：背景和复选框正常、文字全部不可见时，
可能同时存在文本桥接和字体问题，不能仅凭截图认定是颜色或字体，也不能以类型检查通过作为显示正常的依据。

### 先转换为原生 FText，再写入并读回

当前游戏实测 `KismetTextLibrary.Conv_StringToText()` 返回原生 `FText` 对象（`typeof` 为 `object`）。
SDK 0.3.3 已将文本接口修正为不透明的 `UE.$FText`；直接把 JS 字符串传给
`TextBlock.SetText()` 会被类型检查拒绝。应保留转换结果，直接传给文本接口，不要再用 `String()` 或字符串拼接转换它。

```typescript
import UE = require('ue');

// widget 是已创建的 TextBlock；此写法可使用当前 SDK 声明编译。
function setLabel(widget: UE.TextBlock, label: string): void {
    const text = UE.KismetTextLibrary.Conv_StringToText(label);
    widget.SetText(text);
    const actual = UE.KismetTextLibrary.Conv_TextToString(widget.GetText());
    if (actual !== label) {
        throw new Error(`UI 文本读回不一致：expected=${label}, actual=${actual}`);
    }
}
```

读回为空或不一致时，先排查参数转换和文本写入；读回正确但画面不显示时，再排查字体、颜色与布局。
这是当前游戏的实测约定，不代表所有 PuerTS 构建都采用同一种 FText 桥接模式；更换游戏或桥接版本后需重新验证。
不要为绕过这一差异直接修改生成的 SDK 声明，必要的适配应集中在自己的 UI 辅助函数或补充声明中。

### 显式设置字体，并验证中文字形

- 运行时创建的 `TextBlock` 应显式设置 `SlateFontInfo`（字体资源、字形名称、字号）和文字颜色，
  不依赖编辑器默认样式。使用 `SetColorAndOpacity()` 时明确指定颜色规则和非零 Alpha，并确保与背景有足够对比。
- 不要直接复用遍历到的第一个 `TextBlock` 字体：HUD 计数器可能使用仅包含数字的字体。
  F12 窗口曾在修正 FText 后只显示数字，原因就是误用了这类字体。
- 优先复用游戏中已正常显示中文的标签字体，可通过 `Conv_TextToString(existingWidget.GetText())`
  筛选含中文的标签，再读取其 `Font`；记录字体资源路径和样本文本，便于排查。
  字体资源有效或加载成功不等于支持中文，备用字体也必须在目标游戏中确认中文字形与回退行为。

### 验证显示效果

模拟测试应覆盖文本转换与读回，避免用“`SetText` 接受任意字符串”的 mock 掩盖原生 FText 要求。
即使构建、类型检查和模拟测试全部通过，仍需在游戏内确认标题、中文功能名、数字及按钮文字完整可见，
并检查热重载、关闭后重新打开的显示效果。文本读回正确也不能替代画面确认。
在本加载器项目内启动测试时，使用 `scripts/影之刃零-窗口模式1280x720.cmd`，
确认窗口模式及客户区 **1280×720**；游戏已运行或尺寸异常时使用
`scripts/影之刃零-恢复1280x720.cmd`，恢复并确认后再测试。
这些脚本位于加载器仓库，不随本模板复制。

模板的编译和模拟加载检查不等于游戏实测；发布自己的 MOD 前仍需验证实际启动、切图和退出。
