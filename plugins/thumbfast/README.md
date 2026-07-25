# thumbfast

高性能的实时视频缩略图生成器。脚本本身不显示缩略图，只负责按需生成——要看到
预览图，得配合一个知道怎么调用它的 UI 脚本；本仓库里就是配合
[uosc](../uosc/README.md) 使用（uosc 对 thumbfast 是零配置自动集成，检测到
thumbfast 存在就会在进度条上显示预览）。

## 来源

- 上游仓库：https://github.com/po5/thumbfast
- 迁移基准 commit：`0f711de3138c9bd6718209d819ac54022c23ded2`
- 许可证：Mozilla Public License 2.0，见 [LICENSE](LICENSE)（原样复制自上游
  仓库根目录）

## 当前状态

纯 Lua 实现，单文件脚本，直接复制、未作任何改动：

- `lua/thumbfast.lua`：对应上游根目录的 `thumbfast.lua`。mpv 把 `scripts/`
  下的单个 `.lua` 文件按文件名（去掉扩展名）派生 client 名称，所以这个
  文件必须原名 `thumbfast.lua` 放到 `scripts/thumbfast.lua`——uosc 的
  `Timeline.lua` 里 `script-message-to thumbfast thumb ...` 硬编码的就是
  这个名字。
- `thumbfast.conf`：对应上游根目录的 `thumbfast.conf`，默认配置项及注释，
  安装后放到 `script-opts/thumbfast.conf`。

`scripts/collect-dist.sh` 已经把 thumbfast 加入 `lua_plugins` 收集列表。
因为它是单文件脚本（不是像 uosc 那样的目录式脚本，没有 `main.lua`），
收集脚本按有没有 `lua/main.lua` 区分两种拷贝方式：uosc 拷贝整个目录到
`dist/scripts/uosc/`，thumbfast 只拷贝这一个文件到 `dist/scripts/thumbfast.lua`。

## 同步历史

- 2026-07-25：从 `external/plugins/thumbfast`@`0f711de`
  完成初始复制，未做任何本地改动。
