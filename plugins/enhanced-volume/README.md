# enhanced-volume

修改了 mpv 的音量调整逻辑及显示，支持长按连续变化。

## 来源

- 原始上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游（mpv-scripts）的改动

行为与原 Lua 版本一致，未改变任何实际功能：

- `volume-up`/`volume-down` 按键：单击调整 `config.tap_step`（5）个百分点；
  长按先等待 `hold_delay`（0.28s），之后按 `hold_profile` 定义的阶梯
  （0.8s 内每 0.18s、1.8s 内每 0.12s、之后每 0.07s）连续调整，间隔随长按时长
  逐渐变短。
- `adjust-volume` script-message：接受一个数值参数，直接按该增量调整音量，
  供外部（如鼠标滚轮绑定）复用。
- 音量始终夹到 `[0, max_volume]`（300）；OSD 提示静音时显示 `Mute`，否则显示
  `Volume: N%`（四舍五入到整数）。

C++ 版本相对 Lua 版本的实现改动：

- 用 mpv client C API（`mpv_open_cplugin` + `mpv_wait_event` 事件循环）替代
  Lua 的 `mp.add_key_binding`（`complex = true`）、
  `mp.register_script_message` 和 `mp.add_periodic_timer`。按键 down/up/
  repeat/press 状态通过监听 `MPV_EVENT_CLIENT_MESSAGE`
  （`key-binding`/`volume-up`、`key-binding`/`volume-down`）获得，做法与
  enhanced-ab-loop、enhanced-drag 一致；长按连发不用独立定时器，而是复用
  enhanced-drag 已验证的手法——长按会话存在期间把 `mpv_wait_event` 的超时
  参数设成 `timer_resolution`（0.02s），靠超时事件（`MPV_EVENT_NONE`）驱动轮
  询，命中调速阶梯里该出下一步的时刻才真正调整音量。
- 音量数值计算（clamp、加减）、OSD 文案格式化、长按调速阶梯的档位选择与状态
  推进（`HoldState`/`tick_hold`）都抽成纯函数，放在
  `include/enhanced_volume/logic.h` / `src/logic.cpp`，配有 Catch2 单元测试
  （`tests/test_enhanced_volume.cpp`），覆盖音量夹取边界、OSD 四舍五入、
  长按调速阶梯在各时间点的档位切换。
- 复用 `shared/cpp/mpv_util.h` 里已有的属性读写和 OSD 辅助函数，未新增专属
  的 mpv API 使用陷阱。

`input.conf` 中已有的绑定（`script-binding enhanced_volume/volume-up` /
`volume-down`）无需修改即可继续工作，原因同 enhanced-drag/enhanced-rotation：
mpv 按插件文件名派生 client 名称，`enhanced-volume.so` 与原
`enhanced-volume.lua` 派生出的名字相同（`enhanced_volume`）。

## 已知限制

长按连发的实际手感（调速阶梯是否顺滑）依赖真实按键的 down/up 时序，已通过
mpv JSON IPC 验证 `adjust-volume` 单次调整和 `key-binding` 事件路由；核心
数值与调速逻辑有完整的单元测试覆盖，仍建议在真实 mpv 窗口下手动测试长按
手感。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：完成 C++ 重写，行为不变，Lua 实现移入 `lua-reference/`。
