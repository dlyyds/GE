/**
 * @file Mesh.cpp
 * @brief 网格实现 —— 顶点/索引缓冲 + 多格式模型加载（当前支持 OBJ）。
 */

#include "Render/Mesh.h"
#include "Render/AsyncUploadManager.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanQueue.h"
#include "Core/Log.h"

#include "tiny_obj_loader.h"

#include <iostream>
#include <unordered_map>
#include <cmath>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace GE {

// ============================================================================
// 辅助：计算顶点切线（法线贴图需要 TBN 切线空间）
// ============================================================================

/**
 * @brief 根据三角形 (位置, UV) 计算每个顶点的切线向量。
 *
 * 标准算法：对每个三角形计算切线与副切线插值，累加到三个顶点上，
 * 最后按顶点的法线用 Gram-Schmidt 正交化得到最终的切线方向，
 * 并以 w 分量记录手性符号（用于在着色器中重建正确的副切线方向）。
 *
 * @param vertices 顶点数组（就地修改 Tangent 字段）
 * @param indices  索引数组（每 3 个一组构成三角形）
 */
static void ComputeTangents(std::vector<Vertex> &vertices,
                            const std::vector<uint32_t> &indices) {
    std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto &v0 = vertices[indices[i + 0]];
        const auto &v1 = vertices[indices[i + 1]];
        const auto &v2 = vertices[indices[i + 2]];

        glm::vec3 edge1 = v1.Position - v0.Position;
        glm::vec3 edge2 = v2.Position - v0.Position;
        glm::vec2 duv1 = v1.TexCoord - v0.TexCoord;
        glm::vec2 duv2 = v2.TexCoord - v0.TexCoord;

        float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);
        glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
        glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;

        tan1[indices[i + 0]] += tangent;
        tan1[indices[i + 1]] += tangent;
        tan1[indices[i + 2]] += tangent;
        tan2[indices[i + 0]] += bitangent;
        tan2[indices[i + 1]] += bitangent;
        tan2[indices[i + 2]] += bitangent;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        auto &v = vertices[i];
        glm::vec3 n = v.Normal;
        glm::vec3 t = tan1[i];

        // Gram-Schmidt 正交化：使切线垂直于法线
        t = glm::normalize(t - n * glm::dot(n, t));
        v.Tangent = glm::vec4(t, 1.0f);

        // 手性符号：根据切线/副切线/法线的相对方向判断左右手系
        float handedness = glm::dot(glm::cross(n, t), tan2[i]);
        v.Tangent.w = (handedness < 0.0f) ? -1.0f : 1.0f;
    }
}

/**
 * @brief 网格异步加载共享数据容器（decode / upload / finalize 三阶段间传递）。
 *
 * 由主线程组装参数，后台线程写入 decode / upload 结果，最终主线程 finalize 读取。
 * 经 shared_ptr 在阶段回调间共享，保证后台只产出局部对象（不持 Mesh 裸指针），
 * 所有写 Mesh 成员的动作都收敛到主线程 finalize。
 */
struct AsyncMeshLoadData {
    bool parsed = false; ///< OBJ/MTL 是否解析成功（decode 写入）

    // decode 输出（后台线程写入）
    std::vector<Vertex>       vertices;      ///< 顶点数组（已含切线）
    std::vector<uint32_t>     indices;       ///< 索引数组
    std::vector<SubMesh>      subMeshes;     ///< 子网格列表
    std::vector<MaterialData> materialData;  ///< 从 MTL 捕获的材质数据

    // upload 输出（后台线程写入，主线程 finalize 消费）
    std::unique_ptr<VulkanBuffer> stagingVB;    ///< 顶点 staging buffer（GPU 用完后随桶释放）
    std::unique_ptr<VulkanBuffer> stagingIB;    ///< 索引 staging buffer（GPU 用完后随桶释放）
    std::unique_ptr<VulkanBuffer> vertexBuffer; ///< 后台创建的本地顶点缓冲
    std::unique_ptr<VulkanBuffer> indexBuffer;  ///< 后台创建的本地索引缓冲
};

