#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace atriswave {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignas(64) cache-line separation
#endif

/// Lock-free Single-Producer Single-Consumer (SPSC) power-of-two ring buffer.
/// Zero heap allocations and zero locks on push/pop; suitable for real-time audio threads.
template <typename T, std::size_t Capacity>
class SpscRingBuffer final {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscRingBuffer Capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRingBuffer element type must be trivially copyable");

public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;

    constexpr SpscRingBuffer() noexcept = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    /// Producer-only: attempts to push one item. Returns false if full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if ((head - tail) >= kCapacity) {
            return false;
        }
        buffer_[head & kMask] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Producer-only: pushes up to `count` items, returning number actually pushed.
    std::size_t push_batch(const T* items, std::size_t count) noexcept {
        if (items == nullptr || count == 0) {
            return 0;
        }
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t free_slots = kCapacity - (head - tail);
        const std::size_t to_write = count < free_slots ? count : free_slots;
        for (std::size_t i = 0; i < to_write; ++i) {
            buffer_[(head + i) & kMask] = items[i];
        }
        if (to_write > 0) {
            head_.store(head + to_write, std::memory_order_release);
        }
        return to_write;
    }

    /// Consumer-only: attempts to pop one item. Returns false if empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = buffer_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer-only: pops up to `count` items, returning number actually popped.
    std::size_t pop_batch(T* out, std::size_t count) noexcept {
        if (out == nullptr || count == 0) {
            return 0;
        }
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t available = head - tail;
        const std::size_t to_read = count < available ? count : available;
        for (std::size_t i = 0; i < to_read; ++i) {
            out[i] = buffer_[(tail + i) & kMask];
        }
        if (to_read > 0) {
            tail_.store(tail + to_read, std::memory_order_release);
        }
        return to_read;
    }

    /// Non-RT / approximate inspection helpers.
    [[nodiscard]] std::size_t available_read() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] std::size_t available_write() const noexcept {
        return kCapacity - available_read();
    }

    [[nodiscard]] bool empty() const noexcept {
        return available_read() == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return available_read() >= kCapacity;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return kCapacity;
    }

    /// Resets indices (only safe when neither producer nor consumer is active).
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<T, Capacity> buffer_{};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace atriswave
