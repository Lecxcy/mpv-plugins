# enhanced-ab-loop

仿 PotPlayer 的 A/B Loop 表现并进行优化，支持设置多段不相交的 Loop 区间；
C++ 重写后并入了原 `tail-frame-extension` 插件"循环到文件末尾时冻结尾帧"
的能力。完整设计讨论过程见 [SPEC.md](SPEC.md)。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

C++ 实现（mpv C plugin），源码见 `src/`、`include/`；旧 Lua 实现保留在
`lua-reference/` 仅供对照，不再维护、不参与构建。

## 相对上游（mpv-scripts）的改动

原 Lua 版本用 `mp.add_periodic_timer(0.05, check)` 每 50ms 轮询一次
`time-pos` 判断是否越过循环终点，完全没有使用 mpv 原生的
`ab-loop-a`/`ab-loop-b` 属性；`sort_segments` 的比较函数用 epsilon 容差做
tie-break，不满足严格弱序；`eof-reached` 回调硬编码跳到列表里最靠前的一
段；循环终点无法设到 `tail-frame-extension` 补出来的尾部延长段里。

C++ 版本是一次完整重写，不是逐行移植，主要改动（详细讨论过程见
[SPEC.md](SPEC.md)）：

- **原生驱动跳转，取代定时器轮询**：动态写 mpv 原生 `ab-loop-a`/
  `ab-loop-b` 属性，让 mpv 内核用帧级 pts 比较触发跳转（`handle_loop_file`
  在到达 EOF 判定时读取*此刻*的 `ab-loop-a` 实时值作为 seek 目标，
  `MPSEEK_EXACT` 精确 seek）。每次落到某个区间起点后，立刻把
  `ab-loop-a`/`ab-loop-b` 预置为"下一段起点/本段终点"，跳转发生时直接精确
  seek 到下一段，不会闪现一帧再补跳。
- **尾帧冻结并入本插件**：`ab-loop-b` 的 pts 截断判断发生在 vf/af 滤镜链
  之后，`tpad`/`apad` 补出来的克隆帧同样有真实递增的 pts；把终点语义是
  "循环到文件末尾"的区间，实际写入的 `ab-loop-b` 换成"真实末尾 pts + 冻结
  时长"，让原生机制播入（不是 seek 进）延长段后自然触发跳转，不需要暂停
  状态或自定义计时器。文件加载时无条件对视频/音频轨施加
  `tpad=stop_mode=clone`/`apad=pad_dur`（沿用 `tail-frame-extension.lua`
  的做法）。
- **修复两个已确认的 bug**：`sort_segments` 改成精确比较（不再有 epsilon
  tie-break 破坏传递性的问题）；`eof-reached` 兜底改成跳到 active 队列里
  *最后*一项自己的起点（不是硬编码第一项）。
- **插入 / Move / extend / nudge 四类操作边界清晰化**：`set-a`/`set-b` 在
  空隙里操作永远只表示插入独立新区间，冲突直接拒绝，不做隐式的边界借用/
  吞并；光标在已有区间内部操作是 Move（对齐 PotPlayer）；新增
  `extend-prev`/`extend-next` 两个独立键位专门处理"拉伸相邻区间边界"，不
  靠同一个手势事后反推意图；nudge 触发条件从"严格在区间内部"放宽到"落在
  闭区间 `[a,b]` 内"（循环刚跳回边界附近就是最想 nudge 的时机），顶到边界
  统一拒绝、不再静默 clamp。
- **撤销端点不再嫁接到邻居**：原实现撤销某个区间的一个端点后，会把剩下的
  待定端点错误地覆盖到相邻完整区间的显示上；现在待定端点独立展示，不触碰
  任何其他区间。
- **选段循环**：每个区间加 `enabled` 字段，`toggle-segment` 翻转光标所在
  区间是否参与循环，只按位置顺序循环、跳过禁用的区间。
- **存档/读档**：`save-loops`/`load-loops`，用内容采样哈希（文件大小 +
  头尾各 64KiB，不读全文件）定位存档文件，文件内部用**路径的哈希**（不是
  明文绝对路径）做精确 key 区分内容相同但路径不同的重复文件——路径可能
  暴露目录结构、用户名、媒体库组织方式，存档写到磁盘上不应该带这些信息，
  绝对路径只留在内存里用来实际读文件、算哈希，不会被持久化；路径哈希没
  精确命中但只有唯一候选（大概率是同一文件改名了）时会弹出 OSD 提示（不
  展示旧路径，插件这边也只有它的哈希），通过 `confirm-yes`/`confirm-no`
  两个键确认（mpv 没有原生弹窗，这是 mpv 脚本层面实现"确认对话框"的标准
  做法）。

## 实现说明

