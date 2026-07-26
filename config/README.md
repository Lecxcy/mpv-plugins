# config

个人按键绑定和播放配置参考，非插件代码。

- `input.conf`：修改键位绑定，绝大多数与 PotPlayer 对齐。
- `mpv.conf`：默认循环播放，并修改音量上限。

`input.conf` 开头有一段**修饰键约定**，改键位前先读完那一段：无修饰=播放器
通用 + ab-loop 高频操作，`Ctrl+`=微调或写入，`Shift+`=反向/取消（要写 Shift
按下后实际产生的字符，不写 `Shift+X`），`Alt+`=split-zoom-box 专用，`meta+`
键盘上不用（只保留鼠标组合）。两个插件的"段"操作是同构的：动作键相同，用
`Alt` 区分作用域（`[` vs `Alt+[`、`\` vs `Alt+\`、`Ctrl+s` vs `Alt+s`）。

## 来源

- 上游仓库：mpv-scripts（`git@github.com:Lecxcy/mpv-scripts.git`）
- 迁移基准 commit：`96f11ddb66b8eac03380a14ed5a2aa68bd8c0219`
- 许可证：mpv-scripts 未附带 LICENSE 文件，无显式许可证声明；该仓库与本仓库
  同属一人维护
