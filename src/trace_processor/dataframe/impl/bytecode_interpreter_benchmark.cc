/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bytecode_interpreter.h"
#include "src/trace_processor/dataframe/impl/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/dataframe/impl/bytecode_interpreter_test_utils.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/types.h"

// Include absl for comparison (suppress warnings from absl headers)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgcc-compat"
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include "absl/container/flat_hash_map.h"
#pragma clang diagnostic pop

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {

static void BM_BytecodeInterpreter_LinearFilterEqUint32(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;

  // Setup column
  FlexVector<uint32_t> col_data_vec;
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(i % 256);
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter
  std::string bytecode_str = R"(
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(0), op=Op(0)]
    InitRange: [size=1048576, dest_register=Register(1)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(3), dest_span_register=Register(2)]
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(0), source_register=Register(1), update_register=Register(2)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 4, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  fetcher.value.push_back(int64_t(123));

  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BytecodeInterpreter_LinearFilterEqUint32);

static void BM_BytecodeInterpreter_LinearFilterEqString(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;

  // Setup column
  StringPool spool;
  FlexVector<StringPool::Id> col_data_vec;
  std::vector<std::string> string_values;
  for (uint32_t i = 0; i < 256; ++i) {
    string_values.push_back("string_" + std::to_string(i));
  }
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(
        spool.InternString(base::StringView(string_values[i % 256])));
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter
  std::string bytecode_str = R"(
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(0), op=Op(0)]
    InitRange: [size=1048576, dest_register=Register(1)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(3), dest_span_register=Register(2)]
    LinearFilterEq<String>: [col=0, filter_value_reg=Register(0), source_register=Register(1), update_register=Register(2)]
  )";

  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 4, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  fetcher.value.push_back("string_123");

  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BytecodeInterpreter_LinearFilterEqString);

}  // namespace

static void BM_BytecodeInterpreter_SortUint32(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;

  // Setup column
  FlexVector<uint32_t> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(static_cast<uint32_t>(rnd()));
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 4, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BytecodeInterpreter_SortUint32);

static void BM_BytecodeInterpreter_SortString(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;

  // Setup column
  StringPool spool;
  FlexVector<StringPool::Id> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    uint32_t len = 5 + (rnd() % (32 - 6));
    std::string key;
    for (uint32_t j = 0; j < len; ++j) {
      key += static_cast<char>('a' + (rnd() % 26));
    }
    col_data_vec.push_back(spool.InternString(base::StringView(key)));
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    InitRankMap: [dest_register=Register(3)]
    CollectIdIntoRankMap: [col=0, source_register=Register(2), rank_map_register=Register(3)]
    FinalizeRanksInMap: [update_register=Register(3)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(4)]
    CopyToRowLayout<String, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(4), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(3)]
    SortRowLayout: [buffer_register=Register(4), total_row_stride=4, indices_register=Register(2)]
  )";

  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 5, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_BytecodeInterpreter_SortString);

// Benchmark hash-based grouping with low cardinality (many duplicates)
static void BM_BytecodeInterpreter_HashGroupUint32_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  // Setup column with low cardinality (~4000 duplicates per value)
  FlexVector<uint32_t> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(static_cast<uint32_t>(rnd()) % kUniqueValues);
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter with HashGroup bytecode
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(5), dest_span_register=Register(4)]
    HashGroup: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2), scratch_register=Register(4)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 6, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_BytecodeInterpreter_HashGroupUint32_LowCardinality);

// Benchmark hash-based grouping with high cardinality (few duplicates)
static void BM_BytecodeInterpreter_HashGroupUint32_HighCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 500000;  // ~2 duplicates per value

  // Setup column with high cardinality
  FlexVector<uint32_t> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(static_cast<uint32_t>(rnd()) % kUniqueValues);
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter with HashGroup bytecode
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(5), dest_span_register=Register(4)]
    HashGroup: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2), scratch_register=Register(4)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 6, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_BytecodeInterpreter_HashGroupUint32_HighCardinality);

