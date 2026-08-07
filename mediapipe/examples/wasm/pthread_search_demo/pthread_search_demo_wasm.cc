#ifndef __EMSCRIPTEN__
#error "This file must only be compiled for the emscripten/wasm platform."
#endif  // __EMSCRIPTEN__

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/bind.h>

namespace mediapipe {
namespace {

// Sentinel search target. The rest of the array is filled from
// dist(0, 1'000'000'000), so this value never collides by chance.
constexpr int kTarget = -1;

// How many array elements a worker scans between two checks of the shared
// "found" flag. Checking every single iteration would add a load + branch
// to the hot loop for no benefit (readers sharing a cache line don't
// contend with each other); checking too rarely delays the stop. A few
// thousand elements is a good balance for an int scan.
constexpr int kCheckInterval = 4096;

std::vector<int> MakeRandomData(int size, int target_index) {
  std::vector<int> data(size);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 1'000'000'000);
  for (auto& v : data) v = dist(rng);
  if (target_index >= 0 && target_index < size) data[target_index] = kTarget;
  return data;
}

}  // namespace

// Proof of concept for parallel search *without* a merge phase: unlike
// sorting (pthread_sort_demo/), finding a value doesn't need the partial
// results stitched back together — the first worker to find it just needs
// to tell the others to stop. Each of N std::thread scans its own
// contiguous slice of one shared std::vector; a shared std::atomic<int>
// holds the winning index (-1 while unset) and doubles as both the
// "stop signal" other workers poll and the compare-and-swap that decides
// who the winner is if two threads matched at nearly the same time.
class SearchBenchmark {
 public:
  std::string RunBenchmark(int size, int num_threads, int target_percent) {
    std::ostringstream out;
    // target_percent < 0 means "not present" — forces a full scan on both
    // sides, the worst case and the one closest to linear speedup.
    int target_index = target_percent < 0
                            ? -1
                            : static_cast<int>(
                                  (static_cast<int64_t>(target_percent) * (size - 1)) / 100);
    out << "size=" << size << " numThreads=" << num_threads
        << " targetPercent=" << target_percent
        << (target_index < 0 ? " (not present)\n" : "\n");

    std::vector<int> data = MakeRandomData(size, target_index);

    auto t0 = std::chrono::steady_clock::now();
    int single_index = -1;
    for (int i = 0; i < size; ++i) {
      if (data[i] == kTarget) {
        single_index = i;
        break;
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    double single_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    int chunk_size = (size + num_threads - 1) / num_threads;
    std::atomic<int> found_index{-1};
    auto t2 = std::chrono::steady_clock::now();
    {
      std::vector<std::thread> workers;
      for (int i = 0; i < num_threads; ++i) {
        int begin = i * chunk_size;
        int end = std::min(begin + chunk_size, size);
        if (begin >= end) continue;
        workers.emplace_back([&data, &found_index, begin, end]() {
          for (int i = begin; i < end; ++i) {
            if ((i - begin) % kCheckInterval == 0 &&
                found_index.load(std::memory_order_relaxed) != -1) {
              return;  // Another worker already found it — stop scanning.
            }
            if (data[i] == kTarget) {
              int expected = -1;
              // First worker to land here wins; a losing compare_exchange
              // just means someone else's index is already recorded.
              found_index.compare_exchange_strong(expected, i, std::memory_order_relaxed);
              return;
            }
          }
        });
      }
      for (auto& w : workers) w.join();
    }
    // No merge step here — this is the whole point versus sort+merge.
    auto t3 = std::chrono::steady_clock::now();
    double multi_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    int multi_index = found_index.load(std::memory_order_relaxed);

    out << "single-threaded scan: " << single_ms << " ms (found=" << (single_index != -1)
        << ", index=" << single_index << ")\n";
    out << "multi-threaded (" << num_threads
        << " threads) scan+atomic-stop: " << multi_ms << " ms (found="
        << (multi_index != -1) << ", index=" << multi_index << ")\n";
    out << "speedup: " << (single_ms / multi_ms) << "x\n";
    return out.str();
  }
};

}  // namespace mediapipe

EMSCRIPTEN_BINDINGS(pthread_search_demo_module) {
  emscripten::class_<mediapipe::SearchBenchmark>("SearchBenchmark")
      .constructor<>()
      .function("runBenchmark", &mediapipe::SearchBenchmark::RunBenchmark);
}
