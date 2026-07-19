# mpv 插件工作区

本仓库用于开发 C++ mpv 插件、维护基于现有 Lua 插件的派生版本，以及将 Lua
插件逐步重构为 C++ 插件。

## 已实现的插件

来源、许可证与改动详情见各插件目录下的 README。

- [drag-zoom-box](plugins/drag-zoom-box/README.md)（C++）：仿 PotPlayer 的
  手势 Zoom 逻辑并进行优化，仅放大框选范围，只认左上到右下、右下到左上
  两个对角线方向。
- [enhanced-ab-loop](plugins/enhanced-ab-loop/README.md)（C++）：仿 PotPlayer
  的 A/B Loop 表现并进行优化，支持设置多段不相交的 Loop 区间，并入了
  尾帧冻结能力。
- [enhanced-drag](plugins/enhanced-drag/README.md)（C++）：仿 PotPlayer 的
  手势 Pane 拖拽逻辑。
- [enhanced-rotation](plugins/enhanced-rotation/README.md)（C++）：增强了
  mpv 的旋转功能，支持 360 度循环。
- [enhanced-seek](plugins/enhanced-seek/README.md)（Lua）：修改了 mpv 的快进/
  快退显示。
- [enhanced-volume](plugins/enhanced-volume/README.md)（Lua）：修改了 mpv 的
  音量调整逻辑及显示，支持长按连续变化。
- [tail-frame-extension](plugins/tail-frame-extension/README.md)（Lua）：缓
  解了 mpv 在 loop 跳转时跳过尾帧的问题；与 enhanced-ab-loop 同时使用时
  不需要加载本插件，能力已并入。

另有 [config/](config/README.md) 目录保存参考用的个人 `input.conf` /
`mpv.conf`。

## 获取仓库

克隆时同时初始化所有 submodule：

```sh
git clone --recurse-submodules <仓库地址>
```

如果已经克隆但缺少 submodule：

```sh
git submodule update --init --recursive
```

## 构建 C++ 插件

配置并构建整个项目：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

只构建指定插件：

```sh
cmake --build build --target <插件名>
```

构建产物位于：

```text
build/plugins/<插件名>/<插件名>.so
```

## 运行单元测试

C++ 插件使用 Catch2（`external/catch2`，固定版本 submodule）。构建完成后：

```sh
ctest --test-dir build --output-on-failure
```

也可以直接运行某个插件的测试可执行文件（支持 Catch2 原生的 `--list-tests`、
按标签 `"[enhanced-drag]"` 过滤等用法）：

```sh
build/plugins/<插件名>/tests/<插件名>-tests
```

## 本地测试

直接让 mpv 加载构建出的 C++ 插件：

```sh
mpv --script=build/plugins/<插件名>/<插件名>.so <媒体文件>
```

直接加载本项目维护的 Lua 插件：

```sh
mpv --script=plugins/<本地插件名>/lua/<脚本名>.lua <媒体文件>
```

## 收集为可直接使用的 mpv 配置目录

编译完成后，可以用 `scripts/collect-dist.sh` 把 `config/` 和已完成 C++ 重写
的插件构建物（`.so`）收集到 `dist/`（默认路径，可传参数覆盖）。**尚未 C++
重写的纯 Lua 插件不在收集范围内**，仍按上面"本地测试"里的方式单独加载：

```sh
scripts/collect-dist.sh            # 默认 build/ -> dist/
scripts/collect-dist.sh build dist # 等价的显式写法
```

`dist/` 会是一份 mpv 配置目录（`input.conf`、`mpv.conf`、已重写插件的
`.so`），可以直接指向它测试：

```sh
mpv --config-dir="$(pwd)/dist" <媒体文件>
```

或者把 `dist/` 下的内容复制进 `~/.config/mpv/`（注意会覆盖同名文件）。生成的
`mpv.conf` 里插件路径统一用 mpv 的 `~~home/` 元路径写成
`scripts-append=~~home/...`，指向"当前生效的配置目录"，所以整个 `dist/`
目录可以随意移动或复制，不依赖生成时的绝对路径。

## 开发约定

本项目的代码风格、目录组织与协作流程约定见 [AGENTS.md](AGENTS.md)，开始写
代码前请先阅读。