// ============================================================================
// 辅助：上传数据到 GPU buffer（staging buffer 方式）
// ============================================================================

static std::unique_ptr<VulkanBuffer> UploadBuffer(
    VulkanDevice &device,
    vk::BufferUsageFlagBits usage,
    vk::DeviceSize size,
    const void *data) {
    // 创建 staging buffer 并拷贝数据
    auto staging = VulkanBuffer::create_staging_buffer(device, size, data);

    // 创建目标 buffer（GPU 本地，用作顶点/索引缓冲 + 传输目标）
    auto dst = VulkanBufferBuilder(size)
        .with_usage(usage | vk::BufferUsageFlagBits::eTransferDst)
        .with_vma_usage(VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE)
        .build_unique(device);

    // 获取临时 command buffer
    auto &cmd = device.RequestCommandBuffer(vk::CommandBufferLevel::ePrimary, true);

    vk::BufferCopy copyRegion{};
    copyRegion.size = size;
    cmd.GetHandle().copyBuffer(staging.GetHandle(), dst->GetHandle(), copyRegion);

    // 提交并等待完成（按 command buffer 所属队列族提交）
    cmd.End();
    auto &queue = device.GetQueue(cmd.GetQueueFamilyIndex());
    device.FlushCommandBuffer(cmd, queue);

    // staging buffer 在此处自动析构
    return dst;
}

// ============================================================================
// 私有装配：由格式加载器解析出的数据构建最终网格（各格式共享路径）
// ============================================================================

std::unique_ptr<Mesh> Mesh::BuildMesh(VulkanDevice &device,
                                      std::vector<Vertex> vertices,
                                      std::vector<uint32_t> indices,
                                      std::vector<SubMesh> subMeshes,
                                      std::vector<MaterialData> materialData,
                                      std::string filePath) {
    if (vertices.empty() || indices.empty()) {
        return nullptr;
    }

    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_Vertices = std::move(vertices);
    mesh->m_Indices = std::move(indices);
    mesh->m_SubMeshes = std::move(subMeshes);
    mesh->m_MaterialData = std::move(materialData);

    // 计算顶点切线（法线贴图需要）
    ComputeTangents(mesh->m_Vertices, mesh->m_Indices);

    if (!mesh->UploadToGPU(device)) {
        return nullptr;
    }

    // 同步路径加载完成立即就绪（异步路径在 finalize 安装后才置就绪）
    mesh->m_Ready.store(true, std::memory_order_release);

    // 记录源文件路径
    mesh->m_FilePath = std::move(filePath);
    return mesh;
}

// ============================================================================
// 工厂方法：从文件加载（按扩展名分派到对应格式加载器）
// ============================================================================

std::unique_ptr<Mesh> Mesh::LoadFromFile(VulkanDevice &device,
                                         const std::string &filepath) {
    // 按扩展名分派；新增模型格式时，在这里注册对应的私有加载函数即可
    std::string ext = std::filesystem::path(filepath).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c += static_cast<char>('a' - 'A');
        }
    }

    if (ext == ".obj") {
        return LoadFromOBJ(device, filepath);
    }

    std::cerr << "[Mesh] Unsupported model format '" << ext << "': "
              << filepath << std::endl;
    return nullptr;
}

// ============================================================================
// 私有加载器：OBJ（tinyobjloader）
// ============================================================================

/**
 * @brief 从 OBJ 文件解析出几何与材质数据（纯 CPU，不含 GPU 上传）。
 *
 * 内容 = tinyobj 解析 + 顶点装配/量化/去重 + 子网格拆分 + MTL → MaterialData 捕获。
 * 由同步路径 LoadFromOBJ 与异步路径 Mesh::LoadFromFileAsync 的 decode 阶段共用，
 * 保证两套加载路径的解析逻辑完全一致（切线计算不在此处——同步在 BuildMesh、
 * 异步在 decode 中各自调用 ComputeTangents）。
 *
 * @param filepath      OBJ 文件路径
 * @param vertices      顶点数组（输出）
 * @param indices       索引数组（输出）
 * @param subMeshes     子网格列表（输出）
 * @param materialData  MTL 材质数据（输出）
 * @return 解析成功且含有效几何数据返回 true
 */