// Benchmark hash-based grouping with strings (low cardinality)
static void BM_BytecodeInterpreter_HashGroupString_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  // Setup column with string data
  StringPool spool;
  FlexVector<StringPool::Id> col_data_vec;
  std::vector<std::string> string_values;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kUniqueValues; ++i) {
    uint32_t len = 8 + (rnd() % 9);  // 8-16 char strings
    std::string key;
    for (uint32_t j = 0; j < len; ++j) {
      key += static_cast<char>('a' + (rnd() % 26));
    }
    string_values.push_back(key);
  }
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(
        spool.InternString(base::StringView(string_values[i % kUniqueValues])));
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter with HashGroup bytecode
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    InitRankMap: [dest_register=Register(3)]
    CollectIdIntoRankMap: [col=0, source_register=Register(2), rank_map_register=Register(3)]
    FinalizeRanksInMap: [update_register=Register(3)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(4)]
    CopyToRowLayout<String, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(4), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(3)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(6), dest_span_register=Register(5)]
    HashGroup: [buffer_register=Register(4), total_row_stride=4, indices_register=Register(2), scratch_register=Register(5)]
  )";

  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 7, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_BytecodeInterpreter_HashGroupString_LowCardinality);

// Compare against sorting for same data (low cardinality uint32)
static void BM_BytecodeInterpreter_SortUint32_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  // Setup column with low cardinality (same as HashGroup benchmark)
  FlexVector<uint32_t> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(static_cast<uint32_t>(rnd()) % kUniqueValues);
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  // Setup interpreter with SortRowLayout (what GROUP BY used before)
  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 4, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_BytecodeInterpreter_SortUint32_LowCardinality);

// Direct hash map comparison benchmarks
// absl:: flat_hash_map only (base::FlatHashMap has different API)

