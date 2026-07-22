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

TEST_CASE("extract_filename strips leading directory components", "[store][filename]") {
    CHECK(extract_filename("/home/alice/videos/movie.mp4") == "movie.mp4");
    CHECK(extract_filename("C:\\Users\\alice\\videos\\movie.mp4") == "movie.mp4"); // Windows 反斜杠
    CHECK(extract_filename("movie.mp4") == "movie.mp4"); // 没有目录部分，原样返回
}

TEST_CASE("extract_filename gives the same result regardless of which mount point the file "
          "currently sits under (this is the whole point: external drives/NAS change mount "
          "points, filenames don't)",
          "[store][filename]") {
    auto a = extract_filename("/Volumes/MobileDrive/Movies/foo.mp4");
    auto b = extract_filename("/Volumes/MobileDrive-1/Movies/foo.mp4");
    auto c = extract_filename("/Volumes/NAS/Media/Movies/foo.mp4");
    CHECK(a == b);
    CHECK(b == c);
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

TEST_CASE("serialize_pending / deserialize_pending round-trip preserves an open A endpoint",
          "[store][json][pending]") {
    Pending pending;
    pending.has_a = true;
    pending.a = 12.5;

    auto parsed = deserialize_pending(serialize_pending(pending));
    CHECK(parsed.has_a);
    CHECK_FALSE(parsed.has_b);
    CHECK(parsed.a == Approx(12.5));
}

TEST_CASE("serialize_pending / deserialize_pending round-trip preserves an open B endpoint",
          "[store][json][pending]") {
    Pending pending;
    pending.has_b = true;
    pending.b = 40.0;

    auto parsed = deserialize_pending(serialize_pending(pending));
    CHECK_FALSE(parsed.has_a);
    CHECK(parsed.has_b);
    CHECK(parsed.b == Approx(40.0));
}

TEST_CASE("deserialize_pending returns an empty pending for malformed JSON instead of throwing",
          "[store][json][pending]") {
    auto parsed = deserialize_pending("not json");
    CHECK_FALSE(parsed.has_a);
    CHECK_FALSE(parsed.has_b);
}

TEST_CASE("serialize_snapshot / deserialize_snapshot round-trip preserves both segments and a "
          "still-open pending point (this is the whole point of the redesign: users do save "
          "while a segment only has one endpoint set)",
          "[store][json][snapshot]") {
    Snapshot snapshot;
    snapshot.segments = {Segment{0, 5, true}, Segment{10, 20, false}};
    snapshot.pending.has_a = true;
    snapshot.pending.a = 30.0;

    auto parsed = deserialize_snapshot(serialize_snapshot(snapshot));
    REQUIRE(parsed.segments.size() == 2);
    CHECK(parsed.segments[1].a == Approx(10));
    CHECK_FALSE(parsed.segments[1].enabled);
    CHECK(parsed.pending.has_a);
    CHECK(parsed.pending.a == Approx(30.0));
}

TEST_CASE("deserialize_snapshot returns an empty snapshot for malformed JSON", "[store][json][snapshot]") {
    auto parsed = deserialize_snapshot("not json");
    CHECK(parsed.segments.empty());
    CHECK_FALSE(parsed.pending.has_a);
    CHECK_FALSE(parsed.pending.has_b);
}

// 下面这些用例里 Archive::entries 的 key 用了看着像明文路径的字符串
// （"/videos/a.mp4" 这种），这是为了让测试用例本身可读；lookup/upsert/
// Archive 这一层完全不关心 key 是不是真的路径，只把它当不透明字符串处理
// ——生产代码路径上，plugin.cpp 会先用 extract_filename() 把真实路径转成
// 文件名，再用 compute_path_hash() 转成哈希传进来，明文路径本身不会到达
// 这一层。
TEST_CASE("serialize_archive / deserialize_archive round-trip preserves current segments, "
          "pending, and every template slot",
          "[store][json]") {
    Archive archive;

    FileEntry entry_a;
    entry_a.current.segments = {Segment{0, 5, true}};
    archive.entries["/videos/a.mp4"] = entry_a;

    FileEntry entry_b;
    entry_b.current.segments = {Segment{10, 20, false}, Segment{30, 40, true}};
    entry_b.current.pending.has_b = true;
    entry_b.current.pending.b = 45.0;
    entry_b.templates[1] = Snapshot{{Segment{10, 20, true}, Segment{30, 40, true}}, Pending{}};
    entry_b.templates[3] = Snapshot{{Segment{10, 20, true}}, Pending{}};
    archive.entries["/videos/b.mp4"] = entry_b;

    auto json_text = serialize_archive(archive);
    auto parsed = deserialize_archive(json_text);

    REQUIRE(parsed.entries.size() == 2);
    REQUIRE(parsed.entries.count("/videos/a.mp4") == 1);
    REQUIRE(parsed.entries.count("/videos/b.mp4") == 1);

    CHECK(parsed.entries["/videos/a.mp4"].current.segments.size() == 1);
    CHECK(parsed.entries["/videos/a.mp4"].templates.empty());

    const auto &b = parsed.entries["/videos/b.mp4"];
    REQUIRE(b.current.segments.size() == 2);
    CHECK(b.current.segments[0].a == Approx(10));
    CHECK_FALSE(b.current.segments[0].enabled);
    CHECK(b.current.pending.has_b);
    CHECK(b.current.pending.b == Approx(45.0));

    REQUIRE(b.templates.size() == 2);
    REQUIRE(b.templates.count(1) == 1);
    CHECK(b.templates.at(1).segments.size() == 2);
    REQUIRE(b.templates.count(3) == 1);
    CHECK(b.templates.at(3).segments.size() == 1);
}

TEST_CASE("deserialize_archive returns an empty archive for malformed JSON", "[store][json]") {
    CHECK(deserialize_archive("not json").entries.empty());
    CHECK(deserialize_archive("[]").entries.empty()); // 不是期望的 {"entries": {...}} 形状
}

TEST_CASE("deserialize_archive reads an old-format entry (segments only, no pending/templates "
          "fields) as an empty pending and no templates instead of failing (backward "
          "compatibility for archives written before this feature existed)",
          "[store][json][compat]") {
    std::string old_format = R"({"entries": {"somekey": {"segments": [{"a":1.0,"b":2.0,"enabled":true}]}}})";

    auto archive = deserialize_archive(old_format);
    REQUIRE(archive.entries.count("somekey") == 1);
    const auto &entry = archive.entries["somekey"];
    REQUIRE(entry.current.segments.size() == 1);
    CHECK_FALSE(entry.current.pending.has_a);
    CHECK_FALSE(entry.current.pending.has_b);
    CHECK(entry.templates.empty());
}

TEST_CASE("lookup returns an exact match with zero ambiguity when the path is present",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/a.mp4"] = FileEntry{Snapshot{{Segment{0, 5, true}}, Pending{}}, {}};
    archive.entries["/videos/b.mp4"] = FileEntry{Snapshot{{Segment{10, 20, true}}, Pending{}}, {}};

    auto result = lookup(archive, "/videos/a.mp4");
    CHECK(result.kind == LookupResult::Kind::kExactMatch);
    CHECK(result.matched_key == "/videos/a.mp4");
    REQUIRE(result.entry.current.segments.size() == 1);
    CHECK(result.entry.current.segments[0].a == Approx(0));
}

TEST_CASE("lookup with exactly one candidate under a different path reports kSingleCandidate "
          "(likely a rename, needs user confirmation)",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/old-name.mp4"] = FileEntry{Snapshot{{Segment{0, 5, true}}, Pending{}}, {}};

    auto result = lookup(archive, "/videos/new-name.mp4");
    CHECK(result.kind == LookupResult::Kind::kSingleCandidate);
    CHECK(result.matched_key == "/videos/old-name.mp4");
    REQUIRE(result.entry.current.segments.size() == 1);
}

TEST_CASE("lookup with two or more candidates under different paths reports kNoArchive (genuine "
          "duplicate content, no safe automatic choice)",
          "[store][lookup]") {
    Archive archive;
    archive.entries["/videos/copy-1.mp4"] = FileEntry{Snapshot{{Segment{0, 5, true}}, Pending{}}, {}};
    archive.entries["/videos/copy-2.mp4"] = FileEntry{Snapshot{{Segment{10, 20, true}}, Pending{}}, {}};

    auto result = lookup(archive, "/videos/copy-3.mp4");
    CHECK(result.kind == LookupResult::Kind::kNoArchive);
    CHECK(result.entry.current.segments.empty());
}

TEST_CASE("lookup on an empty archive reports kNoArchive", "[store][lookup]") {
    Archive archive;
    auto result = lookup(archive, "/videos/anything.mp4");
    CHECK(result.kind == LookupResult::Kind::kNoArchive);
}

TEST_CASE("upsert without rename_from just adds/overwrites the current path's entry",
          "[store][upsert]") {
    Archive archive;
    upsert(archive, "/videos/a.mp4", FileEntry{Snapshot{{Segment{0, 5, true}}, Pending{}}, {}}, std::nullopt);

    REQUIRE(archive.entries.size() == 1);
    CHECK(archive.entries.count("/videos/a.mp4") == 1);
}

TEST_CASE("upsert with rename_from migrates the old path's entry to the new path instead of "
          "accumulating an orphan (regression test for repeated-rename entry bloat)",
          "[store][upsert]") {
    Archive archive;
    archive.entries["/videos/old-name.mp4"] = FileEntry{Snapshot{{Segment{0, 5, true}}, Pending{}}, {}};

    upsert(archive, "/videos/new-name.mp4",
           FileEntry{Snapshot{{Segment{0, 5, true}, Segment{10, 20, true}}, Pending{}}, {}},
           std::string("/videos/old-name.mp4"));

    REQUIRE(archive.entries.size() == 1); // 旧路径没有留下孤儿
    CHECK(archive.entries.count("/videos/old-name.mp4") == 0);
    REQUIRE(archive.entries.count("/videos/new-name.mp4") == 1);
    CHECK(archive.entries["/videos/new-name.mp4"].current.segments.size() == 2);
}

TEST_CASE("repeated rename-and-save cycles never grow past a single entry", "[store][upsert]") {
    Archive archive;
    auto make_entry = [](double a, double b) {
        return FileEntry{Snapshot{{Segment{a, b, true}}, Pending{}}, {}};
    };
    upsert(archive, "/videos/v1.mp4", make_entry(0, 5), std::nullopt);
    upsert(archive, "/videos/v2.mp4", make_entry(0, 5), std::string("/videos/v1.mp4"));
    upsert(archive, "/videos/v3.mp4", make_entry(0, 5), std::string("/videos/v2.mp4"));
    upsert(archive, "/videos/v4.mp4", make_entry(0, 5), std::string("/videos/v3.mp4"));

    REQUIRE(archive.entries.size() == 1);
    CHECK(archive.entries.count("/videos/v4.mp4") == 1);
}

TEST_CASE("upsert preserves the templates map passed in the FileEntry", "[store][upsert][template]") {
    Archive archive;
    FileEntry entry;
    entry.current.segments = {Segment{0, 5, true}};
    entry.templates[1] = Snapshot{{Segment{0, 5, true}}, Pending{}};
    entry.templates[5] = Snapshot{{Segment{0, 5, false}}, Pending{}};

    upsert(archive, "/videos/a.mp4", entry, std::nullopt);

    REQUIRE(archive.entries["/videos/a.mp4"].templates.size() == 2);
    CHECK(archive.entries["/videos/a.mp4"].templates.count(1) == 1);
    CHECK(archive.entries["/videos/a.mp4"].templates.count(5) == 1);
}
