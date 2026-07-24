# uosc

替代 mpv 内置 `osc.lua` 的完整 OSD 皮肤 + 菜单系统（进度条、控制栏、音量、
弹出菜单等）。从 `external/plugins/uosc` fork 而来，本地只保留了进度条
（Timeline）+ thumbfast 缩略图集成，其余元素通过 `uosc.conf` 的
`disable_elements` 关闭；并给进度条加了一段本地渲染逻辑，展示
[enhanced-ab-loop](../enhanced-ab-loop/README.md) 的多段循环区间。

## 来源

- 上游仓库：https://github.com/tomasklaen/uosc
- 迁移基准 commit：`41040532f840b8089ae1bedba906071959347771`
  （`git submodule status` 对应 `5.12.0-15-g4104053`）
- 许可证：GNU LGPL 2.1，见 [LICENSE.LGPL](LICENSE.LGPL)（原样复制自上游仓库
  根目录的 `LICENSE.LGPL`）

## 当前状态

纯 Lua 实现：

- `lua/`：对应上游 `src/uosc/` 目录（`main.lua` + `elements/`、`lib/`、
  `intl/`、`char-conv/`）。mpv 支持把 `scripts/` 下的一个目录当作脚本加载
  （目录里放 `main.lua`），所以这份 `lua/` 内容原样对应安装后的
  `scripts/uosc/`。文件本身基本保持上游原样，只在 `main.lua`（新增一处
  `user-data` 属性 observer）和 `elements/Timeline.lua`（新增一段区间渲染
  循环）各改了不到 20 行，见下方"相对上游的改动"。
- `uosc.conf`：对应上游 `src/uosc.conf`，全部配置项及默认值/注释，安装后
  放到 `script-opts/uosc.conf`；本地把 `disable_elements` 从空改成了
  `window_border,top_bar,controls,volume,idle_indicator,audio_indicator,
  buffering_indicator,pause_indicator`，只留 `timeline`。
- `fonts/`：`uosc_icons.otf`、`uosc_textures.ttf` 两个图标/纹理字体，安装
  后放到 `fonts/`。

`scripts/collect-dist.sh` 已经把 uosc 加入 `lua_plugins` 收集列表，执行后
会自动把 `lua/` 拷贝到 `dist/scripts/uosc/`、`uosc.conf` 拷贝到
`dist/script-opts/uosc.conf`、`fonts/` 拷贝到 `dist/fonts/`，`mpv
--config-dir=dist` 即可直接用。

## 功能概览（供后续禁用/定制参考）

按 `lua/elements/` 划分、各自独立的 UI 元素：

- **Timeline**：底部/悬浮进度条，支持章节标记、缓存/热力图、拖动 seek。
- **Controls**：可配置图标控制栏（菜单、字幕/音轨/视频轨切换、倍速、
  上一个/下一个等），具体按钮由 `uosc.conf` 的 `controls` 选项拼装。
- **Volume**：音量滑块。
- **TopBar**：顶部标题栏 + 窗口控制按钮（最小化/最大化/关闭），可替代
  系统窗口边框。
- **WindowBorder**：无边框窗口时绘制的自定义边框。
- **Speed**：倍速指示器。
- **PauseIndicator**：暂停状态指示，`static`/`flash`/`manual` 三种模式。
- **BufferingIndicator**：缓冲圈指示。
- **Button / CycleButton / ManagedButton**：控制栏按钮基类，非独立功能。
- **Menu**：可搜索的弹出菜单框架；主菜单、字幕/音轨/视频轨选择、播放
  列表、章节、Edition、流媒体画质、按键一览、打开文件等都是基于它的
  具体菜单实例，不是单独的 element。
- **Curtain**：菜单打开时的背景遮罩。
- **Updater**：只有用户主动执行 `script-binding uosc/update` 命令时才会
  实例化（`main.lua` 里 `bind_command('update', ...)`），向 GitHub API
  查询最新 release、可触发 `installers/unix.sh` 或 `windows.ps1` 自升级；
  不绑定按键就完全不会创建，默认没有任何后台自动检查。

配置层面可以直接用 `uosc.conf` 的 `disable_elements` 整体关掉一个或多个
元素，可选值：`window_border`、`top_bar`、`timeline`、`controls`、
`volume`、`idle_indicator`、`audio_indicator`、`buffering_indicator`、
`pause_indicator`。

会主动发起网络请求的功能只有两处，且都需要用户手动触发一次，默认启动时
不会自动联网：

- `download-subtitles` 菜单命令：向 [OpenSubtitles](https://www.opensubtitles.com)
  查询/下载字幕（同时会把当前文件的内容哈希发给对方用于精确匹配）。
- `update`/`updater` 命令：查询 GitHub 最新 release，并可执行安装脚本
  自升级。

上面这些当前全部通过 `disable_elements` 关闭了实例化（`Manager:_commit()`
按这份名单决定要不要 `:new()`，被禁用的元素模块文件仍会被 `require`——
零行为、零渲染、零事件——但不会执行任何逻辑），只保留 `Timeline` 和框架
本身（`Element`/`Elements`/`Curtain`/`lib/std`/`lib/utils`/`lib/ass`/
`lib/text`/`lib/cursor`/`lib/intl`）。

## 相对上游的改动

- **`elements/Timeline.lua`**：在原有的"Custom ranges"（`chapter_ranges`）
  渲染块之后，新增一段循环，把 `state.ab_loop_segments` 里的每一段区间画
  成进度条上的色块：`enabled=true` 的画绿色（`config.color.success`），
  `enabled=false` 的画暗淡描边；再用 `ab-loop-a` 精确等于某段自己的 `a`
  判定"当前正被原生 ab-loop 机制执行的是哪一段"，给它加亮边框。不改动其他
  任何渲染逻辑或布局代码。
- **`main.lua`**：新增一个 `mp.observe_property('user-data/
  enhanced-ab-loop/segments', 'string', ...)`，解析 enhanced-ab-loop 发布
  的 JSON 写进 `state.ab_loop_segments`（跟已有的 `ab-loop-a`/`chapter-list`
  等 observer 是同一种写法）；`state` 初始表里加了对应的空表默认值。
  enhanced-ab-loop 插件不存在或还没设置过这个属性时，`json` 是 `nil`，保持
  空列表，不影响其他任何功能。
- 具体的"发布"那一侧改动（`user-data/enhanced-ab-loop/segments` 属性怎么
  写入）在 [enhanced-ab-loop 的 README](../enhanced-ab-loop/README.md#实现说明)
  里，不在这个仓库范围内重复。

## 同步历史

- 2026-07-25：从 `external/plugins/uosc`@`4104053`
  （`5.12.0-15-g4104053`）完成初始复制。同一天把 `disable_elements` 收紧到
  只留 `timeline`，并给 `main.lua`/`elements/Timeline.lua` 加了跟
  enhanced-ab-loop 的区间展示集成（见上方"相对上游的改动"）。
