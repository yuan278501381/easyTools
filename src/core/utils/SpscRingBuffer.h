#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SpscRingBuffer.h — 无锁单写单读环形队列缓冲 (Lock-Free SPSC Ring Buffer)
//
// 架构设计:
//   1. 纯单写单读模型：生产者只写 m_head，消费者只写 m_tail，彻底杜绝双写竞态
//   2. 缓存行对齐 (alignas(64)) 彻底消除伪共享 (False Sharing)
//   3. 零锁、零阻塞、微秒级非阻塞推入与弹出
//   4. 滑动窗口模式 (push)：若队列满，消费者消费时自动跨过被覆盖的老数据，保证永不死锁且永远获取最新鲜数据
// ─────────────────────────────────────────────────────────────────────────────

#ifndef EASYTOOLS_CORE_UTILS_SPSC_RING_BUFFER_H
#define EASYTOOLS_CORE_UTILS_SPSC_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace easy::core {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

template <typename T, size_t Capacity = 2048>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                  "T must be nothrow assignable");

public:
    SpscRingBuffer() : m_head(0), m_tail(0) {}
    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// 生产者 (单写线程)：非阻塞推入元素 (滑动窗口覆盖模式)
    /// 生产者永不修改 m_tail，彻底消灭对 m_tail 的多写竞态与 ABA/回退隐患
    bool push(const T& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        m_buffer[head & BufferMask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 生产者 (单写线程)：移动语义非阻塞推入 (滑动窗口覆盖模式)
    bool push(T&& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        m_buffer[head & BufferMask] = std::move(item);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 生产者 (单写线程)：非阻塞推入元素，满载时不覆盖并返回 false
    bool try_push(const T& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            return false;
        }
        m_buffer[head & BufferMask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 生产者 (单写线程)：移动语义非阻塞推入，满载时不覆盖并返回 false
    bool try_push(T&& item) noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            return false;
        }
        m_buffer[head & BufferMask] = std::move(item);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 消费者 (单读线程)：非阻塞弹出元素
    /// 若队列为空，返回 false
    /// 若发生滑窗溢出，由消费者自动前推 tail 至最新的有效窗口起点
    bool pop(T& item) noexcept {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);

        if (tail >= head) {
            return false; // 队列空
        }

        // 滑动窗口溢出检测：如果生产者已经走过超过 Capacity，调整 tail 到最早未被覆盖的有效元素
        if (head - tail > Capacity) {
            tail = head - Capacity;
        }

        item = m_buffer[tail & BufferMask];

        // 读后二次校验：防止极罕见情况下在读取期间被生产者再次飞速覆盖
        const size_t headAfter = m_head.load(std::memory_order_acquire);
        if (headAfter - tail > Capacity) {
            tail = headAfter - Capacity;
            item = m_buffer[tail & BufferMask];
        }

        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// 检查队列是否为空
    bool empty() const noexcept {
        return m_tail.load(std::memory_order_relaxed) >= m_head.load(std::memory_order_relaxed);
    }

    /// 获取队列中待处理元素数量
    size_t size() const noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (head <= tail) return 0;
        const size_t diff = head - tail;
        return (diff > Capacity) ? Capacity : diff;
    }

    /// 队列最大容量
    constexpr size_t capacity() const noexcept {
        return Capacity;
    }

    /// 清空队列
    void clear() noexcept {
        const size_t head = m_head.load(std::memory_order_relaxed);
        m_tail.store(head, std::memory_order_release);
    }

private:
    static constexpr size_t BufferMask = Capacity - 1;

    // 独立 64 字节缓存行对齐，彻底消除多核并发时的伪共享
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};

    // 环形缓冲存储区
    T m_buffer[Capacity];
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace easy::core

#endif // EASYTOOLS_CORE_UTILS_SPSC_RING_BUFFER_H
