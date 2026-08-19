/**
 * @file GEMeshLoader.cpp
 * @brief .gemesh 引擎内置网格格式 —— 序列化 / 反序列化实现。
 */

#include "Render/GEMeshLoader.h"

#include "Core/Log.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdint>

namespace GE {

namespace {

// ============================================================================
// 格式常量
// ============================================================================

constexpr char      kMagic[5] = {'G', 'E', 'M', 'S', 'H'};
constexpr uint16_t  kVersion  = 1;
constexpr uint32_t  kAlign    = 4; ///< chunk 起点对齐字节数

// chunk id
constexpr uint32_t kChunkVertices  = 1;
constexpr uint32_t kChunkIndices   = 2;
constexpr uint32_t kChunkSubMeshes = 3;
constexpr uint32_t kChunkMaterials = 4;
constexpr uint32_t kChunkMeta      = 5;

/// 空字符串在池中的引用值（-1 = 无字符串）
constexpr int32_t kNoString = -1;

static_assert(sizeof(glm::vec3) == 12, "glm::vec3 尺寸与格式不符");
static_assert(sizeof(Vertex) == 48, "Vertex 布局非 48B，.gemesh 顶点流编码失效");

// ============================================================================
// Writer：顺序字节流，支持对齐补齐与整型/浮点写入（固定 little-endian 需逐字节）
// ============================================================================

class Writer {
public:
    void AlignTo(uint32_t a) {
        while (m_buf.size() % a != 0) {
            m_buf.push_back(0);
        }
    }
    void WriteRaw(const void *ptr, size_t n) {
        const auto *p = static_cast<const uint8_t *>(ptr);
        m_buf.insert(m_buf.end(), p, p + n);
    }
    void WriteU8(uint8_t v) { WriteRaw(&v, 1); }
    void WriteU32(uint32_t v) {
        uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        WriteRaw(b, 4);
    }
    void WriteI32(int32_t v) { WriteU32(static_cast<uint32_t>(v)); }
    void WriteU16(uint16_t v) {
        uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        WriteRaw(b, 2);
    }
    void WriteU64(uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<uint8_t>(v >> (8 * i));
        }
        WriteRaw(b, 8);
    }
    void WriteF32(float v) {
        static_assert(sizeof(float) == 4, "float 非 4B");
        uint32_t u;
        std::memcpy(&u, &v, 4);
        WriteU32(u);
    }
    void WriteVec3(const glm::vec3 &v) {
        WriteF32(v.x);
        WriteF32(v.y);
        WriteF32(v.z);
    }
    /// 字符串池：把 string 追加到池末尾，返回索引（负数表示空）。
    int32_t AddString(const std::string &s) {
        if (s.empty()) {
            return kNoString;
        }
        int32_t idx = static_cast<int32_t>(m_strs.size());
        m_strs.push_back(s);
        return idx;
    }
    /// 将累积的字符串池写入（strCount + 每项 [len, bytes 对齐]）。
    void WriteStrPool() {
        WriteU32(static_cast<uint32_t>(m_strs.size()));
        for (const auto &s : m_strs) {
            WriteU32(static_cast<uint32_t>(s.size()));
            WriteRaw(s.data(), s.size());
            // 变长字符串后补齐到 4 字节，使下一项 len 字段对齐
            AlignTo(kAlign);
        }
        m_strs.clear();
    }
    const std::vector<uint8_t> &Buf() const { return m_buf; }
    size_t Size() const { return m_buf.size(); }

private:
    std::vector<uint8_t> m_buf;
    std::vector<std::string> m_strs;
};

// ============================================================================
// Reader：带边界校验的顺序字节流读取（防止损坏文件的越界读）
// ============================================================================

class Reader {
public:
    Reader(const std::vector<uint8_t> &buf, size_t from, size_t size)
        : m_buf(buf), m_pos(from), m_end(from + size) {
    }

    bool InRange(size_t n) const { return m_pos + n <= m_end; }

