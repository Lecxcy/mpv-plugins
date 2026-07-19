# enhanced-seek

修改了 mpv 的快进/快退显示，并新增长按方向键连续快进/快退。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游（mpv-scripts）的改动

原 Lua 版本只做了一件事：把 mpv 默认的快进/快退 OSD 替换成
`position/duration (percent%)` 的自定义显示，每次按键固定 seek
`config.step`（2 秒），不区分单击/长按。

C++ 重写在保留这个显示行为的基础上，新增了用户明确要求的能力：

- **长按方向键连续快进/快退**：短按（松开早于 `hold_delay`）行为不变，只
  触发一次 `step` 秒的 seek；持续按住超过 `hold_delay` 后进入连续模式，之后
  固定每 `repeat_interval` 秒再触发一次 `step` 秒的 seek，直到松开为止。
- **不做加速**：`enhanced-volume` 的长按实现（`lua/enhanced-volume.lua`）会
  随按住时长切换到更快的 `hold_profile` 挡位；这里用户明确要求"不需要像
  volume 一样做加速，保持相同的速度即可"，所以连续模式全程只有一档固定
  `repeat_interval`，不做任何 profile 切换。

## 实现说明

- 分层：`include/enhanced_seek/logic.h` + `src/logic.cpp` 是不依赖 mpv 的
  纯逻辑——`format_time`/`format_status` 两个纯文本格式化函数，以及
  `HoldState` 长按节奏状态机（只记录方向和下一次触发时间点，不知道 seek
  步长、也不碰任何 mpv 状态）；`src/plugin.cpp` 是唯一接触 mpv client API
  的地方，负责发 `seek` 命令、读 `time-pos`/`duration`/`percent-pos`、显示
  OSD。两者都有 Catch2 单元测试（`tests/test_enhanced_seek.cpp`），覆盖了
  短按不触发连续模式、恒定节奏不加速、轮询被推迟时的补触发、以及只有方向
  匹配才会真正停止等场景。
- 按键绑定：C 插件没有 Lua `mp.add_key_binding` 那样的封装，参照
  `enhanced-drag`/`enhanced-ab-loop` 的写法，直接接住 mpv 转发过来的
  `key-binding` client message，只处理 `d`（按下）/`u`（松开）两个 phase。
  `config/input.conf` 里已有的绑定（`RIGHT`/`LEFT` -> `script-binding
  enhanced_seek/seek-forward`/`seek-backward`）不需要修改：这两个绑定没有
  `repeatable` 前缀，不会收到 OS 按键自动重复产生的 `r`（repeat）事件，长按
  时的连续 seek 完全由本插件自己的 `HoldState` 计时驱动，不依赖 mpv 全局的
  按键自动重复速率（`--input-ar-delay`/`--input-ar-rate` 是全局设置，没法
  只给这一个绑定单独调速度，也会影响其他所有按键）。
- 事件循环参照 `enhanced-drag`：`mpv_wait_event` 的超时参数在长按连续模式
  下设为 `repeat_interval`，空闲时设为 `-1`（阻塞等待），靠超时返回的
  `MPV_EVENT_NONE` 驱动轮询，不需要单独的定时器 API。
- `HoldState::poll` 用"下一次触发时间点"（而不是"上次触发时间点 + 经过时长"）
  推进节奏，避免多次轮询之间的微小误差累积漂移；如果某次轮询被推迟了
  （比如宿主进程被挂起后恢复），会一次性补发多个 tick 对应的 seek 距离，
  而不是丢失这段时间本该发生的连续 seek，但补发次数有上限（1000），避免
  时钟跳变导致算出天文数字般的 tick 数。
- `mpv_get_property(handle, "percent-pos", ...)` 读取失败（没有 duration
  信息）时按 `0%` 处理，与原 Lua 版本 `percent_text = "0%"` 的兜底行为一致。

`input.conf` 中已有的绑定无需修改即可继续工作，原因同其他已重写插件：mpv
按插件文件名派生 client 名称，`enhanced-seek.so` 与原 `enhanced-seek.lua`
派生出的名字相同（`enhanced_seek`）。

## 验证

- 单元测试：`ctest --test-dir build -R enhanced-seek`，覆盖 `format_time`/
  `format_status` 的格式化边界、`HoldState` 的短按不触发连续模式、恒定节奏
  （不加速）、轮询延迟时的补触发、以及"只有方向匹配才停止"（同时按住两个
  方向键时，松开其中一个不应该打断另一个仍在连续触发的方向）。
- 端到端：用 `--input-ipc-server` 驱动一个无窗口 mpv 实例（`--vo=null
  --ao=null`）加载编译产物、通过 `script-message-to enhanced_seek
  key-binding ...` 模拟真实的 `key-binding` client message 验证过：短按
  （120ms，早于 `hold_delay`）只触发一次 `step` 秒的 seek；长按
  （650ms，跨过 `hold_delay` 并触发多次 `repeat_interval`）期间持续触发
  连续 seek，松开后立刻停止、position 不再变化。验证过程中发现测试素材本身
  的坑：ffmpeg 默认关键帧间隔（约 10 秒一个）会让非 `exact` 的相对 seek
  吸附到最近关键帧，一度看起来像是"长按触发了异常大幅度的 seek"，实际是
  测试视频关键帧过稀疏导致——和原 Lua 版本使用的 `seek ... relative`（同样
  不带 `exact`）行为一致，不是本次重写引入的问题；改用密集关键帧
  （`-g 5`）重新编码测试素材后行为符合预期。

## 已知限制

延续自原 Lua 版本：`seek` 命令使用默认的 `relative`（非 `exact`）精度，会
吸附到最近的可跳转关键帧，而不是精确到秒；这对长按连续快进/快退是刻意保留
的行为（和单击 seek 一致，不单独改成 `exact`），关键帧稀疏的文件上连续
快进的观感颗粒度会更粗。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：完整重写为 C++，新增长按方向键连续快进/快退（恒定速度、不
  加速），Lua 实现移入 `lua-reference/`。
