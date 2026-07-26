#pragma once

#include <mpv/client.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// mpv 没有原生弹窗。插件弹出一个"必须回答"的确认时，光靠抢占
// confirm-yes/confirm-no 两个 binding 名字并不能阻止用户在看清提示之前误按
// 别的键做出其他编辑操作。这里用 `define-section` +
// `enable-section ... exclusive` 把正常输入整个遮住，只放行这两个 binding
// 实际绑定的物理按键——这正是 mpv 自己的 Lua `mp.add_forced_key_binding` /
// console.lua 实现"独占键盘"的同一套机制（`player/lua/defaults.lua` 的
// `mp.input_define_section` / `mp.input_enable_section` 只是这几个 command
// 的薄包装），C 插件没有那层封装，直接调 `mpv_command` 效果完全一样。
//
// enhanced-ab-loop 和 split-zoom-box 的读档确认都需要这套机制，按 AGENTS.md
// 里"两个插件确实需要同一实现时才提取"的约定挪到 shared/。
namespace mpv_util {

namespace detail {

inline std::string_view trim(std::string_view text) {
    constexpr const char *kSpaces = " \t\r\n";
    std::size_t begin = text.find_first_not_of(kSpaces);
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(kSpaces) - begin + 1);
}

} // namespace detail

// input.conf 允许一行里用 `;` 串接多条命令（`y script-binding a ;
// script-binding b`，同一个键要同时喂给两个插件时只能这么写——同 section 内
// 一个键写两行的话后写的会覆盖先写的）。`input-bindings` 属性的 cmd 字段报
// 的是整行原文，所以判断某个 script-binding 在不在里面必须先按 `;` 拆开逐
// 条比对：整串全等会漏掉串接写法，而子串匹配又会把
// `script-binding foo/confirm-yes-extra` 这种前缀重名的绑定误判成命中。
//
// 不处理 mpv 的引号转义（`show-text "a;b"` 会被拆坏）：拆坏的片段不会等于
// 要找的 `script-binding <名字>`，最坏情况是某条带引号的命令里恰好写了一模
// 一样的字样而被多算一个键，后果只是那个键也能回答确认，不会漏键。
inline bool command_chain_contains(std::string_view cmd, std::string_view needle) {
    std::size_t pos = 0;
    for (;;) {
        std::size_t separator = cmd.find(';', pos);
        std::string_view part =
            separator == std::string_view::npos ? cmd.substr(pos) : cmd.substr(pos, separator - pos);
        if (detail::trim(part) == needle) {
            return true;
        }
        if (separator == std::string_view::npos) {
            return false;
        }
        pos = separator + 1;
    }
}

// 反查绑定到 `script-binding <binding_name>` 的所有物理按键。不硬编码
// "y"/"n"：物理按键是用户在自己 input.conf 里配的，插件这边只知道 binding
// 名字。返回多个键是正常的——同一个动作允许绑多个键（比如 ab-loop 的 set-a
// 同时绑了 `[` 和 `【`）。
inline std::vector<std::string> keys_bound_to_script_binding(mpv_handle *handle, const std::string &binding_name) {
    std::vector<std::string> keys;
    std::string needle = "script-binding " + binding_name;

    mpv_node node;
    if (mpv_get_property(handle, "input-bindings", MPV_FORMAT_NODE, &node) < 0) {
        return keys;
    }
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const mpv_node &item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP) {
                continue;
            }
            const mpv_node_list &map = *item.u.list;
            const char *key = nullptr;
            const char *cmd = nullptr;
            for (int j = 0; j < map.num; ++j) {
                if (std::strcmp(map.keys[j], "key") == 0 && map.values[j].format == MPV_FORMAT_STRING) {
                    key = map.values[j].u.string;
                } else if (std::strcmp(map.keys[j], "cmd") == 0 && map.values[j].format == MPV_FORMAT_STRING) {
                    cmd = map.values[j].u.string;
                }
            }
            if (key && cmd && command_chain_contains(cmd, needle)) {
                keys.emplace_back(key);
            }
        }
    }
    mpv_free_node_contents(&node);
    return keys;
}

// 返回 false 表示当前找不到任何绑定到 yes/no 的物理按键（用户把这两个
// binding 从 input.conf 里删掉了）——这种情况下绝不能真的启用独占区段，否
// 则会把用户锁死在一个连 y/n 都按不出来的暂停画面里，没有任何办法退出确认
// 状态。宁可退化成"不锁键"，也不能造成死锁。调用方可以据此决定 OSD 上要不
// 要写"其他键已禁用"。
inline bool engage_confirm_lockout(mpv_handle *handle, const char *section_name, const std::string &yes_binding,
                                   const std::string &no_binding) {
    std::vector<std::string> yes_keys = keys_bound_to_script_binding(handle, yes_binding);
    std::vector<std::string> no_keys = keys_bound_to_script_binding(handle, no_binding);
    if (yes_keys.empty() || no_keys.empty()) {
        return false;
    }

    // 区段里只重发本插件那半截命令：物理键在 input.conf 里可能是
    // `y script-binding a/confirm-yes ; script-binding b/confirm-yes` 这种串
    // 接写法，锁键期间不该把另一个插件的确认也一起触发（它本来就没在等，
    // 但没必要把这件事赌在对方的空转分支上）。
    std::string contents;
    for (const auto &key : yes_keys) {
        contents += key + " script-binding " + yes_binding + "\n";
    }
    for (const auto &key : no_keys) {
        contents += key + " script-binding " + no_binding + "\n";
    }
    // "unmapped" 是 input.conf 的特殊键名，匹配区段里没有单独绑定的任何键
    // （含鼠标/滚轮），`ignore` 是 mpv 自带的"吃掉这个按键，什么都不做"。
    contents += "unmapped ignore\n";

    const char *define_args[] = {"define-section", section_name, contents.c_str(), "force", nullptr};
    mpv_command(handle, define_args);
    const char *enable_args[] = {"enable-section", section_name, "exclusive", nullptr};
    mpv_command(handle, enable_args);
    return true;
}

inline void release_confirm_lockout(mpv_handle *handle, const char *section_name) {
    const char *args[] = {"disable-section", section_name, nullptr};
    mpv_command(handle, args);
}

} // namespace mpv_util
