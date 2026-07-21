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
- **seek 改用 `exact`**：原 Lua 版本用的是不带 `exact` 的 `relative` seek，
  会吸附到最近的可跳转关键帧；在关键帧稀疏的文件上，单次点按"+2秒"实际可能
  跳出去好几秒（见下面"已知限制"）。真实按键测试中复现过这个问题后改成了
  `exact`，保证每次 seek 都精确到秒。

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
  `key-binding` client message。`config/input.conf` 里已有的绑定
  （`RIGHT`/`LEFT` -> `script-binding enhanced_seek/seek-forward`/
  `seek-backward`）不需要修改。**注意**：`script-binding` 这个 mpv 内置
  命令本身默认 `allow_auto_repeat = true`（`external/mpv/player/command.c`），
  按住方向键不放时，mpv 会按全局的 `--input-ar-delay`/`--input-ar-rate`
  （默认 200ms 后开始，之后每 ~25ms 一次）持续推 phase=`r` 的 `key-binding`
  message 过来——本插件的连续 seek **不使用** `r` 事件本身携带的信息去决定
  seek 多少（那仍然完全由 `HoldState` 按真实经过时间计算），但下面事件循环
  会把它收到的每一次事件（包括这些被忽略掉的 `r`）都当作"该检查一下是否该
  tick 了"的信号来用，原因见下一条。
- 事件循环参照 `enhanced-drag`：`mpv_wait_event` 的超时参数在长按连续模式
  下设为 `repeat_interval`，空闲时设为 `-1`（阻塞等待）。**这里踩过一个坑**：
  最初的实现只在超时返回 `MPV_EVENT_NONE` 时才调用 `hold.poll()`，真实按键
  测试中发现长按完全不触发连续 seek——因为上一条提到的 `r` 事件大约每 25ms
  一条，比 `repeat_interval`（100ms）密得多，`mpv_wait_event` 每次都被 `r`
  提前唤醒、永远等不到超时，`MPV_EVENT_NONE` 分支几乎从来没被走到过，
  `hold.poll()` 被活活饿死。修复方式是把 `hold.poll()` 的检查挪到"处理完
  任意事件之后都做一次"，不再依赖等到 `MPV_EVENT_NONE`——`poll()` 本身按
  真实经过时间判断该不该 tick，被更频繁地调用完全没问题（多余调用只会返回
  0 个 tick），而 mpv 自己密集的自动重复通知反而变成了一个免费、比我们自己
  设的 100ms 更密的心跳。
- `HoldState::poll` 用"下一次触发时间点"（而不是"上次触发时间点 + 经过时长"）
  推进节奏，避免多次轮询之间的微小误差累积漂移；如果某次轮询被推迟了
  （比如宿主进程被挂起后恢复），会一次性补发多个 tick 对应的 seek 距离，
  而不是丢失这段时间本该发生的连续 seek，但补发次数有上限（1000），避免
  时钟跳变导致算出天文数字般的 tick 数。
- `mpv_get_property(handle, "percent-pos", ...)` 读取失败（没有 duration
  信息）时按 `0%` 处理，与原 Lua 版本 `percent_text = "0%"` 的兜底行为一致。
- `key-binding` 的 `d`（按下）和 `p`（up/down 分不清时的单次通知，见
  `input.rst`）分开处理：`d` 会调用 `hold.begin()` 进入长按等待，因为一定
  会有对应的 `u` 来 `stop()`；`p` 只做一次性 tap-seek、**不**进入连续模式——
  `p` 出现的场景本来就是"没法保证一定会有匹配的 u"（比如 up/down 不可追踪的
  遥控器/手柄一类输入源），如果照 `d` 一样进入连续模式，遇到这种输入源会导致
  连续 seek 永远停不下来。
- `seek_by()` 里的 `seek` 命令带 `exact` 参数：非 exact 只吸附最近关键帧，
  关键帧稀疏时单次点按会跳出去远超 `step` 秒的距离（真实按键测试中复现过，
  见下面"已知限制"）。`exact` 需要从关键帧解码到目标位置，长按连续触发
  （每 `repeat_interval` 一次）时如果单次解码耗时超过 `repeat_interval`，
  理论上可能感觉卡顿；目前没有实测到明显卡顿，如果之后在关键帧极稀疏/解码
  很慢的文件上出现长按卡顿，可以考虑只在单次点按时用 `exact`、连续 ticks
  退回非 exact。

`input.conf` 中已有的绑定无需修改即可继续工作，原因同其他已重写插件：mpv
按插件文件名派生 client 名称，`enhanced-seek.so` 与原 `enhanced-seek.lua`
派生出的名字相同（`enhanced_seek`）。

## 验证

- 单元测试：`ctest --test-dir build -R enhanced-seek`，覆盖 `format_time`/
  `format_status` 的格式化边界、`HoldState` 的短按不触发连续模式、恒定节奏
  （不加速）、轮询延迟时的补触发、以及"只有方向匹配才停止"（同时按住两个
  方向键时，松开其中一个不应该打断另一个仍在连续触发的方向）。
