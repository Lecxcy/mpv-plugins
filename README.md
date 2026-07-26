# mpv 插件工作区

本仓库用于开发 C++ mpv 插件、维护基于现有 Lua 插件的派生版本，以及将 Lua
插件逐步重构为 C++ 插件。

## 已实现的插件

来源、许可证与改动详情见各插件目录下的 README。

- [enhanced-ab-loop](plugins/enhanced-ab-loop/README.md)（C++）：仿 PotPlayer
  的 A/B Loop 表现并进行优化，支持设置多段不相交的 Loop 区间，并内建了
  尾帧冻结能力。
- [enhanced-rotation](plugins/enhanced-rotation/README.md)（C++）：增强了
  mpv 的旋转功能，支持 360 度循环。
- [enhanced-seek](plugins/enhanced-seek/README.md)（Lua）：修改了 mpv 的快进/
  快退显示。
- [enhanced-volume](plugins/enhanced-volume/README.md)（C++）：修改了 mpv 的
  音量调整逻辑及显示，支持长按连续变化。
- [split-zoom-box](plugins/split-zoom-box/README.md)（C++）：tmux 式分屏放大，
  把画面切成多个窗格、每格显示同一路解码的不同区域并共用同一条时间轴；
  分屏布局可按时间段生效并存档。单窗格时即原 drag-zoom-box 的框选放大，
  并已并入原 enhanced-drag 的拖拽平移。
- [uosc](plugins/uosc/README.md)（Lua）：替代 mpv 内置 `osc.lua` 的完整
  OSD 皮肤 + 菜单系统，本地只保留进度条并集成了 enhanced-ab-loop 的多段
  循环展示。
- [thumbfast](plugins/thumbfast/README.md)（Lua）：高性能实时视频缩略图
  生成器，配合 uosc 在进度条上显示预览图，未作本地修改。

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

## 构建环境要求

- CMake **3.28+**（顶层 `CMakeLists.txt` 用到 `FetchContent_Declare(...
  EXCLUDE_FROM_ALL)`）。注意 Ubuntu 22.04（3.22）和 Debian 12（3.25）自带的版本
  太旧，需要用 Kitware 的 apt 源；Ubuntu 24.04 自带 3.28 可以直接用。
- 能被 pkg-config 找到的 libmpv 开发包（提供 `mpv.pc`）：macOS `brew install mpv`、
  Debian/Ubuntu `apt install libmpv-dev`。
- 支持 C++20 的编译器。项目用 fmt 而非 `std::format`，所以 GCC 11+ / Clang 14+
  就够，不需要很新的工具链。
- **配置阶段需要联网**：Catch2、fmt、nlohmann_json 由 `FetchContent` 在 configure
  时从 GitHub 拉取（固定 tag），没有 vendored 兜底。首次在新机器上构建前请确认
  能访问 GitHub。

Windows 上请使用 **MSYS2/MinGW-w64**（`pacman -S mingw-w64-ucrt-x86_64-mpv
mingw-w64-ucrt-x86_64-cmake`）：`pkg_check_modules` 需要 pkg-config 和 `mpv.pc`，
而 vcpkg 目前没有官方 mpv port；`scripts/collect-dist.sh` 也需要 bash。

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

mpv 按后缀识别 C 插件，且各平台只认一种：Windows 上产物与加载名都是 `.dll`，
macOS 和 Linux 都是 `.so`（macOS 不是 `.dylib`）。下文提到 `.so` 的地方在
Windows 上都对应 `.dll`，`scripts/collect-dist.sh` 会自动按平台处理。

## 运行单元测试

C++ 插件使用 Catch2（`external/catch2`，固定版本 submodule）。构建完成后：

```sh
ctest --test-dir build --output-on-failure
```

也可以直接运行某个插件的测试可执行文件（支持 Catch2 原生的 `--list-tests`、
按标签 `"[layout]"` 过滤等用法）：

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

编译完成后，可以用 `scripts/collect-dist.sh` 把 `config/`、已完成 C++ 重写
的插件构建物（`.so`）与已登记的纯 Lua 插件（目前是 uosc、thumbfast）收集到
`dist/`（默认路径，可传参数覆盖）。**其余尚未 C++ 重写、未登记的纯 Lua
插件不在收集范围内**，仍按上面"本地测试"里的方式单独加载：

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
