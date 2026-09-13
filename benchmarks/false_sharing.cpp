#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cpp_work/spsc_queue.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using cpp_work::IndexLayout;
constexpr auto line = cpp_work::cache_line_size;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

void relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#else
  std::this_thread::yield();
#endif
}

void pin(int cpu) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  const int error = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
  if (error) {
    std::cerr << "Cannot pin to CPU " << cpu << ": " << std::strerror(error) << '\n';
    std::exit(EXIT_FAILURE);  // Stop both participants, including one at the start barrier.
  }
}

bool shares_line(const void* a, const void* b) {
  return reinterpret_cast<std::uintptr_t>(a) / line == reinterpret_cast<std::uintptr_t>(b) / line;
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Options {
  int cpu_a = -1;
  int cpu_b = -1;
  int cpu_smt = -1;
  std::uint64_t items = 2'000'000;
  int rounds = 11;
  std::uint32_t seed = 20260913;
};
Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string key(argv[i]);
    if (key == "--help") {
      std::cout << "--cpu-a N --cpu-b N [--cpu-smt N] [--items N] [--rounds N] [--seed N]\n";
      std::exit(EXIT_SUCCESS);
    }
    require(i + 1 < argc, "Every option requires an integer value");
    const std::string value(argv[++i]);
    std::uint64_t number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    require(result.ec == std::errc{} && result.ptr == value.data() + value.size(),
            "Invalid integer argument");
    if (key == "--cpu-a" || key == "--cpu-b" || key == "--cpu-smt") {
      require(number < CPU_SETSIZE, "CPU index exceeds CPU_SETSIZE");
      const auto cpu = static_cast<int>(number);
      if (key == "--cpu-a") o.cpu_a = cpu;
      if (key == "--cpu-b") o.cpu_b = cpu;
      if (key == "--cpu-smt") o.cpu_smt = cpu;
    } else if (key == "--items") {
      require(number > 0 && number <= 1'000'000'000, "items must be in [1, 1000000000]");
      o.items = number;
    } else if (key == "--rounds") {
      require(number > 0 && number <= 1000, "rounds must be in [1, 1000]");
      o.rounds = static_cast<int>(number);
    } else if (key == "--seed") {
      require(number <= UINT32_MAX, "Seed exceeds uint32 range");
      o.seed = static_cast<std::uint32_t>(number);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }
  require(o.cpu_a >= 0 && o.cpu_b >= 0 && o.cpu_a != o.cpu_b,
          "Specify two distinct CPUs using --cpu-a and --cpu-b");
  require(o.cpu_smt < 0 || (o.cpu_smt != o.cpu_a && o.cpu_smt != o.cpu_b),
          "SMT control must be a third logical CPU");
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  require(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "sched_getaffinity failed");
  for (const int cpu : {o.cpu_a, o.cpu_b, o.cpu_smt}) {
    if (cpu >= 0) require(CPU_ISSET(cpu, &allowed), "Selected CPU is outside allowed affinity");
  }
  return o;
}

struct alignas(line) SameLine {
  std::atomic<std::uint64_t> a{0};
  std::atomic<std::uint64_t> b{0};
};
static_assert(sizeof(SameLine) == line);
struct SeparateLines {
  alignas(line) std::atomic<std::uint64_t> a{0};
  alignas(line) std::atomic<std::uint64_t> b{0};
};

struct Sample {
  double elapsed_ns;
  std::uint64_t producer_full = 0;
  std::uint64_t consumer_empty = 0;
};

// Start in a barrier completion after affinity has been set. End timestamps are
// taken inside the workers, before join. The coordinator blocks while they run.
// For counter pairs, divide the latest completion time by increments PER THREAD,
// not the sum of both threads' increments.
Sample counters(std::atomic<std::uint64_t>& a, std::atomic<std::uint64_t>* b, std::uint64_t count,
                int cpu_a, int cpu_b) {
  Clock::time_point start;
  struct alignas(line) Finish {
    Clock::time_point time;
  } first{}, second{};
  std::barrier gate(b ? 3 : 2, [&]() noexcept { start = Clock::now(); });
  auto worker = [&](auto* counter, int cpu, auto& end) {
    pin(cpu);
    gate.arrive_and_wait();
    for (std::uint64_t i = 0; i < count; ++i) counter->fetch_add(1, std::memory_order_relaxed);
    end.time = Clock::now();
  };
  std::jthread t1([&] { worker(&a, cpu_a, first); });
  std::jthread t2;
  if (b) t2 = std::jthread([&] { worker(b, cpu_b, second); });
  gate.arrive_and_wait();
  t1.join();
  if (b) t2.join();
  if (b == &a) {
    require(a.load() == 2 * count, "Shared counter lost increments");
  } else {
    require(a.load() == count, "Counter A lost increments");
    if (b) require(b->load() == count, "Counter B lost increments");
  }
  const auto end = b ? std::max(first.time, second.time) : first.time;
  return {std::chrono::duration<double, std::nano>(end - start).count()};
}

template <IndexLayout Layout, bool Cached>
Sample queue_run(std::uint64_t count, int cpu_a, int cpu_b) {
  using Q = cpp_work::SpscQueue<std::uint64_t, 1024, Layout, Cached>;
  auto queue = std::make_unique<Q>();  // Construct and allocate outside timing.
  const auto [head, tail] = queue->index_addresses();
  require(shares_line(head, tail) == (Layout == IndexLayout::shared), "Wrong queue index layout");
  struct alignas(line) ProducerResult {
    std::uint64_t full = 0;
  } producer_result;
  struct alignas(line) ConsumerResult {
    Clock::time_point finish;
    std::uint64_t empty = 0;
    bool valid = true;
  } consumer_result;
  Clock::time_point start;
  std::barrier gate(3, [&]() noexcept { start = Clock::now(); });
  std::jthread producer([&] {
    pin(cpu_a);
    gate.arrive_and_wait();
    std::uint64_t full = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      while (!queue->try_emplace(i)) {
        ++full;
        relax();
      }
    }
    producer_result.full = full;
  });
  std::jthread consumer([&] {
    pin(cpu_b);
    gate.arrive_and_wait();
    std::uint64_t empty = 0;
    bool valid = true;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::optional<std::uint64_t> value;
      while (!(value = queue->try_pop())) {
        ++empty;
        relax();
      }
      valid &= (*value == i);  // Check EVERY value: missing, duplicate, reordered, corrupted.
    }
    consumer_result.finish = Clock::now();
    consumer_result.empty = empty;
    consumer_result.valid = valid;
  });
  gate.arrive_and_wait();
  producer.join();
  consumer.join();
  require(consumer_result.valid, "Queue failed FIFO validation");
  require(!queue->try_pop(), "Queue retained an unexpected item");
  return {std::chrono::duration<double, std::nano>(consumer_result.finish - start).count(),
          producer_result.full, consumer_result.empty};
}

