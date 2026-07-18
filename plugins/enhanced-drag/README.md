# enhanced-drag

仿 PotPlayer 的手势 Pane 拖拽逻辑。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

纯 Lua 实现，尚未开始 C++ 重写。

## 相对上游的改动

仅将脚本从 `scripts/enhanced-drag.lua` 迁移到 `plugins/enhanced-drag/lua/`，
并把 `plugin_config.lua` 的引用路径从脚本同目录改为共享位置
`shared/lua/plugin_config.lua`（该配置文件被本次迁移的全部插件共用）。未改变
任何实际行为。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移。
