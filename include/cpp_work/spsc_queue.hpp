#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#ifndef CPP_WORK_CACHE_LINE_SIZE
#define CPP_WORK_CACHE_LINE_SIZE 64
#endif

namespace cpp_work {

// An experiment parameter, not a claim about every processor.
inline constexpr std::size_t cache_line_size = CPP_WORK_CACHE_LINE_SIZE;

enum class IndexLayout { shared, separated };

namespace detail {
struct Cursor {
  std::atomic<std::size_t> position{0};
  std::size_t cached_remote{0};  // Accessed only by the owner of this cursor.
};

template <IndexLayout Layout, std::size_t Line>
struct Indices;

template <std::size_t Line>
struct alignas(Line) Indices<IndexLayout::shared, Line> {
  static_assert(2 * sizeof(Cursor) <= Line, "Both cursors must fit on one line");
  Cursor producer;
  Cursor consumer;
};

template <std::size_t Line>
struct Indices<IndexLayout::separated, Line> {
  alignas(Line) Cursor producer;
  alignas(Line) Cursor consumer;
};
}  // namespace detail

// Exactly one producer and exactly one consumer. Construction/destruction and
// observer calls require quiescence. Slots is a power of two; usable capacity
// is Slots - 1. One empty slot distinguishes full from empty after wraparound.
//
// Payloads need not be default constructible or assignable. Nothrow construction
// and moves keep publication/reclamation free of exception recovery paths.
// The queue allocates no storage dynamically; T itself can allocate or block.
template <typename T, std::size_t Slots, IndexLayout Layout = IndexLayout::separated,
          bool CacheRemote = true, std::size_t Line = cache_line_size>
  requires(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>)
class SpscQueue {
  static_assert(Slots >= 2 && std::has_single_bit(Slots));
  static_assert(std::has_single_bit(Line) && Line >= alignof(detail::Cursor));
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "This implementation requires lock-free index atomics");

 public:
  using value_type = T;
  static constexpr std::size_t capacity = Slots - 1;
  static constexpr std::size_t slot_storage_bytes = sizeof(std::optional<T>);

  SpscQueue() noexcept = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;
  ~SpscQueue() = default;  // optional destroys any remaining live payloads.

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
    auto& own = indices_.producer;
    const auto head = own.position.load(std::memory_order_relaxed);
    const auto next = (head + 1) & mask;
    if constexpr (CacheRemote) {
      if (next == own.cached_remote) {
        own.cached_remote = indices_.consumer.position.load(std::memory_order_acquire);
        if (next == own.cached_remote) return false;
      }
    } else {
      if (next == indices_.consumer.position.load(std::memory_order_acquire)) return false;
    }
    slots_[head].emplace(std::forward<Args>(args)...);
    // Publish the fully constructed object to the consumer.
    own.position.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_push(T&& value) noexcept { return try_emplace(std::move(value)); }

  [[nodiscard]] std::optional<T> try_pop() noexcept {
    auto& own = indices_.consumer;
    const auto tail = own.position.load(std::memory_order_relaxed);
    if constexpr (CacheRemote) {
      if (tail == own.cached_remote) {
        own.cached_remote = indices_.producer.position.load(std::memory_order_acquire);
        if (tail == own.cached_remote) return std::nullopt;
      }
    } else {
      if (tail == indices_.producer.position.load(std::memory_order_acquire)) return std::nullopt;
    }
    std::optional<T> result(std::in_place, std::move(*slots_[tail]));
    slots_[tail].reset();
    // Publish completion of both the move and destruction before slot reuse.
    own.position.store((tail + 1) & mask, std::memory_order_release);
    return result;
  }

  // Layout diagnostics only; not a concurrent size/empty API.
  [[nodiscard]] std::pair<const void*, const void*> index_addresses() const noexcept {
    return {&indices_.producer.position, &indices_.consumer.position};
  }

 private:
  static constexpr std::size_t mask = Slots - 1;
  // Keep payload storage from overlapping the index lines in both variants.
  alignas(Line) std::array<std::optional<T>, Slots> slots_{};
  alignas(Line) detail::Indices<Layout, Line> indices_{};
};
}  // namespace cpp_work
