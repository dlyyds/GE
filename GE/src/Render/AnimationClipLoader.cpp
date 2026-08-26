/**
 * @file AnimationClipLoader.cpp
 * @brief .geanim 烘焙格式实现 —— 扁平二进制，magic + version + 顺序数据段。
 */

#include "Render/AnimationClipLoader.h"

#include "Core/Log.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace GE {

namespace {

constexpr char     kMagic[] = "GEANIM";
constexpr uint16_t kVersion = 1;
constexpr size_t   kMaxChannels = 4096;   ///< 单 clip channel 上限（防御性）
constexpr size_t   kMaxKeyCount = 1u << 24; ///< 单 channel 键帧数上限（防御性）
constexpr size_t   kMaxNameLen = 1u << 16;  ///< 动画名长度上限（防御性）

// 小端字节流写入助手（无对齐要求，float 直接按内存字节写入）
struct Writer {
    std::vector<uint8_t> buf;

    void U8(uint8_t v)  { buf.push_back(v); }
    void U16(uint16_t v) { buf.push_back(static_cast<uint8_t>(v & 0xFF));
                           buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF)); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
    void I32(int32_t v)  { U32(static_cast<uint32_t>(v)); }
    void F32(float v)    { const auto *p = reinterpret_cast<const uint8_t *>(&v);
                           buf.insert(buf.end(), p, p + sizeof(float)); }
    void Raw(const void *data, size_t n) { const auto *p = static_cast<const uint8_t *>(data);
                                           buf.insert(buf.end(), p, p + n); }
    void Str(const std::string &s) { U32(static_cast<uint32_t>(s.size()));
                                     if (!s.empty()) Raw(s.data(), s.size()); }
};

// 字节流读取助手：每个读取都做边界校验，越界返回 false（调用方据此判损坏）
struct Reader {
    const std::vector<uint8_t> &buf;
    size_t pos = 0;

    explicit Reader(const std::vector<uint8_t> &b) : buf(b) {}

    bool InRange(size_t n) const { return pos + n <= buf.size(); }

    bool U8(uint8_t &v)  { if (!InRange(1)) return false; v = buf[pos++]; return true; }
    bool U16(uint16_t &v) { if (!InRange(2)) return false;
                            v = static_cast<uint16_t>(buf[pos]) | static_cast<uint16_t>(buf[pos + 1] << 8);
                            pos += 2; return true; }
    bool U32(uint32_t &v) { if (!InRange(4)) return false;
                            v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(buf[pos + i]) << (8 * i);
                            pos += 4; return true; }
    bool I32(int32_t &v) { uint32_t u; if (!U32(u)) return false; v = static_cast<int32_t>(u); return true; }
    bool F32(float &v)   { if (!InRange(4)) return false;
                           std::memcpy(&v, buf.data() + pos, 4); pos += 4; return true; }
    bool Raw(void *data, size_t n) { if (!InRange(n)) return false;
                                     std::memcpy(data, buf.data() + pos, n); pos += n; return true; }
    bool Str(std::string &s) { uint32_t len = 0; if (!U32(len) || len > kMaxNameLen) return false;
                               if (!InRange(len)) return false; // 防截断文件越界读
                               s.assign(reinterpret_cast<const char *>(buf.data() + pos),
                                        static_cast<size_t>(len));
                               pos += len; return true; }
};

// channel 键帧遍历的公共写入：先 times，再按路径写对应值数组
// 一致性失败（times 与值数量不符 / 空键帧）返回 false，由调用方中止序列化
bool WriteChannel(Writer &w, const AnimationChannel &ch, std::string *err) {
    const size_t keyCount = ch.times.size();
    const bool isRotation = (ch.path == AnimationChannel::Path::Rotation);
    const size_t valueCount = isRotation ? ch.quatKeys.size() : ch.vecKeys.size();
    if (keyCount == 0 || valueCount != keyCount) {
        if (err) {
            *err = "channel 键帧数不一致（times " + std::to_string(keyCount)
                   + " vs values " + std::to_string(valueCount) + "）";
        }
        return false;
    }

    w.I32(ch.nodeIndex);
    w.U8(static_cast<uint8_t>(ch.path));
    w.U8(static_cast<uint8_t>(ch.interp));
    w.U16(0); // 对齐保留
    w.U32(static_cast<uint32_t>(keyCount));
    for (size_t i = 0; i < keyCount; ++i) {
        w.F32(ch.times[i]);
    }
    if (isRotation) {
        for (size_t i = 0; i < keyCount; ++i) {
            const glm::quat &q = ch.quatKeys[i];
            w.F32(q.w); w.F32(q.x); w.F32(q.y); w.F32(q.z);
        }
    } else {
        for (size_t i = 0; i < keyCount; ++i) {
            const glm::vec3 &v = ch.vecKeys[i];
            w.F32(v.x); w.F32(v.y); w.F32(v.z);
        }
    }
    return true;
}

} // namespace

// ============================================================================
// 序列化：写文件
// ============================================================================

bool SerializeAnimationClip(const std::string &outPath, const AnimationClip &clip,
                            std::string *err) {
    if (clip.channels.size() > kMaxChannels) {
        if (err) {
            *err = "channel 数超上限";
        }
        return false;
    }

    Writer w;
    w.Raw(kMagic, 6);
    w.U16(kVersion);
    w.U16(0); // reserved
    w.F32(clip.duration);
    w.Str(clip.name);
    w.U32(static_cast<uint32_t>(clip.channels.size()));
    for (const auto &ch : clip.channels) {
        if (!WriteChannel(w, ch, err)) {
            return false; // 键帧一致性失败：不产出垃圾文件
        }
    }

    std::ofstream fout(outPath, std::ios::binary | std::ios::trunc);
    if (!fout.is_open()) {
        if (err) {
            *err = "无法打开输出文件: " + outPath;
        }
        GE_CORE_ERROR("[GEAnim] 序列化失败，无法打开: {0}", outPath);
        return false;
    }
    fout.write(reinterpret_cast<const char *>(w.buf.data()),
               static_cast<std::streamsize>(w.buf.size()));
    fout.close();
    GE_CORE_INFO("[GEAnim] 已写出 {0}（{1} channel，时长 {2:.3f}s）",
                 outPath, clip.channels.size(), clip.duration);
    return true;
}