- 分层：`include/enhanced_ab_loop/logic.h` + `src/logic.cpp` 是不依赖 mpv
  的纯状态机（segment/pending 增删改、active 队列、预置下一跳、eof 兜底
  目标），`include/enhanced_ab_loop/store.h` + `src/store.cpp` 是纯存档层
  （内容哈希、JSON 序列化、消歧规则），`src/plugin.cpp` 是唯一接触 mpv
  client API 的地方。两个纯逻辑层都有完整 Catch2 单元测试
  （`tests/test_logic.cpp`、`tests/test_store.cpp`），覆盖了 SPEC.md 里
  提到的每个 bug 的回归用例。
- 按键绑定：C 插件没有 Lua/JS `mp.add_key_binding` 那样的封装，参照
  `enhanced-drag` 的写法，直接接住 mpv 转发过来的 `key-binding` client
  message（`script-binding` 命令内部就是这样转发的，不需要提前注册）。
  `input.conf` 中已有的绑定（`enhanced_ab_loop/set-a` 等）无需修改即可继续
  工作，原因同 `enhanced-drag`/`enhanced-rotation`：mpv 按插件文件名派生
  client 名称，`enhanced-ab-loop.so` 与原 `enhanced-ab-loop.lua` 派生出的
  名字相同（`enhanced_ab_loop`）。新增的 `extend-prev`/`extend-next`/
  `toggle-segment`/`save-loops`/`load-loops`/`confirm-yes`/`confirm-no`
  已加进 `config/input.conf`。
- JSON 序列化用 [nlohmann/json](https://github.com/nlohmann/json)（单
  头文件、MIT 协议），存档目录路径展开用 `~~home/loop-segments`（不用
  `~~/`——后者语义是"子路径已存在时返回已存在的那个目录"，是给读取场景
  设计的，写入场景要明确指向配置目录本身）。
- 依赖（Catch2、fmt、nlohmann/json）都不进 `external/`，走顶层
  `CMakeLists.txt` 的 `FetchContent`，理由见 [AGENTS.md](../../AGENTS.md)。

## 验证

- 单元测试：`ctest --test-dir build -R enhanced-ab-loop`，覆盖排序传递性、
  重叠检测、四类调整操作的边界拒绝、pending 独立展示（不嫁接邻居）、
  active 队列的近端边界、预置下一跳与尾帧冻结加时、eof 兜底目标（对比旧
  bug）、内容哈希稳定性、路径哈希的确定性与不可逆展示、采样窗口比文件本身
  还大时的边界算术、存档序列化往返、改名迁移不产生孤儿 entry 等场景。
- 端到端：用 `--input-ipc-server` 驱动一个无窗口 mpv 实例（`--vo=null
  --ao=null`）加载编译产物、模拟 `script-binding` 的 `key-binding` client
  message 验证过：文件加载时正确施加 `tpad` 滤镜；插入/nudge/extend 操作
  正确反映到 `ab-loop-a`/`ab-loop-b`；**真正解除暂停后，播放位置确实在
  设定的区间内自动循环**（而不仅是属性被设置对）；尾帧冻结区间的
  `ab-loop-b` 精确等于"真实 duration + 冻结时长"，播放位置真的越过文件
  名义时长播进延长段再跳回去，且全程 `pause` 属性保持 `false`（尾帧冻结
  "不能显示为暂停"这条硬性要求）；`toggle-enabled` 正确清空/恢复
  `ab-loop-a`/`ab-loop-b`；存档文件按预期的两级 key 结构写入并能重新
  加载；用一个路径含用户名和敏感目录名的真实文件跑过存档，确认写到磁盘
  上的 JSON 里既不含明文路径、也不含目录名/用户名/文件名，只有哈希；用
  一个远小于 64KiB 采样窗口的真实小文件（头尾采样完全重叠）验证过内容
  哈希计算和存档读写都不会出错。

## 已知限制

延续自 SPEC.md §7：本次重写只保证 `ab-loop-a`/`ab-loop-b` 是 mpv 原生
属性，uosc 不需要改动就能画出*当前激活*区间的括号标记；要在进度条上同时
显示全部多段区间，需要另开一个派生插件 fork `external/plugins/uosc`，不
在本次范围内。

尾帧冻结段没法直接 seek 到延长段内部的某个具体时间点（拖进度条/章节跳转/
`seek` 命令都是 demuxer 层面的操作，延长段在 demuxer 层面不存在），只影响
"主动想跳进延长段内部某处"这种本来就不常见的操作；`duration` 属性和依赖它
的 UI（未来的 uosc 进度条）不会把冻结时长算进去，视觉上会先到 100%、画面
还继续播一小段才真正循环，这是显示层面的落差，不影响功能生效。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：完整重写为 C++，并入 `tail-frame-extension` 的尾帧冻结能力，
  设计讨论过程见 [SPEC.md](SPEC.md)，主要改动见上文。
