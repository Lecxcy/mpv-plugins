#include <mpv/client.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/core.h>

#include "enhanced_ab_loop/logic.h"
#include "enhanced_ab_loop/store.h"
#include "shared/cpp/mpv_util.h"

namespace {

using namespace enhanced_ab_loop;

// 重写后合并了原 tail-frame-extension 插件的配置，不再拆两份。
constexpr double kNudgeStep = 0.05;
constexpr double kStateOsdDuration = 1.8;
constexpr double kToggleOsdDuration = 1.8;
constexpr double kTailFreezeSeconds = 0.3;
// show-text 没有"一直显示直到手动清除"，只有到期时长；用一个大到不会自然
// 到期的时长（24h）模拟"常驻直到用户回答"，期间按键都被 confirm-only 区
// 段吞掉，不会有别的 OSD 把它顶掉，不需要额外的定时器去重发。
constexpr double kConfirmOsdDurationSeconds = 24.0 * 3600.0;
constexpr const char *kVideoFilterLabel = "enhanced-ab-loop-tail-video";
constexpr const char *kAudioFilterLabel = "enhanced-ab-loop-tail-audio";
constexpr const char *kConfirmSectionName = "enhanced_ab_loop_confirm";
constexpr std::size_t kContentSampleBytes = 65536;

struct PluginState {
    mpv_handle *handle = nullptr;

    std::vector<Segment> segments;
    Pending pending;
    bool loop_enabled = true;

    // 原生 ab-loop 只能在"本段自己的 a/b"之间自环，跳到下一段靠自己监测到
    // 这个自环命中了本段的 a，再显式补一次 seek（见 refresh_loop /
    // on_native_landing）。armed_index 记录当前武装的是 active_order 里
    // 哪一项；编辑类操作每次都经 refresh_loop 无条件刷新它，不带跳转判断。
    std::optional<std::size_t> armed_index;
    // true 表示上一次的 seek 是自己为了跨段跳转发出去的，还没等到落地的
    // SEEK/PLAYBACK_RESTART 事件确认，这期间不能把"落在 armed_index 自己
    // 的 a 上"误判成又一次原生自环完成。
    bool pending_self_seek = false;

    std::string current_path;     // 只留在内存里读文件、算哈希，不写盘
    std::string current_path_key; // current_path 的哈希，写进存档文件的是这个
    std::string current_content_hash;

    // 这次会话是否是靠"唯一候选"借用了别的路径下的存档；非空时下次保存要
    // 把那条 entry 迁移到当前路径，而不是新增一条，避免孤儿累积。
    std::optional<std::string> rename_from;
    std::optional<store::LookupResult> pending_confirmation;
    // pending_confirmation 非空期间：进入确认前的 pause 状态，回答完（无论
    // y/n）要还原回这个值，不能无条件恢复播放。
    bool paused_before_confirm = false;
};

double time_pos(mpv_handle *h) {
    return mpv_util::get_double(h, "time-pos", 0.0);
}

double file_duration(mpv_handle *h) {
    return mpv_util::get_double(h, "duration", 0.0);
}

void run_command(mpv_handle *h, std::initializer_list<const char *> args) {
    std::vector<const char *> argv(args);
    argv.push_back(nullptr);
    mpv_command(h, argv.data());
}

void clear_ab_loop_point(mpv_handle *h, const char *name) {
    mpv_set_property_string(h, name, "no");
}

void write_ab_loop_point(mpv_handle *h, const char *name, std::optional<double> value) {
    if (value) {
        mpv_util::set_double(h, name, *value);
    } else {
        clear_ab_loop_point(h, name);
    }
}

std::string expand_path(mpv_handle *h, const std::string &path) {
    const char *args[] = {"expand-path", path.c_str(), nullptr};
    mpv_node result;
    if (mpv_command_ret(h, args, &result) < 0) {
        return "";
    }
    std::string out;
    if (result.format == MPV_FORMAT_STRING && result.u.string) {
        out = result.u.string;
    }
    mpv_free_node_contents(&result);
    return out;
}

bool has_track_of_type(mpv_handle *h, const char *type) {
    mpv_node node;
    if (mpv_get_property(h, "track-list", MPV_FORMAT_NODE, &node) < 0) {
        return false;
    }
    bool found = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num && !found; ++i) {
            const mpv_node &item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP) {
                continue;
            }
            const mpv_node_list &map = *item.u.list;
            const char *track_type = nullptr;
            bool selected = false;
            for (int j = 0; j < map.num; ++j) {
                if (std::strcmp(map.keys[j], "type") == 0 && map.values[j].format == MPV_FORMAT_STRING) {
                    track_type = map.values[j].u.string;
                } else if (std::strcmp(map.keys[j], "selected") == 0 &&
                           map.values[j].format == MPV_FORMAT_FLAG) {
                    selected = map.values[j].u.flag != 0;
                }
            }
            if (selected && track_type && std::strcmp(track_type, type) == 0) {
                found = true;
            }
        }
    }
    mpv_free_node_contents(&node);
    return found;
}

