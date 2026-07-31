#!/bin/bash
# mpv-multi.app 的实际执行体，被 bundle 里的 AppleScript droplet 调用。
# 每个参数是一个文件路径，逐个用独立进程打开——这就是"多开"：mpv 自己的
# .app 收到多选文件时会合成同一条播放列表（bundle 模式下走 openFiles 事件），
# 而这里对每个文件单独起一个实例。
#
# 用法（也可以直接在命令行跑，方便排查）：
#   scripts/mpv-multi/launch-mpv.sh [file ...]
#
# 定位 mpv 的顺序，从最不依赖路径到最兜底：
#
#   1. open -n -b io.mpv
#      按 bundle identifier 让 LaunchServices 去找 mpv.app。app 挪到哪个
#      目录都还能找到——这是本脚本相对老版本 Automator 那句写死的
#      open -n -a "$HOME/Applications/mpv.app" 最主要的改进。
#   2. 候选路径里的 mpv.app
#      LaunchServices 数据库偶尔会过期（比如刚拷进来还没被扫到），按路径
#      再试一遍，顺便把找到的 app 注册回去。
#   3. 命令行二进制 mpv
#      连 .app 都没有了（没跑过 make-macos-app.sh、或者被删了）时，直接跑
#      Homebrew 的二进制。此时没有 bundle 模式，也就没有 Contents/Resources
#      /mpv.conf 里的 pseudo-gui，但放视频完全正常。
#
# 三步都失败才弹窗报错。每次调用都会往 ~/Library/Logs/mpv-multi.log 追加
# 一行，Finder 里双击没反应时先看这个日志。

set -u

readonly MPV_BUNDLE_ID="io.mpv"
readonly LOG="${HOME}/Library/Logs/mpv-multi.log"

# LaunchServices 拉起来的进程 PATH 极简（通常只有 /usr/bin:/bin:/usr/sbin:/sbin），
# 不能指望 command -v 找得到 Homebrew，所以候选位置全部写死。
readonly APP_CANDIDATES=(
    "${HOME}/Applications/mpv.app"
    "/Applications/mpv.app"
)
readonly BIN_CANDIDATES=(
    "/opt/homebrew/bin/mpv"
    "/usr/local/bin/mpv"
)

readonly OPEN=/usr/bin/open
readonly LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

log() {
    printf '%s %s\n' "$(/bin/date '+%Y-%m-%d %H:%M:%S')" "$*" >> "${LOG}" 2>/dev/null || true
}

# 找不到 mpv 时给个能看懂的提示，而不是双击了什么都不发生。
fail() {
    log "ERROR $*"
    /usr/bin/osascript -e 'display alert "找不到 mpv" message "mpv-multi 既没有找到 mpv.app，也没有找到命令行的 mpv。

请先执行 scripts/make-macos-app.sh 生成 ~/Applications/mpv.app，或 brew install mpv。

详情见 ~/Library/Logs/mpv-multi.log" as critical' >/dev/null 2>&1
    exit 1
}

# 依次尝试三种方式启动一个 mpv 实例，最多带一个文件参数（不带就是空窗口）。
# 成功返回 0。
#
# 这里刻意用「有没有参数」两条分支，而不是把参数放进数组统一展开：系统自带
# 的是 bash 3.2，空数组在 set -u 下展开会直接报 unbound variable。
# 另外 open 的 --args 必须放在最后，它之后的一切都原样传给被启动的程序。
launch_one() {
    local target="${1:-}"

    if [[ -n "${target}" ]]; then
        "${OPEN}" -n -b "${MPV_BUNDLE_ID}" --args "${target}" 2>/dev/null && return 0
    else
        "${OPEN}" -n -b "${MPV_BUNDLE_ID}" 2>/dev/null && return 0
    fi

    local app
    for app in "${APP_CANDIDATES[@]}"; do
        [[ -d "${app}" ]] || continue
        # 能按路径找到却按 bundle id 找不到，说明 LaunchServices 的记录过期了，
        # 注册一次，下回就能走上面那条最快的路径。
        [[ -x "${LSREGISTER}" ]] && "${LSREGISTER}" -f "${app}" >/dev/null 2>&1
        if [[ -n "${target}" ]]; then
            "${OPEN}" -n -a "${app}" --args "${target}" 2>/dev/null || continue
        else
            "${OPEN}" -n -a "${app}" 2>/dev/null || continue
        fi
        log "fallback: 按路径启动 ${app}"
        return 0
    done

    local bin
    for bin in "${BIN_CANDIDATES[@]}"; do
        [[ -x "${bin}" ]] || continue
        # 没有 .app 可用，直接起二进制。必须脱离父进程，否则调用方的
        # do shell script 会一直等到 mpv 退出。
        if [[ -n "${target}" ]]; then
            /usr/bin/nohup "${bin}" "${target}" >/dev/null 2>&1 &
        else
            /usr/bin/nohup "${bin}" >/dev/null 2>&1 &
        fi
        disown 2>/dev/null || true
        log "fallback: 直接启动二进制 ${bin}"
        return 0
    done

    return 1
}

main() {
    if [[ $# -eq 0 ]]; then
        # 直接双击 app 本身：开一个空的 mpv 待机窗口。
        launch_one || fail "无参数启动失败"
        log "OK 空窗口"
        return 0
    fi

    local file failed=0
    for file in "$@"; do
        if launch_one "${file}"; then
            log "OK ${file}"
        else
            failed=1
            log "FAIL ${file}"
        fi
    done

    [[ ${failed} -eq 0 ]] || fail "有文件未能打开"
}

main "$@"
