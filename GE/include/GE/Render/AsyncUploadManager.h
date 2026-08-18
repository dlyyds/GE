/**
 * @file AsyncUploadManager.h
 * @brief 异步上传管理器 —— 专职后台线程（数量可配置）+ in-flight 槽位池。
 *
 * 解决"主线程加载大资源时阻塞"的问题：把资源的解码（CPU）与 GPU 上传都移到
 * 专职后台线程，主线程只在每帧 Poll() 时回收已完成的上传。
 *
 * 线程模型：
 * - 后台线程（数量可配置）：执行 decode（文件解析/像素解码）→ upload（在命令缓冲上录
 *   copy/blit 并提交）。后台线程绝不直接写资源对象（Texture/Mesh）的成员。
 * - 主线程：每帧调用 Poll()，检查已完成（fence 置位）的上传，执行 finalize
 *   （创建 view/sampler、安装资源成员、置 ready），并释放 staging buffer。
 *
 * in-flight 槽位：
 * - 槽位数为 max(worker_count, kMinSlots)，每槽一个命令池 + 一个持久 fence（复用）。
 *   多工作线程各自独占一个槽位（槽位归属由互斥锁 + 状态机保证），互不串行。
 * - 槽满时后台线程在 m_slotCv 上阻塞（自然背压），主线程 Poll() 回收后放行。
 * - 主线程 Poll() 用 timeout=0 的非阻塞 fence 查询，从不阻塞主线程。
 *
 * 限制：GPU 提交仍经单条图形队列串行（per-queue 锁），多线程的收益来自并行 CPU 解码。
 */

#pragma once

#include "Render/VulkanBase/VulkanCommandPool.h"
#include "Render/VulkanBase/VulkanBuffer.h"

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace GE {

class VulkanCommandBuffer;
class VulkanDevice;
class VulkanQueue;

/**
 * @brief 资源异步上传管理器。
 *
 * 持有专职上传线程（数量可配置）与 in-flight 槽位池，负责后台解码 + GPU 上传，
 * 主线程每帧回收。生命周期通常与 Rendere 一致（由 Renderer 持有）。
 */
class AsyncUploadManager {
public:
    /**
     * @brief 一个上传任务。
     *
     * decode / upload 在后台线程按其顺序执行；finalize 在主线程 Poll() 中被调用。
     * staging 由 upload 阶段创建并交给本任务持有，主线程回收（GPU 用完后）时释放。
     */
    struct UploadTask {
        /// 解码（后台线程）：读文件、像素解码，产出 CPU 数据（写入本任务捕获的容器）。
        std::function<void()> decode;

        /// 上传（后台线程）：在命令缓冲上录制 copy / blit 命令；可创建 staging buffer。
        std::function<void(VulkanCommandBuffer &)> upload;

        /// 收尾（主线程 Poll()）：创建 view/sampler、安装资源成员、置 ready。
        std::function<void()> finalize;

        /// upload 阶段创建的 staging buffer，由主线程回收时释放（须在 GPU 用完后）。
        std::unique_ptr<VulkanBuffer> staging;
    };

    /**
     * @brief 构造异步上传管理器，启动指定数量的工作线程。
     * @param device Vulkan 设备引用（须在管理器析构前保持存活）。
     * @param worker_count 后台工作线程数（默认 1）；in-flight 槽位数取
     *        max(worker_count, kMinSlots)，确保每个线程有可用槽位。
     */
    explicit AsyncUploadManager(VulkanDevice &device, size_t worker_count = 1);

    ~AsyncUploadManager();

    AsyncUploadManager(const AsyncUploadManager &) = delete;

    AsyncUploadManager &operator=(const AsyncUploadManager &) = delete;

    AsyncUploadManager(AsyncUploadManager &&) = delete;

    AsyncUploadManager &operator=(AsyncUploadManager &&) = delete;

    /**
     * @brief 提交一个上传任务（后台线程异步执行）。
     *
     * @note 仅在 Shutdown() 之前调用有效；任务内部应值捕获数据，不要持有
     *       可能被主线程提前销毁的资源裸指针（参见头文件线程模型说明）。
     */
    void Submit(UploadTask task);

    /**
     * @brief 主线程每帧调用：回收已完成的上传（非阻塞）。
     *
     * 对每个已提交且 fence 置位的槽位：执行 finalize、释放 staging、重置并
     * 归还槽位。用 timeout=0 查询 fence，主线程不会阻塞。
     */
    void Poll();

    /**
     * @brief 排空队列、等待在飞上传完成、终止后台线程。
     *
     * 等待所有已提交的上传完成后执行其 finalize，再 join 线程。析构时自动调用。
     */
    void Shutdown();

    /// 当前在飞行中的上传数量（Acquired + Submitted + Reclaiming，调试用）。
    size_t GetInFlightCount() const;

private:
    /// 槽位状态机。
    enum class SlotState : uint8_t {
        Idle, ///< 空闲，可被后台线程占用
        Acquired, ///< 后台线程占用中（录制/提交中）
        Submitted, ///< 已提交，等待 GPU 完成（等待主线程回收）
        Reclaiming, ///< 主线程正在执行 finalize（锁外），防止后台线程复用
    };

    /// 一个在飞行中的上传槽位。
    struct Slot {
        std::unique_ptr<VulkanCommandPool> pool = nullptr;
        vk::Fence fence = nullptr;
        UploadTask task;
        SlotState state = SlotState::Idle;
    };

    /// 后台线程主循环（每个工作线程各跑一份）。
    void WorkerLoop();

    /// 回收指定槽位（block=false 用 timeout=0 非阻塞查询；block=true 阻塞等待）。
    void ReclaimSlot(Slot &slot, bool block);

    /// in-flight 槽位数下限（实际槽位数 = max(worker_count, kMinSlots)）。
    static constexpr size_t kMinSlots = 10;

    VulkanDevice &m_Device;
    const VulkanQueue *m_GraphicsQueue = nullptr; ///< 提交用的图形队列（由队列族能力选出）

    std::vector<std::thread> m_Threads; ///< 工作线程集合（数量 = worker_count）
    std::atomic<bool> m_RequestExit{false}; ///< 请求后台线程退出

    mutable std::mutex m_Mutex; ///< 保护队列与槽位状态（const 查询方法 GetInFlightCount 需加锁）
    std::condition_variable m_CV; ///< 队列非空通知
    std::condition_variable m_slotCv; ///< 槽位空闲通知（背压）

    std::deque<UploadTask> m_Queue; ///< 待处理任务队列
    std::vector<Slot> m_Slots; ///< in-flight 槽位集合（尺寸 = max(worker_count, kMinSlots)）
};

} // namespace GE