// mpv 的 vf/af 属性里每个 filter 的 label 字段不带 "@" 前缀；对不存在的
// label 调 "remove" 会打印 "item label ... not found" 警告，所以先查一遍再决定要
// 不要 remove。
bool has_filter_label(mpv_handle *h, const char *chain, const char *label) {
    mpv_node node;
    if (mpv_get_property(h, chain, MPV_FORMAT_NODE, &node) < 0) {
        return false;
    }
    bool found = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num && !found; ++i) {
            const mpv_node &item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP) {
                continue;
            }
            const mpv_node_list &map = *item.u.list;
            for (int j = 0; j < map.num; ++j) {
                if (std::strcmp(map.keys[j], "label") == 0 &&
                    map.values[j].format == MPV_FORMAT_STRING &&
                    map.values[j].u.string && std::strcmp(map.values[j].u.string, label) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    mpv_free_node_contents(&node);
    return found;
}

void remove_filter_if_present(PluginState &state, const char *chain, const char *label) {
    if (has_filter_label(state.handle, chain, label)) {
        run_command(state.handle, {chain, "remove", (std::string("@") + label).c_str()});
    }
}

void clear_tail_extension(PluginState &state) {
    remove_filter_if_present(state, "vf", kVideoFilterLabel);
    remove_filter_if_present(state, "af", kAudioFilterLabel);
}

void apply_tail_extension(PluginState &state) {
    clear_tail_extension(state);

    if (kTailFreezeSeconds <= 0.0 || !has_track_of_type(state.handle, "video")) {
        return;
    }

    std::string duration_str = fmt::format("{:.6f}", kTailFreezeSeconds);
    std::string vf_spec =
        fmt::format("@{}:lavfi=[tpad=stop_mode=clone:stop_duration={}]", kVideoFilterLabel, duration_str);
    run_command(state.handle, {"vf", "add", vf_spec.c_str()});

    if (has_track_of_type(state.handle, "audio")) {
        std::string af_spec = fmt::format("@{}:lavfi=[apad=pad_dur={}]", kAudioFilterLabel, duration_str);
        run_command(state.handle, {"af", "add", af_spec.c_str()});
    }
}

// 光标落在空隙里（不在任何激活区间内部）时，loop_enabled 为 true 也不能
// 指望"接着往下播、碰巧自然落回循环范围"：mpv 的 update_ab_loop_clip()
// （playloop.c:665）只要发现 pos 已经超过目标终点就会直接禁用 clip，继续
// 播放只会一路播到文件真正结束（配合 loop-file=inf 就表现成"从头重播"，
// 不是回到定义的循环里）；就算没超过终点，落在空隙里也是"该进的段还没
// 进"，不如显式 seek 过去让循环立刻生效。返回 true 表示已经发出了这次
// seek（落地事件回来经 on_native_landing 确认即可）；返回 false 表示光标
// 本来就在某个激活区间内部，不需要动。
bool try_reengage_seek(PluginState &state, const std::vector<ActiveEntry> &order, double pos) {
    if (!state.loop_enabled) {
        return false;
    }
    auto target = loop_reengage_target(order, pos);
    if (!target) {
        return false;
    }
    // 这次 seek 是我们自己发的，标记一下避免落地时被 on_native_landing 的
    // 完成判定误伤。
    state.pending_self_seek = true;
    std::string target_str = fmt::format("{:.6f}", *target);
    run_command(state.handle, {"seek", target_str.c_str(), "absolute", "exact"});
    return true;
}

// 无条件按"当前光标落在 active_order 的哪一项"重新预置 ab-loop-a/b，不做
// 任何跨段跳转判断——判断"是不是该跳到下一项了"是 on_native_landing 专门
// 的职责，两者不能混在一起，否则编辑操作里凑巧把光标停在某项自己的 a 上
// （比如 D1 测的那种边界落点）会被误判成"这一项播完了，跳下一项"。
void refresh_loop(PluginState &state) {
    double pos = time_pos(state.handle);
    auto order = build_active_order(state.segments, state.pending);

    MPV_UTIL_DEBUG("refresh_loop: pos={:.3f} loop_enabled={} order=[", pos, state.loop_enabled);
    for (const auto &e : order) {
        MPV_UTIL_DEBUG("  ({:.3f},{})", e.a, e.b ? fmt::format("{:.3f}", *e.b) : std::string("open"));
    }
    MPV_UTIL_DEBUG("]\n");

    state.pending_self_seek = false;

    if (!state.loop_enabled || order.empty()) {
        MPV_UTIL_DEBUG("refresh_loop: clearing (loop_enabled={} order.empty={})\n", state.loop_enabled,
                        order.empty());
        state.armed_index.reset();
        clear_ab_loop_point(state.handle, "ab-loop-a");
        clear_ab_loop_point(state.handle, "ab-loop-b");
        return;
    }

    auto idx = locate_active_index(order, pos);
    if (!idx) {
        MPV_UTIL_DEBUG("refresh_loop: clearing (no idx located)\n");
        state.armed_index.reset();
        clear_ab_loop_point(state.handle, "ab-loop-a");
        clear_ab_loop_point(state.handle, "ab-loop-b");
        return;
    }

    auto pair = compute_jump_pair(order, *idx, file_duration(state.handle), kTailFreezeSeconds);
    MPV_UTIL_DEBUG("refresh_loop: idx={} -> ab-loop-a={} ab-loop-b={}\n", *idx,
                    pair.ab_loop_a ? fmt::format("{:.3f}", *pair.ab_loop_a) : std::string("unset"),
                    pair.ab_loop_b ? fmt::format("{:.3f}", *pair.ab_loop_b) : std::string("unset"));
    write_ab_loop_point(state.handle, "ab-loop-a", pair.ab_loop_a);
    write_ab_loop_point(state.handle, "ab-loop-b", pair.ab_loop_b);
    state.armed_index = idx;
}

// 只从 MPV_EVENT_SEEK / MPV_EVENT_PLAYBACK_RESTART 调用：判断"原生 ab-loop
// 是不是刚自环命中了 armed_index 自己的 a"，是的话说明这一项已经完整播完
// 一轮，显式补一次 seek 跳到 active_order 里下一项的起点。三个条件缺一都
// 不能判定为"播完了"：
//   1. pending_self_seek 必须是 false——不能把自己刚发出去的跨段 seek 的
//      回执，当成一次新的"播完"再跳一次。
//   2. 落地的 idx 必须等于 armed_index，且 pos 必须精确落在那一项自己的
//      a 上——原生自环的跳转目标只可能是这个值，落在段内其他位置（比如
//      D 节测 nudge 时手动 seek 到区间中间）不算数。
//   3. 这一项必须有真正的 b（开放到文件末尾的 pending 交给 §2.2 的
//      eof-reached 兜底）。
// next == idx（active_order 只有一项，自己套自己）时不发 seek，直接落到
// refresh_loop 重新武装同一项，省掉一次没必要的 seek 往返。
//
// event_id 用来区分 SEEK 和 PLAYBACK_RESTART：自己主动发出去的显式 seek，
// mpv 恒定会发两条通知——SEEK（排队）和 PLAYBACK_RESTART（真正落地完
// 成）；原生 ab-loop 自环跳转只发一条 SEEK，没有 PLAYBACK_RESTART（它不
// 走完整的"重新开始播放"流程，只是 demux 挪了一下）。pending_self_seek 必
// 须等到两条都收到才能清掉，中途收到的 SEEK 先按兵不动——原来的写法收到
// 第一条就直接清掉标记，等第二条（同一次 seek 的回声）到达时，标记已经
// 是 false，而暂停状态下两条事件之间光标位置分毫不差，会被误判成"原生自
// 环真的又播完一轮"，触发又一次补跳，循环往复：2026-07-19 debug 会话实测
// 复现过，暂停时设置区间边界、重开循环，2 秒内触发了两万多次 seek 的死循
// 环。播放中不会露馅，是因为两条事件之间总有真实播放时间流逝，光标已经
// 往前挪了一点，恰好躲过"精确等于起点"这个判定。
void on_native_landing(PluginState &state, mpv_event_id event_id) {
    if (state.pending_self_seek) {
        if (event_id == MPV_EVENT_PLAYBACK_RESTART) {
            // 我们自己发出去的 seek 真正落地了，确认武装，清掉标记。
            state.pending_self_seek = false;
            refresh_loop(state);
        }
        // event_id == MPV_EVENT_SEEK：只是排队通知，还没真正落地，先不动，
        // 等 PLAYBACK_RESTART。
        return;
    }

    double pos = time_pos(state.handle);
    auto order = build_active_order(state.segments, state.pending);

    bool is_completion = false;
    if (state.loop_enabled && !order.empty() && state.armed_index) {
        auto idx = locate_active_index(order, pos);
        is_completion = idx && *idx == *state.armed_index && order[*idx].b.has_value() &&
                        std::abs(pos - order[*idx].a) <= kCursorEpsilon;
        if (is_completion) {
            std::size_t next = (*idx + 1) % order.size();
            if (next != *idx) {
                state.pending_self_seek = true;
                std::string target = fmt::format("{:.6f}", order[next].a);
                MPV_UTIL_DEBUG("on_native_landing: idx={} finished, redirecting to next={} a={}\n", *idx, next,
                                target);
                run_command(state.handle, {"seek", target.c_str(), "absolute", "exact"});
                return;
            }
        }
    }

    // 不是"这一段刚播完"，那就检查这次落地是不是把光标扔进了空隙里（比如
    // 用户手动拖进度条，不是走 set-a/set-b 之类的编辑操作），见
    // try_reengage_seek 的注释。
    if (try_reengage_seek(state, order, pos)) {
        return;
    }

    refresh_loop(state);
}

// SPEC §2.2：eof-reached 兜底，跳 active_order 最后一项自己的 a（不是硬编码
// 第一项）。
void on_eof_reached(PluginState &state, bool reached) {
    if (!reached || !state.loop_enabled) {
        return;
    }
    auto order = build_active_order(state.segments, state.pending);
    auto target = eof_fallback_target(order);
    if (!target) {
        return;
    }
    // 这次也是我们自己发出去的 seek，标记一下避免落地时被
    // on_native_landing 的完成判定误伤（见该函数注释）。
    state.pending_self_seek = true;
    std::string target_str = fmt::format("{:.6f}", *target);
    run_command(state.handle, {"seek", target_str.c_str(), "absolute", "exact"});
}

std::string format_clock(double seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    long total = static_cast<long>(seconds + 0.5);
    long hours = total / 3600;
    long minutes = (total % 3600) / 60;
    long secs = total % 60;
    if (hours > 0) {
        return fmt::format("{}:{:02}:{:02}", hours, minutes, secs);
    }
    return fmt::format("{:02}:{:02}", minutes, secs);
}

std::string format_precise(double seconds) {
    double h = std::floor(seconds / 3600.0);
    double remainder = std::fmod(seconds, 3600.0);
    double m = std::floor(remainder / 60.0);
    double s = std::fmod(remainder, 60.0);
    if (h > 0) {
        return fmt::format("{}:{:02}:{:06.3f}", static_cast<long>(h), static_cast<long>(m), s);
    }
    return fmt::format("{:02}:{:06.3f}", static_cast<long>(m), s);
}

void show_state(PluginState &state, const std::string &prefix) {
    std::ostringstream text;
    text << prefix << " | " << format_clock(time_pos(state.handle)) << "/"
         << format_clock(file_duration(state.handle));
    // §3.4：pending 的两个端点单独放在首行展示（"A:.. B:.."），不再拼成
    // 一行独立的 "[a,unset]"/"[unset,b]" 混在完整区间列表后面——那样看着
    // 像是被塞进了同一个列表、只是没排好序，容易被误读成 bug。
    text << " | A:" << (state.pending.has_a ? format_precise(state.pending.a) : "--")
         << " B:" << (state.pending.has_b ? format_precise(state.pending.b) : "--");

    auto append_segment = [&text](const Segment &seg) {
        text << "\n" << (seg.enabled ? "" : "(off) ") << "[" << format_precise(seg.a) << ","
             << format_precise(seg.b) << "]";
    };

    std::size_t total = state.segments.size();
    auto plan = plan_segment_display(total);
    for (std::size_t i = 0; i < plan.head_count; ++i) {
        append_segment(state.segments[i]);
    }
    if (plan.hidden_count > 0) {
        text << "\n... (" << plan.hidden_count << " more)";
        for (std::size_t i = total - plan.tail_count; i < total; ++i) {
            append_segment(state.segments[i]);
        }
    }

    MPV_UTIL_DEBUG("show_state: {}\n", text.str());
    mpv_util::show_osd_message(state.handle, text.str(), kStateOsdDuration);
}

void on_set_a(PluginState &state) {
    auto outcome = set_a(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_set_b(PluginState &state) {
    auto outcome = set_b(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_unset_a(PluginState &state) {
    auto outcome = unset_a(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        // 撤销掉的区间如果正好是光标当前所在的这一段，撤销后可能落进一个
        // 不再被任何激活项覆盖的"孤岛"（比如这一段彻底消失、旁边又没有
        // 别的区间挨着），见 try_reengage_seek 的注释。
        auto order = build_active_order(state.segments, state.pending);
        if (!try_reengage_seek(state, order, time_pos(state.handle))) {
            refresh_loop(state);
        }
    }
    show_state(state, outcome.message);
}

void on_unset_b(PluginState &state) {
    auto outcome = unset_b(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        // 同 on_unset_a。
        auto order = build_active_order(state.segments, state.pending);
        if (!try_reengage_seek(state, order, time_pos(state.handle))) {
            refresh_loop(state);
        }
    }
    show_state(state, outcome.message);
}

void on_extend_prev(PluginState &state) {
    auto outcome = extend_prev(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_extend_next(PluginState &state) {
    auto outcome = extend_next(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_nudge_a(PluginState &state, double delta) {
    auto outcome = nudge_a(state.segments, time_pos(state.handle), delta);
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_nudge_b(PluginState &state, double delta) {
    auto outcome = nudge_b(state.segments, time_pos(state.handle), delta);
    if (outcome.ok()) {
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_toggle_segment(PluginState &state) {
    auto outcome = toggle_segment(state.segments, time_pos(state.handle));
    if (outcome.ok()) {
        // 禁用光标所在的这一段会把它从 active_order 里摘掉，同 on_unset_a。
        auto order = build_active_order(state.segments, state.pending);
        if (!try_reengage_seek(state, order, time_pos(state.handle))) {
            refresh_loop(state);
        }
    }
    show_state(state, outcome.message);
}

void on_toggle_enabled(PluginState &state) {
    state.loop_enabled = !state.loop_enabled;

    auto order = build_active_order(state.segments, state.pending);
    if (!try_reengage_seek(state, order, time_pos(state.handle))) {
        refresh_loop(state);
    }

    mpv_util::show_osd_message(state.handle, state.loop_enabled ? "loop on" : "loop off", kToggleOsdDuration);
}

struct ContentSample {
    std::uint64_t size = 0;
    std::string head;
    std::string tail;
};

std::optional<ContentSample> read_content_sample(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    ContentSample sample;
    sample.size = static_cast<std::uint64_t>(size);

    store::SampleRanges ranges = store::compute_sample_ranges(sample.size, kContentSampleBytes);

    sample.head.resize(ranges.head_len);
    file.seekg(0, std::ios::beg);
    file.read(sample.head.data(), static_cast<std::streamsize>(ranges.head_len));

    sample.tail.resize(ranges.tail_len);
    file.seekg(static_cast<std::streamoff>(ranges.tail_offset), std::ios::beg);
    file.read(sample.tail.data(), static_cast<std::streamsize>(ranges.tail_len));

    return sample;
}

std::optional<std::string> read_file_text(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool write_file_text(const std::string &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << text;
    return static_cast<bool>(file);
}

std::string archive_path_for_hash(mpv_handle *h, const std::string &hash) {
    // 用 ~~home/ 而不是 ~~/：后者语义是"子路径已存在时返回已存在的那个
    // 目录"，是给读取用的；这里要写入，需要明确指向 mpv 配置目录。
    std::string dir = expand_path(h, "~~home/loop-segments");
    if (dir.empty()) {
        return "";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/" + hash + ".json";
}

void apply_loaded_segments(PluginState &state, std::vector<Segment> segments) {
    sort_segments(segments);
    state.segments = std::move(segments);
    state.pending.clear();

    // 存档加载进来的区间跟光标当前位置大概率对不上，同 on_unset_a。
    auto order = build_active_order(state.segments, state.pending);
    if (!try_reengage_seek(state, order, time_pos(state.handle))) {
        refresh_loop(state);
    }
}

// mpv 没有原生弹窗，光靠"抢占 confirm-yes/confirm-no 两个 binding 名字"并
// 不能阻止用户在看清提示之前误按别的键做出其他编辑操作。这里用
// `define-section` + `enable-section ... exclusive` 把正常输入完全遮住，
// 只放行 confirm-yes/confirm-no 实际绑定的物理按键——这正是 mpv 自己的
// Lua `mp.add_forced_key_binding`/console.lua 实现"独占键盘"的同一套机制
// （`player/lua/defaults.lua` 的 `mp.input_define_section`/
// `mp.input_enable_section` 只是这几个 command 的薄包装），C 插件没有那层
// 封装，直接调 `mpv_command` 效果完全一样。
//
// 不硬编码 "y"/"n"：物理按键是用户在自己 input.conf 里配的，插件这边只知
// 道 binding 名字。改用 `input-bindings` 属性反查实际绑定的物理键，跟用户
// 自定义按键保持一致，也顺带覆盖了重复键位（比如 set-a 同时绑了 `[` 和
// `【`，confirm-yes/no 理论上也可能这样重复绑定）。
std::vector<std::string> keys_bound_to_script_binding(mpv_handle *h, const std::string &binding_name) {
    std::vector<std::string> keys;
    std::string needle = "script-binding " + binding_name;

    mpv_node node;
    if (mpv_get_property(h, "input-bindings", MPV_FORMAT_NODE, &node) < 0) {
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
            if (key && cmd && needle == cmd) {
                keys.emplace_back(key);
            }
        }
    }
    mpv_free_node_contents(&node);
    return keys;
}

// 返回 false 表示当前找不到任何绑定到 confirm-yes/confirm-no 的物理按键
// （用户把这两个 binding 从 input.conf 里删掉了）——这种情况下绝不能真的
// 启用独占区段，否则会把用户锁死在一个连 y/n 都按不出来的暂停画面里，没
// 有任何办法退出确认状态。宁可退化成"不锁键"，也不能造成死锁。
bool engage_confirm_lockout(PluginState &state) {
    std::vector<std::string> yes_keys = keys_bound_to_script_binding(state.handle, "enhanced_ab_loop/confirm-yes");
    std::vector<std::string> no_keys = keys_bound_to_script_binding(state.handle, "enhanced_ab_loop/confirm-no");
    if (yes_keys.empty() || no_keys.empty()) {
        return false;
    }

    std::string contents;
    for (const auto &key : yes_keys) {
        contents += key + " script-binding enhanced_ab_loop/confirm-yes\n";
    }
    for (const auto &key : no_keys) {
        contents += key + " script-binding enhanced_ab_loop/confirm-no\n";
    }
    // "unmapped" 是 input.conf 的特殊键名，匹配区段里没有单独绑定的任何键
    // （含鼠标/滚轮），`ignore` 是 mpv 自带的"吃掉这个按键，什么都不做"。
    contents += "unmapped ignore\n";

    run_command(state.handle, {"define-section", kConfirmSectionName, contents.c_str(), "force"});
    run_command(state.handle, {"enable-section", kConfirmSectionName, "exclusive"});
    return true;
}

void release_confirm_lockout(PluginState &state) {
    run_command(state.handle, {"disable-section", kConfirmSectionName});
}

void on_save_loops(PluginState &state) {
    if (state.current_path.empty() || state.current_content_hash.empty()) {
        mpv_util::show_osd_message(state.handle, "Save denied: no file", kStateOsdDuration);
        return;
    }

    std::string archive_path = archive_path_for_hash(state.handle, state.current_content_hash);
    if (archive_path.empty()) {
        mpv_util::show_osd_message(state.handle, "Save failed: bad path", kStateOsdDuration);
        return;
    }

    store::Archive archive;
    if (auto text = read_file_text(archive_path)) {
        archive = store::deserialize_archive(*text);
    }

    store::upsert(archive, state.current_path_key, state.segments, state.rename_from);
    state.rename_from.reset();

    if (write_file_text(archive_path, store::serialize_archive(archive))) {
        mpv_util::show_osd_message(state.handle, "Loops saved", kStateOsdDuration);
    } else {
        mpv_util::show_osd_message(state.handle, "Save failed: write error", kStateOsdDuration);
    }
}

void on_load_loops(PluginState &state) {
    if (state.current_path.empty() || state.current_content_hash.empty()) {
        mpv_util::show_osd_message(state.handle, "Load denied: no file", kStateOsdDuration);
        return;
    }

    std::string archive_path = archive_path_for_hash(state.handle, state.current_content_hash);
    store::Archive archive;
    if (auto text = read_file_text(archive_path)) {
        archive = store::deserialize_archive(*text);
    }

    auto result = store::lookup(archive, state.current_path_key);
    switch (result.kind) {
    case store::LookupResult::Kind::kExactMatch:
        apply_loaded_segments(state, result.segments);
        mpv_util::show_osd_message(state.handle, "Loops loaded", kStateOsdDuration);
        break;
    case store::LookupResult::Kind::kSingleCandidate: {
        // 存档里只有路径的哈希，插件这边压根拿不到那条旧路径的明文，提示
        // 文案不带路径，不是能带但故意藏起来。
        state.pending_confirmation = result;
        state.paused_before_confirm = mpv_util::get_flag(state.handle, "pause", false);
        mpv_util::set_flag(state.handle, "pause", true);
        bool locked = engage_confirm_lockout(state);

        std::string message =
            "Found an archive with matching content under a different name.\n"
            "Playback paused - confirm-yes to load & migrate, confirm-no to cancel.";
        if (locked) {
            message += "\n(other keys are disabled until you answer)";
        }
        mpv_util::show_osd_message(state.handle, message, kConfirmOsdDurationSeconds);
        break;
    }
    case store::LookupResult::Kind::kNoArchive:
        mpv_util::show_osd_message(state.handle, "No archive found", kStateOsdDuration);
        break;
    }
}

void on_confirm(PluginState &state, bool yes) {
    if (!state.pending_confirmation) {
        return;
    }
    store::LookupResult result = *state.pending_confirmation;
    state.pending_confirmation.reset();

    // 不管答的是 y 还是 n，都要解除独占按键区段、还原 pause 状态。
    release_confirm_lockout(state);
    mpv_util::set_flag(state.handle, "pause", state.paused_before_confirm);

    if (!yes) {
        mpv_util::show_osd_message(state.handle, "Cancelled", kToggleOsdDuration);
        return;
    }

    apply_loaded_segments(state, result.segments);
    state.rename_from = result.matched_key;
    mpv_util::show_osd_message(state.handle, "Loops loaded (will migrate archive on next save)",
                                kStateOsdDuration);
}

void on_file_loaded(PluginState &state) {
    // 理论上确认锁键期间几乎不可能触发新文件加载，但外部 IPC 仍可能在这
    // 期间发 loadfile/playlist-next；防御性地解除独占区段，避免残留下去
    // 把新文件的正常操作也锁死。
    if (state.pending_confirmation) {
        release_confirm_lockout(state);
    }

    state.segments.clear();
    state.pending.clear();
    state.loop_enabled = true;
    state.rename_from.reset();
    state.pending_confirmation.reset();

    apply_tail_extension(state);

    char *path_cstr = nullptr;
    if (mpv_get_property(state.handle, "path", MPV_FORMAT_STRING, &path_cstr) >= 0 && path_cstr) {
        state.current_path = path_cstr;
        mpv_free(path_cstr);
    } else {
        state.current_path.clear();
    }

    state.current_path_key = state.current_path.empty() ? "" : store::compute_path_hash(state.current_path);

    state.current_content_hash.clear();
    if (!state.current_path.empty()) {
        if (auto sample = read_content_sample(state.current_path)) {
            state.current_content_hash = store::compute_content_hash(sample->size, sample->head, sample->tail);
        }
    }

    refresh_loop(state);
}

void on_end_file(PluginState &state) {
    clear_tail_extension(state);
    clear_ab_loop_point(state.handle, "ab-loop-a");
    clear_ab_loop_point(state.handle, "ab-loop-b");
}

// C 插件没有 mp.add_key_binding 那样的封装，参照 enhanced-drag 的写法，直
// 接接住 mpv 转发过来的 "key-binding" client message（input.rst:1399-1439：
// script-binding 命令内部就是这样转发的，不需要提前注册）。
void handle_client_message(PluginState &state, mpv_event_client_message *message) {
    if (message->num_args < 3 || std::strcmp(message->args[0], "key-binding") != 0) {
        return;
    }
    // 状态第一个字符：d=按下 u=松开 r=连发中 p=按下抬起分不清时的单次触发。
    // 这里只在按下/单次触发时响应一次，忽略松开/连发。
    char phase = message->args[2][0];
    if (phase != 'd' && phase != 'p') {
        return;
    }

    std::string binding = message->args[1];
    if (binding == "set-a") {
        on_set_a(state);
    } else if (binding == "set-b") {
        on_set_b(state);
    } else if (binding == "unset-a") {
        on_unset_a(state);
    } else if (binding == "unset-b") {
        on_unset_b(state);
    } else if (binding == "extend-prev") {
        on_extend_prev(state);
    } else if (binding == "extend-next") {
        on_extend_next(state);
    } else if (binding == "nudge-a-back") {
        on_nudge_a(state, -kNudgeStep);
    } else if (binding == "nudge-a-forward") {
        on_nudge_a(state, kNudgeStep);
    } else if (binding == "nudge-b-back") {
        on_nudge_b(state, -kNudgeStep);
    } else if (binding == "nudge-b-forward") {
        on_nudge_b(state, kNudgeStep);
    } else if (binding == "toggle-segment") {
        on_toggle_segment(state);
    } else if (binding == "toggle-enabled") {
        on_toggle_enabled(state);
    } else if (binding == "show-state") {
        show_state(state, "State");
    } else if (binding == "save-loops") {
        on_save_loops(state);
    } else if (binding == "load-loops") {
        on_load_loops(state);
    } else if (binding == "confirm-yes") {
        on_confirm(state, true);
    } else if (binding == "confirm-no") {
        on_confirm(state, false);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    PluginState state;
    state.handle = handle;

    mpv_observe_property(handle, 0, "eof-reached", MPV_FORMAT_FLAG);

    while (true) {
        mpv_event *event = mpv_wait_event(handle, -1);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }

        switch (event->event_id) {
        case MPV_EVENT_CLIENT_MESSAGE:
            handle_client_message(state, static_cast<mpv_event_client_message *>(event->data));
            break;
        case MPV_EVENT_FILE_LOADED:
            on_file_loaded(state);
            break;
        case MPV_EVENT_END_FILE:
            on_end_file(state);
            break;
        case MPV_EVENT_SEEK:
        case MPV_EVENT_PLAYBACK_RESTART:
            // 两个事件都可能触发，重复调用是幂等的，不需要去重（见
            // on_native_landing 的注释）。
            MPV_UTIL_DEBUG("event: {} pos={:.3f}\n",
                            event->event_id == MPV_EVENT_SEEK ? "SEEK" : "PLAYBACK_RESTART",
                            time_pos(state.handle));
            on_native_landing(state, event->event_id);
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            if (prop->name && std::strcmp(prop->name, "eof-reached") == 0 && prop->format == MPV_FORMAT_FLAG &&
                prop->data) {
                on_eof_reached(state, *static_cast<int *>(prop->data) != 0);
            }
            break;
        }
        default:
            break;
        }
    }
}
