#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace meridian
{

/**
    Wait-free single-producer / single-consumer ring buffer.

    - Capacity N must be a power of two. One slot is never "wasted": head and
      tail are free-running counters and the fill level is (head - tail).
    - push() is called only by the producer thread, pop() only by the consumer.
    - No allocation, no locks, no blocking: safe for the audio thread.
    - head and tail live on separate cache lines to avoid false sharing.
    - clear() must only be called by the consumer (it drains by advancing tail).
*/
template <typename T, std::size_t N>
class LockFreeQueue
{
    static_assert (N >= 2, "LockFreeQueue capacity must be at least 2");
    static_assert ((N & (N - 1)) == 0, "LockFreeQueue capacity N must be a power of two");
    static_assert (std::is_copy_assignable_v<T>, "LockFreeQueue element type must be copy-assignable");

public:
    static constexpr std::size_t capacity = N;

    LockFreeQueue() = default;
    LockFreeQueue (const LockFreeQueue&) = delete;
    LockFreeQueue& operator= (const LockFreeQueue&) = delete;

    /** Producer only. Returns false (and drops the item) if the queue is full. */
    bool push (const T& item) noexcept (std::is_nothrow_copy_assignable_v<T>)
    {
        const std::size_t head = head_.value.load (std::memory_order_relaxed);
        const std::size_t tail = tail_.value.load (std::memory_order_acquire);

        if (head - tail >= N)
            return false;

        buffer_[head & mask] = item;
        head_.value.store (head + 1, std::memory_order_release);
        return true;
    }

    /** Consumer only. Returns false if the queue is empty. */
    bool pop (T& out) noexcept (std::is_nothrow_copy_assignable_v<T>)
    {
        const std::size_t tail = tail_.value.load (std::memory_order_relaxed);
        const std::size_t head = head_.value.load (std::memory_order_acquire);

        if (head == tail)
            return false;

        out = buffer_[tail & mask];
        tail_.value.store (tail + 1, std::memory_order_release);
        return true;
    }

    /** Approximate number of queued items (exact when called from either endpoint thread). */
    std::size_t size() const noexcept
    {
        const std::size_t head = head_.value.load (std::memory_order_acquire);
        const std::size_t tail = tail_.value.load (std::memory_order_acquire);
        return head - tail;
    }

    bool empty() const noexcept { return size() == 0; }
    bool full() const noexcept  { return size() >= N; }

    /** Consumer only: discards all currently queued items. */
    void clear() noexcept
    {
        const std::size_t head = head_.value.load (std::memory_order_acquire);
        tail_.value.store (head, std::memory_order_release);
    }

private:
    static constexpr std::size_t mask = N - 1;
    static constexpr std::size_t cacheLine = 64;

    struct alignas (cacheLine) PaddedIndex
    {
        std::atomic<std::size_t> value { 0 };
        char padding[cacheLine - sizeof (std::atomic<std::size_t>)] {};
    };

    PaddedIndex head_;
    PaddedIndex tail_;
    alignas (cacheLine) std::array<T, N> buffer_ {};

    static_assert (std::atomic<std::size_t>::is_always_lock_free, "size_t atomics must be lock-free");
};

} // namespace meridian
