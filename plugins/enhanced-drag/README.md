# enhanced-drag

仿 PotPlayer 的手势 Pane 拖拽逻辑。

## 来源

- 原始上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护
- C++ 重写参考仓库：`occivink/mpv-image-viewer`
  （`external/plugins/mpv-image-viewer`，固定在 commit
  `128b498e3e57a14deea5ca9bbf662f8c1ca79e8d`），具体参考
  `scripts/image-positioning.lua` 中的 `drag_to_pan_handler`；许可证为 The
  Unlicense（公有领域声明）

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游（mpv-scripts）的改动

原 Lua 版本（`lua-reference/enhanced-drag.lua`）通过 `video-target-params` /
`video-out-params` 手动重新计算画面在窗口中的缩放尺寸，且没有边界约束——
拖拽可以把画面拖到完全看不见的位置。

C++ 版本改为直接读取 mpv 的 `osd-dimensions` 属性（`w`/`h`/`ml`/`mr`/`mt`/
`mb`），这是 mpv 核心已经算好的、把当前缩放/旋转都考虑在内的画面渲染区域，
比手动用 `video-zoom` 反推更简洁可靠；最初参考了 `occivink/mpv-image-viewer`
的 `drag_to_pan_handler`，但以下两处上游行为都已按用户反馈去掉：

- **不限制"画面已完整可见"时的拖拽**：上游默认（`drag_to_pan_move_if_full_view
  = false`）在某个轴两侧 margin 都 `>= 0`（即 `video-zoom` 为 0、该轴完整
  可见）时不响应该轴拖拽，最初的 C++ 版本也照搬了这个门槛，但用户反馈"缩放
  为 0 时也应该能拖拽"，故已移除，任何缩放级别下两个轴都会跟随鼠标。
- **不做边界约束**：拖拽可以把画面拖到完全看不见的位置，行为与原 Lua 版本
  一致。上游脚本本身带有 `drag_to_pan_margin` 边界约束（防止画面被拖出屏幕
  太远），最初的 C++ 版本也照搬移植了这部分，但实测后用户明确反馈"拖到边界
  后就不能再往外拖"体验不好，要求取消，因此这部分边界约束已移除，不再是
  当前实现的一部分。

拖拽会把 `video-pan-x`/`video-pan-y` 累加到任意值，配合 `config/input.conf`
里 `0`/`KP0` 绑定的 `set video-zoom 0 ; set video-pan-x 0 ; set video-pan-y 0`
可以一键把缩放和拖拽偏移都重置回初始状态。

## 实现说明

- 用 mpv client C API（`mpv_open_cplugin` + `mpv_wait_event` 事件循环）替代
  Lua 的 `mp.add_key_binding` / `mp.add_periodic_timer`；拖拽中的 `key-binding`
  down/up 事件通过监听 `MPV_EVENT_CLIENT_MESSAGE`（`key-binding` /
  `drag-pan-free`）获得，鼠标位置轮询通过 `mpv_wait_event` 的超时机制实现
  （拖拽中每 1/120 秒轮询一次 `mouse-pos`，与原 Lua 版本刷新率一致）。
- `mouse-pos`、`osd-dimensions` 统一整个属性读成 `MPV_FORMAT_NODE` 再从
  node map 里取字段，不使用 `"mouse-pos/x"` 这种子属性路径 + 显式请求
  `MPV_FORMAT_DOUBLE`/`MPV_FORMAT_FLAG` 的写法——原因见下方"已修复的问题"。
- 纯几何/pan 计算逻辑抽成 `enhanced_drag::compute_drag_pan`
  （`include/enhanced_drag/logic.h`），配有 Catch2 单元测试
  （`tests/test_enhanced_drag.cpp`），覆盖缩放为 0、缩放溢出、无边界约束
  可持续拖出画面外、视频可用尺寸为 0 时保持原 pan 值等场景。
- 复用 `enhanced-rotation` 迁移时发现的 `shared/cpp/mpv_util.h`
  （整数属性需要用 `MPV_FORMAT_INT64` 而非 `MPV_FORMAT_DOUBLE` 设置）。

`input.conf` 中已有的绑定（`script-binding enhanced_drag/drag-pan-free`）无需
修改即可继续工作，原因同 enhanced-rotation：mpv 按插件文件名派生 client
名称，`enhanced-drag.so` 与原 `enhanced-drag.lua` 派生出的名字相同
（`enhanced_drag`）。

