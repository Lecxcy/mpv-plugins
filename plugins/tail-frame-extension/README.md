# tail-frame-extension

缓解了 mpv 在 loop 跳转时跳过尾帧的问题。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护

## 当前状态

纯 Lua 实现，尚未开始 C++ 重写。

## 相对上游的改动

仅将脚本从 `scripts/tail-frame-extension.lua` 迁移到
`plugins/tail-frame-extension/lua/`，并把 `plugin_config.lua` 的引用路径从
脚本同目录改为共享位置 `shared/lua/plugin_config.lua`（该配置文件被本次迁移
的全部插件共用）。未改变任何实际行为。

## 与 enhanced-ab-loop 的关系

[enhanced-ab-loop](../enhanced-ab-loop/README.md) 重写为 C++ 后已经并入了
本插件"循环到文件末尾时冻结尾帧"的能力（不是简单叠加两个插件——原因见
enhanced-ab-loop 的 [SPEC.md](../enhanced-ab-loop/SPEC.md) §4，两者原来
无法直接配合生效）。**同时使用 enhanced-ab-loop 时不需要再加载本插件**，
它的尾帧冻结逻辑会接管这部分能力。

本插件继续作为独立、轻量的实现保留，适用于完全不用 enhanced-ab-loop、只
用 mpv 原生 `loop-file` 循环整个文件的场景。

## 已知问题

延续自上游 mpv-scripts：单独使用本插件（不配合 enhanced-ab-loop）时，为
缓解尾帧问题所补的帧无法被原生 `ab-loop-a`/`ab-loop-b` 选中，因为 A/B
Loop 只在实际文件长度范围内有效。

## 同步历史

- 2026-07-18：从 mpv-scripts@`96f11dd` 完成初始迁移。
- 2026-07-19：`enhanced-ab-loop` 完整重写为 C++ 并并入本插件的尾帧冻结
  能力，补充与它的关系说明。本插件自身实现未改动。