    /// 读取 n 字节到 out；越界返回 false 且不动游标。
    bool Raw(void *out, size_t n) {
        if (!InRange(n)) {
            return false;
        }
        std::memcpy(out, m_buf.data() + m_pos, n);
        m_pos += n;
        return true;
    }
    bool U8(uint8_t &v) { return Raw(&v, 1); }
    bool U16(uint16_t &v) {
        uint8_t b[2];
        if (!Raw(b, 2)) {
            return false;
        }
        v = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
        return true;
    }
    bool U32(uint32_t &v) {
        uint8_t b[4];
        if (!Raw(b, 4)) {
            return false;
        }
        v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
            | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }
    bool I32(int32_t &v) {
        uint32_t u;
        if (!U32(u)) {
            return false;
        }
        v = static_cast<int32_t>(u);
        return true;
    }
    bool U64(uint64_t &v) {
        uint8_t b[8];
        if (!Raw(b, 8)) {
            return false;
        }
        v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | b[i];
        }
        return true;
    }
    bool F32(float &v) {
        uint32_t u;
        if (!U32(u)) {
            return false;
        }
        std::memcpy(&v, &u, 4);
        return true;
    }
    bool Vec3(glm::vec3 &v) {
        return F32(v.x) && F32(v.y) && F32(v.z);
    }
    /// 读取字符串池（strCount + 每项 [len, bytes]），返回字符串列表。
    bool ReadStrPool(std::vector<std::string> &out) {
        uint32_t count = 0;
        if (!U32(count)) {
            return false;
        }
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t len = 0;
            if (!U32(len)) {
                return false;
            }
            if (!InRange(len)) {
                return false;
            }
            std::string s(reinterpret_cast<const char *>(m_buf.data() + m_pos), len);
            m_pos += len;
            out.push_back(std::move(s));
            // 跳过字符串后的补齐字节
            const size_t pad = (kAlign - (len % kAlign)) % kAlign;
            if (!InRange(pad)) {
                return false;
            }
            m_pos += pad;
        }
        return true;
    }

    size_t Pos() const { return m_pos; }

private:
    const std::vector<uint8_t> &m_buf;
    size_t m_pos;
    size_t m_end;
};

// ============================================================================
// chunk 表项：写在文件开头的定位信息
// ============================================================================

struct ChunkEntry {
    uint32_t id;
    uint64_t offset;
    uint64_t size;
};

void WriteChunkEntry(Writer &out, uint32_t id, uint64_t offset, uint64_t size) {
    out.WriteU32(id);
    out.WriteU32(0); // reserved 对齐到 16B
    out.WriteU64(offset);
    out.WriteU64(size);
}

// ============================================================================
// 序列化：把各 chunk 装进 Writer，返回完整文件字节
// ============================================================================

