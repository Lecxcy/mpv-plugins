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
- **Controls**：可配置图标控制栏（菜单、音轨/视频轨切换、倍速、上一个/
  下一个等），具体按钮由 `uosc.conf` 的 `controls` 选项拼装。
- **Volume**：音量滑块。
- **TopBar**：顶部标题栏 + 窗口控制按钮（最小化/最大化/关闭），可替代
  系统窗口边框。
- **WindowBorder**：无边框窗口时绘制的自定义边框。
- **Speed**：倍速指示器。
- **PauseIndicator**：暂停状态指示，`static`/`flash`/`manual` 三种模式。
- **BufferingIndicator**：缓冲圈指示。
- **Button / CycleButton / ManagedButton**：控制栏按钮基类，非独立功能。
- **Menu**：可搜索的弹出菜单框架；主菜单、音轨/视频轨选择、播放列表、
  章节、Edition、流媒体画质、按键一览、打开文件等都是基于它的具体菜单
  实例，不是单独的 element（字幕相关的菜单/命令已经整个删掉，见下方）。
- **Curtain**：菜单打开时的背景遮罩。
- **Updater**：只有用户主动执行 `script-binding uosc/update` 命令时才会
  实例化（`main.lua` 里 `bind_command('update', ...)`），向 GitHub API
  查询最新 release、可触发 `installers/unix.sh` 或 `windows.ps1` 自升级；
  不绑定按键就完全不会创建，默认没有任何后台自动检查。

配置层面可以直接用 `uosc.conf` 的 `disable_elements` 整体关掉一个或多个
元素，可选值：`window_border`、`top_bar`、`timeline`、`controls`、
`volume`、`idle_indicator`、`audio_indicator`、`buffering_indicator`、
`pause_indicator`。

会主动发起网络请求的功能现在只剩一处，需要用户手动触发一次，默认启动时
不会自动联网：

- `update`/`updater` 命令：查询 GitHub 最新 release，并可执行安装脚本
  自升级。

