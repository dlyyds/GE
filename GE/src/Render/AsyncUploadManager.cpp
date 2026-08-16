/**
 * @file AsyncUploadManager.cpp
 * @brief 异步上传管理器实现。
 */

#include "Render/AsyncUploadManager.h"

#include "Render/VulkanBase/VulkanCommandBuffer.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Core/Log.h"

#include <utility>

namespace GE {

// ============================================================================
// 构造 / 析构
// ============================================================================

AsyncUploadManager::AsyncUploadManager(VulkanDevice &device) : m_Device(device) {
    // 选择同时支持 Graphics 的队列族创建命令池，并取出该族的一条图形队列用于提交。
    // 若该族有多个同能力队列实例（如 Family 0 常含 16 个），用第 2 个（queue 1）做
    // 上传，与渲染用的 queue 0 是不同 VkQueue，Vulkan 允许并发提交且不争 per-queue
    // 锁；单队列设备则退化为 queue 0（此时仍由 per-queue 锁串行）。
    const uint32_t family_index =
        m_Device.GetQueueByFlags(vk::QueueFlagBits::eGraphics, 0).GetFamilyIndex();
    const auto &family_props = m_Device.GetQueue(family_index, 0).GetProperties();
    const uint32_t upload_queue_index = family_props.queueCount > 1 ? 1 : 0;
    m_GraphicsQueue = &m_Device.GetQueue(family_index, upload_queue_index);

    // 为每个槽位创建命令池 + 持久 fence（复用，避免反复 create/destroy）
    for (auto &slot : m_Slots) {
        slot.pool = std::make_unique<VulkanCommandPool>(m_Device, family_index);
        slot.fence = m_Device.GetHandle().createFence(vk::FenceCreateInfo{});
    }

    // 启动专职后台线程
    m_Thread = std::thread(&AsyncUploadManager::WorkerLoop, this);

    GE_CORE_INFO("AsyncUploadManager: 后台上传线程已启动 (in-flight={0})", kMaxInFlight);
}

AsyncUploadManager::~AsyncUploadManager() {
    Shutdown();
}

// ============================================================================
// WorkerLoop — 后台线程主循环
// ============================================================================

// 后台线程循环：取任务 → 占槽 → 解码 → 录命令 → 提交 → 回到循环。
// 提交后不等待自己的 fence，等待与回收的职责交给主线程 Poll()。
void AsyncUploadManager::WorkerLoop() {
    while (true) {
        // ── 1. 取一个任务（队列空时等待；退出信号且队列空则结束） ──
        UploadTask task;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_CV.wait(lock, [this] {
                return m_RequestExit.load() || !m_Queue.empty();
            });
            if (m_RequestExit.load() && m_Queue.empty()) {
                break;
            }
            task = std::move(m_Queue.front());
            m_Queue.pop_front();
        }

        // ── 2. 找一个空闲槽位（无空闲则等待主线程回收，形成背压） ──
        Slot *slot = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_slotCv.wait(lock, [this] {
                if (m_RequestExit.load()) {
                    return true;
                }
                for (const auto &s : m_Slots) {
                    if (s.state == SlotState::Idle) {
                        return true;
                    }
                }
                return false;
            });

            for (auto &s : m_Slots) {
                if (s.state == SlotState::Idle) {
                    slot = &s;
                    s.state = SlotState::Acquired;
                    break;
                }
            }
            // 若因退出信号醒来且无空闲槽，则结束线程
            if (!slot) {
                if (m_RequestExit.load()) {
                    break;
                }
                continue; // 理论不可达，防御
            }
        }

        // ── 3. 解码（后台线程，CPU 密集） ──
        if (task.decode) {
            task.decode();
        }

        // ── 4. 从槽位命令池取命令缓冲，录制上传命令 ──
        auto &cmd = slot->pool->RequestCommandBuffer(vk::CommandBufferLevel::ePrimary);
        cmd.Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        if (task.upload) {
            task.upload(cmd);
        }
        cmd.End();