std::vector<uint8_t> BuildFile(const MeshData &data, const GEMeshMeta &meta) {
    // 先单独构建每个 chunk 的字节（writer），需要知道其大小以填 chunk 表。
    // 每个 chunk 独立 build：起点对齐 + 内容。chunk 表本身在文件头。
    std::vector<uint8_t> verts, indices, subs, mats, met;
    {
        Writer w;
        w.AlignTo(kAlign);
        w.WriteU32(static_cast<uint32_t>(data.vertices.size()));
        w.WriteRaw(data.vertices.data(), data.vertices.size() * sizeof(Vertex));
        verts = w.Buf();
    }
    {
        Writer w;
        w.AlignTo(kAlign);
        w.WriteU32(static_cast<uint32_t>(data.indices.size()));
        w.WriteRaw(data.indices.data(), data.indices.size() * sizeof(uint32_t));
        indices = w.Buf();
    }
    {
        Writer w;
        w.AlignTo(kAlign);
        w.WriteU32(static_cast<uint32_t>(data.subMeshes.size()));
        // materialName 字符串池：先收集所有引用，再写池，再写子网格项
        std::vector<int32_t> refs;
        refs.reserve(data.subMeshes.size());
        for (const auto &sm : data.subMeshes) {
            refs.push_back(w.AddString(sm.materialName));
        }
        w.WriteStrPool();
        // 子网格项
        for (size_t i = 0; i < data.subMeshes.size(); ++i) {
            const auto &sm = data.subMeshes[i];
            w.WriteU32(sm.firstVertex);
            w.WriteU32(sm.vertexCount);
            w.WriteU32(sm.firstIndex);
            w.WriteU32(sm.indexCount);
            w.WriteI32(refs[i]);
        }
        subs = w.Buf();
    }
    {
        // 材质项与字符串池：需要先收集所有 MaterialData 的字符串。
        Writer w;
        w.AlignTo(kAlign);
        w.WriteU32(static_cast<uint32_t>(data.materialData.size()));
        // 收集全部字符串引用（顺序写入），字符串本身入池
        std::vector<int32_t> refs;
        for (const auto &md : data.materialData) {
            refs.push_back(w.AddString(md.name));
            refs.push_back(w.AddString(md.albedoMap));
            refs.push_back(w.AddString(md.normalMap));
            refs.push_back(w.AddString(md.emissiveMap));
            refs.push_back(w.AddString(md.metallicMap));
            refs.push_back(w.AddString(md.roughnessMap));
        }
        w.WriteStrPool();
        // 材质项
        for (size_t i = 0; i < data.materialData.size(); ++i) {
            const auto &md = data.materialData[i];
            w.WriteVec3(md.baseColor);
            w.WriteVec3(md.specular);
            w.WriteVec3(md.emissive);
            w.WriteF32(md.shininess);
            w.WriteF32(md.dissolve);
            w.WriteF32(md.metallic);
            w.WriteF32(md.roughness);
            w.WriteU32(md.hasPBR ? 1u : 0u);
            w.WriteI32(refs[i * 6 + 0]);
            w.WriteI32(refs[i * 6 + 1]);
            w.WriteI32(refs[i * 6 + 2]);
            w.WriteI32(refs[i * 6 + 3]);
            w.WriteI32(refs[i * 6 + 4]);
            w.WriteI32(refs[i * 6 + 5]);
        }
        mats = w.Buf();
    }
    {
        Writer w;
        w.AlignTo(kAlign);
        w.WriteVec3(meta.aabbMin);
        w.WriteVec3(meta.aabbMax);
        (void)w.AddString(meta.sourceAsset);
        w.WriteStrPool();
        // source 引用（索引 0 即第一个入池的 sourceAsset，若空则 -1）
        int32_t srcIdx = meta.sourceAsset.empty() ? kNoString : 0;
        w.WriteI32(srcIdx);
        met = w.Buf();
    }

    // ---- 组装文件头 + chunk 表 + chunk 数据 ----
    Writer out;
    out.WriteRaw(kMagic, 5);
    out.WriteU16(kVersion);
    out.WriteU8(0); // reserved

    struct Pending { uint32_t id; const std::vector<uint8_t> *bytes; };
    const std::vector<Pending> chunks = {
        {kChunkVertices, &verts}, {kChunkIndices, &indices},
        {kChunkSubMeshes, &subs}, {kChunkMaterials, &mats}, {kChunkMeta, &met},
    };
    const size_t tableOffset = 8;
    const size_t headerSize = tableOffset + 4 + chunks.size() * sizeof(ChunkEntry);

    // 先算各 chunk 的布局偏移（每个 chunk 起点对齐），再写入 chunk 表与数据
    uint64_t cursor = headerSize;
    std::vector<uint64_t> offsets(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        // 每个 chunk 起点对齐
        cursor = (cursor + kAlign - 1) / kAlign * kAlign;
        offsets[i] = cursor;
        cursor += chunks[i].bytes->size();
    }

    // 写头部 magic/version/reserved + chunk 表
    out.WriteU32(static_cast<uint32_t>(chunks.size()));
    for (size_t i = 0; i < chunks.size(); ++i) {
        WriteChunkEntry(out, chunks[i].id, offsets[i], chunks[i].bytes->size());
    }
    // 写 chunk 数据：每个 chunk 前对齐，与 offsets 计算规则（每 chunk 前 align）一致
    for (size_t i = 0; i < chunks.size(); ++i) {
        out.AlignTo(kAlign);
        out.WriteRaw(chunks[i].bytes->data(), chunks[i].bytes->size());
    }

    return out.Buf();
}

} // namespace anonymous

// ============================================================================
// 序列化：写文件
// ============================================================================

