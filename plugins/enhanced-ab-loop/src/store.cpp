#include "enhanced_ab_loop/store.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace enhanced_ab_loop::store {

namespace {

std::uint64_t fnv1a_64(std::string_view data, std::uint64_t hash) {
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ULL; // FNV prime
    }
    return hash;
}

std::string to_hex16(std::uint64_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 0; i < 16; ++i) {
        int shift = (15 - i) * 4;
        result[static_cast<std::size_t>(i)] = kHex[(value >> shift) & 0xF];
    }
    return result;
}

nlohmann::json segment_to_json(const Segment &seg) {
    return {{"a", seg.a}, {"b", seg.b}, {"enabled", seg.enabled}};
}

std::optional<Segment> segment_from_json(const nlohmann::json &item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    Segment seg;
    seg.a = item.value("a", 0.0);
    seg.b = item.value("b", 0.0);
    seg.enabled = item.value("enabled", true);
    return seg;
}

nlohmann::json segments_to_json_array(const std::vector<Segment> &segments) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &seg : segments) {
        arr.push_back(segment_to_json(seg));
    }
    return arr;
}

std::vector<Segment> segments_from_json_array(const nlohmann::json &arr) {
    std::vector<Segment> result;
    if (!arr.is_array()) {
        return result;
    }
    for (const auto &item : arr) {
        if (auto seg = segment_from_json(item)) {
            result.push_back(*seg);
        }
    }
    return result;
}

nlohmann::json pending_to_json(const Pending &pending) {
    return {{"has_a", pending.has_a}, {"has_b", pending.has_b}, {"a", pending.a}, {"b", pending.b}};
}

Pending pending_from_json(const nlohmann::json &item) {
    Pending pending;
    if (!item.is_object()) {
        return pending;
    }
    pending.has_a = item.value("has_a", false);
    pending.has_b = item.value("has_b", false);
    pending.a = item.value("a", 0.0);
    pending.b = item.value("b", 0.0);
    return pending;
}

nlohmann::json snapshot_to_json(const Snapshot &snapshot) {
    return {{"segments", segments_to_json_array(snapshot.segments)}, {"pending", pending_to_json(snapshot.pending)}};
}

// 老存档格式的条目只有 "segments" 字段（没有 "pending"），item.value 系的
// contains 检查缺失字段时自然落到默认值，向后兼容不需要单独分支。
Snapshot snapshot_from_json(const nlohmann::json &item) {
    Snapshot snapshot;
    if (!item.is_object()) {
        return snapshot;
    }
    if (item.contains("segments")) {
        snapshot.segments = segments_from_json_array(item["segments"]);
    }
    if (item.contains("pending")) {
        snapshot.pending = pending_from_json(item["pending"]);
    }
    return snapshot;
}

nlohmann::json file_entry_to_json(const FileEntry &entry) {
    nlohmann::json templates_json = nlohmann::json::object();
    for (const auto &[slot, snapshot] : entry.templates) {
        // JSON object 的 key 只能是字符串，槽位号（int）转成十进制字符串；
        // 读回时用 std::stoi 还原，见 file_entry_from_json。
        templates_json[std::to_string(slot)] = snapshot_to_json(snapshot);
    }
    nlohmann::json result = snapshot_to_json(entry.current);
    result["templates"] = templates_json;
    return result;
}

FileEntry file_entry_from_json(const nlohmann::json &item) {
    FileEntry entry;
    if (!item.is_object()) {
        return entry;
    }
    entry.current = snapshot_from_json(item);
    if (item.contains("templates") && item["templates"].is_object()) {
        for (const auto &[key, value] : item["templates"].items()) {
            try {
                int slot = std::stoi(key);
                entry.templates[slot] = snapshot_from_json(value);
            } catch (const std::exception &) {
                // 槽位号不是合法整数（存档被手动改坏），跳过这一条，不让
                // 一条格式错误的数据拖垮整个 entry 的解析。
            }
        }
    }
    return entry;
}

} // namespace