// ============================================================================
// 反序列化：读取 + 完整边界校验
// ============================================================================

bool ParseAnimationClip(const std::string &filepath, AnimationClip &out, std::string *err) {
    std::ifstream fin(filepath, std::ios::binary | std::ios::ate);
    if (!fin.is_open()) {
        GE_CORE_ERROR("[GEAnim] 无法打开文件: {0}", filepath);
        return false;
    }
    const std::streamsize fileSize = fin.tellg();
    if (fileSize <= 0) {
        GE_CORE_ERROR("[GEAnim] 文件为空: {0}", filepath);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
    fin.seekg(0, std::ios::beg);
    fin.read(reinterpret_cast<char *>(buf.data()), fileSize);
    fin.close();

    Reader r(buf);
    char magic[6];
    if (!r.Raw(magic, 6) || std::memcmp(magic, kMagic, 6) != 0) {
        if (err) *err = "非 GEANIM 文件";
        GE_CORE_ERROR("[GEAnim] 非 GEANIM 文件: {0}", filepath);
        return false;
    }
    uint16_t version = 0, reserved = 0;
    if (!r.U16(version) || version != kVersion) {
        if (err) *err = "geanim 版本不支持";
        GE_CORE_ERROR("[GEAnim] 版本不支持: {0} (文件 {1}, 当前 {2})", filepath, version, kVersion);
        return false;
    }
    if (!r.U16(reserved)) {
        return false;
    }
    if (!r.F32(out.duration) || !r.Str(out.name)) {
        return false;
    }

    uint32_t channelCount = 0;
    if (!r.U32(channelCount) || channelCount > kMaxChannels) {
        if (err) *err = "channel 数异常";
        GE_CORE_ERROR("[GEAnim] channel 数异常: {0}", filepath);
        return false;
    }
    out.channels.clear();
    out.channels.reserve(channelCount);
    for (uint32_t ci = 0; ci < channelCount; ++ci) {
        AnimationChannel ch;
        int32_t nodeIndex = 0;
        uint8_t pathRaw = 0, interpRaw = 0, padHi = 0, padLo = 0;
        if (!r.I32(nodeIndex) || !r.U8(pathRaw) || !r.U8(interpRaw)
            || !r.U8(padHi) || !r.U8(padLo)) {
            return false;
        }
        if (pathRaw > static_cast<uint8_t>(AnimationChannel::Path::Scale)
            || interpRaw > static_cast<uint8_t>(AnimationChannel::Interp::CubicSpline)) {
            if (err) *err = "channel 枚举非法";
            GE_CORE_ERROR("[GEAnim] channel 枚举非法: {0} (channel {1})", filepath, ci);
            return false;
        }
        ch.nodeIndex = nodeIndex;
        ch.path = static_cast<AnimationChannel::Path>(pathRaw);
        ch.interp = static_cast<AnimationChannel::Interp>(interpRaw);

        uint32_t keyCount = 0;
        if (!r.U32(keyCount) || keyCount == 0 || keyCount > kMaxKeyCount) {
            if (err) *err = "键帧数异常";
            GE_CORE_ERROR("[GEAnim] 键帧数异常: {0} (channel {1})", filepath, ci);
            return false;
        }
        ch.times.resize(keyCount);
        for (size_t i = 0; i < keyCount; ++i) {
            if (!r.F32(ch.times[i])) {
                return false;
            }
        }
        if (ch.path == AnimationChannel::Path::Rotation) {
            ch.quatKeys.resize(keyCount);
            for (size_t i = 0; i < keyCount; ++i) {
                float w = 0, x = 0, y = 0, z = 0;
                if (!r.F32(w) || !r.F32(x) || !r.F32(y) || !r.F32(z)) {
                    return false;
                }
                ch.quatKeys[i] = glm::quat(w, x, y, z);
            }
        } else {
            ch.vecKeys.resize(keyCount);
            for (size_t i = 0; i < keyCount; ++i) {
                if (!r.F32(ch.vecKeys[i].x) || !r.F32(ch.vecKeys[i].y) || !r.F32(ch.vecKeys[i].z)) {
                    return false;
                }
            }
        }
        out.channels.push_back(std::move(ch));
    }

    // 长度一致性兜底：keyCount 与值数组由写入方维护，读取端已按 keyCount 校验边界
    return true;
}

// ============================================================================
// 烘焙路径推导（与 .gemesh 同目录同命名约定）
// ============================================================================

std::string DeriveAnimationBakePath(const std::string &sourceKey) {
    std::string filePath = sourceKey;
    size_t animIdx = 0;
    const size_t hashPos = sourceKey.rfind('#');
    if (hashPos != std::string::npos) {
        filePath = sourceKey.substr(0, hashPos);
        try {
            animIdx = static_cast<size_t>(std::stoull(sourceKey.substr(hashPos + 1)));
        } catch (...) {
            animIdx = 0; // 后缀非法按 anim 0 处理
        }
    }
    const std::filesystem::path src(filePath);
    if (animIdx > 0) {
        return (src.parent_path() /
                (src.stem().string() + "_" + std::to_string(animIdx) + ".geanim")).string();
    }
    return (src.parent_path() / (src.stem().string() + ".geanim")).string();
}

} // namespace GE