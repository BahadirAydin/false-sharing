#include "cpp_work/spsc_queue.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <random>
#include <semaphore>
#include <thread>

namespace {
using namespace std::chrono_literals;
using cpp_work::IndexLayout;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
  }
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Gate {
  std::binary_semaphore entered{0};
  std::binary_semaphore resume{0};
  void pause() noexcept {
    entered.release();
    CHECK(resume.try_acquire_for(10s));
  }
  void wait() { CHECK(entered.try_acquire_for(10s)); }
};

struct Payload {
  static inline std::atomic<int> live{0};
  int value;
  Gate* on_move;
  explicit Payload(int v, Gate* construction = nullptr, Gate* moving = nullptr) noexcept
      : value(v), on_move(moving) {
    if (construction) construction->pause();
    ++live;
  }
  Payload(Payload&& other) noexcept : value(other.value), on_move(nullptr) {
    if (auto* gate = std::exchange(other.on_move, nullptr)) gate->pause();
    ++live;
  }
  Payload(const Payload&) = delete;
  Payload& operator=(Payload&&) = delete;
  ~Payload() { --live; }
};

template <IndexLayout Layout, bool Cached>
void basic_and_model() {
  using Q = cpp_work::SpscQueue<std::uint64_t, 8, Layout, Cached>;
  Q queue;
  static_assert(Q::capacity == 7);
  CHECK(!queue.try_pop());
  for (std::uint64_t i = 0; i < Q::capacity; ++i) CHECK(queue.try_emplace(i));
  CHECK(!queue.try_emplace(999));
  for (std::uint64_t i = 0; i < Q::capacity; ++i) CHECK(queue.try_pop() == i);
  CHECK(!queue.try_pop());

  // Mixed full, empty, partially filled, and wraparound states, against an oracle.
  std::mt19937 random(20260913);
  std::deque<std::uint64_t> reference;
  for (std::uint64_t i = 0; i < 100'000; ++i) {
    if ((random() & 1u) != 0) {
      const bool expected = reference.size() < Q::capacity;
      CHECK(queue.try_emplace(i) == expected);
      if (expected) reference.push_back(i);
    } else {
      const auto value = queue.try_pop();
      CHECK(value.has_value() == !reference.empty());
      if (value) {
        CHECK(*value == reference.front());
        reference.pop_front();
      }
    }
  }
  while (!reference.empty()) {
    CHECK(queue.try_pop() == reference.front());
    reference.pop_front();
  }
  CHECK(!queue.try_pop());

  cpp_work::SpscQueue<int, 2, Layout, Cached> tiny;
  for (int i = 0; i < 1000; ++i) {
    CHECK(tiny.try_emplace(i));
    CHECK(!tiny.try_emplace(-1));
    CHECK(tiny.try_pop() == i);
    CHECK(!tiny.try_pop());
  }

  const auto [head, tail] = queue.index_addresses();
  const bool same = reinterpret_cast<std::uintptr_t>(head) / cpp_work::cache_line_size ==
                    reinterpret_cast<std::uintptr_t>(tail) / cpp_work::cache_line_size;
  CHECK(same == (Layout == IndexLayout::shared));
}

template <IndexLayout Layout, bool Cached>
void ownership() {
  cpp_work::SpscQueue<std::unique_ptr<int>, 2, Layout, Cached> queue;
  auto first = std::make_unique<int>(42);
  CHECK(queue.try_push(std::move(first)));
  CHECK(!first);
  auto second = std::make_unique<int>(99);
  CHECK(!queue.try_push(std::move(second)));
  CHECK(second && *second == 99);  // A failed push must not consume its argument.
  const auto result = queue.try_pop();
  CHECK(result && **result == 42);
  CHECK(!queue.try_pop());

  CHECK(Payload::live == 0);
  {
    cpp_work::SpscQueue<Payload, 4, Layout, Cached> objects;
    CHECK(objects.try_emplace(1));
    CHECK(objects.try_emplace(2));
    CHECK(Payload::live == 2);
    {
      auto object = objects.try_pop();
      CHECK(object && object->value == 1);
      CHECK(Payload::live == 2);
    }
    CHECK(Payload::live == 1);
    // Destroy with one item still queued.
  }
  CHECK(Payload::live == 0);
}

template <IndexLayout Layout, bool Cached>
void publication_and_reuse() {
  using Q = cpp_work::SpscQueue<Payload, 2, Layout, Cached>;
  {
    Q queue;
    Gate construction;
    std::jthread producer([&] { CHECK(queue.try_emplace(7, &construction)); });
    construction.wait();  // Producer is inside T's constructor, before publication.
    CHECK(!queue.try_pop());
    construction.resume.release();
    producer.join();
    CHECK(queue.try_pop()->value == 7);
  }
  {
    Q queue;
    Gate moving;
    CHECK(queue.try_emplace(10, nullptr, &moving));
    std::jthread consumer([&] {
      auto item = queue.try_pop();
      CHECK(item && item->value == 10);
    });
    moving.wait();  // Consumer is still moving from slot zero.
    CHECK(!queue.try_emplace(20));
    moving.resume.release();
    consumer.join();
    CHECK(queue.try_emplace(20));
    CHECK(queue.try_pop()->value == 20);
  }
  CHECK(Payload::live == 0);
}

template <IndexLayout Layout, bool Cached, std::size_t Slots>
void concurrent_fifo() {
  cpp_work::SpscQueue<std::uint64_t, Slots, Layout, Cached> queue;
  constexpr std::uint64_t count = 250'000;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  auto check_deadline = [&](std::uint64_t attempt) {
    if ((attempt & 4095u) == 0) CHECK(std::chrono::steady_clock::now() < deadline);
  };
  std::jthread producer([&] {
    std::uint64_t attempt = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      while (!queue.try_emplace(i)) check_deadline(++attempt);
      if ((i & 1023u) == 0) std::this_thread::yield();
    }
  });
  std::jthread consumer([&] {
    std::uint64_t attempt = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::optional<std::uint64_t> item;
      while (!(item = queue.try_pop())) check_deadline(++attempt);
      CHECK(*item == i);
      if ((i & 2047u) == 0) std::this_thread::yield();
    }
  });
  producer.join();
  consumer.join();
  CHECK(!queue.try_pop());
}

template <IndexLayout Layout, bool Cached>
void suite() {
  basic_and_model<Layout, Cached>();
  ownership<Layout, Cached>();
  publication_and_reuse<Layout, Cached>();
  concurrent_fifo<Layout, Cached, 2>();
  concurrent_fifo<Layout, Cached, 8>();
  concurrent_fifo<Layout, Cached, 1024>();
  std::cout << "PASS " << (Layout == IndexLayout::shared ? "shared" : "separated")
            << " cached=" << Cached << " (6 test groups)\n";
}
}  // namespace

int main() {
  suite<IndexLayout::shared, false>();
  suite<IndexLayout::shared, true>();
  suite<IndexLayout::separated, false>();
  suite<IndexLayout::separated, true>();
  std::cout << "PASS all 24 test groups\n";
}
