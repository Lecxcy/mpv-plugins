#!/usr/bin/env bash
# 把命令行版 mpv（例如 Homebrew 的 /opt/homebrew/bin/mpv）包装成一个真正的
# macOS 应用 mpv.app，这样才能在"访达 → 显示简介 → 打开方式"里把视频的默认
# 打开方式设为 mpv——macOS 只允许 .app 充当文件的默认打开方式。
#
# 用法：
#   scripts/steps/make-macos-app.sh [app_path]
#
# 默认 app_path=~/Applications/mpv.app。可用 MPV_BIN 指定要包装的 mpv 二进制。
#
# 这里不自己拼 Info.plist，而是直接用 mpv 上游自带的 bundle 骨架
# （external/mpv/TOOLS/osxbundle/mpv.app，需要先初始化 submodule）：里面已经
# 声明了所有音视频 UTI、文档图标、应用图标，以及
# LSEnvironment.MPVBUNDLE=true。mpv 只有读到这个环境变量才会进入 bundle 模式
# （见上游 osdep/path-mac.m 与 osdep/mac/app_hub.swift），进而
#   - 额外读取 Contents/Resources/mpv.conf（里面是 pseudo-gui，所以直接双击
#     应用会打开待机窗口，而不是什么都不发生）；
#   - 用原生的 openFiles 事件接收访达传来的文件，多选打开会合成同一条播放
#     列表，而不是每个文件起一个进程。
# 用户配置目录仍然是 ~/.config/mpv，不受 bundle 影响。
#
# 注意：
#   - Contents/MacOS/mpv 必须是真实文件，不能是指向 Homebrew 的符号链接。
#     软链接的 bundle 能在命令行直接跑，但 LaunchServices 会拒绝启动它
#     （open 静默失败，codesign 也会报 "must be a regular file"）。
#     代价是 brew upgrade mpv 之后要重新跑一次本脚本。
#   - 不要往 Info.plist 的 LSEnvironment 里加自定义变量（试过塞 MPV_HOME），
#     加了之后 LaunchServices 就不再启动这个应用了，且不报任何错误。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "错误：本脚本只适用于 macOS。" >&2
    exit 1
fi

app_path="${1:-${HOME}/Applications/mpv.app}"
[[ "${app_path}" != /* ]] && app_path="${PWD}/${app_path}"

mpv_bin="${MPV_BIN:-$(command -v mpv || true)}"
if [[ -z "${mpv_bin}" ]]; then
    echo "错误：找不到 mpv 可执行文件，请先 brew install mpv，或用 MPV_BIN 指定。" >&2
    exit 1
fi

skeleton="${repo_root}/external/mpv/TOOLS/osxbundle/mpv.app"
if [[ ! -f "${skeleton}/Contents/Info.plist" ]]; then
    echo "错误：缺少 mpv 上游的 bundle 骨架 ${skeleton}。" >&2
    echo "请先执行：git submodule update --init external/mpv" >&2
    exit 1
fi

version="$("${mpv_bin}" --version | sed -n '1s/^mpv v\([^ ]*\).*/\1/p')"
version="${version:-UNKNOWN}"

echo "包装 ${mpv_bin}（v${version}）-> ${app_path}"

rm -rf "${app_path}"
mkdir -p "$(dirname "${app_path}")"
cp -R "${skeleton}" "${app_path}"

# 骨架里的 .gitkeep 只是为了让空目录进 git，留着会让 codesign 报
# "code object is not signed at all"。
rm -f "${app_path}/Contents/MacOS/.gitkeep" "${app_path}/Contents/MacOS/lib/.gitkeep"

# cp 默认解引用符号链接，所以 Homebrew 那个指向 Cellar 的 /opt/homebrew/bin/mpv
# 拷进来的是真实二进制，正好满足 LaunchServices 的要求。
cp "${mpv_bin}" "${app_path}/Contents/MacOS/mpv"

# 上游把版本号和分类留成占位符，由 TOOLS/osxbundle.py 在打包时填。这里不调
# 那个脚本（它默认还会把所有依赖 dylib 复制进 bundle，我们要的是跟着
# Homebrew 走的瘦包），所以自己替换。
/usr/bin/sed -i '' \
    -e "s/\${VERSION}/${version}/g" \
    -e "s/\${CATEGORY}/video/g" \
    "${app_path}/Contents/Info.plist"

# Apple Silicon 上没有有效签名的二进制根本无法启动。Homebrew 的 mpv 自带
# ad-hoc 签名，拷贝会保留；这里再对整个 bundle 重签一次，把改过的
# Info.plist 一起封进签名。
codesign --force -s - "${app_path}" >/dev/null

lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
[[ -x "${lsregister}" ]] && "${lsregister}" -f "${app_path}"

# 仓库里的 mpv 骨架自己也是个 bundle id 为 io.mpv 的 .app，LaunchServices 会
# 把它一起扫进数据库。git worktree 每多一个，就多一份。实测过一台机器上同时
# 注册了四份 io.mpv：
#   ~/Applications/mpv.app                                    ← 真的
#   <repo>/external/mpv/TOOLS/osxbundle/mpv.app               ← 骨架
#   <repo>/.claude/worktrees/*/external/mpv/.../mpv.app       ← 每个 worktree 一份
#
# 而 mpv-multi 的 launch-mpv.sh 第一优先级就是 open -n -b io.mpv。同一个 id
# 有多个注册项时，系统挑哪个是不确定的——挑中骨架就等于启动一个
# Contents/MacOS 里没有可执行文件的空壳，症状是双击视频毫无反应，而且时灵
# 时不灵，极难查。
#
# 所以每次生成真正的 mpv.app 之后，顺手把仓库内的所有骨架副本注销掉。
# 注销不改动文件本身，只是把它们从 LaunchServices 数据库里摘掉。
if [[ -x "${lsregister}" ]]; then
    while IFS= read -r skeleton_app; do
        [[ -n "${skeleton_app}" ]] || continue
        echo "  注销仓库内的 mpv 骨架：${skeleton_app#"${repo_root}/"}"
        "${lsregister}" -u "${skeleton_app}" >/dev/null 2>&1 || true
    done < <(find "${repo_root}" -type d -name "mpv.app" -path "*/osxbundle/*" 2>/dev/null)
fi

echo "完成：${app_path}"
echo ""
echo "接下来："
echo "  1. 访达里选中任意视频 → 显示简介 → 打开方式选 mpv → 全部更改。"
echo "  2. brew upgrade mpv 之后重新跑一次本脚本，否则应用里的旧二进制会"
echo "     因为依赖库被换掉而起不来。"
