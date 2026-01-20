// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Example demonstrating high-performance interval self-intersection
// using the C++ implementation with bitsets for dense IDs.
//
// This example shows how to:
// 1. Generate a large set of overlapping intervals
// 2. Compute self-intersections efficiently
// 3. Aggregate data across overlapping intervals
// 4. Measure performance

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

namespace perfetto {
namespace trace_processor {

using Clock = std::chrono::high_resolution_clock;

// Event for sweep line algorithm
struct Event {
  int64_t ts;
  uint32_t id;
  bool is_start;

  bool operator<(const Event& other) const {
    if (ts != other.ts)
      return ts < other.ts;
    return is_start > other.is_start;  // starts before ends
  }
};

// Interval with metadata
struct Interval {
  uint32_t id;
  int64_t start_ts;
  int64_t end_ts;
  double value;  // Example value to aggregate
};

// Efficiently computes self-intersections and aggregations
class IntervalSelfIntersector {
 public:
  IntervalSelfIntersector() = default;
  ~IntervalSelfIntersector() = default;

  void AddInterval(const Interval& interval) { intervals_.push_back(interval); }

  void Finalize() {
    // Build events for sweep line
    events_.reserve(intervals_.size() * 2);
    for (const auto& interval : intervals_) {
      events_.push_back(Event{interval.start_ts, interval.id, true});
      events_.push_back(Event{interval.end_ts, interval.id, false});
    }
    std::sort(events_.begin(), events_.end());
    finalized_ = true;
  }

  // Process each bucket where active set is stable.
  // Callback: void(int64_t start_ts, int64_t end_ts, uint32_t count,
  //                double sum_value, double max_value)
  template <typename Callback>
  void ForEachBucket(Callback callback) const {
    if (!finalized_) {
      std::cerr << "Must call Finalize() before ForEachBucket()\n";
      return;
    }

    if (events_.empty())
      return;

    // Use bitset for dense IDs
    uint32_t max_id = 0;
    for (const auto& interval : intervals_) {
      max_id = std::max(max_id, interval.id);
    }

    std::vector<bool> active(max_id + 1, false);
    int64_t prev_ts = events_[0].ts;

    for (const auto& event : events_) {
      // Emit bucket [prev_ts, event.ts) before processing this event
      if (event.ts > prev_ts) {
        uint32_t count = 0;
        double sum_value = 0.0;
        double max_value = 0.0;

        for (size_t i = 0; i <= max_id; ++i) {
          if (active[i]) {
            count++;
            sum_value += intervals_[i].value;
            max_value = std::max(max_value, intervals_[i].value);
          }
        }

        if (count > 0) {
          callback(prev_ts, event.ts, count, sum_value, max_value);
        }
      }

      // Update active set
      if (event.is_start) {
        active[event.id] = true;
      } else {
        active[event.id] = false;
      }

      prev_ts = event.ts;
    }
  }

  size_t size() const { return events_.size(); }
  bool finalized() const { return finalized_; }

 private:
  std::vector<Interval> intervals_;
  std::vector<Event> events_;
  bool finalized_ = false;
};

}  // namespace trace_processor
}  // namespace perfetto

int main() {
  using namespace perfetto::trace_processor;

  constexpr uint32_t kNumIntervals = 100'000;
  constexpr int64_t kMaxTimestamp = 1'000'000'000;

  std::mt19937_64 rng(123);
  std::uniform_int_distribution<int64_t> dist_ts(0, kMaxTimestamp);
  std::uniform_int_distribution<int64_t> dist_len(1, 10'000);
  std::uniform_real_distribution<double> dist_value(1.0, 100.0);

  auto t0 = Clock::now();

  IntervalSelfIntersector intersector;

  // Generate and add intervals
  for (uint32_t i = 0; i < kNumIntervals; ++i) {
    int64_t start = dist_ts(rng);
    int64_t end = start + dist_len(rng);
    double value = dist_value(rng);
    intersector.AddInterval(Interval{i, start, end, value});
  }

  auto t1 = Clock::now();

  // Finalize (sort events)
  intersector.Finalize();

  auto t2 = Clock::now();

  // Process buckets and compute statistics
  size_t num_buckets = 0;
  size_t max_active = 0;
  int64_t total_coverage = 0;
  double total_sum_value = 0.0;
  std::unordered_map<uint32_t, int64_t> concurrency_histogram;

  intersector.ForEachBucket([&](int64_t start, int64_t end, uint32_t count,
                                double sum_value, double max_value) {
    num_buckets++;
    max_active = std::max(max_active, static_cast<size_t>(count));

    int64_t duration = end - start;
    total_coverage += duration;
    total_sum_value += sum_value * duration;
    concurrency_histogram[count] += duration;
  });

  auto t3 = Clock::now();

  // Print results
  auto ms = [](auto a, auto b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
  };

  std::cout << "=== Interval Self-Intersection Performance ===" << std::endl;
  std::cout << "Intervals:       " << kNumIntervals << "\n";
  std::cout << "Events:          " << intersector.size() << "\n";
  std::cout << "Buckets:         " << num_buckets << "\n";
  std::cout << "Max active:      " << max_active << "\n";
  std::cout << "Total coverage:  " << total_coverage << " ns\n";
  std::cout << "Total sum*dur:   " << total_sum_value << "\n";
  std::cout << "\n";

  std::cout << "=== Timing ===" << std::endl;
  std::cout << "Add intervals:   " << ms(t0, t1) << " ms\n";
  std::cout << "Finalize (sort): " << ms(t1, t2) << " ms\n";
  std::cout << "Process buckets: " << ms(t2, t3) << " ms\n";
  std::cout << "Total:           " << ms(t0, t3) << " ms\n";
  std::cout << "\n";

  // Print concurrency histogram (top 10 levels)
  std::vector<std::pair<uint32_t, int64_t>> hist_vec(
      concurrency_histogram.begin(), concurrency_histogram.end());
  std::sort(hist_vec.begin(), hist_vec.end());

  std::cout << "=== Concurrency Histogram (top 10) ===" << std::endl;
  size_t to_print = std::min(size_t{10}, hist_vec.size());
  for (size_t i = 0; i < to_print; ++i) {
    std::cout << "  " << hist_vec[i].first << " active: " << hist_vec[i].second
              << " ns\n";
  }

  std::cout << "\n=== Performance Summary ===" << std::endl;
  std::cout << "Throughput: " << (kNumIntervals * 1000.0 / ms(t0, t3))
            << " intervals/sec\n";
  std::cout << "Bucket processing rate: " << (num_buckets * 1000.0 / ms(t2, t3))
            << " buckets/sec\n";

  return 0;
}
