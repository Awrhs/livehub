#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace streaming {

/**
 * Mp4Faststart: 把 MP4 文件末尾的 moov box 移动到文件前部（ftyp 之后），
 * 使播放器不必先下载整个文件即可开始解码（progressive playback）。
 *
 * 原理：
 *  1. 扫描顶层 box（ftyp/moov/mdat...），记录 moov 的偏移与长度。
 *  2. 把 moov 读入内存（通常较小），更新其中 stco/co64 chunk offset：
 *     因为 moov 位置从尾部前移，文件布局改变，mdat 起始偏移变化需要补偿。
 *  3. 重写文件：[ftyp][moov][其余 boxes 原样]。
 *
 * 返回 true 表示成功；失败返回 false（文件保持原样）。
 */
class Mp4Faststart {
public:
    static bool relocate(const std::string& path);

    /// 对内存中的 fMP4/MP4 字节做同样的 moov 前移（便于测试）
    static bool relocateBuffer(std::vector<uint8_t>& data);
};

}  // namespace streaming
