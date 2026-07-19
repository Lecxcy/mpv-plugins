#include <mpv/client.h>

#include <algorithm>
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

// 对应 shared/lua/plugin_config.lua 里 enhanced_ab_loop / tail_frame_extension
// 两段配置，重写后合并进同一个插件，这里就不再拆两份配置文件。
constexpr double kNudgeStep = 0.05;
constexpr double kStateOsdDuration = 1.8;
constexpr double kToggleOsdDuration = 1.8;
constexpr double kTailFreezeSeconds = 0.3;
constexpr const char *kVideoFilterLabel = "enhanced-ab-loop-tail-video";
constexpr const char *kAudioFilterLabel = "enhanced-ab-loop-tail-audio";
// 内容采样哈希只读头尾各 64KiB，不读全文件（SPEC §6：毫秒级完成）。
constexpr std::size_t kContentSampleBytes = 65536;

struct PluginState {
    mpv_handle *handle = nullptr;

    std::vector<Segment> segments;
    Pending pending;
    bool loop_enabled = true;

    // ---- §2.1 跨段跳转的状态机（见 refresh_loop / on_native_landing 的
    // 注释）：原生 ab-loop 只能在“本段自己的 a/b”之间自环，跳到下一段靠
    // 我们自己监测到这个自环命中了本段的 a，再显式补一次 seek。----
    // armed_index：当前把 ab-loop-a/b 设成了 active_order 里哪一项自己的
    // a/b（编辑类操作——set-a/set-b/nudge/extend/toggle 等——每次都会经
    // refresh_loop 无条件刷新这个值，不带跨段跳转判断，避免误触发）。
    std::optional<std::size_t> armed_index;
    // pending_self_seek：true 表示上一次的 seek 是我们自己为了跨段跳转发
    // 出去的，还没等到它落地的 SEEK/PLAYBACK_RESTART 事件确认。这期间不能
    // 把“落在 armed_index 自己的 a 上”误判成又一次原生自环完成。
    bool pending_self_seek = false;

    std::string current_path;      // 真实绝对路径，只留在内存里，不写盘
    std::string current_path_key;  // current_path 的哈希，写进存档文件的是这个
    std::string current_content_hash;

    // §6：这次会话是否是靠“唯一候选”借用了别的路径下的存档；非空时下次
    // 保存要把那条 entry 迁移到当前路径，而不是新增一条，避免孤儿累积。
    std::optional<std::string> rename_from;
    // §6：等待用户按 Y/N 确认的“唯一候选疑似改名”查找结果。
    std::optional<store::LookupResult> pending_confirmation;
};

// ---- mpv 属性/命令的小工具 ----

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

// ---- SPEC §4.2：尾帧冻结滤镜，直接搬 tail-frame-extension.lua 的做法 ----

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

// ---- SPEC §2.1：核心跳转机制 ----

// 无条件按“当前光标落在 active_order 的哪一项”重新预置 ab-loop-a/b 为
// 那一项自己的 a/b（恒 a<b，见 logic.cpp compute_jump_pair 的注释）。所有
// 编辑类操作（set-a/set-b/nudge/extend/unset/toggle-segment/
// toggle-enabled/文件加载）都走这条路径，不做任何跨段跳转判断——这只是
// “刷新当前该看住哪一项”，判断“是不是该跳到下一项了”是
// on_native_landing 专门的职责，两者不能混在一起，否则编辑操作里凑巧把
// 光标停在某项自己的 a 上（比如 D1 测的那种边界落点）会被误判成“这一项
// 播完了，跳下一项”。
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