static bool ParseOBJData(const std::string &filepath,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices,
                         std::vector<SubMesh> &subMeshes,
                         std::vector<MaterialData> &materialData) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    // 以 OBJ 所在目录作为 MTL 搜索基准目录（默认搜工作目录，会导致模型目录
    // 下的 .mtl 找不到）。同时用于后续把 MTL 内的相对纹理路径拼成绝对路径。
    const std::string baseDir = std::filesystem::path(filepath).parent_path().string();
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                filepath.c_str(),
                                baseDir.empty() ? nullptr : baseDir.c_str());

    if (!warn.empty()) {
        std::cout << "[Mesh] Warning: " << warn << std::endl;
    }
    if (!err.empty()) {
        std::cerr << "[Mesh] Error loading '" << filepath << "': " << err << std::endl;
    }
    if (!ret) {
        return false;
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    // 遍历所有形状，几何合并到共享缓冲，并按 (shape, material_id) 拆分子网格。
    // OBJ 材质（material_id）是逐面绑定的，一个 shape（o/g 对象）内部可能含多种材质，
    // 故需在 shape 内按 material_id 对连续面分组，每组一个子网格。
    auto makeMaterialName = [&](int matId) -> std::string {
        if (matId >= 0 && static_cast<size_t>(matId) < materials.size()) {
            return materials[matId].name;
        }
        return {};
    };

    for (const auto &shape : shapes) {
        const auto &shapeIndices = shape.mesh.indices;
        const auto &materialIds = shape.mesh.material_ids;

        // 当前材质组：material_id 变化时封存上一个连续段为一个子网格
        int curMaterial = -2; // 哨兵值，表示 shape 起始
        uint32_t groupStartIndex = static_cast<uint32_t>(indices.size());

        for (size_t fi = 0; fi + 2 < shapeIndices.size(); fi += 3) {
            // material_ids 为空（无 MTL）时视为 -1
            int matId = materialIds.empty() ? -1 : materialIds[fi / 3];

            // 材质切换：封存上一个材质组为子网格
            if (fi > 0 && matId != curMaterial) {
                uint32_t firstIndex = groupStartIndex;
                uint32_t indexCount = static_cast<uint32_t>(indices.size()) - groupStartIndex;
                subMeshes.push_back({0, static_cast<uint32_t>(vertices.size()),
                                     firstIndex, indexCount,
                                     makeMaterialName(curMaterial), nullptr});
                groupStartIndex = static_cast<uint32_t>(indices.size());
            }
            curMaterial = matId;

            // 处理当前面的 3 个顶点
            for (int k = 0; k < 3; ++k) {
                const auto &index = shapeIndices[fi + k];
                Vertex v{};

                // 位置
                v.Position = {
                    attrib.vertices[3 * index.vertex_index + 0],
                    attrib.vertices[3 * index.vertex_index + 1],
                    attrib.vertices[3 * index.vertex_index + 2],
                };

                // 法线（如果有）
                if (index.normal_index >= 0) {
                    v.Normal = {
                        attrib.normals[3 * index.normal_index + 0],
                        attrib.normals[3 * index.normal_index + 1],
                        attrib.normals[3 * index.normal_index + 2],
                    };
                }

                // 纹理坐标（如果有）
                if (index.texcoord_index >= 0) {
                    v.TexCoord = {
                        attrib.texcoords[2 * index.texcoord_index + 0],
                        1 - attrib.texcoords[2 * index.texcoord_index + 1],
                    };
                }

                // 一次性量化清洗：消除浮点精度误差导致的「逻辑相同但位表示不同」的顶点，
                // 使下方去重的精确比较 / 精确哈希能正确判定（见 Vertex::Quantize 注释）
                v.Position = Vertex::Quantize(v.Position);
                v.Normal = Vertex::Quantize(v.Normal);
                v.TexCoord = Vertex::Quantize(v.TexCoord);
                v.Tangent = Vertex::Quantize(v.Tangent);

                // 去重：相同顶点复用索引
                if (uniqueVertices.find(v) == uniqueVertices.end()) {
                    uniqueVertices[v] = static_cast<uint32_t>(vertices.size());
                    vertices.push_back(v);
                }
                indices.push_back(uniqueVertices[v]);
            }
        }

        // 封存 shape 内最后一个材质组
        if (!shapeIndices.empty()) {
            uint32_t firstIndex = groupStartIndex;
            uint32_t indexCount = static_cast<uint32_t>(indices.size()) - groupStartIndex;
            subMeshes.push_back({0, static_cast<uint32_t>(vertices.size()),
                                 firstIndex, indexCount,
                                 makeMaterialName(curMaterial), nullptr});
        }
    }

    if (vertices.empty() || indices.empty()) {
        std::cerr << "[Mesh] Empty mesh loaded from '" << filepath << "'" << std::endl;
        return false;
    }

    // 从 tinyobj 解析出的 MTL 材质捕获为引擎侧 MaterialData。
    // 各子网格的 materialName 即对应 MTL 材质名，MeshManager 据此匹配构建材质。
    auto resolveTex = [&](const std::string &tex) -> std::string {
        if (tex.empty()) {
            return {};
        }
        std::filesystem::path p(tex);
        if (p.is_absolute()) {
            return p.lexically_normal().string();
        }
        return (std::filesystem::path(filepath).parent_path() / tex)
            .lexically_normal().string();
    };

    std::vector<MaterialData> materialData;
    materialData.reserve(materials.size());
    for (const auto &src : materials) {
        MaterialData md;
        md.name = src.name;
        md.baseColor = {src.diffuse[0], src.diffuse[1], src.diffuse[2]};
        md.specular = {src.specular[0], src.specular[1], src.specular[2]};
        md.emissive = {src.emission[0], src.emission[1], src.emission[2]};
        md.shininess = src.shininess > 0.0f ? src.shininess : 32.0f;
        md.dissolve = src.dissolve;
        md.metallic = src.metallic;
        md.roughness = src.roughness > 0.0f ? src.roughness : 0.5f;
        // 存在 PBR 扩展参数（非零标量或 MR 贴图）即视为 PBR 材质
        md.hasPBR = (src.metallic > 0.0f || src.roughness > 0.0f
                     || !src.metallic_texname.empty() || !src.roughness_texname.empty());

        md.albedoMap = resolveTex(src.diffuse_texname);
        // 法线贴图：优先 norm，其次 map_bump
        md.normalMap = resolveTex(!src.normal_texname.empty()
                                      ? src.normal_texname
                                      : src.bump_texname);
        md.emissiveMap = resolveTex(src.emissive_texname);
        md.metallicMap = resolveTex(src.metallic_texname);
        md.roughnessMap = resolveTex(src.roughness_texname);

        materialData.push_back(std::move(md));
    }

    return true;
}

