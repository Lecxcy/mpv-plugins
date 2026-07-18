# mpv 插件工作区

本仓库用于开发 C++ mpv 插件、维护基于现有 Lua 插件的派生版本，以及将 Lua
插件逐步重构为 C++ 插件。

## 已实现的插件

<!-- TODO: 迁移 mpv-scripts 后回填 -->

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
