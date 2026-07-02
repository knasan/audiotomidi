#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace dsp {

/// Single-producer / single-consumer lock-free ring buffer.
///
/// The audio thread is the sole producer; the debug logging thread is the sole
/// consumer. Capacity must be a power of two so index wrap uses a bitmask.
/// T must be trivially copyable so slots can be written without locks.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for lock-free transfer");

public:
    /// Try to enqueue one item from the producer (audio) thread.
    ///
    /// @return false when the buffer is full; the caller should drop the sample.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Try to dequeue one item on the consumer (logging) thread.
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = buffer_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    /// Approximate number of queued items (may be stale; debug use only).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace dsp
