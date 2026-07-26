#!/usr/bin/env bash
# 把 scripts/collect-dist.sh 收集出来的 dist/ 安装进 mpv 的用户配置目录
# （默认 ~/.config/mpv），让本仓库的插件对日常使用的 mpv 生效。
#
# 用法：
#   scripts/install-config.sh [-n] [-y] [dist_dir] [config_dir]
#
#   -n  只打印将要发生的改动，不实际写入
#   -y  跳过确认提示
#
# 只接管下面这几个"由本仓库管理"的路径，其余一律不碰：
#   input.conf  mpv.conf          直接覆盖
#   plugins/ scripts/ script-opts/ fonts/
#                                 整目录替换（先删后拷）
#
# 整目录替换是有意的：删掉的插件、改名的脚本要能跟着消失，否则
# ~/.config/mpv/scripts/ 下的旧 Lua 脚本会被 mpv 自动加载，和已经重写成 C++
# 的同名插件抢同一批快捷键。
#
# 配置目录里的运行时状态不在管理范围内，不会被删：
#   watch_later/     mpv 的续播位置
#   loop-segments/   enhanced-ab-loop 存的循环区间
#   split-layouts/   split-zoom-box 存的分屏布局
# 也正因为这些状态写在配置目录里，不要把 ~/.config/mpv 直接软链到 dist/：
# collect-dist.sh 每次都会 rm -rf dist/，会把这些状态一起清掉。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

dry_run=0
assume_yes=0
while getopts ":ny" opt; do
    case "${opt}" in
        n) dry_run=1 ;;
        y) assume_yes=1 ;;
        *) echo "用法：scripts/install-config.sh [-n] [-y] [dist_dir] [config_dir]" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

dist_dir="${1:-dist}"
config_dir="${2:-${HOME}/.config/mpv}"
[[ "${dist_dir}" != /* ]] && dist_dir="${repo_root}/${dist_dir}"
[[ "${config_dir}" != /* ]] && config_dir="${PWD}/${config_dir}"

if [[ ! -f "${dist_dir}/mpv.conf" ]]; then
    echo "错误：${dist_dir} 里没有 mpv.conf，请先执行 scripts/collect-dist.sh。" >&2
    exit 1
fi

managed_files=(input.conf mpv.conf)
managed_dirs=(plugins scripts script-opts fonts)

echo "从 ${dist_dir}"
echo "安装到 ${config_dir}"
echo ""
echo "将被覆盖的文件："
for f in "${managed_files[@]}"; do
    [[ -f "${dist_dir}/${f}" ]] && echo "  ${f}"
done
echo "将被整体替换的目录（旧内容会先删除）："
for d in "${managed_dirs[@]}"; do
    [[ ! -d "${dist_dir}/${d}" ]] && continue
    if [[ -d "${config_dir}/${d}" ]]; then
        removed="$(cd "${config_dir}/${d}" && find . -type f | wc -l | tr -d ' ')"
        echo "  ${d}/  （现有 ${removed} 个文件会被删除）"
    else
        echo "  ${d}/  （新建）"
    fi
done
echo ""

if (( dry_run )); then
    echo "-n：未做任何改动。"
    exit 0
fi

if (( ! assume_yes )); then
    read -r -p "继续？[y/N] " reply
    [[ "${reply}" == "y" || "${reply}" == "Y" ]] || { echo "已取消。"; exit 1; }
fi

mkdir -p "${config_dir}"
for f in "${managed_files[@]}"; do
    [[ -f "${dist_dir}/${f}" ]] && cp "${dist_dir}/${f}" "${config_dir}/${f}"
done
for d in "${managed_dirs[@]}"; do
    [[ ! -d "${dist_dir}/${d}" ]] && continue
    rm -rf "${config_dir:?}/${d}"
    cp -R "${dist_dir}/${d}" "${config_dir}/${d}"
done

echo "完成。"
echo ""
echo "验证：mpv --msg-level=cplayer=v <媒体文件> 2>&1 | grep scripts-append"
