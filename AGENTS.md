# 项目约定

本文件记录本仓库的协作与开发约定，人类和 AI 在开始写代码前都应先读完本文件。
仓库本身的介绍（有哪些插件、如何构建）见 [README.md](README.md)。

- 本项目自行维护的 README、说明文档和代码注释统一使用中文。
- 文件名、程序标识符、CMake 命令和第三方 API 名称保留其原始英文形式。
- `external/` 中的上游仓库保持原样，不翻译、不直接修改。
- 上游参考代码与本地维护代码必须分开，不能直接在 submodule 中进行二次开发。
- `external/mpv`、`external/plugins/` 下的代码仅供人工和 AI 阅读参考、不参与
  编译，其版本可能与实际构建链接的 libmpv 不同。判断某个 API 是否可用、行为
  如何时，以系统 pkg-config 提供的 mpv 版本或官方文档为准，不能仅因为参考
  代码里存在某段实现就当作当前可用的事实去参考实现——这一条对 AI 辅助开发
  同样适用，避免照抄参考代码产生幻觉。
- 引入第三方代码前必须确认许可证允许使用、修改和再分发，并保留许可证要求的
  版权与许可声明。
- 每个派生插件都应在自己的 README 中用中文记录上游地址、基准提交、许可证、
  开发路线、主要本地改动和上游同步历史。
- 纯 Lua 插件不加入 CMake；每个 C++ 插件必须在顶层 `CMakeLists.txt` 中显式
  使用 `add_subdirectory()` 加入构建。
- 每个 C++ 插件的 `CMakeLists.txt` 落地前，`tests/` 下必须已有至少一个能通过
  `ctest` 跑通的 Catch2 单元测试，详见"新建 C++ 插件"一节。
- 不提前抽象公共代码。至少有两个插件确实需要同一实现时，才将代码提取到
  `shared/`。

这些约定只适用于本项目维护的内容，不要求修改 `external/mpv` 或第三方插件
仓库中的上游文档和注释。

## 目录结构

```text
mpv-plugins/
├── external/
│   ├── mpv/                    # 固定版本的 mpv 上游源码，只作 API 参考
│   └── plugins/                # 固定版本的第三方插件，只作上游参考
├── plugins/                    # 本项目实际维护的插件
│   └── <插件名>/
│       ├── README.md           # 来源、许可证、改动与使用说明
│       ├── CMakeLists.txt      # 仅 C++ 插件需要
│       ├── lua/                # 持续维护的 Lua 实现
│       ├── lua-reference/      # 仅供重构对照、不再维护的 Lua 实现
│       ├── include/            # C++ 头文件
│       ├── src/                # C++ 源文件
│       └── tests/              # Catch2 单元测试、测试数据和行为对照
│                                # （见"新建 C++ 插件"一节）
├── shared/                     # 多个插件共同使用的代码（C++ 或 Lua）
├── config/                     # 参考用的个人 mpv 配置（input.conf、mpv.conf 等）
├── scripts/                    # 构建、安装和启动测试的辅助脚本
├── cmake/                      # 项目自定义的 CMake 模块
└── build/                      # 本地构建产物，不提交到 Git
```

插件只需要建立实际使用的子目录。例如纯 Lua 插件不需要 `include/`、`src/` 和
`CMakeLists.txt`；完全重写为 C++ 后，也不一定需要保留 `lua/`。

## 添加现有插件作为上游参考

先检查上游许可证，再将仓库固定为 submodule：

```sh
git submodule add <上游仓库地址> external/plugins/<上游插件名>
```

然后在 `plugins/<本地插件名>/` 中建立需要的目录并进行二次开发。不要直接修改
`external/plugins/<上游插件名>/`。

每个派生插件的 `README.md` 至少应记录：

- 上游项目名称和仓库地址；
- 开始派生时的完整 commit；
- 上游许可证及其许可证文件位置；
- 当前采用 Lua 修改、C++ 重写还是混合迁移；
- 与上游相比有意改变的行为；
- 每次同步的新上游版本、移植内容和冲突处理。

如果上游是本人自行维护的仓库（不属于第三方），可以不走 submodule 参考流程，
直接把文件复制进 `plugins/<本地插件名>/`，但仍要在 README 中记录来源仓库地址
和迁移时的 commit，作为溯源依据。

## 同步第三方插件

先只更新参考 submodule：

```sh
git -C external/plugins/<上游插件名> fetch
git -C external/plugins/<上游插件名> checkout <新的提交或标签>
```

对照插件 README 中记录的基准提交审查上游差异，再将需要的修改手动移植到
`plugins/<本地插件名>/`。完成测试后，更新 README 中的同步记录。

## 新建 C++ 插件

在 `plugins/<插件名>/` 下建立 `CMakeLists.txt`、`include/`、`src/`、`tests/`
（以及需要的话 `README.md`），并在顶层 `CMakeLists.txt` 中显式
`add_subdirectory(plugins/<插件名>)`。

### CMakeLists.txt 模板

mpv 的 C 插件在编译期只需要头文件，链接时使用宿主 mpv 进程导出的客户端 API
符号、不能直接链接 libmpv；以下几条规则已经踩过坑验证过，新插件直接照抄即可：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

add_library(<插件名> MODULE
    src/plugin.cpp
)

target_include_directories(<插件名> PRIVATE
    include
    ${MPV_INCLUDE_DIRS}
)

# mpv C 插件使用宿主 mpv 进程导出的客户端 API 符号，不能链接 libmpv。
# 在 macOS 上保留这些未解析符号，等 mpv 加载插件时再解析。
if(APPLE)
    target_link_options(<插件名> PRIVATE
        "-undefined"
        "dynamic_lookup"
    )
endif()

# Windows 加载器不会直接暴露宿主进程符号；定义此宏后，mpv 客户端头文件会
# 为 C 插件提供函数指针表。
if(WIN32)
    target_compile_definitions(<插件名> PRIVATE MPV_CPLUGIN_DYNAMIC_SYM)
endif()

# mpv 按文件名加载 C 插件，因此去掉常规的 "lib" 前缀。
set_target_properties(<插件名> PROPERTIES PREFIX "")
```

### 单元测试

测试框架统一用 [Catch2](https://github.com/catchorg/Catch2)，以固定版本的
submodule 引入到 `external/catch2`（首个需要跑测试的 C++ 插件落地时再实际添加，
现在还不存在这个 submodule）：

```sh
git submodule add https://github.com/catchorg/Catch2.git external/catch2
git -C external/catch2 checkout <固定的 tag，例如 v3.x.x>
```

和 `external/mpv` 不同，`external/catch2` 是要参与编译链接的构建依赖，不是纯
参考代码，但同样要保持上游原样、不做本地修改。

`plugins/<插件名>/tests/CMakeLists.txt` 示例：

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/external/catch2
                  ${CMAKE_BINARY_DIR}/external/catch2 EXCLUDE_FROM_ALL)

add_executable(<插件名>-tests
    test_<插件名>.cpp
)

target_link_libraries(<插件名>-tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(<插件名>-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/<插件名>/include
)

include(Catch)
catch_discover_tests(<插件名>-tests)
```

再从插件的 `CMakeLists.txt` 里 `add_subdirectory(tests)`。每个 C++ 插件的
`CMakeLists.txt` 落地前，这里必须有至少一个能跑通的测试；`ctest` 的启用已在
顶层 `CMakeLists.txt` 中通过 `include(CTest)` 完成。