std::unique_ptr<Mesh> Mesh::LoadFromOBJ(VulkanDevice &device,
                                        const std::string &filepath) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> subMeshes;
    std::vector<MaterialData> materialData;
    if (!ParseOBJData(filepath, vertices, indices, subMeshes, materialData)) {
        return nullptr;
    }
    // 同步路径：解析后走共享装配（切线计算 + GPU 上传）
    return BuildMesh(device, std::move(vertices), std::move(indices),
                     std::move(subMeshes), std::move(materialData), filepath);
}

// ============================================================================
// 工厂方法：异步加载（LoadFromFileAsync）
// ============================================================================
// 统一模型：
//   1. 主线程创建空壳 Mesh（无 CPU 数据与 GPU 缓冲，m_Ready=false），组装
//      AsyncMeshLoadData 与 UploadTask。
//   2. 后台线程执行 decode（OBJ/MTL 解析 + 材质数据捕获 + 切线计算）与 upload
//      （创建 local 顶点/索引缓冲 + staging，录拷贝命令）。
//   3. 主线程每帧 Poll()，fence 完成后执行 finalize：经 AsyncPendingSlot 门控后
//      调用 InstallAsyncData 注入，再调 onInstalled 构建材质，置 m_Ready=true。
// 后台线程只产出局部对象，绝不写 Mesh 成员；staging 由 AsyncMeshLoadData 持有，
// 随任务 finalize（主线程、GPU 完成后）释放。