std::string compute_content_hash(std::uint64_t file_size, std::string_view head_sample,
                                  std::string_view tail_sample) {
    // FNV-1a 64 位，不追求密码学强度，只要求对不同视频文件的碰撞概率低到
    // 可以忽略——不引入额外的 hash 依赖，跟项目里“不提前引入不必要依赖”的
    // 取向一致。文件大小也参与哈希，避免头尾字节恰好相同、大小不同的文件
    // 撞车。
    std::uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
    std::string_view size_bytes(reinterpret_cast<const char *>(&file_size), sizeof(file_size));
    hash = fnv1a_64(size_bytes, hash);
    hash = fnv1a_64(head_sample, hash);
    hash = fnv1a_64(tail_sample, hash);
    return to_hex16(hash);
}

std::string extract_filename(std::string_view path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(pos + 1));
}

std::string compute_path_hash(std::string_view path) {
    // 同一套 FNV-1a，跟内容哈希用途不同（这里只是给一个字符串算个不可逆的
    // 短标识，不需要抗碰撞强度以外的属性），没必要换一种算法。调用方传入的
    // 是文件名（extract_filename 的输出），语义见 store.h 里的注释。
    return to_hex16(fnv1a_64(path, 0xcbf29ce484222325ULL));
}

SampleRanges compute_sample_ranges(std::uint64_t file_size, std::uint64_t max_sample_bytes) {
    SampleRanges ranges;
    ranges.head_len = std::min(file_size, max_sample_bytes);
    ranges.tail_len = std::min(file_size, max_sample_bytes);
    ranges.tail_offset = file_size - ranges.tail_len;
    return ranges;
}

std::string serialize_segments(const std::vector<Segment> &segments) {
    return segments_to_json_array(segments).dump();
}

std::vector<Segment> deserialize_segments(const std::string &json_text) {
    try {
        return segments_from_json_array(nlohmann::json::parse(json_text));
    } catch (const nlohmann::json::exception &) {
        return {};
    }
}

std::string serialize_pending(const Pending &pending) {
    return pending_to_json(pending).dump();
}

Pending deserialize_pending(const std::string &json_text) {
    try {
        return pending_from_json(nlohmann::json::parse(json_text));
    } catch (const nlohmann::json::exception &) {
        return Pending{};
    }
}

std::string serialize_snapshot(const Snapshot &snapshot) {
    return snapshot_to_json(snapshot).dump();
}

Snapshot deserialize_snapshot(const std::string &json_text) {
    try {
        return snapshot_from_json(nlohmann::json::parse(json_text));
    } catch (const nlohmann::json::exception &) {
        return Snapshot{};
    }
}

std::string serialize_archive(const Archive &archive) {
    nlohmann::json entries = nlohmann::json::object();
    for (const auto &[key, entry] : archive.entries) {
        entries[key] = file_entry_to_json(entry);
    }
    nlohmann::json root = {{"entries", entries}};
    return root.dump(2);
}

Archive deserialize_archive(const std::string &json_text) {
    Archive archive;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception &) {
        return archive;
    }
    if (!parsed.is_object() || !parsed.contains("entries") || !parsed["entries"].is_object()) {
        return archive;
    }
    for (const auto &[key, value] : parsed["entries"].items()) {
        archive.entries[key] = file_entry_from_json(value);
    }
    return archive;
}

LookupResult lookup(const Archive &archive, const std::string &current_path_key) {
    LookupResult result;

    if (auto it = archive.entries.find(current_path_key); it != archive.entries.end()) {
        result.kind = LookupResult::Kind::kExactMatch;
        result.matched_key = current_path_key;
        result.entry = it->second;
        return result;
    }

    if (archive.entries.size() == 1) {
        auto only = archive.entries.begin();
        result.kind = LookupResult::Kind::kSingleCandidate;
        result.matched_key = only->first;
        result.entry = only->second;
        return result;
    }

    result.kind = LookupResult::Kind::kNoArchive;
    return result;
}

void upsert(Archive &archive, const std::string &current_path_key, const FileEntry &entry,
            std::optional<std::string> rename_from) {
    if (rename_from && *rename_from != current_path_key) {
        archive.entries.erase(*rename_from);
    }
    archive.entries[current_path_key] = entry;
}

} // namespace enhanced_ab_loop::store
