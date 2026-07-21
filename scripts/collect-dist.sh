#!/usr/bin/env bash
# 编译完成后，把 config/ 与已完成 C++ 重写的插件构建物（.so）收集到一个可直接
# 交给 mpv 使用的目录（默认 dist/），方便本地测试或分发。
#
# 尚未重写为 C++ 的插件仍是纯 Lua 实现，不在此脚本的收集范围内（见各插件
# README 的"当前状态"）；等它们完成 C++ 重写后再加入 cpp_plugins 列表。
#
# 用法：
#   scripts/collect-dist.sh [build_dir] [dist_dir]
#
# 默认 build_dir=build，dist_dir=dist（均相对仓库根目录，也可传绝对路径）。
#
# 使用方式（二选一）：
#   mpv --config-dir="$(pwd)/dist" <媒体文件>
#   或将 dist/ 下的内容拷贝进 ~/.config/mpv/（会覆盖同名文件，请自行确认）
#
# 生成的 mpv.conf 里追加的 scripts-append 一律使用 mpv 的 ~~home/ 元路径
# （即"当前生效的配置目录"），所以 dist/ 整体移动或被复制进
# ~/.config/mpv/ 都能正常工作，不依赖生成时的绝对路径。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${1:-build}"
dist_dir="${2:-dist}"
[[ "${build_dir}" != /* ]] && build_dir="${repo_root}/${build_dir}"
[[ "${dist_dir}" != /* ]] && dist_dir="${repo_root}/${dist_dir}"

# 已完成 C++ 重写的插件；名字与 CMake target 名 / .so 文件名一致。
cpp_plugins=(enhanced-rotation enhanced-drag enhanced-ab-loop enhanced-seek)

missing=()
for name in "${cpp_plugins[@]}"; do
    if [[ ! -f "${build_dir}/plugins/${name}/${name}.so" ]]; then
        missing+=("${build_dir}/plugins/${name}/${name}.so")
    fi
done
if (( ${#missing[@]} > 0 )); then
    echo "错误：缺少以下构建产物，请先执行 cmake --build ${build_dir}：" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

rm -rf "${dist_dir}"
mkdir -p "${dist_dir}"

cp "${repo_root}/config/input.conf" "${dist_dir}/input.conf"
cp "${repo_root}/config/mpv.conf" "${dist_dir}/mpv.conf"

for name in "${cpp_plugins[@]}"; do
    mkdir -p "${dist_dir}/plugins/${name}"
    cp "${build_dir}/plugins/${name}/${name}.so" "${dist_dir}/plugins/${name}/${name}.so"
done

{
    echo ""
    echo "# 以下由 scripts/collect-dist.sh 自动生成，加载已完成 C++ 重写的插件。"
    echo "# ~~home/ 会展开为当前生效的配置目录（见 mpv --config-dir）。"
    for name in "${cpp_plugins[@]}"; do
        echo "scripts-append=~~home/plugins/${name}/${name}.so"
    done
} >> "${dist_dir}/mpv.conf"

echo "已收集到 ${dist_dir}："
find "${dist_dir}" -type f | sed "s#^${dist_dir}/#  #" | sort
echo ""
echo "注意：尚未 C++ 重写的插件（纯 Lua）不在收集范围内，仍需按各自 README"
echo "中的方式单独加载（例如 mpv --script=plugins/<名字>/lua/<名字>.lua）。"
echo ""
echo "测试方式："
echo "  mpv --config-dir=\"${dist_dir}\" <媒体文件>"