bool SerializeGEMesh(const std::string &outPath, const MeshData &data,
                     const GEMeshMeta &meta, std::string *err) {
    if (data.vertices.empty() || data.indices.empty()) {
        if (err) {
            *err = "MeshData 无有效几何数据";
        }
        return false;
    }

    std::vector<uint8_t> bytes = BuildFile(data, meta);

    std::ofstream fout(outPath, std::ios::binary | std::ios::trunc);
    if (!fout.is_open()) {
        if (err) {
            *err = "无法打开输出文件: " + outPath;
        }
        GE_CORE_ERROR("[GEMesh] 序列化失败，无法打开: {0}", outPath);
        return false;
    }
    fout.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    fout.close();
    GE_CORE_INFO("[GEMesh] 已写出 {0}（{1} 顶点 / {2} 索引，{3} 子网格，{4} 材质）",
                 outPath, data.vertices.size(), data.indices.size(),
                 data.subMeshes.size(), data.materialData.size());
    return true;
}

// ============================================================================
// 反序列化：解析 chunk 表 + 各 chunk 到 MeshData
// ============================================================================

bool ParseGEMesh(const std::string &filepath, MeshData &out, GEMeshMeta *outMeta) {
    // 读入完整文件
    std::ifstream fin(filepath, std::ios::binary | std::ios::ate);
    if (!fin.is_open()) {
        GE_CORE_ERROR("[GEMesh] 无法打开文件: {0}", filepath);
        return false;
    }
    const std::streamsize fileSize = fin.tellg();
    if (fileSize <= 0) {
        GE_CORE_ERROR("[GEMesh] 文件为空: {0}", filepath);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
    fin.seekg(0, std::ios::beg);
    fin.read(reinterpret_cast<char *>(buf.data()), fileSize);
    fin.close();

    // ---- 头校验：magic + version ----
    if (buf.size() < 8 || std::memcmp(buf.data(), kMagic, 5) != 0) {
        GE_CORE_ERROR("[GEMesh] 非 GEMSH 文件: {0}", filepath);
        return false;
    }
    Reader head(buf, 5, buf.size() - 5);
    uint16_t version = 0;
    if (!head.U16(version) || version > kVersion) {
        GE_CORE_ERROR("[GEMesh] 版本不支持: {0} (文件 {1}, 当前 {2})", filepath, version, kVersion);
        return false;
    }
    uint8_t reserved = 0;
    if (!head.U8(reserved)) {
        return false;
    }

    // ---- chunk 表 ----
    uint32_t chunkCount = 0;
    if (!head.U32(chunkCount) || chunkCount > 64) {
        GE_CORE_ERROR("[GEMesh] chunk 表异常: {0}", filepath);
        return false;
    }
    struct Entry { uint32_t id; uint64_t offset, size; };
    std::vector<Entry> entries;
    entries.reserve(chunkCount);
    for (uint32_t i = 0; i < chunkCount; ++i) {
        uint32_t id = 0, rsv = 0;
        uint64_t off = 0, sz = 0;
        if (!head.U32(id) || !head.U32(rsv) || !head.U64(off) || !head.U64(sz)) {
            return false;
        }
        // 边界校验：offset+size 必须在文件范围内
        if (off > buf.size() || sz > buf.size() - off) {
            GE_CORE_ERROR("[GEMesh] chunk 越界 (id {0}): {1}", id, filepath);
            return false;
        }
        entries.push_back({id, off, sz});
    }

    auto findChunk = [&entries](uint32_t id) -> const Entry * {
        for (const auto &e : entries) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    };

    // ---- VERTICES ----
    if (const Entry *e = findChunk(kChunkVertices)) {
        Reader r(buf, e->offset, e->size);
        uint32_t count = 0;
        if (!r.U32(count) || !r.InRange(static_cast<size_t>(count) * sizeof(Vertex))) {
            GE_CORE_ERROR("[GEMesh] VERTICES chunk 越界: {0}", filepath);
            return false;
        }
        out.vertices.resize(count);
        r.Raw(out.vertices.data(), static_cast<size_t>(count) * sizeof(Vertex));
    }

    // ---- INDICES ----
    if (const Entry *e = findChunk(kChunkIndices)) {
        Reader r(buf, e->offset, e->size);
        uint32_t count = 0;
        if (!r.U32(count) || !r.InRange(static_cast<size_t>(count) * sizeof(uint32_t))) {
            GE_CORE_ERROR("[GEMesh] INDICES chunk 越界: {0}", filepath);
            return false;
        }
        out.indices.resize(count);
        r.Raw(out.indices.data(), static_cast<size_t>(count) * sizeof(uint32_t));
    }

    // ---- SUBMESHES ----
    if (const Entry *e = findChunk(kChunkSubMeshes)) {
        Reader r(buf, e->offset, e->size);
        uint32_t count = 0;
        if (!r.U32(count)) {
            return false;
        }
        std::vector<std::string> strPool;
        if (!r.ReadStrPool(strPool)) {
            GE_CORE_ERROR("[GEMesh] SUBMESHES 字符串池异常: {0}", filepath);
            return false;
        }
        out.subMeshes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            SubMesh sm;
            if (!r.U32(sm.firstVertex) || !r.U32(sm.vertexCount) ||
                !r.U32(sm.firstIndex) || !r.U32(sm.indexCount)) {
                return false;
            }
            int32_t nameIdx = 0;
            if (!r.I32(nameIdx)) {
                return false;
            }
            if (nameIdx >= 0 && static_cast<size_t>(nameIdx) < strPool.size()) {
                sm.materialName = strPool[static_cast<size_t>(nameIdx)];
            }
            out.subMeshes.push_back(sm);
        }
    }

    // ---- MATERIALS ----
    if (const Entry *e = findChunk(kChunkMaterials)) {
        Reader r(buf, e->offset, e->size);
        uint32_t count = 0;
        if (!r.U32(count)) {
            return false;
        }
        std::vector<std::string> strPool;
        if (!r.ReadStrPool(strPool)) {
            GE_CORE_ERROR("[GEMesh] MATERIALS 字符串池异常: {0}", filepath);
            return false;
        }
        out.materialData.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            MaterialData md;
            uint32_t has = 0;
            if (!r.Vec3(md.baseColor) || !r.Vec3(md.specular) || !r.Vec3(md.emissive) ||
                !r.F32(md.shininess) || !r.F32(md.dissolve) ||
                !r.F32(md.metallic) || !r.F32(md.roughness) || !r.U32(has)) {
                return false;
            }
            md.hasPBR = (has != 0);
            int32_t refs[6];
            for (int k = 0; k < 6; ++k) {
                if (!r.I32(refs[k])) {
                    return false;
                }
            }
            auto get = [&strPool](int32_t idx) -> std::string {
                if (idx >= 0 && static_cast<size_t>(idx) < strPool.size()) {
                    return strPool[static_cast<size_t>(idx)];
                }
                return {};
            };
            md.name         = get(refs[0]);
            md.albedoMap    = get(refs[1]);
            md.normalMap    = get(refs[2]);
            md.emissiveMap  = get(refs[3]);
            md.metallicMap  = get(refs[4]);
            md.roughnessMap = get(refs[5]);
            out.materialData.push_back(md);
        }
    }

    // ---- META ----
    if (outMeta) {
        if (const Entry *e = findChunk(kChunkMeta)) {
            Reader r(buf, e->offset, e->size);
            if (!r.Vec3(outMeta->aabbMin) || !r.Vec3(outMeta->aabbMax)) {
                return false;
            }
            std::vector<std::string> strPool;
            if (!r.ReadStrPool(strPool)) {
                return false;
            }
            int32_t srcIdx = 0;
            if (r.I32(srcIdx) && srcIdx >= 0 && static_cast<size_t>(srcIdx) < strPool.size()) {
                outMeta->sourceAsset = strPool[static_cast<size_t>(srcIdx)];
            }
        }
    }

    if (out.vertices.empty() || out.indices.empty()) {
        GE_CORE_ERROR("[GEMesh] 无有效几何数据: {0}", filepath);
        return false;
    }

    GE_CORE_TRACE("[GEMesh] 已加载 {0}（{1} 顶点 / {2} 索引，{3} 子网格，{4} 材质）",
                  filepath, out.vertices.size(), out.indices.size(),
                  out.subMeshes.size(), out.materialData.size());
    return true;
}

} // namespace GE
