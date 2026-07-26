#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "split_zoom_box/layout.h"

// 存档层。整套两级 key 方案、采样哈希和消歧规则直接沿用 enhanced-ab-loop 的
// store（那边已经在真机上验证过），这里只把载荷从 A/B 区间换成分屏布局。
//
// 和那边一样：这一层只处理纯数据（哈希计算、序列化、消歧），不做任何文件
// IO——读文件头尾字节、展开 ~~home/ 路径、读写磁盘都留在 plugin.cpp，
// 好让这里的逻辑不依赖 mpv、可以直接用 Catch2 测。
namespace split_zoom_box::store {

// 内容采样哈希：不读全文件，只用文件大小 + 头部/尾部采样字节。
std::string compute_content_hash(std::uint64_t file_size, std::string_view head_sample,
                                  std::string_view tail_sample);

std::string extract_filename(std::string_view path);

// 次级 key 用的是**文件名**的哈希，不是完整绝对路径：绝对路径会暴露目录
// 结构/用户名/媒体库组织方式，而且移动硬盘、NAS 换挂载点后整体失效。文件名
// 不受挂载点影响。调用方必须自己先 extract_filename 再传进来。
std::string compute_path_hash(std::string_view path);

struct SampleRanges {
    std::uint64_t head_len = 0;
    std::uint64_t tail_offset = 0;
    std::uint64_t tail_len = 0;
};

// 文件小于两倍采样窗口时头尾会重叠（足够小时完全相同，等价于全文件哈希），
// 这是刻意允许的行为：此时哈希反而更精确。
SampleRanges compute_sample_ranges(std::uint64_t file_size, std::uint64_t max_sample_bytes);

// 布局树序列化成嵌套 JSON（而不是把 arena 的下标直接写出去）：下标是内存
// 表示的实现细节，一旦以后改了节点回收策略，存档就会跟着失效。
std::string serialize_layout(const Layout &layout);
// 解析失败按"单窗格 + 完整画面"处理，不抛异常给调用方。
Layout deserialize_layout(const std::string &json_text);

// 一个文件的存档条目。base 是不落在任何时间段内时生效的布局——没有配置任何
// 时间段时，它就是全程生效的那个布局（等价于旧 drag-zoom-box 的持久缩放）。
struct FileEntry {
    Layout base;
    std::vector<LayoutSegment> segments;
};

// 两级 key：**内容采样哈希**定位存档**文件**，**文件名哈希**是文件内部区分
// 具体条目的次级 key。
struct Archive {
    std::map<std::string, FileEntry> entries;
};

std::string serialize_archive(const Archive &archive);
// 解析失败按空 archive 处理。缺字段的老格式条目按默认值读，不需要迁移脚本。
Archive deserialize_archive(const std::string &json_text);

struct LookupResult {
    enum class Kind {
        kExactMatch,      // 路径 key 精确命中
        kSingleCandidate, // 没命中但只有唯一候选，大概率是同一文件改了名，
                          // 调用方应提示用户确认（提示文案不能带明文路径）
        kNoArchive,       // 没命中且候选是 0 个或 >=2 个
    };

    Kind kind = Kind::kNoArchive;
    std::string matched_key;
    FileEntry entry;
};

LookupResult lookup(const Archive &archive, const std::string &current_path_key);

// rename_from 有值时把旧 key 那条迁移成当前 key，避免反复改名越攒越多。
void upsert(Archive &archive, const std::string &current_path_key, const FileEntry &entry,
            std::optional<std::string> rename_from);

} // namespace split_zoom_box::store
