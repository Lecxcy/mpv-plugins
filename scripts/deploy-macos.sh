#!/usr/bin/env bash
# 一条命令跑完从源码到"访达里双击视频就用 mpv 多开"的整条流水线。
#
# 这是 scripts/ 下唯一的入口脚本，各阶段的实现都在 scripts/steps/，也可以
# 单独执行（每个都有自己的 --help）。
#
# 用法：
#   scripts/deploy-macos.sh                    跑完整流水线
#   scripts/deploy-macos.sh --list             列出所有阶段
#   scripts/deploy-macos.sh -n                 只打印每步要执行的命令，不实际跑
#   scripts/deploy-macos.sh -y                 不等确认（透传给会提问的阶段）
#   scripts/deploy-macos.sh --only build,collect
#   scripts/deploy-macos.sh --skip handlers
#   scripts/deploy-macos.sh --from mpv-app     从某个阶段开始往后跑
#   scripts/deploy-macos.sh --no-cache         本次编译不用 ccache
#   scripts/deploy-macos.sh --fresh            清掉 CMake 缓存重新配置
#
# 阶段顺序是有依赖的，不要随便调换：
#
#   build      编译 C++ 插件（自动启用 ccache）
#   collect    把构建物和 config/ 收集到 dist/
#   fetch-data 拉取用 git 管理的用户数据（分段、分屏布局；清单为空则跳过）
#   install    把 dist/ 装进 ~/.config/mpv
#   mpv-app    把 Homebrew 的 mpv 二进制包成 ~/Applications/mpv.app
#   multi-app  生成 ~/Applications/mpv-multi.app（每个文件一个独立进程）
#   handlers   把视频格式的默认打开方式绑到 mpv-multi.app
#   verify     校验绑定是否真的生效
#
# 其中 mpv-app 必须排在 multi-app 和 handlers 前面：多开 app 只是个转发壳，
# 它按 bundle id 找 mpv.app，mpv.app 起不来的话前面几步做得再对也没用。
# 而 mpv.app 里那份二进制是 brew 二进制的拷贝，brew upgrade 之后不重跑
# mpv-app 阶段就会因为依赖的 dylib 版本被换掉而在 dyld 阶段崩溃
# （见 steps/make-macos-app.sh 的注释）——所以日常改插件也建议整条跑一遍。
#
# 几个刻意留白的地方：
#   - verify 失败只警告不中断，因为"漏网 UTI"在不同机器上差异很大，
#     不该因此让整条流水线红掉。想让它致命的话改 stage_verify 里那行。
#   - 没有卸载/回滚。install-config.sh 会整目录替换，跑之前它自己会确认。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
steps_dir="${script_dir}/steps"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "错误：本脚本只适用于 macOS（mpv-app 之后的阶段都是 macOS 专属）。" >&2
    exit 1
fi

# --------------------------------------------------------------------------
# 可调参数。以后要细化流水线，多半是往这里加东西。
# --------------------------------------------------------------------------
BUILD_DIR="${BUILD_DIR:-${repo_root}/build}"
DIST_DIR="${DIST_DIR:-${repo_root}/dist}"
CONFIG_DIR="${CONFIG_DIR:-${HOME}/.config/mpv}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
MPV_APP="${MPV_APP:-${HOME}/Applications/mpv.app}"
MULTI_APP="${MULTI_APP:-${HOME}/Applications/mpv-multi.app}"
DATA_REPOS="${DATA_REPOS:-${steps_dir}/data-repos.txt}"

ALL_STAGES=(build collect fetch-data install mpv-app multi-app handlers verify)

stage_desc() {
    case "$1" in
        build)      echo "编译 C++ 插件" ;;
        collect)    echo "收集构建物到 ${DIST_DIR#"${repo_root}/"}/" ;;
        fetch-data) echo "拉取用户数据仓库" ;;
        install)   echo "安装配置到 ${CONFIG_DIR}" ;;
        mpv-app)   echo "打包 ${MPV_APP}" ;;
        multi-app) echo "打包 ${MULTI_APP}" ;;
        handlers)  echo "绑定视频默认打开方式" ;;
        verify)    echo "校验绑定" ;;
        *)         echo "?" ;;
    esac
}

