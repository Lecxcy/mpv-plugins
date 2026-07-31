#!/usr/bin/env bash
# 生成 mpv-multi.app —— 一个"每个文件起一个独立 mpv 进程"的启动器。
#
# 用法：
#   scripts/steps/make-mpv-multi-app.sh [app_path]
#
# 默认 app_path=~/Applications/mpv-multi.app。
#
# 为什么需要它：mpv 自己的 .app 在 bundle 模式下用 openFiles 事件接收访达传来
# 的文件，多选打开会合成同一条播放列表（见 make-macos-app.sh 的注释）。想让
# 每个文件各占一个窗口，就得在外面套一层，对每个文件单独 open -n。
#
# 这份脚本替代了原先用 Automator 图形界面存出来的 mpv_automator.app。相对那
# 个版本的三点改进：
#
#   1. 不写死 mpv 路径。老版本是 open -n -a "$HOME/Applications/mpv.app"，
#      mpv.app 一挪窝或被删就彻底哑掉。新版本按 bundle id 让 LaunchServices
#      去找，找不到再退回候选路径、再退回 Homebrew 的命令行二进制，全失败
#      才弹窗说明原因（逻辑见 mpv-multi/launch-mpv.sh）。
#   2. 声明真实的文档类型。Automator 存出来的 app 只有
#      CFBundleTypeExtensions=["*"] 这种通配声明，没有 LSItemContentTypes；
#      这种 handler 在访达的"打开方式"列表里可能不出现，系统重建
#      LaunchServices 数据库后也容易被别的 app 顶掉。这里按
#      mpv-multi/video-utis.txt 逐个 UTI 声明，LSHandlerRank=Owner。
#   3. 整个 app 可以从仓库重新生成，源码是 mpv-multi/main.applescript，
#      不再依赖图形界面里存过一次的那个二进制产物。
#
# 注意：
#   - 改完 Info.plist 必须重新 ad-hoc 签名。osacompile 出来的 applet 自带
#     签名，改了 bundle 内容而不重签，Gatekeeper 会拒绝启动且不给提示。
#   - 这个 app 只负责转发，不含 mpv 本体。真正的播放器仍然由
#     make-macos-app.sh 生成，两个脚本各跑各的。
#   - 生成完要跑 scripts/steps/set-video-handlers.sh 才会真正接管视频文件。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
src_dir="${script_dir}/mpv-multi"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "错误：本脚本只适用于 macOS。" >&2
    exit 1
fi

# shellcheck source=mpv-multi/config.sh
source "${src_dir}/config.sh"