// 只从 MPV_EVENT_SEEK / MPV_EVENT_PLAYBACK_RESTART 调用：判断“原生 ab-loop
// 是不是刚自环命中了 armed_index 自己的 a”，是的话说明这一项已经完整播完
// 一轮，显式补一次 seek 跳到 active_order 里下一项的起点，再交给
// refresh_loop 去预置下一项自己的 a/b。三个条件缺一都不能判定为“播完了”：
//   1. pending_self_seek 必须是 false——如果这次落地本来就是我们自己刚发
//      出去的跨段 seek 的回执，不能又把它当成一次新的“播完”再跳一次。
//   2. 落地的 idx 必须等于 armed_index，且 pos 必须精确落在那一项自己的
//      a 上——原生自环的跳转目标只可能是这个值，落在段内其他位置（比如
//      D 节测 nudge 时手动 seek 到区间中间）不算数，避免把普通编辑操作
//      误判成“这段播完了”。
//   3. 这一项必须有真正的 b（开放到文件末尾的 pending 不适用，交给
//      §2.2 的 eof-reached 兜底）。
// next == idx（active_order 只有一项，自己套自己）时不需要真的发 seek——
// 已经就在正确的位置上，直接落到 refresh_loop 重新武装同一项即可，省掉一
// 次没必要的 seek 往返（F1 单段循环最在意“跳转瞬间不能有卡顿”，多一次
// seek 就是多一次风险）。
void on_native_landing(PluginState &state) {
    double pos = time_pos(state.handle);
    auto order = build_active_order(state.segments, state.pending);

    bool is_completion = false;
    if (!state.pending_self_seek && state.loop_enabled && !order.empty() && state.armed_index) {
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

// ---- OSD 展示 ----

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

    for (const auto &seg : state.segments) {
        text << "\n" << (seg.enabled ? "" : "(off) ") << "[" << format_precise(seg.a) << ","
             << format_precise(seg.b) << "]";
    }
    // SPEC §3.4：pending 独立展示一行，不嫁接到相邻的完整区间上。
    if (state.pending.has_a) {
        text << "\n[" << format_precise(state.pending.a) << ",unset]";
    } else if (state.pending.has_b) {
        text << "\n[unset," << format_precise(state.pending.b) << "]";
    }

    mpv_util::show_osd_message(state.handle, text.str(), kStateOsdDuration);
}

// ---- 按键动作：每个动作调用纯逻辑层，成功就 refresh_loop，最后展示状态 ----

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
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_unset_b(PluginState &state) {
    auto outcome = unset_b(state.segments, state.pending, time_pos(state.handle));
    if (outcome.ok()) {
        refresh_loop(state);
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
        refresh_loop(state);
    }
    show_state(state, outcome.message);
}

void on_toggle_enabled(PluginState &state) {
    state.loop_enabled = !state.loop_enabled;

    if (state.loop_enabled) {
        auto order = build_active_order(state.segments, state.pending);
        auto target = loop_reengage_target(order, time_pos(state.handle));
        if (target) {
            // 光标当前没有落在任何激活区间内部——可能是关闭循环期间播到了
            // 空隙里，也可能已经越过了全部区间——不能指望“接着往下播、碰
            // 巧自然落回循环范围”，mpv 的 update_ab_loop_clip()
            // （playloop.c:665）只要发现 pos 已经超过目标终点就会直接禁用
            // clip，继续播放只会一路播到文件真正结束（配合 loop-file=inf
            // 就表现成“从头重播”，不是回到我们定义的循环里）。显式 seek
            // 过去，让循环立刻生效。这次 seek 是我们自己发的，标记一下避免
            // 落地时被 on_native_landing 的完成判定误伤。
            state.pending_self_seek = true;
            std::string target_str = fmt::format("{:.6f}", *target);
            run_command(state.handle, {"seek", target_str.c_str(), "absolute", "exact"});
        } else {
            refresh_loop(state);
        }
    } else {
        refresh_loop(state);
    }

    mpv_util::show_osd_message(state.handle, state.loop_enabled ? "loop on" : "loop off", kToggleOsdDuration);
}

// ---- SPEC §6：存档 ----

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

    // 头/尾各读多少字节、尾部从哪个偏移开始读，这段算术在 store::
    // compute_sample_ranges 里，覆盖了“文件比采样窗口还小”的边界，这里只
    // 负责按算好的范围做实际的文件 IO。
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
    // 用 ~~home/ 而不是 ~~/：后者在“子路径已存在时返回已存在的那个目录”，
    // 语义上是给读取用的；这里是要写入的目录，明确指向 mpv 配置目录，避免
    // 依赖“碰巧存在”这种不确定性（--no-config 时两者都会变成空字符串，
    // 这个降级行为是 mpv 自身文档明确写的，插件不需要额外处理）。
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
    refresh_loop(state);
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
    case store::LookupResult::Kind::kSingleCandidate:
        // mpv 没有原生弹窗，靠 OSD 文字 + 临时抢占 confirm-yes/confirm-no 两
        // 个键来实现确认（SPEC §6）。存档里只有路径的哈希，插件这边压根拿不
        // 到那条旧路径的明文，提示文案不带路径，不是能带但故意藏起来。
        state.pending_confirmation = result;
        mpv_util::show_osd_message(
            state.handle,
            "Found an archive with matching content under a different name.\nconfirm-yes to load & "
            "migrate, confirm-no to cancel",
            kStateOsdDuration);
        break;
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

    if (!yes) {
        mpv_util::show_osd_message(state.handle, "Cancelled", kToggleOsdDuration);
        return;
    }

    apply_loaded_segments(state, result.segments);
    state.rename_from = result.matched_key;
    mpv_util::show_osd_message(state.handle, "Loops loaded (will migrate archive on next save)",
                                kStateOsdDuration);
}

// ---- 文件生命周期 ----

void on_file_loaded(PluginState &state) {
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

    // current_path 只留在内存里用来实际读文件、算哈希；写进存档文件的
    // current_path_key 是它的哈希，不会把真实路径持久化到磁盘上。
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

// ---- 按键分发：C 插件没有 mp.add_key_binding 那样的封装，参照
// enhanced-drag 的写法，直接接住 mpv 转发过来的 "key-binding" client
// message（input.rst:1399-1439：script-binding 命令内部就是这样转发的，不
// 需要提前注册）----

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
            // SPEC §2.1 第 2 点：确认已经落到某个 active 项的起点后，判断
            // 是不是原生自环刚播完这一项、要不要补一次跨段跳转（见
            // on_native_landing 的注释）。两个事件都可能触发，重复调用是
            // 幂等的，不需要去重。
            MPV_UTIL_DEBUG("event: {} pos={:.3f}\n",
                            event->event_id == MPV_EVENT_SEEK ? "SEEK" : "PLAYBACK_RESTART",
                            time_pos(state.handle));
            on_native_landing(state);
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