std::unique_ptr<Mesh> Mesh::LoadFromFileAsync(
    VulkanDevice &device, AsyncUploadManager &upload,
    const std::string &filepath, std::function<void(Mesh &)> onInstalled) {

    // 空壳网格：无 CPU 数据与 GPU 缓冲，注入槽位指向自身
    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_FilePath  = filepath;
    mesh->m_AsyncSlot = std::make_shared<AsyncPendingSlot>();
    mesh->m_AsyncSlot->target = mesh.get();

    auto bucket = std::make_shared<AsyncMeshLoadData>();

    // finalize 捕获的注入目标（shared_ptr 槽位，空壳销毁后自动作废）
    auto slot = mesh->m_AsyncSlot;

    AsyncUploadManager::UploadTask task;
    task.decode = [bucket, filepath] {
        // 后台：OBJ/MTL 解析 + MTL 材质数据捕获（纯 CPU）
        bucket->parsed = ParseOBJData(filepath, bucket->vertices, bucket->indices,
                                      bucket->subMeshes, bucket->materialData);
        if (!bucket->parsed) {
            return;
        }
        // 切线计算（供法线贴图 TBN 使用），与同步路径 BuildMesh 中的一致
        ComputeTangents(bucket->vertices, bucket->indices);
        GE_CORE_TRACE("网格异步解析完成: {0} ({1} 顶点 / {2} 索引)", filepath,
                      bucket->vertices.size(), bucket->indices.size());
    };

    task.upload = [&device, bucket](VulkanCommandBuffer &cmd) {
        // 后台：创建本地顶点/索引缓冲 + staging，录拷贝命令。
        // 解析失败时跳过创建；finalize 见到空缓冲会跳过注入（与纹理失败行为一致）
        if (!bucket->parsed || bucket->vertices.empty() || bucket->indices.empty()) {
            return;
        }

        auto recordCopy = [&cmd, &device, bucket](
                vk::BufferUsageFlagBits usage, vk::DeviceSize size, const void *data,
                std::unique_ptr<VulkanBuffer> &stagingOut,
                std::unique_ptr<VulkanBuffer> &dstOut) {
            stagingOut = std::make_unique<VulkanBuffer>(std::move(
                VulkanBuffer::create_staging_buffer(device, size, data)));
            dstOut = VulkanBufferBuilder(size)
                .with_usage(usage | vk::BufferUsageFlagBits::eTransferDst)
                .with_vma_usage(VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE)
                .build_unique(device);
            vk::BufferCopy copyRegion{};
            copyRegion.size = size;
            cmd.GetHandle().copyBuffer(stagingOut->GetHandle(), dstOut->GetHandle(), copyRegion);
        };

        recordCopy(vk::BufferUsageFlagBits::eVertexBuffer,
                   static_cast<vk::DeviceSize>(bucket->vertices.size() * sizeof(Vertex)),
                   bucket->vertices.data(), bucket->stagingVB, bucket->vertexBuffer);
        recordCopy(vk::BufferUsageFlagBits::eIndexBuffer,
                   static_cast<vk::DeviceSize>(bucket->indices.size() * sizeof(uint32_t)),
                   bucket->indices.data(), bucket->stagingIB, bucket->indexBuffer);
    };

    task.finalize = [slot, bucket, onInstalled] {
        // 主线程：空壳已销毁或加载失败（无数据/无缓冲）则跳过注入与材质构建
        if (slot->abandoned || !slot->target) {
            return;
        }
        Mesh *mesh = slot->target;
        if (!bucket->parsed || !bucket->vertexBuffer || !bucket->indexBuffer) {
            return; // 解析/上传失败，保持未就绪（与纹理失败行为一致）
        }
        mesh->InstallAsyncData(std::move(bucket->vertices), std::move(bucket->indices),
                               std::move(bucket->subMeshes), std::move(bucket->materialData),
                               std::move(bucket->vertexBuffer), std::move(bucket->indexBuffer));
        // 先构建子网格材质再置就绪，保证渲染看到就绪网格时材质已绑定
        if (onInstalled) {
            onInstalled(*mesh);
        }
        mesh->m_Ready.store(true, std::memory_order_release);
        GE_CORE_INFO("网格异步就绪: {0} ({1} 顶点 / {2} 索引)", mesh->m_FilePath,
                     mesh->m_Vertices.size(), mesh->m_Indices.size());
    };

    upload.Submit(std::move(task));
    GE_CORE_INFO("网格异步加载提交: {0}", filepath);
    return mesh;
}

