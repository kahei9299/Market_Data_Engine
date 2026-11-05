#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>

// Lock-free single-producer single-consumer bounded queue.
//
// Invariants:
// - Only the producer thread calls try_push().
// - Only the consumer thread calls try_pop().
// - Capacity is rounded up to the next power of two for fast modulo.
// - head_ and tail_ are on separate cache lines to avoid false sharing.
//
// Memory ordering:
// - Producer: store to tail_ with release after writing the slot.
// - Consumer: load tail_ with acquire before reading the slot.
// - The slot write happens-before the tail_ publish, so the consumer
//   sees the fully constructed element.
template <typename T>
class SpscQueue {
  public:
    explicit SpscQueue(std::size_t min_capacity)
        : mask_(next_power_of_two(min_capacity) - 1),
          buf_(next_power_of_two(min_capacity)) {}

    // Producer only. Returns false if full (backpressure).
    bool try_push(T const& item) {
        auto tail = tail_.load(std::memory_order_relaxed);
        auto head = head_.load(std::memory_order_acquire);
        if (tail - head > mask_)
            return false;
        buf_[tail & mask_] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) {
        auto tail = tail_.load(std::memory_order_relaxed);
        auto head = head_.load(std::memory_order_acquire);
        if (tail - head > mask_)
            return false;
        buf_[tail & mask_] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns nullopt if empty.
    std::optional<T> try_pop() {
        auto head = head_.load(std::memory_order_relaxed);
        auto tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return std::nullopt;
        T item = std::move(buf_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return item;
    }

    std::size_t capacity() const { return mask_ + 1; }

  private:
    static std::size_t next_power_of_two(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    std::size_t mask_;
    std::vector<T> buf_;

    // Separate cache lines to avoid false sharing between producer and consumer.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
