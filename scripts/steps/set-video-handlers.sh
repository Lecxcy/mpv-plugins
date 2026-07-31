#!/usr/bin/env bash
# 把所有视频格式的默认打开方式绑到 mpv-multi.app，并提供校验模式。
#
# 用法：
#   scripts/steps/set-video-handlers.sh            应用绑定（只补没绑过的）
#   scripts/steps/set-video-handlers.sh -y         不等确认直接下发
#   scripts/steps/set-video-handlers.sh -n         只打印将要下发什么，不写入
#   scripts/steps/set-video-handlers.sh --force    忽略"已绑过"记录，全量重下
#   scripts/steps/set-video-handlers.sh --check    校验当前绑定，不写入
#
# 需要 duti：brew install duti
#
# ==== 弹窗！先读这段 ====
#
# macOS 26 起，程序改默认打开方式会被系统拦下来，每个 UTI 弹一次确认框。
# 本清单有 40 个 UTI，全量下发 = 连点 40 次确认。所以这个脚本默认只下发
# "还没绑过的"，重复执行是静默的（0 个弹窗），并且在真正下发前会先告诉你
# 这次要点多少次确认。
#
# "已绑过"由两处合并判断：
#   1. LaunchServices 的偏好文件里已经记着这个 UTI 归我们
#      （~/Library/Preferences/com.apple.LaunchServices/...secure.plist）；
#   2. 本脚本自己的状态文件（见下面的 state_file）。
# 之所以需要第 2 处：有些 UTI 靠 app 自己 Info.plist 里的 LSHandlerRank=Owner
# 就直接生效了，压根不会写进那个偏好文件（本机的
# com.microsoft.windows-media-wmv、com.real.realmedia-vbr 等就是这样）。
# 只看第 1 处的话，这些会被误判成"没绑过"，于是每次重跑都白弹几次窗。
#
# ==== 为什么按 UTI 绑而不是按扩展名 ====
#
# duti 绑的是 UTI，而一个扩展名往往对应好几个 UTI——.mp4 有 public.mpeg-4 /
# public.mpeg-4-audio / com.microsoft.ppt.export.mp4，.ts 有
# public.mpeg-2-transport-stream 和 io.mpv.mts。只绑其中一个，另一个仍归旧
# app，于是同样是 .ts，这个文件用 mpv 开、那个用 QuickTime 开。所以这里以
# UTI 清单为准一次绑全，并且用 --check 反查"某个扩展名是否还有清单外的 UTI
# 漏网"。
#
# 清单在 mpv-multi/video-utis.txt，改那里，不要在这里散着加。
# 明确不想绑的放 mpv-multi/ignored-utis.txt，--check 会跳过它们。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_dir="${script_dir}/mpv-multi"

# shellcheck source=mpv-multi/config.sh
source "${src_dir}/config.sh"

uti_list="${src_dir}/video-utis.txt"
ext_list="${src_dir}/video-extensions.txt"
ignore_list="${src_dir}/ignored-utis.txt"

ls_prefs="${HOME}/Library/Preferences/com.apple.LaunchServices/com.apple.launchservices.secure.plist"
state_file="${HOME}/.config/mpv/.mpv-multi-bound-utis"

dry_run=0
assume_yes=0
force=0
mode="apply"
for arg in "$@"; do
    case "${arg}" in
        -n) dry_run=1 ;;
        -y) assume_yes=1 ;;
        --force) force=1 ;;
        --check) mode="check" ;;
        --mark-bound) mode="mark" ;;
        -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "错误：未知参数 ${arg}" >&2; exit 1 ;;
    esac
done

if ! command -v duti >/dev/null 2>&1; then
    echo "错误：找不到 duti，请先 brew install duti。" >&2
    exit 1
fi

for f in "${uti_list}" "${ext_list}"; do
    [[ -f "${f}" ]] || { echo "错误：缺少 ${f}。" >&2; exit 1; }
done

