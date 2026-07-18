# enhanced-rotation

增强了 mpv 的旋转功能，支持 360 度循环。

## 来源

- 原始上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护
- C++ 重写参考仓库：`VideoPlayerCode/mpv-tools`
  （`external/plugins/mpv-tools`，固定在 commit
  `39e49a4e17d522bb9f4b8e0f95b2c5ac5a1270c6`），具体参考
  `scripts/cycle-video-rotate.lua`；许可证为 Apache License 2.0（声明于该仓库
  README，未附带独立 LICENSE 文件）

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游的改动

行为与原 Lua 版本一致：收到 `rotate-video <delta>` script-message 时，将
`video-rotate` 加上 `delta` 后归一化到 `[0, 360)`（`cycle-video-rotate.lua`
采用完全相同的取模归一化思路，验证了这个做法的正确性）；收到
`rotate-video-reset` 时归零。两者都通过 OSD 提示当前角度。

C++ 版本相对 Lua 版本的实现改动：

- 用 mpv client C API（`mpv_open_cplugin` + `mpv_wait_event` 事件循环）替代
  Lua 的 `mp.register_script_message`。
- 归一化逻辑抽成纯函数 `enhanced_rotation::normalize_rotation`
  （`include/enhanced_rotation/logic.h`），配有 Catch2 单元测试
  （`tests/test_enhanced_rotation.cpp`），覆盖正向/负向增量的回绕情况。
- 修正了一个在移植过程中发现的 mpv client API 使用陷阱：`video-rotate`
  底层是整数/choice 类型的属性，用 `MPV_FORMAT_DOUBLE` 写入会被 mpv 静默拒绝
  （读取时 INT64→DOUBLE 的隐式放宽转换没问题，但反过来设置不行）。已按照 mpv
  自带 Lua 绑定（`player/lua.c` 的 `script_set_property_number`）的做法，在
  `shared/cpp/mpv_util.h` 里对整数值改用 `MPV_FORMAT_INT64` 设置。

`input.conf` 中已有的绑定（`script-message-to enhanced_rotation ...`）无需修改
即可继续工作，因为 mpv 按插件文件名派生 client 名称，`enhanced-rotation.so`
与原来的 `enhanced-rotation.lua` 派生出的名字相同（`enhanced_rotation`）。

## 已知限制

拖拽/鼠标相关的交互无法在无 GUI 窗口的环境下自动化验证（`--vo=null` 时
`osd-dimensions`、`mouse-pos` 等属性不可用），本插件不涉及鼠标交互，已通过
mpv JSON IPC 端到端验证 `rotate-video` / `rotate-video-reset` 的行为正确。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-18：参考 `VideoPlayerCode/mpv-tools`@`39e49a4e` 的
  `cycle-video-rotate.lua` 完成 C++ 重写，Lua 实现移入 `lua-reference/`。