`download-subtitles`（向 [OpenSubtitles](https://www.opensubtitles.com)
查询/下载字幕）已经整个删掉了，见下方"相对上游的改动"。

上面这些当前全部通过 `disable_elements` 关闭了实例化（`Manager:_commit()`
按这份名单决定要不要 `:new()`，被禁用的元素模块文件仍会被 `require`——
零行为、零渲染、零事件——但不会执行任何逻辑），只保留 `Timeline` 和框架
本身（`Element`/`Elements`/`Curtain`/`lib/std`/`lib/utils`/`lib/ass`/
`lib/text`/`lib/cursor`/`lib/intl`）。

## 相对上游的改动

### 进度条上叠加 split-zoom-box 的分屏段（2026-07-26）

`lua/main.lua`：新增 `state.split_zoom_segments` 及对
`user-data/split-zoom-box/segments` 的 `observe_property(..., 'native', ...)`。
必须用 `'native'`——用 `'string'` + `parse_json` 会静默拿到原字符串而不是
table（与 enhanced-ab-loop 那次是同一个坑）。

`lua/elements/Timeline.lua`：ab-loop 区间与分屏段的绘制分两种情形——

- **两种区间都存在、且条高足够（>= 8*scale）时上下分层**：时间轴高度对半分，
  分屏段画上层（`config.color.match`，蓝）、ab-loop 画下层
  （`config.color.success`，绿）。
- **其余情况各自占满整条高度**：只有一种区间时没有必要让出空间；时间轴收拢
  成细进度条时也不分层——那里只有一两个像素高，切成两条 1px 既难看也读不出
  信息。

  这种情况下会把两份区间切成互不重叠的基本区间，**每段只画一次**：只有
  ab-loop 的用绿、只有分屏段的用蓝、两者重叠的用**两色逐通道平均出来的
  混合色**。不是把两个半透明矩形叠着画——那样重叠处会被后画的那个盖住、
  糊成一片，看着像渲染错误。

分层时中线取整（`round`），保证上下两条都是整像素高；中线落在半像素上会留下
一行混合像素，让其中一条看起来比另一条矮。

两者都用同一套"当前所在区间加亮 + 描边"的处理。原生 A/B 楔形标记在**任一种**
区间存在时都隐藏。

`uosc.conf`：`progress` 由 `windowed` 改为 `always`，让细进度条在全屏下也显示。


- **`elements/Timeline.lua`**：在原有的"Custom ranges"（`chapter_ranges`）
  渲染块之后，新增一段循环，把 `state.ab_loop_segments` 里的每一段区间画
  成进度条上的实心色块：`enabled=true` 的画绿色（`config.color.success`），
  `enabled=false` 的画暗淡描边；"当前激活"（播放位置落在某个 enabled 区间
  内）额外加亮边框——这个判定直接比较 `state.time` 和区间自己的
  `a`/`b`，不依赖 `ab-loop-a`/`ab-loop-b`/`loop_enabled`，所以主循环开关
  关闭时区间依然照常显示（只是不会再被 mpv 实际强制循环）。同时把原有的
  "A-B loop indicators"（原生 `ab-loop-a`/`ab-loop-b` 画的两个三角形/风筝
  形标记，只能表示唯一一个当前段）改成只在 `state.ab_loop_segments` 为空
  时才画——一旦 enhanced-ab-loop 在发布完整区间列表，就完全交给上面的实心
  色块渲染，避免同一个边界同时画三角形和色块两份重复信息。不改动其他任何
  渲染逻辑或布局代码。
- **`main.lua`**：新增一个 `mp.observe_property('user-data/
  enhanced-ab-loop/segments', 'native', ...)`，把 enhanced-ab-loop 发布的
  table 直接写进 `state.ab_loop_segments`（跟已有的
  `ab-loop-a`/`chapter-list` 等 observer 是同一种写法）；`state` 初始表里
  加了对应的空表默认值。enhanced-ab-loop 插件不存在或还没设置过这个属性
  时，值是 `nil`，保持空列表，不影响其他任何功能。**踩过的坑**：最初这
  里用的是 `'string'` 格式 + `mp.utils.parse_json` 解析 JSON 文本，结果
  发现 `parse_json` 处理"从 `observe_property` 回调参数拿到的字符串"时会
  静默返回原字符串本身、不是解析出的 table（也不报错，`get_property`/日志
  全都看着正常），导致进度条上什么都画不出来——用真实窗口截图逐像素采样
  才定位到问题不在渲染代码，而在这一步解析失败。改成让 enhanced-ab-loop
  直接发布原生 `MPV_FORMAT_NODE`（而不是 JSON 字符串）、这里用 `'native'`
  格式接收后，问题消失，具体原因见
  [enhanced-ab-loop 的 README](../enhanced-ab-loop/README.md#实现说明)。
- 具体的"发布"那一侧改动（`user-data/enhanced-ab-loop/segments` 属性怎么
  写入）在 [enhanced-ab-loop 的 README](../enhanced-ab-loop/README.md#实现说明)
  里，不在这个仓库范围内重复。
- **`lib/utils.lua` 的 `format_time`**：改成固定 `MM:SS`（分钟至少两位、
  不封顶，超过 99 分钟就自然变成三位数，比如 `123:45`），不再用
  `mp.format_time` 的 `HH:MM:SS` 再按 `max_seconds` 裁剪——原来的裁剪逻辑
  在文件时长 < 1 分钟时会把分钟也裁掉、只剩秒数（比如 20 秒的文件上显示
  `12`/`-08`），时长 ≥ 1 小时又会显示出小时位（比如 70 分钟显示成
  `01:10:00`）。这个函数是进度条左右两侧时间、hover 时间提示（含
  thumbfast 缩略图旁边那个提示）、章节 tooltip、顶栏剩余时间共用的唯一
  实现，改这一处全部生效。
- **删掉了字幕相关的全部功能代码**（不只是禁用，是整个删掉，起因是上游
  自带的 OpenSubtitles 应用 key 被公司的通用密钥扫描器误报成疑似泄露——
  这个 key 其实是标识客户端用的公开值，不是账号凭证，类似 OAuth 里的
  `client_id` 而不是 `client_secret`，但扫描器分不出这个区别）：
  - `lib/menus.lua`：删掉整个 `open_subtitle_downloader` 函数（原文件
    最后约 250 行，从 `download-subtitles` 菜单一直到文件末尾）。
  - `main.lua`：删掉 `open_subtitles_api_key`/`open_subtitles_agent`
    配置项、`subtitle_types`/`subtitles_directory` 默认值、
    `config.types.subtitle`、默认菜单树里的"Subtitles"条目、
    `download-subtitles`/`load-subtitles`/`subtitles` 三个
    `bind_command`，以及 `controls` 默认值字符串里的 `<video,audio>subtitles`
    段。`load-audio`/`load-video`/`audio`/`video`/`playlist` 等其余
    `bind_command` 均未受影响，共用的
    `create_track_loader_menu_opener`/`create_select_tracklist_type_menu_opener`
    两个通用菜单构造函数也原样保留（本来就是 audio/video/subtitle 共用，
    参数化的，不是字幕专属代码）。
  - `lib/utils.lua`：`load_track(type, path)` 去掉了
    `if type == 'sub' then ... end` 那个字幕专属分支，类型注解从
    `'sub'|'audio'|'video'` 改成 `'audio'|'video'`。
  - `uosc.conf`：同步删掉 `subtitle_types`/`subtitles_directory` 两个
    选项及其注释、`controls` 默认值里的 `<video,audio>subtitles`、
    `{shorthand}` 语法说明里的 `subtitles` 示例。**没有**动
    `has_sub`/`has_many_sub` 这两个 disposition 说明——它们是根据文件轨道
    信息算出来的通用状态标记（跟 `has_audio`/`has_video` 同一套机制），
    不是"字幕下载/选择"这个功能本身。
  - **没有动** `elements/Controls.lua` 里那个 `subtitles = 'command:...'`
    按钮定义——`Controls` 整个元素都在 `disable_elements` 里，永远不会
    被实例化，这个文件完全不运行，改它没有实际收益，纯粹是历史遗留在一个
    死文件里。

  以上删除后，`ctest` 115 个测试仍然全绿，也用 IPC 重新跑过一遍
  ab-loop 区间集成的端到端验证（插入/切换区间、读回
  `user-data/enhanced-ab-loop/segments`、匹配当前激活段），结果和删除前
  完全一致，确认这次删除没有影响任何实际在用的功能。

## 同步历史

- 2026-07-25：从 `external/plugins/uosc`@`4104053`
  （`5.12.0-15-g4104053`）完成初始复制。同一天把 `disable_elements` 收紧到
  只留 `timeline`，并给 `main.lua`/`elements/Timeline.lua` 加了跟
  enhanced-ab-loop 的区间展示集成（见上方"相对上游的改动"）；随后根据实测
  反馈调整了三处：区间显示不再依赖 `ab-loop-a`/主循环开关（改成直接比较
  `state.time`）、原有的单一三角形标记在有区间列表时让位给实心色块（避免
  重复），确认多段区间会同时全部画出来，不再只剩一段；随后发现这几处改动
  在真机上跟改之前毫无视觉差异，用真实窗口截图排查后定位到根因其实是
  `user-data/enhanced-ab-loop/segments` 用 JSON 字符串 + `parse_json`
  这条链路会静默解析失败（`state.ab_loop_segments` 一直是空表），把这个
  observer 换成 `'native'` 格式接收 enhanced-ab-loop 直接发布的
  `MPV_FORMAT_NODE` 后解决，用截图逐像素采样确认了两段区间同时正确显示
  为实心色块；同一天还把 `lib/utils.lua` 的 `format_time` 改成固定
  `MM:SS`（不再在短文件上裁剪掉分钟、也不再在超过 1 小时后冒出小时位），
  用截图确认 20 秒的测试文件上正确显示 `00:12`/`-00:08` 而不是原来的
  `12`/`-08`；最初只把 `open_subtitles_api_key` 清空成空字符串应对密钥
  扫描器误报，后来确定这套配置固定不再跟上游同步，索性把字幕相关的
  功能代码整个删掉（`lib/menus.lua` 的 `open_subtitle_downloader`、
  `main.lua` 里对应的配置项/菜单条目/`bind_command`、
  `lib/utils.lua` 的 `load_track` 里字幕专属分支），细节见上方
  "相对上游的改动"；删除后 `ctest` 和 ab-loop 区间集成的端到端验证
  结果与删除前一致。