static void BM_AbslHashMap_TwoPass_LowCardinality(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<std::string_view> keys;
  keys.reserve(kTableSize);
  for (uint32_t val : data) {
    keys.emplace_back(reinterpret_cast<const char*>(&val), sizeof(uint32_t));
  }

  std::vector<uint32_t> scratch(kTableSize);

  for (auto _ : state) {
    absl::flat_hash_map<std::string_view, uint32_t> group_info;

    // Pass 1: Count items per group
    for (size_t i = 0; i < kTableSize; ++i) {
      group_info[keys[i]]++;
    }

    // Transform counts to write positions
    uint32_t write_pos = 0;
    for (auto& [key, count] : group_info) {
      uint32_t old_count = count;
      count = write_pos;
      write_pos += old_count;
    }

    // Pass 2: Write indices to scratch
    for (size_t i = 0; i < kTableSize; ++i) {
      scratch[group_info[keys[i]]++] = static_cast<uint32_t>(i);
    }

    benchmark::DoNotOptimize(scratch.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_TwoPass_LowCardinality);

static void BM_AbslHashMap_SinglePass_LowCardinality(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<std::string_view> keys;
  keys.reserve(kTableSize);
  for (uint32_t val : data) {
    keys.emplace_back(reinterpret_cast<const char*>(&val), sizeof(uint32_t));
  }

  for (auto _ : state) {
    absl::flat_hash_map<std::string_view, std::vector<uint32_t>> groups;

    for (size_t i = 0; i < kTableSize; ++i) {
      groups[keys[i]].push_back(static_cast<uint32_t>(i));
    }

    size_t idx = 0;
    for (const auto& [key, vec] : groups) {
      idx += vec.size();
    }
    benchmark::DoNotOptimize(idx);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_SinglePass_LowCardinality);

static void BM_AbslHashMap_IntegerKeys_LowCardinality(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  for (auto _ : state) {
    absl::flat_hash_map<uint32_t, std::vector<uint32_t>> groups;

    for (size_t i = 0; i < kTableSize; ++i) {
      groups[data[i]].push_back(static_cast<uint32_t>(i));
    }

    size_t idx = 0;
    for (const auto& [key, vec] : groups) {
      idx += vec.size();
    }
    benchmark::DoNotOptimize(idx);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_IntegerKeys_LowCardinality);

// Test different inline sizes to find sweet spot
template <size_t kInlineSize>
static void BM_AbslHashMap_SmallVector_StringKeys_Template(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<std::string_view> keys;
  keys.reserve(kTableSize);
  for (uint32_t val : data) {
    keys.emplace_back(reinterpret_cast<const char*>(&val), sizeof(uint32_t));
  }

  for (auto _ : state) {
    absl::flat_hash_map<std::string_view,
                        base::SmallVector<uint32_t, kInlineSize>>
        groups;

    for (size_t i = 0; i < kTableSize; ++i) {
      groups[keys[i]].emplace_back(static_cast<uint32_t>(i));
    }

    size_t idx = 0;
    for (const auto& [key, vec] : groups) {
      idx += vec.size();
    }
    benchmark::DoNotOptimize(idx);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}

// Instantiate with different inline sizes
static void BM_AbslHashMap_SmallVector_StringKeys_64(benchmark::State& state) {
  BM_AbslHashMap_SmallVector_StringKeys_Template<64>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_StringKeys_64);

static void BM_AbslHashMap_SmallVector_StringKeys_256(benchmark::State& state) {
  BM_AbslHashMap_SmallVector_StringKeys_Template<256>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_StringKeys_256);

static void BM_AbslHashMap_SmallVector_StringKeys_512(benchmark::State& state) {
  BM_AbslHashMap_SmallVector_StringKeys_Template<512>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_StringKeys_512);

template <size_t kInlineSize>
static void BM_AbslHashMap_SmallVector_IntegerKeys_Template(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  for (auto _ : state) {
    absl::flat_hash_map<uint32_t, base::SmallVector<uint32_t, kInlineSize>>
        groups;

    for (size_t i = 0; i < kTableSize; ++i) {
      groups[data[i]].emplace_back(static_cast<uint32_t>(i));
    }

    size_t idx = 0;
    for (const auto& [key, vec] : groups) {
      idx += vec.size();
    }
    benchmark::DoNotOptimize(idx);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}

static void BM_AbslHashMap_SmallVector_IntegerKeys_64(benchmark::State& state) {
  BM_AbslHashMap_SmallVector_IntegerKeys_Template<64>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_IntegerKeys_64);

static void BM_AbslHashMap_SmallVector_IntegerKeys_256(
    benchmark::State& state) {
  BM_AbslHashMap_SmallVector_IntegerKeys_Template<256>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_IntegerKeys_256);

static void BM_AbslHashMap_SmallVector_IntegerKeys_512(
    benchmark::State& state) {
  BM_AbslHashMap_SmallVector_IntegerKeys_Template<512>(state);
}
BENCHMARK(BM_AbslHashMap_SmallVector_IntegerKeys_512);

// Group ID assignment approach - original 3-pass version
static void BM_AbslHashMap_GroupId_StringKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<std::string_view> keys;
  keys.reserve(kTableSize);
  for (uint32_t val : data) {
    keys.emplace_back(reinterpret_cast<const char*>(&val), sizeof(uint32_t));
  }

  std::vector<uint32_t> scratch(kTableSize);

  for (auto _ : state) {
    // Pass 1: Assign group IDs (only 1 hash lookup per element!)
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<std::string_view, uint32_t> key_to_group;
    uint32_t next_group_id = 0;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      auto [it, inserted] = key_to_group.try_emplace(keys[i], next_group_id);
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = it->second;
    }

    // Pass 2: Count elements per group (no hashing, just array access!)
    std::vector<uint32_t> group_counts(next_group_id, 0);
    for (uint32_t i = 0; i < kTableSize; ++i) {
      group_counts[group_id[i]]++;
    }

    // Transform counts to write positions
    uint32_t write_pos = 0;
    for (uint32_t& count : group_counts) {
      uint32_t old_count = count;
      count = write_pos;
      write_pos += old_count;
    }

    // Pass 3: Write indices using group_id (array access, not hash!)
    for (uint32_t i = 0; i < kTableSize; ++i) {
      scratch[group_counts[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(scratch.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupId_StringKeys_LowCardinality);

// Improved: Count while assigning group IDs (2-pass instead of 3-pass)
static void BM_AbslHashMap_GroupIdWithCount_StringKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<std::string_view> keys;
  keys.reserve(kTableSize);
  for (uint32_t val : data) {
    keys.emplace_back(reinterpret_cast<const char*>(&val), sizeof(uint32_t));
  }

  std::vector<uint32_t> scratch(kTableSize);

  for (auto _ : state) {
    // Pass 1: Assign group IDs AND count simultaneously
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<std::string_view, std::pair<uint32_t, uint32_t>>
        key_to_group;  // {group_id, count}
    uint32_t next_group_id = 0;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      auto [it, inserted] = key_to_group.try_emplace(keys[i], next_group_id, 0);
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = it->second.first;
      it->second.second++;  // Increment count
    }

    // Transform counts to positions (stored in separate array indexed by
    // group_id)
    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (const auto& [key, info] : key_to_group) {
      uint32_t gid = info.first;
      uint32_t count = info.second;
      group_positions[gid] = write_pos;
      write_pos += count;
    }

    // Pass 2: Write indices using group_id
    for (uint32_t i = 0; i < kTableSize; ++i) {
      scratch[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(scratch.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupIdWithCount_StringKeys_LowCardinality);

static void BM_AbslHashMap_GroupId_IntegerKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> scratch(kTableSize);

  for (auto _ : state) {
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<uint32_t, uint32_t> key_to_group;
    uint32_t next_group_id = 0;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      auto [it, inserted] = key_to_group.try_emplace(data[i], next_group_id);
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = it->second;
    }

    std::vector<uint32_t> group_counts(next_group_id, 0);
    for (uint32_t i = 0; i < kTableSize; ++i) {
      group_counts[group_id[i]]++;
    }

    uint32_t write_pos = 0;
    for (uint32_t& count : group_counts) {
      uint32_t old_count = count;
      count = write_pos;
      write_pos += old_count;
    }

    for (uint32_t i = 0; i < kTableSize; ++i) {
      scratch[group_counts[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(scratch.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupId_IntegerKeys_LowCardinality);

static void BM_AbslHashMap_GroupIdWithCount_IntegerKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> scratch(kTableSize);

  for (auto _ : state) {
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<uint32_t, std::pair<uint32_t, uint32_t>> key_to_group;
    uint32_t next_group_id = 0;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      auto [it, inserted] = key_to_group.try_emplace(data[i], next_group_id, 0);
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = it->second.first;
      it->second.second++;
    }

    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (const auto& [key, info] : key_to_group) {
      uint32_t gid = info.first;
      uint32_t count = info.second;
      group_positions[gid] = write_pos;
      write_pos += count;
    }

    for (uint32_t i = 0; i < kTableSize; ++i) {
      scratch[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(scratch.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupIdWithCount_IntegerKeys_LowCardinality);

// Optimized: No scratch buffer, write directly to output
static void BM_AbslHashMap_GroupIdDirect_IntegerKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> output(kTableSize);  // Simulate indices array

  for (auto _ : state) {
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<uint32_t, std::pair<uint32_t, uint32_t>> key_to_group;
    uint32_t next_group_id = 0;

    // Pass 1: Assign group IDs and count
    for (uint32_t i = 0; i < kTableSize; ++i) {
      uint32_t key = data[i];
      auto [it, inserted] = key_to_group.try_emplace(key, next_group_id, 0);
      uint32_t gid = it->second.first;
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = gid;
      it->second.second++;
    }

    // Transform counts to positions
    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (const auto& [key, info] : key_to_group) {
      group_positions[info.first] = write_pos;
      write_pos += info.second;
    }

    // Pass 2: Write directly to output (no scratch buffer!)
    for (uint32_t i = 0; i < kTableSize; ++i) {
      output[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupIdDirect_IntegerKeys_LowCardinality);

// High cardinality benchmarks (500K unique values out of 1M elements)
static void BM_AbslHashMap_SinglePass_IntegerKeys_HighCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 500 * 1024;  // 50% cardinality

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  for (auto _ : state) {
    absl::flat_hash_map<uint32_t, std::vector<uint32_t>> groups;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      groups[data[i]].push_back(i);
    }

    size_t count = 0;
    for (const auto& [key, vec] : groups) {
      count += vec.size();
    }
    benchmark::DoNotOptimize(count);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_SinglePass_IntegerKeys_HighCardinality);

static void BM_AbslHashMap_GroupIdWithCount_IntegerKeys_HighCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 500 * 1024;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> output(kTableSize);

  for (auto _ : state) {
    std::vector<uint32_t> group_id(kTableSize);
    absl::flat_hash_map<uint32_t, std::pair<uint32_t, uint32_t>> key_to_group;
    key_to_group.reserve(kUniqueValues);  // Avoid rehashing!
    uint32_t next_group_id = 0;

    for (uint32_t i = 0; i < kTableSize; ++i) {
      auto [it, inserted] = key_to_group.try_emplace(data[i], next_group_id, 0);
      if (inserted) {
        next_group_id++;
      }
      group_id[i] = it->second.first;
      it->second.second++;
    }

    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (const auto& [key, info] : key_to_group) {
      group_positions[info.first] = write_pos;
      write_pos += info.second;
    }

    for (uint32_t i = 0; i < kTableSize; ++i) {
      output[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_AbslHashMap_GroupIdWithCount_IntegerKeys_HighCardinality);

// Sort-based grouping for high cardinality (direct comparison)
static void BM_Sort_IntegerKeys_HighCardinality(benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 500 * 1024;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> output(kTableSize);

  for (auto _ : state) {
    // Create (key, index) pairs
    std::vector<std::pair<uint32_t, uint32_t>> key_index_pairs(kTableSize);
    for (uint32_t i = 0; i < kTableSize; ++i) {
      key_index_pairs[i] = {data[i], i};
    }

    // Sort by key (stable_sort maintains order within groups)
    std::stable_sort(
        key_index_pairs.begin(), key_index_pairs.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Write back sorted indices
    for (uint32_t i = 0; i < kTableSize; ++i) {
      output[i] = key_index_pairs[i].second;
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_Sort_IntegerKeys_HighCardinality);

// Direct addressing optimization for low-cardinality integer keys
static void BM_DirectAddress_GroupId_IntegerKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> output(kTableSize);

  for (auto _ : state) {
    std::vector<uint32_t> group_id(kTableSize);

    // Use direct addressing instead of hash table!
    // key_to_group[key] = {group_id, count}
    std::array<std::pair<uint32_t, uint32_t>, kUniqueValues> key_to_group;
    std::fill(key_to_group.begin(), key_to_group.end(),
              std::pair<uint32_t, uint32_t>{0xFFFFFFFF, 0});
    uint32_t next_group_id = 0;

    // Pass 1: Assign group IDs and count (no hash table lookups!)
    for (uint32_t i = 0; i < kTableSize; ++i) {
      uint32_t key = data[i];
      auto& entry = key_to_group[key];
      if (entry.first == 0xFFFFFFFF) {
        entry.first = next_group_id++;
      }
      group_id[i] = entry.first;
      entry.second++;
    }

    // Transform counts to positions
    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (uint32_t key = 0; key < kUniqueValues; ++key) {
      if (key_to_group[key].first != 0xFFFFFFFF) {
        uint32_t gid = key_to_group[key].first;
        group_positions[gid] = write_pos;
        write_pos += key_to_group[key].second;
      }
    }

    // Pass 2: Write indices
    for (uint32_t i = 0; i < kTableSize; ++i) {
      output[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_DirectAddress_GroupId_IntegerKeys_LowCardinality);

// AVX2-optimized Pass 2 (requires -mavx2 flag)
#ifdef __AVX2__
#include <immintrin.h>

static void BM_DirectAddress_AVX2_IntegerKeys_LowCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 256;

  std::vector<uint32_t> data(kTableSize);
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    data[i] = static_cast<uint32_t>(rnd()) % kUniqueValues;
  }

  std::vector<uint32_t> output(kTableSize);
  alignas(32) std::vector<uint32_t> group_id(kTableSize);

  for (auto _ : state) {
    std::array<std::pair<uint32_t, uint32_t>, kUniqueValues> key_to_group;
    std::fill(key_to_group.begin(), key_to_group.end(),
              std::pair<uint32_t, uint32_t>{0xFFFFFFFF, 0});
    uint32_t next_group_id = 0;

    // Pass 1: Direct addressing (same as before)
    for (uint32_t i = 0; i < kTableSize; ++i) {
      uint32_t key = data[i];
      auto& entry = key_to_group[key];
      if (entry.first == 0xFFFFFFFF) {
        entry.first = next_group_id++;
      }
      group_id[i] = entry.first;
      entry.second++;
    }

    std::vector<uint32_t> group_positions(next_group_id);
    uint32_t write_pos = 0;
    for (uint32_t key = 0; key < kUniqueValues; ++key) {
      if (key_to_group[key].first != 0xFFFFFFFF) {
        uint32_t gid = key_to_group[key].first;
        group_positions[gid] = write_pos;
        write_pos += key_to_group[key].second;
      }
    }

    // Pass 2: AVX2-vectorized write (process 8 elements at once)
    uint32_t i = 0;
    for (; i + 8 <= kTableSize; i += 8) {
      // Load 8 group IDs
      __m256i gids =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&group_id[i]));

      // Extract each group ID and write (scalar fallback for now)
      // Full vectorization would need AVX512 scatter
      alignas(32) uint32_t gid_array[8];
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(gid_array), gids);

      for (uint32_t j = 0; j < 8; ++j) {
        output[group_positions[gid_array[j]]++] = i + j;
      }
    }

    // Handle remaining elements
    for (; i < kTableSize; ++i) {
      output[group_positions[group_id[i]]++] = i;
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_DirectAddress_AVX2_IntegerKeys_LowCardinality);
#endif  // __AVX2__

// Sorting benchmark for high cardinality comparison
static void BM_BytecodeInterpreter_SortUint32_HighCardinality(
    benchmark::State& state) {
  constexpr uint32_t kTableSize = 1024 * 1024;
  constexpr uint32_t kUniqueValues = 500 * 1024;

  FlexVector<uint32_t> col_data_vec;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < kTableSize; ++i) {
    col_data_vec.push_back(static_cast<uint32_t>(rnd()) % kUniqueValues);
  }
  Column col{Storage{std::move(col_data_vec)}, NullStorage::NonNull{},
             Unsorted{}, HasDuplicates{}};
  Column* col_ptr = &col;

  std::string bytecode_str = R"(
    InitRange: [size=1048576, dest_register=Register(0)]
    AllocateIndices: [size=1048576, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=4194304, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2)]
  )";

  StringPool spool;
  std::vector<dataframe::Index> indexes;
  Interpreter<Fetcher> interpreter;
  interpreter.Initialize(ParseBytecodeToVec(bytecode_str), 4, &col_ptr,
                         indexes.data(), &spool);

  Fetcher fetcher;
  for (auto _ : state) {
    interpreter.Execute(fetcher);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(
      static_cast<int64_t>(state.iterations() * kTableSize));
}
BENCHMARK(BM_BytecodeInterpreter_SortUint32_HighCardinality);

}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