# 剥掉注释和空行，得到干净的清单。
read_list() {
    [[ -f "$1" ]] || return 0
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$1"
}

utis="$(read_list "${uti_list}")"
exts="$(read_list "${ext_list}")"
ignored="$(read_list "${ignore_list}")"

# --------------------------------------------------------------------------
# 校验模式
# --------------------------------------------------------------------------
if [[ "${mode}" == "check" ]]; then
    problems=0

    echo "=== 逐个扩展名核对实际 handler ==="
    while read -r ext; do
        # duti -x 输出三行：应用名、路径、bundle id。我们只认第三行。
        actual="$(duti -x "${ext}" 2>/dev/null | sed -n '3p')"
        if [[ -z "${actual}" ]]; then
            printf '  %-6s 没有默认 handler\n' "${ext}"
            problems=$((problems + 1))
        elif [[ "${actual}" != "${MPV_MULTI_BUNDLE_ID}" ]]; then
            printf '  %-6s 归 %s\n' "${ext}" "${actual}"
            problems=$((problems + 1))
        fi
    done <<< "${exts}"
    [[ ${problems} -eq 0 ]] && echo "  全部正确"

    echo ""
    echo "=== 反查清单外漏网的 UTI ==="
    # 一个扩展名能解析出的 UTI 只要有一个不在清单里，那个扩展名迟早会时灵
    # 时不灵。dyn.* 是系统给未知类型现编的动态 UTI，绑不了也没必要绑；
    # ignored-utis.txt 里的是明确决定不绑的。
    missing=0
    while read -r ext; do
        while read -r uti; do
            [[ -z "${uti}" || "${uti}" == dyn.* ]] && continue
            grep -qxF "${uti}" <<< "${utis}" && continue
            [[ -n "${ignored}" ]] && grep -qxF "${uti}" <<< "${ignored}" && continue
            printf '  %-6s -> %s 不在 video-utis.txt 里\n' "${ext}" "${uti}"
            missing=$((missing + 1))
        done <<< "$(duti -e "${ext}" 2>/dev/null | awk '/^identifier: /{print $2}')"
    done <<< "${exts}"
    if [[ ${missing} -eq 0 ]]; then
        echo "  没有漏网"
    else
        echo ""
        echo "  确实该绑就补进 mpv-multi/video-utis.txt，明确不想绑就写进"
        echo "  mpv-multi/ignored-utis.txt，然后重跑："
        echo "    scripts/steps/make-mpv-multi-app.sh && scripts/steps/set-video-handlers.sh"
    fi

    exit $(( problems + missing > 0 ? 1 : 0 ))
fi

# --------------------------------------------------------------------------
# 标记模式：把清单里全部 UTI 记为"已绑"，一条都不下发。
#
# 用途：--check 已经显示全绿，但其中一部分是靠 app 的 LSHandlerRank=Owner
# 自动生效的，不会写进 LaunchServices 偏好文件。不标记的话，脚本每次都会
# 把这几条当成"没绑过"再下发一遍，白白弹几次确认框。确认现状正确之后跑一次
# 这个，之后重跑就彻底安静了。
# --------------------------------------------------------------------------
if [[ "${mode}" == "mark" ]]; then
    mkdir -p "$(dirname "${state_file}")"
    printf '%s\n' "${utis}" | sed '/^$/d' | sort -u > "${state_file}"
    echo "已把 $(wc -l < "${state_file}" | tr -d ' ') 个 UTI 记为已绑：${state_file}"
    echo "（想重新下发用 --force）"
    exit 0
fi

# --------------------------------------------------------------------------
# 应用模式
# --------------------------------------------------------------------------

# app 不存在时 duti 仍会把绑定写进 LaunchServices，然后所有视频都变成双击
# 没反应，所以先挡一道。
if [[ ${dry_run} -eq 0 && ! -d "${MPV_MULTI_APP_PATH}" ]]; then
    echo "错误：找不到 ${MPV_MULTI_APP_PATH}，请先执行 scripts/steps/make-mpv-multi-app.sh。" >&2
    exit 1
