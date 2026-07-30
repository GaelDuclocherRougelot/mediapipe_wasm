#ifndef __EMSCRIPTEN__
#error "This file must only be compiled for the emscripten/wasm platform."
#endif  // __EMSCRIPTEN__

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/bind.h>

namespace mediapipe {
namespace {

std::vector<int> MakeRandomData(int size) {
  std::vector<int> data(size);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 1'000'000'000);
  for (auto& v : data) v = dist(rng);
  return data;
}

// Merges numThreads pre-sorted contiguous chunks of `data` into fully sorted
// order, in place, via repeated pairwise std::inplace_merge (doubling the
// merged width each pass, same shape as the merge phase of merge sort).
void MergeSortedChunks(std::vector<int>& data, int chunk_size) {
  int size = static_cast<int>(data.size());
  for (int width = chunk_size; width < size; width *= 2) {
    for (int left = 0; left < size; left += 2 * width) {
      int mid = std::min(left + width, size);
      int right = std::min(left + 2 * width, size);
      std::inplace_merge(data.begin() + left, data.begin() + mid, data.begin() + right);
    }
  }
}

}  // namespace

// Proof of concept for real parallel C++ work under Emscripten -pthread:
// sorts the same random array single-threaded (baseline) and multi-threaded
// (N std::thread, each sorting its own contiguous slice of one shared
// std::vector, joined, then merged), and reports both timings.
class SortBenchmark {
 public:
  std::string RunBenchmark(int size, int num_threads) {
    std::ostringstream out;
    out << "size=" << size << " numThreads=" << num_threads << "\n";

    std::vector<int> single = MakeRandomData(size);
    auto t0 = std::chrono::steady_clock::now();
    std::sort(single.begin(), single.end());
    auto t1 = std::chrono::steady_clock::now();
    double single_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    bool single_ok = std::is_sorted(single.begin(), single.end());

    std::vector<int> multi = MakeRandomData(size);
    int chunk_size = (size + num_threads - 1) / num_threads;
    auto t2 = std::chrono::steady_clock::now();
    {
      std::vector<std::thread> workers;
      for (int i = 0; i < num_threads; ++i) {
        int begin = i * chunk_size;
        int end = std::min(begin + chunk_size, size);
        if (begin >= end) continue;
        workers.emplace_back([&multi, begin, end]() {
          std::sort(multi.begin() + begin, multi.begin() + end);
        });
      }
      for (auto& w : workers) w.join();
    }
    MergeSortedChunks(multi, chunk_size);
    auto t3 = std::chrono::steady_clock::now();
    double multi_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    bool multi_ok = std::is_sorted(multi.begin(), multi.end());

    out << "single-threaded std::sort: " << single_ms << " ms (sorted=" << single_ok << ")\n";
    out << "multi-threaded (" << num_threads << " threads) sort+merge: " << multi_ms
        << " ms (sorted=" << multi_ok << ")\n";
    out << "speedup: " << (single_ms / multi_ms) << "x\n";
    return out.str();
  }
};

}  // namespace mediapipe

EMSCRIPTEN_BINDINGS(pthread_sort_demo_module) {
  emscripten::class_<mediapipe::SortBenchmark>("SortBenchmark")
      .constructor<>()
      .function("runBenchmark", &mediapipe::SortBenchmark::RunBenchmark);
}
