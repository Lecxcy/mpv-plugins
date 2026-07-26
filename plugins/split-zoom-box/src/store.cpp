#include "split_zoom_box/store.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace split_zoom_box::store {

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

nlohmann::json region_to_json(const Region &region) {
    return nlohmann::json::array({region.x1, region.y1, region.x2, region.y2});
}

Region region_from_json(const nlohmann::json &item) {
    Region region;
    if (!item.is_array() || item.size() != 4) {
        return region;
    }
    for (const auto &value : item) {
        if (!value.is_number()) {
            return region;
        }
    }
    region.x1 = item[0].get<double>();
    region.y1 = item[1].get<double>();
    region.x2 = item[2].get<double>();
    region.y2 = item[3].get<double>();
    if (!region_valid(region)) {
        return Region{};
    }
    return region;
}

// 树按嵌套结构写出，不写 arena 下标。
nlohmann::json node_to_json(const Layout &layout, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= layout.nodes.size()) {
        return {{"leaf", true}, {"region", region_to_json(Region{})}};
    }
    const Node &node = layout.nodes[index];
    if (node.leaf) {
        nlohmann::json leaf = {{"leaf", true}, {"region", region_to_json(node.region)}};
        // 位移只在非零时写出，省得每个叶子都拖两个 0.0；老存档没有这两个字段
        // 时读回来自然是 0（居中），向后兼容不需要迁移。
        if (node.offset_x != 0.0 || node.offset_y != 0.0) {
            leaf["offset"] = nlohmann::json::array({node.offset_x, node.offset_y});
        }
        return leaf;
    }
    return {{"leaf", false},
            {"dir", node.dir == SplitDir::kHorizontal ? "h" : "v"},
            {"children", nlohmann::json::array({node_to_json(layout, node.first),
                                                node_to_json(layout, node.second)})}};
}

// 递归把嵌套 JSON 还原进 arena，返回新节点的下标。
int node_from_json(Layout &layout, const nlohmann::json &item) {
    Node node;
    if (!item.is_object() || item.value("leaf", true)) {
        node.leaf = true;
        if (item.is_object() && item.contains("region")) {
            node.region = region_from_json(item["region"]);
        }
        if (item.is_object() && item.contains("offset") && item["offset"].is_array() &&
            item["offset"].size() == 2 && item["offset"][0].is_number() && item["offset"][1].is_number()) {
            node.offset_x = item["offset"][0].get<double>();
            node.offset_y = item["offset"][1].get<double>();
        }
        layout.nodes.push_back(node);
        return static_cast<int>(layout.nodes.size()) - 1;
    }

    const nlohmann::json &children = item.contains("children") ? item["children"] : nlohmann::json::array();
    if (!children.is_array() || children.size() != 2) {
        // 结构坏了就退化成一个完整画面的叶子，不让半棵树漏进运行时。
        node.leaf = true;
        layout.nodes.push_back(node);
        return static_cast<int>(layout.nodes.size()) - 1;
    }

    node.leaf = false;
    node.dir = item.value("dir", std::string("h")) == "v" ? SplitDir::kVertical : SplitDir::kHorizontal;
    layout.nodes.push_back(node);
    int self = static_cast<int>(layout.nodes.size()) - 1;
    int first = node_from_json(layout, children[0]);
    int second = node_from_json(layout, children[1]);
    layout.nodes[self].first = first;
    layout.nodes[self].second = second;
    return self;
}

nlohmann::json layout_to_json(const Layout &layout) {
    if (layout.nodes.empty()) {
        return node_to_json(make_layout(), 0);
    }
    return node_to_json(layout, 0);
}

// 焦点不持久化：它是编辑期状态，读档后统一落到第一个窗格上。
Layout layout_from_json(const nlohmann::json &item) {
    Layout layout;
    node_from_json(layout, item);
    if (layout.nodes.empty()) {
        return make_layout();
    }
    std::vector<int> leaves = leaf_order(layout);
    layout.focused = leaves.empty() ? 0 : leaves.front();
    return layout;
}

nlohmann::json segment_to_json(const LayoutSegment &seg) {
    return {{"a", seg.a}, {"b", seg.b}, {"layout", layout_to_json(seg.layout)}};
}

std::optional<LayoutSegment> segment_from_json(const nlohmann::json &item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    LayoutSegment seg;
    seg.a = item.value("a", 0.0);
    seg.b = item.value("b", 0.0);
    // 老存档里的 "enabled" 字段直接忽略：这个概念已经去掉了，不做迁移。
    seg.layout = item.contains("layout") ? layout_from_json(item["layout"]) : make_layout();
    return seg;
}

nlohmann::json file_entry_to_json(const FileEntry &entry) {
    nlohmann::json segments = nlohmann::json::array();
    for (const auto &seg : entry.segments) {
        segments.push_back(segment_to_json(seg));
    }
    return {{"base", layout_to_json(entry.base)}, {"segments", segments}};
}

FileEntry file_entry_from_json(const nlohmann::json &item) {
    FileEntry entry;
    if (!item.is_object()) {
        entry.base = make_layout();
        return entry;
    }
    entry.base = item.contains("base") ? layout_from_json(item["base"]) : make_layout();
    if (item.contains("segments") && item["segments"].is_array()) {
        for (const auto &value : item["segments"]) {
            if (auto seg = segment_from_json(value)) {
                entry.segments.push_back(*seg);
            }
        }
    }
    sort_segments(entry.segments);
    return entry;
}

} // namespace

std::string compute_content_hash(std::uint64_t file_size, std::string_view head_sample,
                                  std::string_view tail_sample) {
    // FNV-1a 64 位，不追求密码学强度，只要求对不同视频文件碰撞概率低到可以
    // 忽略。文件大小也参与哈希，避免头尾字节恰好相同、大小不同的文件撞车。
    std::uint64_t hash = 0xcbf29ce484222325ULL;
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
    return to_hex16(fnv1a_64(path, 0xcbf29ce484222325ULL));
}

SampleRanges compute_sample_ranges(std::uint64_t file_size, std::uint64_t max_sample_bytes) {
    SampleRanges ranges;
    ranges.head_len = std::min(file_size, max_sample_bytes);
    ranges.tail_len = std::min(file_size, max_sample_bytes);
    ranges.tail_offset = file_size - ranges.tail_len;
    return ranges;
}

std::string serialize_layout(const Layout &layout) {
    return layout_to_json(layout).dump();
}

Layout deserialize_layout(const std::string &json_text) {
    try {
        return layout_from_json(nlohmann::json::parse(json_text));
    } catch (const nlohmann::json::exception &) {
        return make_layout();
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

} // namespace split_zoom_box::store
