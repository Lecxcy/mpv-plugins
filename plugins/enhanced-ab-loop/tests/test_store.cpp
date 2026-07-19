#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "enhanced_ab_loop/store.h"

using namespace enhanced_ab_loop;
using namespace enhanced_ab_loop::store;
using Catch::Approx;

TEST_CASE("compute_content_hash is deterministic for identical input", "[store][hash]") {
    auto h1 = compute_content_hash(1234, "head-bytes", "tail-bytes");
    auto h2 = compute_content_hash(1234, "head-bytes", "tail-bytes");
    CHECK(h1 == h2);
    CHECK(h1.size() == 16); // 64 位十六进制
}

TEST_CASE("compute_content_hash differs when size differs but samples are identical",
          "[store][hash]") {
    auto h1 = compute_content_hash(1000, "same", "same");
    auto h2 = compute_content_hash(2000, "same", "same");
    CHECK(h1 != h2);
}

TEST_CASE("compute_content_hash differs when head or tail sample differs", "[store][hash]") {
    auto base = compute_content_hash(1000, "head-a", "tail-a");
    CHECK(base != compute_content_hash(1000, "head-b", "tail-a"));
    CHECK(base != compute_content_hash(1000, "head-a", "tail-b"));
}

TEST_CASE("compute_path_hash is deterministic and differs across distinct paths", "[store][hash]") {
    auto h1 = compute_path_hash("/home/alice/videos/movie.mp4");
    auto h2 = compute_path_hash("/home/alice/videos/movie.mp4");
    auto h3 = compute_path_hash("/home/bob/videos/movie.mp4");
    CHECK(h1 == h2);
    CHECK(h1 != h3);
    CHECK(h1.size() == 16);
}

TEST_CASE("compute_path_hash output never contains the original path text (this is the whole "
          "point: archives on disk must not leak plaintext paths)",
          "[store][hash]") {
    std::string path = "/Users/someone/Private Folder/secret-name.mkv";
    auto hashed = compute_path_hash(path);
    CHECK(hashed.find("secret-name") == std::string::npos);
    CHECK(hashed.find("someone") == std::string::npos);
}

TEST_CASE("compute_sample_ranges: file larger than 2x the sample window has no overlap",
          "[store][sample-ranges]") {
    auto ranges = compute_sample_ranges(/*file_size=*/1'000'000, /*max_sample_bytes=*/65536);
    CHECK(ranges.head_len == 65536);
    CHECK(ranges.tail_len == 65536);
    CHECK(ranges.tail_offset == 1'000'000 - 65536);
    CHECK(ranges.tail_offset >= ranges.head_len); // 头尾窗口不重叠
}

TEST_CASE("compute_sample_ranges: file smaller than the sample window makes head and tail cover "
          "the entire file (overlap is intentional, not a bug)",
          "[store][sample-ranges]") {
    auto ranges = compute_sample_ranges(/*file_size=*/1000, /*max_sample_bytes=*/65536);
    CHECK(ranges.head_len == 1000);
    CHECK(ranges.tail_len == 1000);
    CHECK(ranges.tail_offset == 0); // 尾部窗口也是从头开始，头尾读到的是同一段字节
}

TEST_CASE("compute_sample_ranges: file exactly at the sample window boundary", "[store][sample-ranges]") {
    auto ranges = compute_sample_ranges(/*file_size=*/65536, /*max_sample_bytes=*/65536);
    CHECK(ranges.head_len == 65536);
    CHECK(ranges.tail_len == 65536);
    CHECK(ranges.tail_offset == 0);
}

TEST_CASE("compute_sample_ranges: file between 1x and 2x the sample window makes head and tail "
          "windows overlap partially",
          "[store][sample-ranges]") {
    auto ranges = compute_sample_ranges(/*file_size=*/100000, /*max_sample_bytes=*/65536);
    CHECK(ranges.head_len == 65536);
    CHECK(ranges.tail_len == 65536);
    CHECK(ranges.tail_offset == 100000 - 65536); // 34464，比 head_len 小，头尾确实重叠
    CHECK(ranges.tail_offset < ranges.head_len);
}

TEST_CASE("compute_sample_ranges: empty file (size 0) does not underflow and yields empty "
          "windows instead of a huge unsigned wraparound",
          "[store][sample-ranges]") {
    auto ranges = compute_sample_ranges(/*file_size=*/0, /*max_sample_bytes=*/65536);
    CHECK(ranges.head_len == 0);
    CHECK(ranges.tail_len == 0);
    CHECK(ranges.tail_offset == 0); // 不是 0 - 65536 在无符号数下的环绕值
}

TEST_CASE("compute_content_hash stays well-defined (no crash, deterministic) for an empty sample "
          "matching a zero-byte file",
          "[store][hash]") {
    auto h1 = compute_content_hash(0, "", "");
    auto h2 = compute_content_hash(0, "", "");
    CHECK(h1 == h2);
    CHECK(h1.size() == 16);
}

TEST_CASE("serialize_segments / deserialize_segments round-trip preserves all fields",
          "[store][json]") {
    std::vector<Segment> segments{Segment{1.5, 2.5, true}, Segment{10.0, 20.0, false}};

    auto json_text = serialize_segments(segments);
    auto parsed = deserialize_segments(json_text);

    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].a == Approx(1.5));
    CHECK(parsed[0].b == Approx(2.5));
    CHECK(parsed[0].enabled);
    CHECK(parsed[1].a == Approx(10.0));
    CHECK(parsed[1].b == Approx(20.0));
    CHECK_FALSE(parsed[1].enabled);
}

