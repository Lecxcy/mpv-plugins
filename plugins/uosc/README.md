# uosc

替代 mpv 内置 `osc.lua` 的完整 OSD 皮肤 + 菜单系统（进度条、控制栏、音量、
弹出菜单等）。当前只是从 `external/plugins/uosc` 原样复制过来准备做本地
定制，尚未修改任何逻辑。

## 来源

- 上游仓库：https://github.com/tomasklaen/uosc
- 迁移基准 commit：`41040532f840b8089ae1bedba906071959347771`
  （`git submodule status` 对应 `5.12.0-15-g4104053`）
- 许可证：GNU LGPL 2.1，见 [LICENSE.LGPL](LICENSE.LGPL)（原样复制自上游仓库
  根目录的 `LICENSE.LGPL`）

## 当前状态

纯 Lua 实现，直接复制、未作任何改动：

- `lua/`：对应上游 `src/uosc/` 目录（`main.lua` + `elements/`、`lib/`、
  `intl/`、`char-conv/`）。mpv 支持把 `scripts/` 下的一个目录当作脚本加载
  （目录里放 `main.lua`），所以这份 `lua/` 内容原样对应安装后的
  `scripts/uosc/`。
- `uosc.conf`：对应上游 `src/uosc.conf`，全部配置项及默认值/注释，安装后
  放到 `script-opts/uosc.conf`。
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

## 同步历史

- 2026-07-25：从 `external/plugins/uosc`@`4104053`
  （`5.12.0-15-g4104053`）完成初始复制，尚未做任何本地改动。