app_path="${1:-${MPV_MULTI_APP_PATH}}"
[[ "${app_path}" != /* ]] && app_path="${PWD}/${app_path}"

bundle_id="${MPV_MULTI_BUNDLE_ID}"
uti_list="${src_dir}/video-utis.txt"
applescript="${src_dir}/main.applescript"
launcher="${src_dir}/launch-mpv.sh"

for f in "${uti_list}" "${applescript}" "${launcher}"; do
    if [[ ! -f "${f}" ]]; then
        echo "错误：缺少 ${f}。" >&2
        exit 1
    fi
done

# mpv 上游的 bundle 骨架里带着 io.mpv.* 这些自造 UTI 的完整声明。把其中我们
# 要用到的原样搬进来，这样即使 mpv.app 被删了，io.mpv.mts 之类的 UTI 依然
# 有人声明，绑定不会跟着失效。没有 submodule 也能继续，只是少了这层保险。
skeleton_plist="${repo_root}/external/mpv/TOOLS/osxbundle/mpv.app/Contents/Info.plist"
if [[ ! -f "${skeleton_plist}" ]]; then
    echo "提示：找不到 mpv 骨架 ${skeleton_plist}，将跳过 UTI 声明的搬运。" >&2
    echo "      （io.mpv.* 这些 UTI 会继续依赖 mpv.app 自己声明。）" >&2
    echo "      需要的话先执行：git submodule update --init external/mpv" >&2
    skeleton_plist=""
fi

echo "生成 ${app_path}"

rm -rf "${app_path}"
mkdir -p "$(dirname "${app_path}")"

# 不加 -x（execute-only）：保留 AppleScript 源码，以后想直接查 app 里到底跑
# 了什么，用 osadecompile 就能看到。
/usr/bin/osacompile -o "${app_path}" "${applescript}"

cp "${launcher}" "${app_path}/Contents/Resources/launch-mpv.sh"
chmod +x "${app_path}/Contents/Resources/launch-mpv.sh"

BUNDLE_ID="${bundle_id}" UTI_LIST="${uti_list}" SKELETON="${skeleton_plist}" \
APP_PLIST="${app_path}/Contents/Info.plist" /usr/bin/python3 - <<'PY'
import os
import plistlib

app_plist = os.environ["APP_PLIST"]
uti_list = os.environ["UTI_LIST"]
skeleton = os.environ["SKELETON"]
bundle_id = os.environ["BUNDLE_ID"]

# 清单里允许行尾写 # 注释，先剥掉再取值。
utis = []
with open(uti_list, encoding="utf-8") as fh:
    for line in fh:
        line = line.split("#", 1)[0].strip()
        if line:
            utis.append(line)
if not utis:
    raise SystemExit("错误：UTI 清单为空。")

with open(app_plist, "rb") as fh:
    info = plistlib.load(fh)

info["CFBundleIdentifier"] = bundle_id
info["CFBundleName"] = "mpv-multi"
info["CFBundleDisplayName"] = "mpv-multi"
# 转发完就退出，没必要在程序坞里占个图标。
info["LSUIElement"] = True

# 整段替换掉 osacompile 默认那个 ["*"] / "****" 的通配声明。Owner 这个 rank
# 表示"我就是这类文件的主人"，优先级高于系统里其它只声明 Default 的 app。
info["CFBundleDocumentTypes"] = [{
    "CFBundleTypeName": "Video File",
    "CFBundleTypeRole": "Viewer",
    "LSHandlerRank": "Owner",
    "LSTypeIsPackage": False,
    "LSItemContentTypes": utis,
}]

# 只搬清单里真正用到的那几条，不把 mpv 的字幕/音频 UTI 也一起声明进来。
imported = []
if skeleton:
    with open(skeleton, "rb") as fh:
        skel = plistlib.load(fh)
    wanted = set(utis)
    imported = [d for d in skel.get("UTImportedTypeDeclarations", [])
                if d.get("UTTypeIdentifier") in wanted]
info["UTImportedTypeDeclarations"] = imported

with open(app_plist, "wb") as fh:
    plistlib.dump(info, fh)

print(f"  声明了 {len(utis)} 个 UTI，搬运了 {len(imported)} 条 UTI 定义")
PY

# 必须在所有文件都改完之后再签，签名会把 Info.plist 和 Resources 一起封住。
codesign --force -s - "${app_path}" >/dev/null

lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
[[ -x "${lsregister}" ]] && "${lsregister}" -f "${app_path}"

echo "完成：${app_path}"

# 装到非默认位置时提醒一句：bundle id 是写死的，同一个 id 注册了两份 app，
# LaunchServices 挑哪一份是不确定的。真踩过——把 app 生成到临时目录做验证，
# 结果 duti -x 开始解析到那个临时副本上去了。
if [[ "${app_path}" != "${MPV_MULTI_APP_PATH}" ]]; then
    echo ""
    echo "注意：这不是默认位置 ${MPV_MULTI_APP_PATH}。"
    echo "      两处都存在的话，bundle id ${bundle_id} 会有两个注册项，"
    echo "      系统挑哪个不确定。不再需要这一份时记得注销并删掉："
    echo "        ${lsregister} -u '${app_path}'"
    echo "        rm -rf '${app_path}'"
fi
echo ""
echo "接下来："
echo "  1. scripts/steps/set-video-handlers.sh        把视频格式绑到这个 app"
echo "  2. scripts/steps/set-video-handlers.sh --check 校验绑定是否生效"
echo ""
echo "排查：双击视频没反应时看 ~/Library/Logs/mpv-multi.log。"