// ============================================================================
// 工厂方法：从 CPU 端数据创建
// ============================================================================

std::unique_ptr<Mesh> Mesh::Create(VulkanDevice &device,
                                   const std::vector<Vertex> &vertices,
                                   const std::vector<uint32_t> &indices) {
    if (vertices.empty() || indices.empty()) {
        return nullptr;
    }

    // 统一子网格：单整体网格也生成一个覆盖全部索引的子网格，
    // 使所有 mesh（含内置几何体 / CPU 直建）都走统一的子网格绘制路径
    std::vector<SubMesh> subMeshes;
    subMeshes.push_back(SubMesh{
        0, static_cast<uint32_t>(vertices.size()),
        0, static_cast<uint32_t>(indices.size()),
        {}, nullptr});

    // 复用共享装配路径：切线计算 + GPU 上传
    return BuildMesh(device, vertices, indices, std::move(subMeshes), {}, {});
}

// ============================================================================
// 工厂方法：创建内置几何体
// ============================================================================

std::unique_ptr<Mesh> Mesh::CreateBuiltin(VulkanDevice &device, const std::string &type) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    if (type == "cube") {
        // 立方体：边长 2，中心在原点，6 个面各 4 顶点 = 24 顶点，36 索引
        vertices.reserve(24);
        indices.reserve(36);

        auto add_quad = [&](const glm::vec3 &p0, const glm::vec3 &p1,
                            const glm::vec3 &p2, const glm::vec3 &p3,
                            const glm::vec3 &normal) {
            uint32_t base = static_cast<uint32_t>(vertices.size());
            glm::vec2 uvs[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            glm::vec3 pos[] = {p0, p1, p2, p3};
            for (int i = 0; i < 4; ++i) {
                vertices.push_back({pos[i], normal, uvs[i]});
            }
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };

        // +Z 面（前）
        add_quad({-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
                 {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f});
        // -Z 面（后）
        add_quad({1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f},
                 {-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f},
                 {0.0f, 0.0f, -1.0f});
        // +X 面（右）
        add_quad({1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 0.0f, 0.0f});
        // -X 面（左）
        add_quad({-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, 1.0f},
                 {-1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, -1.0f},
                 {-1.0f, 0.0f, 0.0f});
        // +Y 面（上）
        add_quad({-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
                 {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
                 {0.0f, 1.0f, 0.0f});
        // -Y 面（下）
        add_quad({-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
                 {1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f},
                 {0.0f, -1.0f, 0.0f});
    } else if (type == "plane") {
        // 平面：XY 平面，边长 2，中心在原点，法线 +Z
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "quad") {
        // 四边形（plane 的别名）
        vertices = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
            {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        };
        indices = {0, 1, 2, 0, 2, 3};
    } else if (type == "sphere") {
        // 球体：半径 1，中心在原点，UV 球体
        const int latBands = 20;
        const int lonBands = 20;
        const float radius = 1.0f;

        for (int lat = 0; lat <= latBands; ++lat) {
            float theta = static_cast<float>(lat) * static_cast<float>(M_PI) / static_cast<float>(latBands);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            for (int lon = 0; lon <= lonBands; ++lon) {
                float phi = static_cast<float>(lon) * 2.0f * static_cast<float>(M_PI) / static_cast<float>(lonBands);
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);

                glm::vec3 pos{
                    radius * cosPhi * sinTheta,
                    radius * cosTheta,
                    radius * sinPhi * sinTheta
                };
                glm::vec3 normal = glm::normalize(pos);
                glm::vec2 uv{
                    static_cast<float>(lon) / static_cast<float>(lonBands),
                    static_cast<float>(lat) / static_cast<float>(latBands)
                };
                vertices.push_back({pos, normal, uv});
            }
        }

        for (int lat = 0; lat < latBands; ++lat) {
            for (int lon = 0; lon < lonBands; ++lon) {
                // 注意：绕序必须与法线一致（CCW 朝外）。原实现 (first,second,first+1)
                // 的叉积法线朝内，导致外侧被当作背面剔除、法线背离相机，
                // 所有直接光照失效（只剩环境光）。这里交换 last two 顶点翻转绕序。
                uint32_t first = static_cast<uint32_t>(lat * (lonBands + 1) + lon);
                uint32_t second = first + static_cast<uint32_t>(lonBands + 1);
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(second);
                indices.push_back(first + 1);
                indices.push_back(second + 1);
            }
        }
    } else {
        return nullptr; // 未知类型
    }

    auto mesh = Create(device, vertices, indices);
    if (mesh) {
        mesh->m_FilePath = "builtin:" + type;
    }
    return mesh;
}

// ============================================================================
// 析构 / 移动
// ============================================================================

Mesh::~Mesh() {
    // 空壳网格销毁时作废异步注入槽位，使在途 finalize 安全跳过注入/材质构建
    if (m_AsyncSlot) {
        m_AsyncSlot->abandoned = true;
        m_AsyncSlot->target    = nullptr;
    }
}

Mesh::Mesh(Mesh &&other) noexcept
    : m_Vertices(std::move(other.m_Vertices)),
      m_Indices(std::move(other.m_Indices)),
      m_SubMeshes(std::move(other.m_SubMeshes)),
      m_MaterialData(std::move(other.m_MaterialData)),
      m_FilePath(std::move(other.m_FilePath)),
      m_VertexBuffer(std::move(other.m_VertexBuffer)),
      m_IndexBuffer(std::move(other.m_IndexBuffer)),
      m_Ready(other.m_Ready.load()),
      m_AsyncSlot(other.m_AsyncSlot) {
    // 转移后把注入槽位目标重定向到新对象，避免在途 finalize 注入进已移动的空壳
    if (m_AsyncSlot) {
        m_AsyncSlot->target = this;
    }
    other.m_AsyncSlot = nullptr;
}

// ============================================================================
// 上传到 GPU
// ============================================================================

bool Mesh::UploadToGPU(VulkanDevice &device) {
    vk::DeviceSize vbSize = m_Vertices.size() * sizeof(Vertex);
    vk::DeviceSize ibSize = m_Indices.size() * sizeof(uint32_t);

    m_VertexBuffer = UploadBuffer(device,
                                  vk::BufferUsageFlagBits::eVertexBuffer,
                                  vbSize, m_Vertices.data());
    if (!m_VertexBuffer) {
        return false;
    }

    m_IndexBuffer = UploadBuffer(device,
                                 vk::BufferUsageFlagBits::eIndexBuffer,
                                 ibSize, m_Indices.data());
    if (!m_IndexBuffer) {
        return false;
    }

    return true;
}

// ============================================================================
// 异步：安装后台加载完成的数据与 GPU 缓冲
// ============================================================================

void Mesh::InstallAsyncData(std::vector<Vertex> vertices,
                            std::vector<uint32_t> indices,
                            std::vector<SubMesh> subMeshes,
                            std::vector<MaterialData> materialData,
                            std::unique_ptr<VulkanBuffer> vertexBuffer,
                            std::unique_ptr<VulkanBuffer> indexBuffer) {
    m_Vertices = std::move(vertices);
    m_Indices  = std::move(indices);
    m_SubMeshes = std::move(subMeshes);
    m_MaterialData = std::move(materialData);
    m_VertexBuffer = std::move(vertexBuffer);
    m_IndexBuffer  = std::move(indexBuffer);
}

// ============================================================================
// 调试名称
// ============================================================================

void Mesh::SetDebugName(const std::string &name) {
    if (m_VertexBuffer) {
        m_VertexBuffer->SetDebugName(name + "_VB");
    }
    if (m_IndexBuffer) {
        m_IndexBuffer->SetDebugName(name + "_IB");
    }
}

} // namespace GE
