# drag-zoom-box

仿 PotPlayer 的手势 Zoom 逻辑并进行优化，仅放大框选范围，且只允许两个对角
方向的手势：左上到右下拖拽放大，右下到左上拖拽恢复原始缩放。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游（mpv-scripts）的改动

原 Lua 版本（`lua-reference/drag-zoom-box.lua`）的 `is_zoom_selection` 只看
水平方向（`current_pos.x >= start_pos.x` 就判定为放大，否则判定为恢复），
四个对角线拖拽方向里，右上到左下、左下到右上这两个"反对角线"分别被错误地
归到了放大/恢复，纯水平拖拽同样会触发；纯垂直拖拽虽然 `x` 不变也会落进
"放大"分支，但因为框选宽度为零，`apply_box_zoom` 里 `du<=0` 的检查会让它
静默不生效。

C++ 版本按用户明确要求收紧为**只认两个对角线方向**：

- **左上→右下**（`dx>0` 且 `dy>0`）：放大到框选范围（`DragDirection::kZoomIn`）。
- **右下→左上**（`dx<0` 且 `dy<0`）：恢复缩放（`DragDirection::kReset`）。
- 其余方向（两个"反对角线"、纯水平、纯垂直、原地未动）一律判定为
  `DragDirection::kNone`，不触发任何动作。

两个方向都额外要求两个轴上的位移分别达到 `kMinDragPixels`（4 像素，沿用
原 `shared/lua/plugin_config.lua` 里 `min_selection_pixels` 的值）才算数，
不是从零开始就判定方向——原版这个阈值只用来判定"框选是否足够大到可以应用
缩放"，恢复分支完全没有下限，一像素的轻微抖动就会把缩放清零；重写后阈值改
成方向判定本身的门槛，两个分支都受它保护，减少手抖误触发恢复。

拖拽中框选矩形的可视化叠加层新增了第三种边框颜色（`808080` 中性灰）：当前
方向还没达到 `kZoomIn`/`kReset` 的判定条件时用它显示，让用户在拖拽过程中
就能看出"这个方向不会触发任何动作"，这是上游 Lua 版本没有的反馈——原版
overlay 只有 `color_zoom`/`color_reset` 两种颜色，任何拖拽方向最终都会被
归到其中一种。

几何计算改用 mpv 的 `osd-dimensions` 属性（`ml`/`mr`/`mt`/`mb`）直接取"视频
当前渲染进窗口的矩形"，取代原版手动用 `video-target-params`/
`video-out-params` 配合 `video-zoom`/`video-pan-x`/`video-pan-y` 重新推算
同一个矩形——这是 mpv 核心已经算好、把当前缩放/平移/旋转都考虑在内的结果，
做法与 `enhanced-drag` 重写时的改进一致（见该插件 README）。`video-zoom`/
`video-pan-x`/`video-pan-y` 三个属性在放大分支里完全不需要读取，只在应用新
缩放结果、以及恢复分支清零时写入。

## 实现说明

- 分层：`include/drag_zoom_box/logic.h` + `src/logic.cpp` 是不依赖 mpv 的纯
  函数（方向判定、矩形归一化、几何换算、缩放/平移计算），配 Catch2 单元
  测试（`tests/test_drag_zoom_box.cpp`），覆盖四个对角线方向的判定、纯
  水平/垂直拖拽、最小拖拽距离阈值边界、几何输入非法/退化的拒绝、框选越出
  可见视频矩形时的裁剪与整体落空两种场景。
- 按键绑定：C 插件没有 Lua `mp.add_key_binding(..., {complex=true})` 的直接
  等价物，参照 `enhanced-drag`/`enhanced-ab-loop` 的写法，直接接住 mpv
  转发过来的 `key-binding` client message，只响应 `d`（按下开始拖拽）/`u`
  （松开结束拖拽，应用动作）两种状态，忽略 `r`/`p`。`input.conf` 中已有的
  绑定（`script-binding drag_zoom_box/drag-zoom-box`）无需修改即可继续
  工作：mpv 按插件文件名派生 client 名称，`drag-zoom-box.so` 与原
  `drag-zoom-box.lua` 派生出的名字相同（`drag_zoom_box`）。
- `mouse-pos`/`osd-dimensions`/`video-target-params` 统一整个属性读成
  `MPV_FORMAT_NODE` 再从 node map 里取字段，不用子属性路径 + 显式格式读取
  ——沿用 `enhanced-drag` 已经验证过的做法（子属性路径在实测环境下会静默
  返回 0，见该插件 README「已修复的问题」）。`mouse-pos` 不检查 `hover`
  字段，原因同 `enhanced-drag`：三个读取时机（down/拖拽中轮询/up）必然都
  有一个鼠标按键正按着，"hover 为 false 时忽略坐标"这条规则的前提本就不
  成立。
- 拖拽中鼠标位置的轮询通过 `mpv_wait_event` 的超时机制实现（拖拽中每 1/60
  秒轮询一次，沿用原 `shared/lua/plugin_config.lua` 里 `refresh_rate` 的
  值），拖拽结束后恢复无限等待，不占用空转 CPU。
- 框选叠加层用 `osd-overlay` 命令（`format=ass-events`）画一个闭合矩形
  路径，ASS 片段格式与原 Lua 版本一致；C 插件用 `mpv_command` 传纯字符串
  参数，`osd-overlay` 各参数按 mpv 内部定义的位置顺序解析（`id`/`format`/
  `data`/`res_x`/`res_y`/`z`/`hidden`/`compute_bounds`），不依赖具名参数。
- 依赖（Catch2、fmt）不进 `external/`，走顶层 `CMakeLists.txt` 的
  `FetchContent`，理由见 [AGENTS.md](../../AGENTS.md)。

## 验证

- 单元测试：`ctest --test-dir build -R "compute_zoom|classify_direction|normalize_box|compute_geometry"`，
  覆盖方向判定、几何换算、越界裁剪等纯逻辑场景。
- 端到端：用 `--input-ipc-server` 驱动一个无窗口 mpv 实例（`--vo=null
  --ao=null`）加载编译产物，绑定一个测试按键触发
  `script-binding drag_zoom_box/drag-zoom-box`，确认插件能正常加载、
  `key-binding` client message 被正确路由、过程中不产生任何警告/错误、
  mpv 进程不崩溃。真实的鼠标拖拽路径依赖 `mouse-pos`/`osd-dimensions` 在
  有真实显示窗口时才会返回有效值（`--vo=null` 下是哑值），与
  `enhanced-drag` 遇到的限制相同，无法在无 GUI 的自动化环境中完整验证，
  建议在真实 mpv 窗口下手动测试实际拖拽手感（对角线判定、中性灰提示色、
  恢复缩放）。

## 已知限制

延续自 `enhanced-drag`：`mouse-pos`、`osd-dimensions` 依赖真实的 mpv 显示
窗口，`--vo=null` 等无窗口环境下不可用/返回哑值，因此完整的鼠标拖拽路径
无法在无 GUI 的自动化环境中端到端验证。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：应用户要求，完整重写为 C++，并将拖拽方向判定从"只看水平
  方向"收紧为"只认左上→右下（放大）、右下→左上（恢复）两个对角线"，其余
  方向不再触发任何动作；几何计算改用 `osd-dimensions` margins，不再手动
  用 `video-zoom`/`video-pan-x`/`video-pan-y` 重新推算渲染矩形。Lua 实现
  移入 `lua-reference/`。
