#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "enhanced_ab_loop/logic.h"

// SPEC.md §6：存档层。这里只处理纯数据（哈希计算、JSON 序列化、消歧规则），
// 不做任何文件 IO——真实的“读文件头尾字节”“展开 ~~/loop-segments 路径”
// “读写磁盘文件”都留在 plugin.cpp 里的 mpv 交互层，方便这里的逻辑不依赖
// mpv、可以直接用 Catch2 测。
namespace enhanced_ab_loop::store {

// 内容采样哈希：不读全文件，只用文件大小 + 头部/尾部采样字节。纯函数，输入
// 是调用方已经读好的字节样本，方便测试稳定性。
std::string compute_content_hash(std::uint64_t file_size, std::string_view head_sample,
                                  std::string_view tail_sample);

// 从完整路径里取出文件名部分（不含目录）。纯字符串处理，同时认 '/' 和
// '\\' 两种分隔符（Windows 下 mpv 路径也可能带反斜杠），不做任何路径规范化
// 或文件系统访问（不解析 . / ..，不追符号链接）。找不到分隔符时整个输入就
// 是文件名，原样返回。
std::string extract_filename(std::string_view path);

// 路径 key 哈希：调用方传入的是 extract_filename() 的输出（文件名），不是
// 完整绝对路径——绝对路径本身既不会明文写进存档文件（可能暴露目录结构、
// 用户名、媒体库组织方式），也不适合直接做 key：移动硬盘/NAS 每次挂载点
// 名字可能不一样，同一个文件的绝对路径会跟着变，导致存档在换挂载点后整体
// 失联。文件名不受挂载点影响，只要不手动改文件名就能稳定命中；同一内容哈希
// 文件里的重复文件本来就要求用不同文件名区分（见 SPEC.md §6），文件名天然
// 满足这个消歧需求。调用方必须自己先把真实路径转成这个 key 再传给下面的
// lookup/upsert/Archive——这一层不知道、也不需要知道真实路径长什么样。
std::string compute_path_hash(std::string_view path);

// 给定文件大小和采样窗口上限，算出头/尾各应该读多少字节、尾部从哪个偏移
// 开始读——纯算术，不做文件 IO，方便覆盖“文件比采样窗口还小”这类边界。
// 文件小于两倍采样窗口时头尾会重叠（文件足够小时甚至完全相同，相当于把
// 整个文件都采样了一遍），这是刻意允许的行为，不是 bug：此时哈希反而更
// 精确（等价于全文件哈希），不会因为“采样”而丢失区分度。
struct SampleRanges {
    std::uint64_t head_len = 0;
    std::uint64_t tail_offset = 0;
    std::uint64_t tail_len = 0;
};

SampleRanges compute_sample_ranges(std::uint64_t file_size, std::uint64_t max_sample_bytes);

// 一个内容哈希文件内部的结构：路径 key（compute_path_hash 的输出，不是明文
// 文件名）-> segments。
struct Archive {
    std::map<std::string, std::vector<Segment>> entries;
};

std::string serialize_segments(const std::vector<Segment> &segments);
// 解析失败（格式非法/不是数组）按空列表处理，不抛异常给调用方。
std::vector<Segment> deserialize_segments(const std::string &json_text);

std::string serialize_archive(const Archive &archive);
// 解析失败（格式非法）按空 archive 处理，不抛异常给调用方。
Archive deserialize_archive(const std::string &json_text);

struct LookupResult {
    enum class Kind {
        kExactMatch,      // 当前路径 key 精确命中，直接用
        kSingleCandidate, // 没命中，但唯一候选——大概率是同一文件改名了，
                          // 调用方应该提示用户确认后再用（提示文案不能带
                          // 明文路径，调用方手上也只有 key，没有路径原文）
        kNoArchive,       // 没命中，且候选数量是 0 或 >=2，按没有存档处理
    };

    Kind kind = Kind::kNoArchive;
    std::string matched_key; // kExactMatch/kSingleCandidate 时是命中的那条
                             // entry 的 key（哈希，不是明文路径），upsert
                             // 迁移时要传回这个值
    std::vector<Segment> segments;
};

LookupResult lookup(const Archive &archive, const std::string &current_path_key);

// 保存：rename_from 有值时（对应 lookup 返回 kSingleCandidate、用户确认过
// 之后，传入 LookupResult::matched_key），把旧 key 那条 entry 迁移成当前
// path key，不会让 entries 因为反复改名越攒越多；没有值就是普通的按当前
// path key upsert。
void upsert(Archive &archive, const std::string &current_path_key, const std::vector<Segment> &segments,
            std::optional<std::string> rename_from);

} // namespace enhanced_ab_loop::store