        // ── 5. 提交到图形队列（绑定槽位 fence，GPU 完成后由主线程回收） ──
        // 统一经队列的提交入口（内部对本队列提交加互斥锁串行，与帧提交并发安全；
        // 若上传用的队列与渲染不同，则各用各的锁互不阻塞）。
        m_GraphicsQueue->Submit({cmd.GetHandle()}, slot->fence);

        // ── 6. 把任务（含 staging / finalize）交给槽位，标记已提交，回到循环 ──
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            slot->task = std::move(task);
            slot->state = SlotState::Submitted;
        }
    }
}

// ============================================================================
// Submit / Poll
// ============================================================================

void AsyncUploadManager::Submit(UploadTask task) {
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Queue.push_back(std::move(task));
    }
    m_CV.notify_one();
}

void AsyncUploadManager::Poll() {
    // 遍历所有已提交的槽位，非阻塞查询 fence；fence 置位则开始回收
    for (auto &slot : m_Slots) {
        bool ready = false;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (slot.state != SlotState::Submitted) {
                continue;
            }
            // timeout=0：非阻塞查询，主线程不卡
            const vk::Result r = m_Device.GetHandle().getFenceStatus(slot.fence);
            if (r != vk::Result::eSuccess) {
                continue; // 尚未完成，留待后续帧
            }
            // 标记回收中（锁外执行 finalize），防止后台线程此刻复用该槽
            slot.state = SlotState::Reclaiming;
            ready = true;
        }

        if (ready) {
            ReclaimSlot(slot, /*block=*/false);
        }
    }
}

// ============================================================================
// ReclaimSlot — 单个槽位回收
// ============================================================================

// 回收一个已确认 GPU 完成的槽位：执行 finalize（主线程）、释放 staging、
// 重置 fence 与命令池，最后把槽位归还为空闲（并唤醒可能等待的后台线程）。
void AsyncUploadManager::ReclaimSlot(Slot &slot, bool block) {
    auto &dev = m_Device.GetHandle();

    if (block) {
        // 阻塞等待 fence（Shutdown 路径使用）
        dev.waitForFences(1, &slot.fence, VK_TRUE, uint64_t(-1));
    }

    // ── 主线程执行收尾（创建 view/sampler、安装成员、置 ready） ──
    if (slot.task.finalize) {
        slot.task.finalize();
    }

    // ── GPU 已用完，释放 staging buffer ──
    slot.task.staging.reset();

    // ── 重置 fence 与命令池，槽位可复用 ──
    dev.resetFences(1, &slot.fence);
    slot.pool->ResetPool();
    slot.task = {};

    // ── 归还为空闲，唤醒等待的后台线程 ──
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        slot.state = SlotState::Idle;
    }
    m_slotCv.notify_one();
}

// ============================================================================
// Shutdown
// ============================================================================

void AsyncUploadManager::Shutdown() {
    if (m_RequestExit.load()) {
        return; // 已停机
    }
    m_RequestExit.store(true);
    m_CV.notify_all();
    m_slotCv.notify_all();

    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    // join 后，剩余在飞上传（Submitted / Reclaiming）阻塞等待完成并回收
    for (auto &slot : m_Slots) {
        if (slot.state == SlotState::Submitted || slot.state == SlotState::Reclaiming) {
            ReclaimSlot(slot, /*block=*/true);
        }
    }

    // 丢弃队列中尚未处理的任务（其 staging 尚未创建，无 GPU 泄漏）
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Queue.clear();
    }

    // 释放持久 fence
    for (auto &slot : m_Slots) {
        if (slot.fence) {
            m_Device.GetHandle().destroyFence(slot.fence);
            slot.fence = nullptr;
        }
    }

    GE_CORE_INFO("AsyncUploadManager: 后台上传线程已终止");
}

// ============================================================================
// GetInFlightCount
// ============================================================================

size_t AsyncUploadManager::GetInFlightCount() const {
    std::unique_lock<std::mutex> lock(m_Mutex);
    size_t count = 0;
    for (const auto &s : m_Slots) {
        if (s.state != SlotState::Idle) {
            ++count;
        }
    }
    return count;
}

} // namespace GE