TEST_CASE("deserialize_segments returns an empty list for malformed JSON instead of throwing",
          "[store][json]") {
    CHECK(deserialize_segments("{not valid json").empty());
    CHECK(deserialize_segments("{\"not\": \"an array\"}").empty());
    CHECK(deserialize_segments("").empty());
}

// 下面这些用例里 Archive::entries 的 key 用了看着像明文路径的字符串
// （"/videos/a.mp4" 这种），这是为了让测试用例本身可读；lookup/upsert/
// Archive 这一层完全不关心 key 是不是真的路径，只把它当不透明字符串处理
// ——生产代码路径上，plugin.cpp 会先用 compute_path_hash() 把真实路径转成
// 哈希再传进来，明文路径本身不会到达这一层。
TEST_CASE("serialize_archive / deserialize_archive round-trip preserves multiple path entries",
          "[store][json]") {
    Archive archive;
    archive.entries["/videos/a.mp4"] = {Segment{0, 5, true}};
    archive.entries["/videos/b.mp4"] = {Segment{10, 20, false}, Segment{30, 40, true}};

    auto json_text = serialize_archive(archive);
    auto parsed = deserialize_archive(json_text);

    REQUIRE(parsed.entries.size() == 2);
    REQUIRE(parsed.entries.count("/videos/a.mp4") == 1);
    REQUIRE(parsed.entries.count("/videos/b.mp4") == 1);
    CHECK(parsed.entries["/videos/a.mp4"].size() == 1);
    REQUIRE(parsed.entries["/videos/b.mp4"].size() == 2);
    CHECK(parsed.entries["/videos/b.mp4"][0].a == Approx(10));
    CHECK_FALSE(parsed.entries["/videos/b.mp4"][0].enabled);
}

TEST_CASE("deserialize_archive returns an empty archive for malformed JSON", "[store][json]") {
    CHECK(deserialize_archive("not json").entries.empty());
    CHECK(deserialize_archive("[]").entries.empty()); // 不是期望的 {"entries": {...}} 形状
}

TEST_CASE("lookup returns an exact match with zero ambiguity when the path is present",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/a.mp4"] = {Segment{0, 5, true}};
    archive.entries["/videos/b.mp4"] = {Segment{10, 20, true}};

    auto result = lookup(archive, "/videos/a.mp4");
    CHECK(result.kind == LookupResult::Kind::kExactMatch);
    CHECK(result.matched_key == "/videos/a.mp4");
    REQUIRE(result.segments.size() == 1);
    CHECK(result.segments[0].a == Approx(0));
}

TEST_CASE("lookup with exactly one candidate under a different path reports kSingleCandidate "
          "(likely a rename, needs user confirmation)",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/old-name.mp4"] = {Segment{0, 5, true}};

    auto result = lookup(archive, "/videos/new-name.mp4");
    CHECK(result.kind == LookupResult::Kind::kSingleCandidate);
    CHECK(result.matched_key == "/videos/old-name.mp4");
    REQUIRE(result.segments.size() == 1);
}

TEST_CASE("lookup with two or more candidates under different paths reports kNoArchive (genuine "
          "duplicate content, no safe automatic choice)",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/copy-1.mp4"] = {Segment{0, 5, true}};
    archive.entries["/videos/copy-2.mp4"] = {Segment{10, 20, true}};

    auto result = lookup(archive, "/videos/copy-3.mp4");
    CHECK(result.kind == LookupResult::Kind::kNoArchive);
    CHECK(result.segments.empty());
}

TEST_CASE("lookup on an empty archive reports kNoArchive", "[store][lookup]") {
    Archive archive;
    auto result = lookup(archive, "/videos/anything.mp4");
    CHECK(result.kind == LookupResult::Kind::kNoArchive);
}

TEST_CASE("upsert without rename_from just adds/overwrites the current path's entry",
          "[store][upsert]") {
    Archive archive;
    upsert(archive, "/videos/a.mp4", {Segment{0, 5, true}}, std::nullopt);

    REQUIRE(archive.entries.size() == 1);
    CHECK(archive.entries.count("/videos/a.mp4") == 1);
}

TEST_CASE("upsert with rename_from migrates the old path's entry to the new path instead of "
          "accumulating an orphan (regression test for repeated-rename entry bloat)",
          "[store][upsert]") {
    Archive archive;
    archive.entries["/videos/old-name.mp4"] = {Segment{0, 5, true}};

    upsert(archive, "/videos/new-name.mp4", {Segment{0, 5, true}, Segment{10, 20, true}},
           std::string("/videos/old-name.mp4"));

    REQUIRE(archive.entries.size() == 1); // 旧路径没有留下孤儿
    CHECK(archive.entries.count("/videos/old-name.mp4") == 0);
    REQUIRE(archive.entries.count("/videos/new-name.mp4") == 1);
    CHECK(archive.entries["/videos/new-name.mp4"].size() == 2);
}

TEST_CASE("repeated rename-and-save cycles never grow past a single entry", "[store][upsert]") {
    Archive archive;
    upsert(archive, "/videos/v1.mp4", {Segment{0, 5, true}}, std::nullopt);
    upsert(archive, "/videos/v2.mp4", {Segment{0, 5, true}}, std::string("/videos/v1.mp4"));
    upsert(archive, "/videos/v3.mp4", {Segment{0, 5, true}}, std::string("/videos/v2.mp4"));
    upsert(archive, "/videos/v4.mp4", {Segment{0, 5, true}}, std::string("/videos/v3.mp4"));

    REQUIRE(archive.entries.size() == 1);
    CHECK(archive.entries.count("/videos/v4.mp4") == 1);
}
