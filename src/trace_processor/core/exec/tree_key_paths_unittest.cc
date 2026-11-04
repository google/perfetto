/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/exec/tree_key_paths.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

// Emits id, parent id and a key, `chunk_rows` rows at a time.
class KeyedRows final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  KeyedRows(std::vector<int64_t> parents,
            std::vector<int64_t> keys,
            uint32_t chunk_rows)
      : parents_(std::move(parents)),
        keys_(std::move(keys)),
        chunk_rows_(chunk_rows) {}

  std::unique_ptr<OperatorState> MakeState(Context& context) const override {
    auto state = std::make_unique<State>();
    state->context = &context;
    return state;
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto total = static_cast<uint32_t>(parents_.size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, total - s.offset);
    s.ids.resize(count);
    s.parents.resize(count);
    s.keys.resize(count);
    s.validity = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t row = s.offset + i;
      s.ids[i] = row;
      s.keys[i] = keys_[row];
      s.parents[i] = std::max<int64_t>(parents_[row], 0);
      if (parents_[row] >= 0) {
        s.validity.set(i);
      }
    }
    out.Reset();
    test::AddCopy(*s.context, s.ids, nullptr, &out);
    test::AddCopy(*s.context, s.parents, &s.validity, &out);
    test::AddCopy(*s.context, s.keys, nullptr, &out);
    out.SetRowCount(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    Context* context = nullptr;
    uint32_t offset = 0;
    std::vector<int64_t> ids;
    std::vector<int64_t> parents;
    std::vector<int64_t> keys;
    BitVector validity;
  };

  std::vector<int64_t> parents_;
  std::vector<int64_t> keys_;
  uint32_t chunk_rows_;
};

KeyedRows::State::~State() = default;

struct Paths {
  std::vector<int64_t> keys;
  std::vector<std::optional<int64_t>> parent_keys;
};

Paths KeyPaths(const std::vector<int64_t>& parents,
               const std::vector<int64_t>& keys,
               uint32_t chunk_rows) {
  KeyedRows source(parents, keys, chunk_rows);
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  ops.push_back(std::make_unique<TreeParentFirst>(3, 4));
  ops.push_back(std::make_unique<TreeKeyPaths>(TreeKeyPathsSpec{3, 4, {2}}));
  Pipeline pipeline(source, std::move(ops));

  std::unique_ptr<OperatorState> state =
      pipeline.MakeState(test::TestContext());
  RowBatch batch;
  Paths result;
  result.keys.assign(parents.size(), -1);
  result.parent_keys.assign(parents.size(), std::nullopt);
  while (pipeline.GetData(batch, *state)) {
    FlatColumnReader<int64_t> ids(batch, 0);
    FlatColumnReader<int64_t> path(batch, 5);
    FlatColumnReader<int64_t> parent_path(batch, 6);
    for (uint32_t row = 0; row < batch.size(); ++row) {
      int64_t id = 0;
      int64_t value = 0;
      EXPECT_TRUE(ids.Read(row, &id));
      EXPECT_TRUE(path.Read(row, &value));
      result.keys[static_cast<size_t>(id)] = value;
      if (parent_path.Read(row, &value)) {
        result.parent_keys[static_cast<size_t>(id)] = value;
      }
    }
  }
  EXPECT_TRUE(pipeline.status(*state).ok())
      << pipeline.status(*state).message();
  return result;
}

constexpr int64_t kRoot = -1;

TEST(TreeKeyPathsTest, CousinsWithSamePathShareAKey) {
  // Two 1 nodes under the root, and a 2 under each.
  Paths paths = KeyPaths({kRoot, 0, 0, 1, 2}, {0, 1, 1, 2, 2}, 2048);
  EXPECT_THAT(paths.keys, ElementsAre(0, 1, 1, 2, 2));
  EXPECT_THAT(paths.parent_keys, ElementsAre(std::nullopt, 0, 0, 1, 1));
}

TEST(TreeKeyPathsTest, SameKeyUnderDifferentPathsDiffers) {
  // 7 under 1 and 7 under 2.
  Paths paths = KeyPaths({kRoot, 0, 0, 1, 2}, {0, 1, 2, 7, 7}, 2048);
  EXPECT_NE(paths.keys[3], paths.keys[4]);
  EXPECT_THAT(paths.parent_keys,
              ElementsAre(std::nullopt, paths.keys[0], paths.keys[0],
                          paths.keys[1], paths.keys[2]));
}

TEST(TreeKeyPathsTest, RootsAreKeyedByTheirColumns) {
  Paths paths = KeyPaths({kRoot, kRoot, kRoot}, {5, 6, 5}, 2048);
  EXPECT_THAT(paths.keys, ElementsAre(0, 1, 0));
  EXPECT_THAT(paths.parent_keys,
              ElementsAre(std::nullopt, std::nullopt, std::nullopt));
}

TEST(TreeKeyPathsTest, ChainWithinABatchAndAcrossBatches) {
  // Every row's parent is the row before, so each must wait for the last.
  std::vector<int64_t> parents{kRoot};
  std::vector<int64_t> keys{0};
  for (int64_t i = 1; i < 5000; ++i) {
    parents.push_back(i - 1);
    keys.push_back(i % 3);
  }
  for (uint32_t chunk_rows : {1u, 7u, 2048u}) {
    Paths paths = KeyPaths(parents, keys, chunk_rows);
    for (uint32_t i = 0; i < keys.size(); ++i) {
      ASSERT_EQ(paths.keys[i], i);
      if (i > 0) {
        ASSERT_EQ(paths.parent_keys[i], i - 1);
      }
    }
  }
}

TEST(TreeKeyPathsTest, MatchesTheDefinitionAcrossBatchSizes) {
  std::vector<int64_t> parents{kRoot, kRoot};
  std::vector<int64_t> keys{0, 1};
  uint64_t rand = 42;
  for (uint32_t i = 2; i < 6000; ++i) {
    rand = rand * 6364136223846793005u + 1442695040888963407u;
    parents.push_back(static_cast<int64_t>((rand >> 33) % i));
    keys.push_back(static_cast<int64_t>((rand >> 20) % 3));
  }

  std::map<std::pair<int64_t, int64_t>, int64_t> numbers;
  std::vector<int64_t> expected;
  for (size_t i = 0; i < parents.size(); ++i) {
    int64_t parent = parents[i] < 0 ? -1 : expected[size_t(parents[i])];
    auto [it, inserted] = numbers.emplace(std::make_pair(parent, keys[i]),
                                          int64_t(numbers.size()));
    expected.push_back(it->second);
  }

  for (uint32_t chunk_rows : {1u, 13u, 2048u}) {
    EXPECT_EQ(KeyPaths(parents, keys, chunk_rows).keys, expected);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