struct Case {
  std::string group;
  std::string name;
  std::string placement;
  std::function<Sample(std::uint64_t)> run;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    std::vector<Case> cases;
    cases.push_back({"counters", "single_writer", "one_thread", [&](auto n) {
                       SameLine data;
                       return counters(data.a, nullptr, n, options.cpu_a, options.cpu_b);
                     }});
    auto shared = [&](auto n, int second_cpu) {
      SameLine data;
      require(shares_line(&data.a, &data.b), "Counters must share a line");
      return counters(data.a, &data.b, n, options.cpu_a, second_cpu);
    };
    cases.push_back({"counters", "shared_line", "different_cores",
                     [&](auto n) { return shared(n, options.cpu_b); }});
    cases.push_back({"counters", "separate_lines", "different_cores", [&](auto n) {
                       SeparateLines data;
                       require(!shares_line(&data.a, &data.b),
                               "Counters must occupy different lines");
                       return counters(data.a, &data.b, n, options.cpu_a, options.cpu_b);
                     }});
    cases.push_back({"counters", "same_counter", "different_cores", [&](auto n) {
                       SameLine data;
                       return counters(data.a, &data.a, n, options.cpu_a, options.cpu_b);
                     }});
    if (options.cpu_smt >= 0) {
      cases.push_back({"counters", "shared_line", "smt_siblings",
                       [&](auto n) { return shared(n, options.cpu_smt); }});
    }
    cases.push_back({"queue", "shared_uncached", "different_cores", [&](auto n) {
                       return queue_run<IndexLayout::shared, false>(n, options.cpu_a,
                                                                    options.cpu_b);
                     }});
    cases.push_back({"queue", "shared_cached", "different_cores", [&](auto n) {
                       return queue_run<IndexLayout::shared, true>(n, options.cpu_a, options.cpu_b);
                     }});
    cases.push_back({"queue", "separated_uncached", "different_cores", [&](auto n) {
                       return queue_run<IndexLayout::separated, false>(n, options.cpu_a,
                                                                       options.cpu_b);
                     }});
    cases.push_back({"queue", "separated_cached", "different_cores", [&](auto n) {
                       return queue_run<IndexLayout::separated, true>(n, options.cpu_a,
                                                                      options.cpu_b);
                     }});
    using Q = cpp_work::SpscQueue<std::uint64_t, 1024>;
    std::cerr << "C++=" << __cplusplus << " cache_line=" << line
              << " payload_bytes=" << sizeof(std::uint64_t)
              << " slot_storage_bytes=" << Q::slot_storage_bytes
              << " usable_capacity=" << Q::capacity << " validation=every_item\n";
    std::mt19937 random(options.seed);
    std::shuffle(cases.begin(), cases.end(), random);
    const auto warmup = std::min(options.items, std::uint64_t{100'000});
    for (const auto& c : cases) (void)c.run(warmup);
    std::cout << "group,variant,placement,round,operations,elapsed_ns,ns_per_op,ops_per_second,"
                 "producer_full,consumer_empty\n";
    std::cout << std::fixed << std::setprecision(6);
    for (int round = 0; round < options.rounds; ++round) {
      std::shuffle(cases.begin(), cases.end(), random);
      for (const auto& c : cases) {
        const auto sample = c.run(options.items);
        const auto ns = sample.elapsed_ns / static_cast<double>(options.items);
        std::cout << c.group << ',' << c.name << ',' << c.placement << ',' << round << ','
                  << options.items << ',' << sample.elapsed_ns << ',' << ns << ',' << 1e9 / ns
                  << ',' << sample.producer_full << ',' << sample.consumer_empty << '\n';
      }
      std::cerr << "Completed round " << round + 1 << '/' << options.rounds << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