- 端到端（合成 IPC 消息）：用 `--input-ipc-server` 驱动一个无窗口 mpv 实例
  （`--vo=null --ao=null`）加载编译产物、通过 `script-message-to
  enhanced_seek key-binding ...` 手动拼 `key-binding` client message 验证：
  短按（120ms，早于 `hold_delay`）只触发一次 `step` 秒的 seek；长按
  （650ms，跨过 `hold_delay` 并触发多次 `repeat_interval`）期间持续触发
  连续 seek，松开后立刻停止、position 不再变化。这个方式验证过程中也发现过
  测试素材本身的坑：ffmpeg 默认关键帧间隔（约 10 秒一个）会让非 `exact` 的
  相对 seek 吸附到最近关键帧，一度看起来像"长按触发了异常大幅度的 seek"，
  改用密集关键帧（`-g 5`）重新编码测试素材后现象消失——但这个方式是手动拼
  `d`/`u` 消息，不经过 mpv 真实的按键输入/自动重复管线，没能覆盖到下面这个
  真实场景。
- 真实按键（`--input-conf` + 物理按键 + `MPV_UTIL_DEBUG` 埋点）：用
  `-DCMAKE_BUILD_TYPE=Debug` 单独编一份带调试输出的插件，加载
  `config/input.conf` 后用真实键盘操作，把 `on_key_down`/`on_key_up`/`poll`
  的时间戳、按住时长、seek 前后 position 一起打到 stderr 里核对。这次测试中
  发现：单次点按（`held_for` 只有 60~80ms，远低于 `hold_delay`，日志里全程
  没有一行 `poll:`，证明连续 seek 逻辑完全没被触发）也会跳出去远超 2 秒——
  根因和上面 IPC 测试踩过的坑一样，都是非 `exact` 相对 seek 吸附到关键帧，
  只是这次是在真实素材、真实点按下暴露的，说明连续 seek 的节奏逻辑本身没
  问题，问题出在 `seek` 命令本身的精度上；改成 `exact` 后已解决。
  另外这次也顺带查了 mpv 源码：`script-binding` 命令本身
  `.allow_auto_repeat = true`（`player/command.c`），按住不放时 mpv 确实会
  按 `--input-ar-delay`/`--input-ar-rate`（默认 200ms 后每 25ms 一次）持续
  推 phase=`r` 的 `key-binding` message。改完 `exact` 之后再测一轮真实长按，
  日志显示连续 seek 完全没触发：整个按住期间只在刚进入连续模式那一刻打过一次
  `poll:`，之后哪怕按住几秒钟也再没出现过，同时能看到一长串每 20~30ms 一条
  的 `phase=r` 消息——这就实锤了事件循环里的第二个问题：`hold.poll()`
  只在 `mpv_wait_event` 超时返回 `MPV_EVENT_NONE` 时才会被调用，而这些 `r`
  消息比我们自己 100ms 的 `repeat_interval` 密得多，会一直抢在超时之前把
  `mpv_wait_event` 唤醒，导致 `MPV_EVENT_NONE` 分支几乎永远走不到。修复见
  上面"实现说明"的事件循环部分（改成每次醒来都检查一次是否该 tick）。

## 已知限制

无。原 Lua 版本非 `exact` 相对 seek 会吸附关键帧、单次跳跃距离可能远超
`step` 的问题，以及本次重写早期版本里"真实长按时连续 seek 被 mpv 自动重复
消息饿死、完全不触发"的问题，均已在下方"同步历史"记录的日期修复。`exact`
需要从关键帧解码到目标位置，理论上关键帧极稀疏/解码很慢的文件上长按连续
seek 可能有卡顿，目前没有实测到，先记在"实现说明"里备查。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：完整重写为 C++，新增长按方向键连续快进/快退（恒定速度、不
  加速），Lua 实现移入 `lua-reference/`。
- 2026-07-21：真实按键测试中复现两个问题并修复：(1) 单次点按跳出去远超
  `step` 秒——非 `exact` 相对 seek 吸附关键帧导致，`seek` 命令改用 `exact`；
  (2) 改完 (1) 后长按完全不触发连续 seek——mpv `script-binding` 命令默认
  自动重复、按住时持续推送的 `r` 事件把 `hold.poll()` 依赖的
  `MPV_EVENT_NONE` 超时分支饿死了，改成每次事件循环醒来都检查一次是否该
  tick 修复。复查代码时顺带修了一个还没实际遇到过、但确认存在的隐患：
  `p`（up/down 分不清的单次通知）之前和 `d` 一样会进入长按等待，遇到不发
  `u` 的输入源会导致连续 seek 停不下来，改成 `p` 只做一次性 tap-seek、不进
  连续模式。顺带补上 `scripts/collect-dist.sh` 里遗漏的 `enhanced-seek`
  收集项，`.gitignore` 补上 `/build-debug/`。
