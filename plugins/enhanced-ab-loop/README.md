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
  待定端点错误地覆盖到相邻完整区间的显示上；现在待定端点的 A/B 单独标在
  `show-state` 首行（`A:.. B:..`），不再拼成一行伪区间混进完整区间列表，
  不触碰任何其他区间。
- **选段循环**：每个区间加 `enabled` 字段，`toggle-segment` 翻转光标所在
  区间是否参与循环，只按位置顺序循环、跳过禁用的区间。`solo-segment`
  （`T`）一步到位：只启用光标所在区间，其余全部禁用，不用对着其他区间
  一个个按 `toggle-segment`；再在同一个区间上按一次 `T` 会原样恢复成 solo
  之前各区间的 enabled 状态（不是简单地"全部重新启用"），换到另一个区间上
  按则丢弃旧的撤销记录、重新记一份新的。
- **模板**（详细设计见 [SPEC.md §10](SPEC.md#10-模板templates)）：9 个编号
  槽位（`!@#$%^&*(` 依次对应 1-9 应用 / `Ctrl+1..9` 保存 / `Alt+1..9`
  删除），每个槽位是一份独立的 `(segments, pending)` 完整快照，不是对当前
  segments 的引用或一份启用掩码——最简单的用法是同一批区间、不同槽位只是
  `enabled` 不同，也支持每个槽位存完全不同的区间。保存/删除只改内存，仍然
  要 `Ctrl+S` 才连同 segments/pending 一起落盘。应用键位经过两轮真机测试
  才定下来：先是 `meta+1..9` 撞上了 mpv 自己 macOS 原生窗口菜单的"实际
  大小"/"双倍大小"（`Cmd+1`/`Cmd+2`，比 `input.conf` 更底层，且 mpv 是命令
  行启动、不是独立 `.app`，没法用系统"App 快捷键"让路）；改成 `Shift+1..9`
  又踩到 mpv 对可打印字符不支持 `Shift+` 前缀写法这条规则（要写 Shift 按下
  后实际产生的字符本身，跟这个仓库里 `unset-a`/`unset-b` 一直用字面
  `{`/`}` 而不是 `Shift+[`/`Shift+]` 是同一条规则），最终落地成
  US 键盘布局下的 `!@#$%^&*(`（数字 3 对应的 `#` 因为会被 mpv 当整行注释，
  改用按键别名 `SHARP`）。完整排查过程见
  [SPEC.md §10.5](SPEC.md#105-键位三组单修饰键不用双修饰键组合)。
- **快速跳段**：循环开着时按 `Tab`（`skip-next`），不管当前播到段内哪个
  位置，立刻跳到下一个启用的段——跟"当前段自然播完一轮"共用同一处跳转
  实现（`advance_to_next`），只是触发条件从"原生自环命中终点"换成"用户
  主动要求"。
- **pending（未闭合端点）现在会被持久化**：早期设计认为"待定状态不用存"，
  改成支持——用户确实会在只设了一个端点时就想保存，不应该被存档层的限制
  丢掉。老格式的存档（没有 `pending`/`templates` 字段）读取时按空处理，
  自动兼容。
- **存档/读档**：`save-loops`/`load-loops`，用内容采样哈希（文件大小 +
  头尾各 64KiB，不读全文件）定位存档文件，文件内部用**文件名的哈希**（不是
  明文文件名，也不是完整绝对路径）做精确 key 区分内容相同但文件名不同的
  重复文件——完整路径可能暴露目录结构、用户名、媒体库组织方式，且移动
  硬盘/NAS 换挂载点会导致绝对路径整体失效，所以只取文件名参与哈希（换挂
  载点、甚至把目录树整体搬到 NAS 上都不受影响，只要文件名不变）；绝对
  路径只留在内存里用来实际读文件、算哈希，不会被持久化；文件名哈希没
  精确命中但只有唯一候选（大概率是同一文件真的改名了）时会弹出 OSD 提示（不
  展示旧路径，插件这边也只有它的哈希），通过 `confirm-yes`/`confirm-no`
  两个键确认（mpv 没有原生弹窗，这是 mpv 脚本层面实现"确认对话框"的标准
  做法）；提示期间会强制暂停播放，并临时启用一个独占 input section 挡掉
  除 `confirm-yes`/`confirm-no` 实际绑定的物理键之外的所有按键（反查
  `input-bindings` 得到具体是哪个键，不硬编码，找不到就安全退化成不锁
  键），逼用户先回答完再继续操作；回答完成后把 pause 还原成确认前的状态。

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
  `toggle-segment`/`solo-segment`/`save-loops`/`load-loops`/`confirm-yes`/
  `confirm-no` 已加进 `config/input.conf`。
- JSON 序列化用 [nlohmann/json](https://github.com/nlohmann/json)（单
  头文件、MIT 协议），存档目录路径展开用 `~~home/loop-segments`（不用
  `~~/`——后者语义是"子路径已存在时返回已存在的那个目录"，是给读取场景
  设计的，写入场景要明确指向配置目录本身）。
- 依赖（Catch2、fmt、nlohmann/json）都不进 `external/`，走顶层
  `CMakeLists.txt` 的 `FetchContent`，理由见 [AGENTS.md](../../AGENTS.md)。
- **与 [uosc](../uosc/README.md) 进度条集成**：`refresh_loop`（唯一的
  segments/pending/loop_enabled 变更汇合点）每次都会把完整的 `segments`
  列表序列化成 JSON，写进 `user-data/enhanced-ab-loop/segments` 这个 mpv
  `user-data` 属性（专门给"脚本/插件之间共享任意数据"设计的机制，可读可写
  可 observe，序列化直接复用存档层已有单测覆盖的 `store::serialize_segments`，
  不重新发明格式）。`plugins/uosc/lua/main.lua` observe 这个属性，
  `elements/Timeline.lua` 在进度条上把每一段区间的范围和 `enabled` 状态画
  出来（enabled 用绿色，disabled 用暗淡描边），并通过播放位置是否落在某段
  自己的 `[a,b)` 内识别出"当前激活"的那一段、加亮它的边框——特意不依赖
  `ab-loop-a`/`ab-loop-b`/`loop_enabled`，这样主循环开关关掉之后区间依然
  会显示，只是不再被 mpv 实际强制循环。这条集成只对本仓库的 `plugins/uosc`
  派生版生效，不影响 `external/plugins/uosc` 这份上游参考。

## 验证

- 单元测试：`ctest --test-dir build -R enhanced-ab-loop`，覆盖排序传递性、
  重叠检测、四类调整操作的边界拒绝、pending 独立展示（不嫁接邻居）、
  active 队列的近端边界、预置下一跳与尾帧冻结加时、eof 兜底目标（对比旧
  bug）、内容哈希稳定性、文件名提取与哈希的确定性/不可逆展示、采样窗口比
  文件本身还大时的边界算术、存档序列化往返、改名迁移不产生孤儿 entry 等
  场景。
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
- uosc 集成：用同一套 IPC 驱动方式，插入两个区间（`[2,6]` 与 `[10,14]`）
  后 `get_property user-data/enhanced-ab-loop/segments` 精确返回两段完整
  JSON；对第二段执行 `toggle-segment` 后属性实时更新为
  `enabled:false`；重新打开循环后光标落回第一段，`ab-loop-a`/`ab-loop-b`
  精确等于 `2.0`/`6.0`，与该段自己的 `a`/`b` 一致（Timeline 据此判定"当前
  激活段"的匹配逻辑成立）；全程 mpv 日志无 Lua 报错。

## 已知限制

尾帧冻结段没法直接 seek 到延长段内部的某个具体时间点（拖进度条/章节跳转/
`seek` 命令都是 demuxer 层面的操作，延长段在 demuxer 层面不存在），只影响
"主动想跳进延长段内部某处"这种本来就不常见的操作；`duration` 属性和依赖它
的 UI（未来的 uosc 进度条）不会把冻结时长算进去，视觉上会先到 100%、画面
还继续播一小段才真正循环，这是显示层面的落差，不影响功能生效。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移（纯 Lua）。
- 2026-07-19：完整重写为 C++，并入 `tail-frame-extension` 的尾帧冻结能力，
  设计讨论过程见 [SPEC.md](SPEC.md)，主要改动见上文。
- 2026-07-22：新增模板（9 个编号槽位，完整快照式而非共享池+掩码）、快速
  跳段（`Tab`）、`pending` 持久化，设计讨论过程见 [SPEC.md §10-§11](SPEC.md)。
- 2026-07-25：`refresh_loop` 新增把 segments 发布到
  `user-data/enhanced-ab-loop/segments`，配合本仓库派生的
  [plugins/uosc](../uosc/README.md) 在进度条上展示全部区间及其
  enabled/激活状态，此前 SPEC.md §7 记录的这条已知限制解除。
