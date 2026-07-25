#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "split_zoom_box/store.h"

using namespace split_zoom_box;

namespace {

Layout make_2x2() {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kVertical));
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    focus_next(layout);
    focus_next(layout);
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    return layout;
}

} // namespace

TEST_CASE("布局树序列化往返保持结构与区域", "[store]") {
    Layout layout = make_2x2();
    std::vector<int> leaves = leaf_order(layout);
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        double x = 0.1 * static_cast<double>(i);
        layout.nodes[leaves[i]].region = Region{x, x, x + 0.5, x + 0.5};
    }

    Layout restored = store::deserialize_layout(store::serialize_layout(layout));
    REQUIRE(leaf_count(restored) == 4);

    // 结构等价性用滤镜图字符串比对：它同时反映了树形状、窗格顺序和区域
    REQUIRE(store::serialize_layout(restored) == store::serialize_layout(layout));
    REQUIRE(build_filter_graph(restored, 640, 360, 640, 360) ==
            build_filter_graph(layout, 640, 360, 640, 360));
}

TEST_CASE("读档后焦点落到第一个窗格", "[store]") {
    Layout layout = make_2x2();
    focus_next(layout);
    focus_next(layout);

    Layout restored = store::deserialize_layout(store::serialize_layout(layout));
    REQUIRE(restored.focused == leaf_order(restored).front());
}

TEST_CASE("损坏的布局 JSON 退化成单窗格完整画面", "[store]") {
    REQUIRE(leaf_count(store::deserialize_layout("{ not json")) == 1);
    REQUIRE(leaf_count(store::deserialize_layout("[]")) == 1);
    // 内部节点缺子节点：不能让半棵树漏进运行时
    REQUIRE(leaf_count(store::deserialize_layout(R"({"leaf":false,"dir":"h"})")) == 1);
    REQUIRE(region_is_full(store::deserialize_layout("null").nodes[0].region));
}

TEST_CASE("越界区域读档时被丢弃成完整画面", "[store]") {
    Layout bad = store::deserialize_layout(R"({"leaf":true,"region":[0.5,0.5,0.4,0.9]})");
    REQUIRE(region_is_full(bad.nodes[0].region));
}

TEST_CASE("存档序列化往返", "[store]") {
    store::Archive archive;
    store::FileEntry entry;
    entry.base = make_layout(Region{0.25, 0.25, 0.75, 0.75});

    LayoutSegment seg;
    seg.a = 5.0;
    seg.b = 15.0;
    seg.enabled = true;
    seg.layout = make_2x2();
    entry.segments.push_back(seg);

    archive.entries["abc"] = entry;

    store::Archive restored = store::deserialize_archive(store::serialize_archive(archive));
    REQUIRE(restored.entries.count("abc") == 1);
    const store::FileEntry &back = restored.entries.at("abc");
    REQUIRE(back.segments.size() == 1);
    REQUIRE(back.segments[0].a == Catch::Approx(5.0));
    REQUIRE(back.segments[0].b == Catch::Approx(15.0));
    REQUIRE(leaf_count(back.segments[0].layout) == 4);
    REQUIRE(back.base.nodes[0].region.x1 == Catch::Approx(0.25));
}

TEST_CASE("缺字段的老格式条目按默认值读", "[store]") {
    // 只有 entries、条目里什么都没有
    store::Archive archive = store::deserialize_archive(R"({"entries":{"k":{}}})");
    REQUIRE(archive.entries.count("k") == 1);
    REQUIRE(leaf_count(archive.entries.at("k").base) == 1);
    REQUIRE(archive.entries.at("k").segments.empty());
}

TEST_CASE("损坏的存档按空处理，不抛异常", "[store]") {
    REQUIRE(store::deserialize_archive("{ broken").entries.empty());
    REQUIRE(store::deserialize_archive("[1,2,3]").entries.empty());
    REQUIRE(store::deserialize_archive(R"({"entries":42})").entries.empty());
}

TEST_CASE("读档时区间被排序", "[store]") {
    store::Archive archive = store::deserialize_archive(R"({"entries":{"k":{"segments":[
        {"a":30.0,"b":40.0},{"a":5.0,"b":15.0}]}}})");
    const auto &segments = archive.entries.at("k").segments;
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].a == Catch::Approx(5.0));
    REQUIRE(segments[1].a == Catch::Approx(30.0));
}

TEST_CASE("lookup 三种结果", "[store]") {
    store::Archive archive;
    archive.entries["key-a"] = store::FileEntry{};

    REQUIRE(store::lookup(archive, "key-a").kind == store::LookupResult::Kind::kExactMatch);
    // 唯一候选：大概率是同一文件改名了
    auto single = store::lookup(archive, "key-b");
    REQUIRE(single.kind == store::LookupResult::Kind::kSingleCandidate);
    REQUIRE(single.matched_key == "key-a");

    archive.entries["key-c"] = store::FileEntry{};
    REQUIRE(store::lookup(archive, "key-b").kind == store::LookupResult::Kind::kNoArchive);
}

TEST_CASE("改名迁移不留孤儿条目", "[store]") {
    store::Archive archive;
    archive.entries["old"] = store::FileEntry{};

    store::upsert(archive, "new", store::FileEntry{}, std::string("old"));
    REQUIRE(archive.entries.count("old") == 0);
    REQUIRE(archive.entries.count("new") == 1);
    REQUIRE(archive.entries.size() == 1);
}

TEST_CASE("采样窗口比文件还大时的边界算术", "[store]") {
    // 头尾完全重叠，等价于把整个文件都采样了一遍
    auto ranges = store::compute_sample_ranges(100, 65536);
    REQUIRE(ranges.head_len == 100);
    REQUIRE(ranges.tail_len == 100);
    REQUIRE(ranges.tail_offset == 0);

    auto empty = store::compute_sample_ranges(0, 65536);
    REQUIRE(empty.head_len == 0);
    REQUIRE(empty.tail_offset == 0);
}

TEST_CASE("哈希稳定且区分文件大小", "[store]") {
    REQUIRE(store::compute_content_hash(100, "head", "tail") ==
            store::compute_content_hash(100, "head", "tail"));
    // 头尾字节相同、大小不同的文件不能撞车
    REQUIRE(store::compute_content_hash(100, "head", "tail") !=
            store::compute_content_hash(200, "head", "tail"));
    REQUIRE(store::compute_content_hash(100, "head", "tail").size() == 16);
}

TEST_CASE("extract_filename 认两种分隔符且不含目录", "[store]") {
    REQUIRE(store::extract_filename("/home/user/movies/a.mkv") == "a.mkv");
    REQUIRE(store::extract_filename(R"(C:\videos\b.mp4)") == "b.mp4");
    REQUIRE(store::extract_filename("bare.mkv") == "bare.mkv");
    // 路径 key 里不该出现明文文件名
    REQUIRE(store::compute_path_hash("secret-name.mkv").find("secret") == std::string::npos);
}