## 已知限制

拖拽逻辑依赖真实的 mpv 显示窗口（`osd-dimensions`、`mouse-pos` 在
`--vo=null` 等无窗口环境下不可用/返回哑值），因此完整的鼠标拖拽路径无法在
无 GUI 的自动化环境中端到端验证。已验证：插件可正常加载、`key-binding`
client message 能被正确路由匹配；核心 pan 计算逻辑有完整的单元测试覆盖；
共享的属性读写辅助函数已通过 enhanced-rotation 的 IPC 测试验证正确。仍建议
在真实 mpv 窗口下手动测试实际拖拽手感。

## 已修复的问题

- **根因：子属性路径 + 显式格式读取 `mouse-pos`/`osd-dimensions` 会静默返回
  错误值**。最初实现用 `mpv_get_property(handle, "mouse-pos/x",
  MPV_FORMAT_DOUBLE, ...)` 这种"点号子路径 + 显式请求 DOUBLE/FLAG"的写法读
  鼠标坐标（`osd-dimensions/w` 等同理）。在用户的真实测试环境（macOS，
  `--vo=gpu-next`）下用 `--msg-level` 加自定义调试输出实测确认：这种写法下
  `mouse-pos/x`、`mouse-pos/y` 拖拽期间稳定返回 `0`（不报错，就是返回了错误
  的值），但把 `mouse-pos` 作为整个属性以 `MPV_FORMAT_NODE` 读出来、再从
  node map 里取 `x`/`y` 字段（也就是 mpv 自带 Lua 绑定
  `mp.get_property_native("mouse-pos")`、以及本插件参考的
  `occivink/mpv-image-viewer` 脚本本身的做法），坐标是实时且正确的。据此判断
  是 mpv 属性系统里"子属性路径 GET 后再转换成显式 FORMAT"这条路径本身在
  当前 mpv 版本上有问题，而非 mouse-pos 数据本身过期。修复方式：`mouse-pos`
  和 `osd-dimensions` 统一改为读整个 NODE 再取字段，不再使用子属性路径。
- 排查过程中还发现并移除了一个**方向错误但无害**的早期尝试：曾经额外检查
  `mouse-pos/hover`，若为 false 就放弃这次拖拽/更新，理由是 mpv 文档说
  "hover 为 false 时应忽略坐标"——但那条规则的前提是没有鼠标按键按下，而
  我们读取鼠标位置的三个时机必然都有一个按键正按着，前提从不成立，所以这
  个检查本就不该存在。实测中 hover 其实全程是 `true`，说明这不是真正的
  阻塞点，但检查本身逻辑有误，予以移除。连带删除了只为它添加的
  `shared/cpp/mpv_util.h::get_flag` 帮助函数（已无调用方）。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-18：参考 `occivink/mpv-image-viewer`@`128b498e` 的
  `image-positioning.lua` 完成 C++ 重写（含边界约束改动），Lua 实现移入
  `lua-reference/`。
- 2026-07-18：修复拖拽在真实 mpv 窗口下完全无效的问题：`mouse-pos`/
  `osd-dimensions` 改为整体读 NODE 再取字段，不再用子属性路径 + 显式格式
  读取（用户在 `--vo=gpu-next` 下实测复现并协助定位）。
- 2026-07-18：应用户要求，移除从 `occivink/mpv-image-viewer` 移植的
  `margin_px` 边界约束（拖到边界后不能继续往外拖的限制），`compute_drag_pan`
  不再接受 `margin_px` 参数，`enhanced_drag::clamp` 随之删除；行为回到与原
  Lua 版本一致的无边界拖拽。
- 2026-07-18：应用户要求，移除缩放为 0（画面完整可见）时不响应拖拽的门槛；
  `enhanced_drag::DragAxes`/`resolve_drag_axes` 随之整体删除，
  `compute_drag_pan` 不再需要 axes 参数，任何缩放级别下两个轴都跟随鼠标。
  同时给 `config/input.conf` 里已有的 `0`/`KP0` 缩放重置绑定追加了
  `set video-pan-x 0 ; set video-pan-y 0`，一键同时重置缩放和拖拽。
