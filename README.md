# mpv 插件工作区

本仓库用于开发 C++ mpv 插件、维护基于现有 Lua 插件的派生版本，以及将 Lua
插件逐步重构为 C++ 插件。

## 已实现的插件

目前均为纯 Lua 实现，尚未开始 C++ 重构；来源、许可证与改动详情见各插件目录
下的 README。

- [drag-zoom-box](plugins/drag-zoom-box/README.md)：仿 PotPlayer 的手势 Zoom
  逻辑并进行优化，仅放大框选范围。
- [enhanced-ab-loop](plugins/enhanced-ab-loop/README.md)：仿 PotPlayer 的
  A/B Loop 表现并进行优化，支持设置多段 Loop 区间。
- [enhanced-drag](plugins/enhanced-drag/README.md)：仿 PotPlayer 的手势 Pane
  拖拽逻辑。
- [enhanced-rotation](plugins/enhanced-rotation/README.md)：增强了 mpv 的旋
  转功能，支持 360 度循环。
- [enhanced-seek](plugins/enhanced-seek/README.md)：修改了 mpv 的快进/快退
  显示。
- [enhanced-volume](plugins/enhanced-volume/README.md)：修改了 mpv 的音量调
  整逻辑及显示，支持长按连续变化。
- [tail-frame-extension](plugins/tail-frame-extension/README.md)：缓解了
  mpv 在 loop 跳转时跳过尾帧的问题。

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

## 本地测试

直接让 mpv 加载构建出的 C++ 插件：

```sh
mpv --script=build/plugins/<插件名>/<插件名>.so <媒体文件>
```

直接加载本项目维护的 Lua 插件：

```sh
mpv --script=plugins/<本地插件名>/lua/<脚本名>.lua <媒体文件>
```

## 开发约定

本项目的代码风格、目录组织与协作流程约定见 [AGENTS.md](AGENTS.md)，开始写
代码前请先阅读。