fi

# 已经绑给我们的 UTI：LaunchServices 偏好文件 + 本脚本的状态文件。
already_bound() {
    if [[ -f "${ls_prefs}" ]]; then
        /usr/bin/python3 - "${ls_prefs}" "${MPV_MULTI_BUNDLE_ID}" <<'PY' 2>/dev/null || true
import plistlib, sys
path, bundle_id = sys.argv[1], sys.argv[2]
try:
    with open(path, "rb") as fh:
        handlers = plistlib.load(fh).get("LSHandlers", [])
except Exception:
    handlers = []
for h in handlers:
    if h.get("LSHandlerRoleAll") == bundle_id and h.get("LSHandlerContentType"):
        print(h["LSHandlerContentType"])
PY
    fi
    [[ -f "${state_file}" ]] && cat "${state_file}"
    # 必须显式 return 0：状态文件第一次运行时还不存在，上面那句 [[ ]] 会返回
    # 1，而调用处是 already_bound | sort -u，脚本开头又开了 pipefail，于是
    # 整个管道被判定为失败，set -e 让脚本一声不吭地退出。
    return 0
}

bound=""
[[ ${force} -eq 0 ]] && bound="$(already_bound | sort -u)"

pending=""
skipped=0
while read -r uti; do
    if [[ -n "${bound}" ]] && grep -qxF "${uti}" <<< "${bound}"; then
        skipped=$((skipped + 1))
        continue
    fi
    pending+="${uti}"$'\n'
done <<< "${utis}"
pending="$(printf '%s' "${pending}" | sed '/^$/d')"

if [[ -z "${pending}" ]]; then
    echo "全部 $(wc -l <<< "${utis}" | tr -d ' ') 个 UTI 都已绑到 ${MPV_MULTI_BUNDLE_ID}，无需下发。"
    echo "（要强制重下：--force）"
    exit 0
fi

count="$(wc -l <<< "${pending}" | tr -d ' ')"

echo "待下发 ${count} 个 UTI -> ${MPV_MULTI_BUNDLE_ID}（已跳过 ${skipped} 个绑好的）："
sed 's/^/  /' <<< "${pending}"

if [[ ${dry_run} -eq 1 ]]; then
    echo ""
    echo "（-n 演练模式，未写入）"
    exit 0
fi

# 提前把弹窗次数说清楚。被这个突袭过一次就知道有多烦。
echo ""
echo "注意：macOS 会为每个 UTI 弹一次确认框，也就是接下来要点 ${count} 次「使用…」。"
if [[ ${assume_yes} -eq 0 ]]; then
    printf "继续？[y/N] "
    read -r reply
    [[ "${reply}" =~ ^[Yy]$ ]] || { echo "已取消。"; exit 0; }
fi

# duti 的配置文件格式：每行「bundle_id<TAB>UTI<TAB>role」。role 用 all，表示
# 查看和编辑都归它。
conf="$(mktemp -t mpv-multi-duti)"
trap 'rm -f "${conf}"' EXIT
while read -r uti; do
    printf '%s\t%s\tall\n' "${MPV_MULTI_BUNDLE_ID}" "${uti}" >> "${conf}"
done <<< "${pending}"

duti "${conf}"

# 记下这次下发过的，下回就不再重复骚扰。注意记的是"下发过"而不是"确认成
# 功"——duti 无法得知用户在弹窗里点了什么。想重来就用 --force。
mkdir -p "$(dirname "${state_file}")"
{ [[ -f "${state_file}" ]] && cat "${state_file}"; printf '%s\n' "${pending}"; } \
    | sed '/^$/d' | sort -u > "${state_file}.tmp"
mv "${state_file}.tmp" "${state_file}"

echo ""
echo "完成。校验：scripts/steps/set-video-handlers.sh --check"
