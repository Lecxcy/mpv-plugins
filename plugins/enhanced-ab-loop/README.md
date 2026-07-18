# enhanced-ab-loop

仿 PotPlayer 的 A/B Loop 表现并进行优化，支持设置多段 Loop 区间。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

纯 Lua 实现，尚未开始 C++ 重写。

## 相对上游的改动

仅将脚本从 `scripts/enhanced-ab-loop.lua` 迁移到
`plugins/enhanced-ab-loop/lua/`，并把 `plugin_config.lua` 的引用路径从脚本同
目录改为共享位置 `shared/lua/plugin_config.lua`（该配置文件被本次迁移的全部
插件共用）。未改变任何实际行为。

## 已知问题

延续自上游 mpv-scripts：

1. A/B Loop 只在实际文件长度范围内有效；`tail-frame-extension` 为缓解尾帧
   问题补的帧无法被选中。
2. 循环区间过短时会有显示问题。
3. 全屏播放时有概率遇到大量 output 丢帧，怀疑是 mpv 本身的问题，目前没有
   搜索到解决方法。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移。