# --------------------------------------------------------------------------
# 参数解析
# --------------------------------------------------------------------------
dry_run=0
assume_yes=0
use_cache=1
fresh=0
only_list=""
skip_list=""
from_stage=""

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n|--dry-run)  dry_run=1 ;;
        -y|--yes)      assume_yes=1 ;;
        --no-cache)    use_cache=0 ;;
        --fresh)       fresh=1 ;;
        --only)        only_list="${2:-}"; shift ;;
        --skip)        skip_list="${2:-}"; shift ;;
        --from)        from_stage="${2:-}"; shift ;;
        --list)
            echo "阶段（按执行顺序）："
            for s in "${ALL_STAGES[@]}"; do
                printf '  %-10s %s\n' "${s}" "$(stage_desc "${s}")"
            done
            exit 0
            ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "错误：未知参数 $1（--help 看用法）" >&2; exit 1 ;;
    esac
    shift
done

# 逗号分隔的列表里是否包含某个阶段。
in_list() {
    local needle="$1" list="$2" items i
    IFS=',' read -ra items <<< "${list}"
    for i in "${items[@]}"; do
        [[ "${i}" == "${needle}" ]] && return 0
    done
    return 1
}

# 校验用户给的阶段名，写错了要立刻报，不能默默跳过所有阶段。
validate_names() {
    local list="$1" label="$2" names name
    [[ -z "${list}" ]] && return 0
    IFS=',' read -ra names <<< "${list}"
    for name in "${names[@]}"; do
        if ! in_list "${name}" "$(IFS=,; echo "${ALL_STAGES[*]}")"; then
            echo "错误：${label} 里的 ${name} 不是有效阶段名（--list 看全部）" >&2
            exit 1
        fi
    done
}
validate_names "${only_list}" "--only"
validate_names "${skip_list}" "--skip"
validate_names "${from_stage}" "--from"

selected=()
reached_from=0
for s in "${ALL_STAGES[@]}"; do
    [[ -n "${from_stage}" && "${s}" == "${from_stage}" ]] && reached_from=1
    [[ -n "${from_stage}" && ${reached_from} -eq 0 ]] && continue
    [[ -n "${only_list}" ]] && ! in_list "${s}" "${only_list}" && continue
    [[ -n "${skip_list}" ]] && in_list "${s}" "${skip_list}" && continue
    selected+=("${s}")
done

