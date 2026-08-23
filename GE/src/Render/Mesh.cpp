/**
 * @file Mesh.cpp
 * @brief 网格实现 —— "MeshData → 资源"的装配 + 被动异步状态。
 *
 * 职责收敛：切线计算 + GPU 上传 + 摘要提取 + 空壳/注入状态机。
 * 格式解析（OBJ 等）与加载编排（同步/异步分派）已外移到 ModelLoader / OBJLoader /
 * MeshManager，本文件不含任何文件格式相关代码。
 */

#include "Render/Mesh.h"
#include "Render/ModelLoader.h"
#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanQueue.h"
#include "Core/Log.h"

namespace GE {

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
// 私有装配：由格式解析/几何生成产出的 MeshData 构建最终网格（各来源共享路径）
// ============================================================================

std::unique_ptr<Mesh> Mesh::BuildMesh(VulkanDevice &device,
                                      MeshData &&data,
                                      std::string filePath) {
    if (data.vertices.empty() || data.indices.empty()) {
        return nullptr;
    }

    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_SubMeshes = std::move(data.subMeshes);
    mesh->m_MaterialData = std::move(data.materialData);
    // 摘要计数：上传后整份 CPU 顶点/索引数组随 data 析构释放，Mesh 不常驻
    mesh->m_VertexCount = static_cast<uint32_t>(data.vertices.size());
    mesh->m_IndexCount = static_cast<uint32_t>(data.indices.size());
    // 模型空间包围盒摘要：优先复用解析器预计算（如 .gemesh META 回填），缺失则现算兜底
    mesh->m_AABB = data.aabb;
    if (!mesh->m_AABB.IsValid()) {
        for (const auto &v : data.vertices) {
            mesh->m_AABB.Expand(v.Position);
        }
    }

    // 计算顶点切线（法线贴图需要），与异步 decode 共用 ModelLoader 的共享装配
    ModelLoader::ComputeTangents(data);

    mesh->m_VertexBuffer = UploadBuffer(device,
                                        vk::BufferUsageFlagBits::eVertexBuffer,
                                        static_cast<vk::DeviceSize>(data.vertices.size() * sizeof(Vertex)),
                                        data.vertices.data());
    if (!mesh->m_VertexBuffer) {
        return nullptr;
    }

    mesh->m_IndexBuffer = UploadBuffer(device,
                                       vk::BufferUsageFlagBits::eIndexBuffer,
                                       static_cast<vk::DeviceSize>(data.indices.size() * sizeof(uint32_t)),
                                       data.indices.data());
    if (!mesh->m_IndexBuffer) {
        return nullptr;
    }

    // 同步路径加载完成立即就绪（异步路径在 finalize 安装后才置就绪）
    mesh->m_Ready.store(true, std::memory_order_release);

    // 记录源文件路径
    mesh->m_FilePath = std::move(filePath);
    return mesh;
}

// ============================================================================
// 工厂方法：从 CPU 端数据（MeshData）创建（同步，构建完即就绪）
// ============================================================================

std::unique_ptr<Mesh> Mesh::Create(VulkanDevice &device, MeshData &&data) {
    if (data.vertices.empty() || data.indices.empty()) {
        return nullptr;
    }

    // 统一子网格：单整体网格也生成一个覆盖全部索引的子网格，
    // 使所有 mesh（含内置几何体 / CPU 直建）都走统一的子网格绘制路径。
    // 调用方传入了子网格（如 OBJ 多材质拆分）则保留。
    if (data.subMeshes.empty()) {
        data.subMeshes.push_back(SubMesh{
            0, static_cast<uint32_t>(data.vertices.size()),
            0, static_cast<uint32_t>(data.indices.size()),
            {}, nullptr});
    }

    // 复用共享装配路径：切线计算 + GPU 上传 + 摘要提取
    return BuildMesh(device, std::move(data), {});
}

// ============================================================================
// 工厂方法：创建异步加载"空壳"（带注入槽位，编排在 MeshManager）
// ============================================================================

std::unique_ptr<Mesh> Mesh::CreateShell(const std::string &filepath) {
    // 造空壳 + 初始化注入槽位。槽位指向自身，最终 finalize 据此安全注入（空壳
    // 销毁时析构函数将槽位作废，在途 finalize 据此跳过，避免 use-after-free）。
    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->m_FilePath = filepath;
    mesh->m_AsyncSlot = std::make_shared<AsyncPendingSlot>();
    mesh->m_AsyncSlot->target = mesh.get();
    return mesh;
}

// ============================================================================
// 析构 / 移动
// ============================================================================

Mesh::~Mesh() {
    // 空壳网格销毁时作废异步注入槽位，使在途 finalize 安全跳过注入/材质构建
    if (m_AsyncSlot) {
        m_AsyncSlot->abandoned = true;
        m_AsyncSlot->target = nullptr;
    }
}

Mesh::Mesh(Mesh &&other) noexcept
    : m_SubMeshes(std::move(other.m_SubMeshes)),
      m_MaterialData(std::move(other.m_MaterialData)),
      m_VertexCount(other.m_VertexCount),
      m_IndexCount(other.m_IndexCount),
      m_AABB(other.m_AABB),
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
// 异步：安装后台加载完成的数据与 GPU 缓冲（主线程 finalize 调用）
// ============================================================================

void Mesh::InstallAsyncData(MeshData &&data,
                            std::unique_ptr<VulkanBuffer> vertexBuffer,
                            std::unique_ptr<VulkanBuffer> indexBuffer) {
    // 只回收轻量摘要（子网格 / 材质数据 / 计数），vertices/indices 随传入的
    // MeshData 析构释放，Mesh 生命周期内不整份常驻 CPU 顶点数组。
    m_SubMeshes = std::move(data.subMeshes);
    m_MaterialData = std::move(data.materialData);
    m_VertexCount = static_cast<uint32_t>(data.vertices.size());
    m_IndexCount = static_cast<uint32_t>(data.indices.size());
    // 模型空间包围盒摘要：优先复用解析器预计算（如 .gemesh META 回填），缺失则现算兜底
    m_AABB = data.aabb;
    if (!m_AABB.IsValid()) {
        for (const auto &v : data.vertices) {
            m_AABB.Expand(v.Position);
        }
    }
    m_VertexBuffer = std::move(vertexBuffer);
    m_IndexBuffer = std::move(indexBuffer);
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