if [[ ${#selected[@]} -eq 0 ]]; then
    echo "错误：筛选之后没有任何阶段可执行。" >&2
    exit 1
fi

# --------------------------------------------------------------------------
# 执行框架
# --------------------------------------------------------------------------
warnings=()

# 打印并执行一条命令。-n 时只打印。所有实际动作都要经过它，这样 -n 才可信。
#
# 调用处一律要写成 `run ... || return 1`。原因是主循环用 `if ! dispatch` 调
# 阶段函数，而 bash 在条件上下文里会关掉 set -e——连带函数体内部也不生效。
# 少写一个 || return 1，那一步失败后阶段函数会继续往下跑，最后还可能返回 0，
# 于是编译都挂了流水线却接着走下一阶段。踩过一次，别再踩。
run() {
    printf '    $ %s\n' "$*"
    [[ ${dry_run} -eq 1 ]] && return 0
    "$@"
}

note() { printf '    %s\n' "$*"; }

# --------------------------------------------------------------------------
# fetch-data：把 data-repos.txt 里登记的用户数据目录拉到 ~/.config/mpv 下
#
# 拉的是数据不是代码——做好的循环区间和分屏布局。插件能重新编译，这些不能，
# 所以这一步的每个失败分支都宁可停下让人来处理，不自作主张。
#
# 放在 collect 之后、install 之前：install-config.sh 只整目录替换
# plugins/ scripts/ script-opts/ fonts/ 这四个，不碰数据目录，所以先后都安全；
# 排在 install 前是为了让"配置目录相关的动作"连在一起。
# --------------------------------------------------------------------------
stage_fetch_data() {
    if [[ ! -f "${DATA_REPOS}" ]]; then
        note "没有 ${DATA_REPOS#"${repo_root}/"}，跳过。"
        return 0
    fi

    local entries
    entries="$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "${DATA_REPOS}")"
    if [[ -z "${entries}" ]]; then
        note "清单里还没有登记任何仓库，跳过。"
        return 0
    fi

    local dir url branch target
    while read -r dir url branch; do
        [[ -z "${dir}" || -z "${url}" ]] && continue
        target="${CONFIG_DIR}/${dir}"

        if [[ ! -e "${target}" ]]; then
            if [[ -n "${branch}" ]]; then
                run git clone --branch "${branch}" "${url}" "${target}" || return 1
            else
                run git clone "${url}" "${target}" || return 1
            fi
            continue
        fi

        # 目录在，但还不是 git 仓库——里面是已经做好、尚未纳入版本管理的数据。
        # clone 会因为目录非空而失败，而绝不能为了让 clone 成功去清空它：
        # 这些数据正是整个机制要保护的东西。停下来，把认领命令打出来。
        if [[ ! -d "${target}/.git" ]]; then
            echo "" >&2
            echo "错误：${target} 已存在，但还不是 git 仓库。" >&2
            echo "      里面的数据不会被自动删除或覆盖。" >&2
            echo "" >&2
            echo "      如果远端就是从这份数据推上去的，就地认领即可（不动工作区文件）：" >&2
            echo "        git -C '${target}' init" >&2
            echo "        git -C '${target}' remote add origin '${url}'" >&2
            echo "        git -C '${target}' fetch origin" >&2
            echo "        git -C '${target}' reset --mixed origin/${branch:-HEAD}" >&2
            echo "" >&2
            echo "      如果远端另有内容，先把现有目录改名备份，再重跑本阶段。" >&2
            return 1
        fi

        # 本机新做的分段还没提交时提醒一句。ff-only 的 pull 只有在远端动了
        # 同一批文件时才会失败，但先说清楚总好过让人对着 git 报错发愣。
        if [[ ${dry_run} -eq 0 && -n "$(git -C "${target}" status --porcelain 2>/dev/null)" ]]; then
            note "提示：${dir} 有未提交的本地改动（新做的数据？），记得自己 commit + push。"
        fi

        # pull 用 --ff-only：需要 merge 时宁可失败，也不擅自合并或丢弃。
        run git -C "${target}" fetch --prune || return 1
        if [[ -n "${branch}" ]]; then
            run git -C "${target}" checkout "${branch}" || return 1
        fi
        run git -C "${target}" pull --ff-only || return 1
    done <<< "${entries}"
}

# --------------------------------------------------------------------------
# build：编译，尽量走编译缓存
# --------------------------------------------------------------------------
# 返回可用的编译器 launcher 名字（ccache / sccache），都没有就返回空。
detect_compiler_cache() {
    local c
    for c in ccache sccache; do
        command -v "${c}" >/dev/null 2>&1 && { echo "${c}"; return 0; }
    done
    return 0
}

# ccache 的命中/未命中累计计数。ccache 4.x 的 --print-stats 是机器可读的
# tab 分隔格式，比 --show-stats 那种给人看的排版稳定得多。
ccache_counter() {
    ccache --print-stats 2>/dev/null \
        | awk -F'\t' '$1=="direct_cache_hit"||$1=="preprocessed_cache_hit"{h+=$2}
                      $1=="cache_miss"{m+=$2}
                      END{print h+0, m+0}'
}

stage_build() {
    local launcher=""
    [[ ${use_cache} -eq 1 ]] && launcher="$(detect_compiler_cache)"

    if [[ ${use_cache} -eq 1 && -z "${launcher}" ]]; then
        note "没装 ccache，本次全量编译。想加速：brew install ccache"
    fi

    local cc_before=""
    [[ "${launcher}" == "ccache" && ${dry_run} -eq 0 ]] && cc_before="$(ccache_counter)"

    # pkg_check_modules 之类的探测结果是写进 CMakeCache.txt 的，重跑 cmake
    # 不会重新探测。所以 brew 升级过依赖之后（Cellar 路径带版本号，旧目录会
    # 被删），缓存里就留着一个已经不存在的 -I 路径，症状是「头文件突然找不
    # 到了」。--fresh 清掉缓存重新配置即可。
    local fresh_arg=""
    [[ ${fresh} -eq 1 ]] && fresh_arg="--fresh"

    # launcher 是 configure 期写进 CMakeCache.txt 的，所以两种情况都要显式
    # 传：要么设上，要么用 -U 取消掉上次设的，否则 --no-cache 会没有效果。
    if [[ -n "${launcher}" ]]; then
        note "编译缓存：${launcher}"
        run cmake -S "${repo_root}" -B "${BUILD_DIR}" ${fresh_arg} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
            -DCMAKE_CXX_COMPILER_LAUNCHER="${launcher}" \
            -DCMAKE_C_COMPILER_LAUNCHER="${launcher}" || return 1
    else
        run cmake -S "${repo_root}" -B "${BUILD_DIR}" ${fresh_arg} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
            -UCMAKE_CXX_COMPILER_LAUNCHER \
            -UCMAKE_C_COMPILER_LAUNCHER || return 1
    fi

    if ! run cmake --build "${BUILD_DIR}"; then
        echo "" >&2
        echo "      编译失败。如果报的是「某个头文件找不到」而那个库其实装着，" >&2
        echo "      多半是 brew 升级后 CMake 缓存里的路径失效了，用 --fresh 重来：" >&2
        echo "        scripts/deploy-macos.sh --fresh --from build" >&2
        return 1
    fi

    # 报一下**本次**的命中情况。必须用前后计数做差：ccache 自己的统计是自
    # 安装以来的累计值，直接打出来会看到「0 hits」而误以为缓存没工作，其实
    # 只是这次增量构建根本没调用过编译器。
    if [[ "${launcher}" == "ccache" && ${dry_run} -eq 0 && -n "${cc_before}" ]]; then
        local h0 m0 h1 m1 dh dm
        read -r h0 m0 <<< "${cc_before}"
        read -r h1 m1 <<< "$(ccache_counter)"
        dh=$(( h1 - h0 ))
        dm=$(( m1 - m0 ))
        if [[ $(( dh + dm )) -eq 0 ]]; then
            note "ccache: 本次没有编译发生（增量构建，全部是最新的）"
        else
            note "ccache: 本次命中 ${dh} / $(( dh + dm ))，未命中 ${dm}"
        fi
    fi
}

stage_collect() {
    run "${steps_dir}/collect-dist.sh" "${BUILD_DIR}" "${DIST_DIR}" || return 1
}

stage_install() {
    # install-config.sh 会整目录替换 plugins/ scripts/ script-opts/ fonts/，
    # 默认要人确认；流水线里跑就把 -y 透传下去，否则会卡住等输入。
    if [[ ${assume_yes} -eq 1 ]]; then
        run "${steps_dir}/install-config.sh" -y "${DIST_DIR}" "${CONFIG_DIR}" || return 1
    else
        run "${steps_dir}/install-config.sh" "${DIST_DIR}" "${CONFIG_DIR}" || return 1
    fi
}

stage_mpv_app() {
    run "${steps_dir}/make-macos-app.sh" "${MPV_APP}" || return 1
}

stage_multi_app() {
    run "${steps_dir}/make-mpv-multi-app.sh" "${MULTI_APP}" || return 1
}

stage_handlers() {
    # 这一步会让 macOS 对每个待绑 UTI 弹一次确认框，所以同样透传 -y。
    if [[ ${assume_yes} -eq 1 ]]; then
        run "${steps_dir}/set-video-handlers.sh" -y || return 1
    else
        run "${steps_dir}/set-video-handlers.sh" || return 1
    fi
}

stage_verify() {
    # 故意不让它中断流水线，见文件头的说明。
    if ! run "${steps_dir}/set-video-handlers.sh" --check; then
        warnings+=("verify：有绑定未生效或有清单外的 UTI，见上面的输出")
    fi
}

dispatch() {
    case "$1" in
        build)      stage_build ;;
        collect)    stage_collect ;;
        fetch-data) stage_fetch_data ;;
        install)    stage_install ;;
        mpv-app)   stage_mpv_app ;;
        multi-app) stage_multi_app ;;
        handlers)  stage_handlers ;;
        verify)    stage_verify ;;
    esac
}

# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
[[ ${dry_run} -eq 1 ]] && echo "（-n 演练模式，不会实际执行）" && echo

total=${#selected[@]}
index=0
started_at=$(date +%s)

for s in "${selected[@]}"; do
    index=$((index + 1))
    printf '[%d/%d] %-10s %s\n' "${index}" "${total}" "${s}" "$(stage_desc "${s}")"
    if ! dispatch "${s}"; then
        echo "" >&2
        echo "失败：阶段 ${s} 出错，流水线中止。" >&2
        echo "修好之后可以从这一步继续：scripts/deploy-macos.sh --from ${s}" >&2
        exit 1
    fi
    echo ""
done

elapsed=$(( $(date +%s) - started_at ))
echo "流水线完成，用时 ${elapsed}s。"

if [[ ${#warnings[@]} -gt 0 ]]; then
    echo ""
    echo "有告警："
    for w in "${warnings[@]}"; do
        echo "  - ${w}"
    done
fi
