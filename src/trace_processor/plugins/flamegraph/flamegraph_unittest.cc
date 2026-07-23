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

#include "src/trace_processor/plugins/flamegraph/flamegraph.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::flamegraph {
namespace {

struct FrameSpec {
  std::string name;
  std::optional<uint32_t> parent;
  std::vector<double> metrics;
};

using Cell = std::variant<std::monostate, int64_t, double, std::string>;

Cell I(int64_t v) {
  return Cell(v);
}
Cell D(double v) {
  return Cell(v);
}
Cell S(const char* v) {
  return Cell(std::string(v));
}

struct CellCollector : core::dataframe::CellCallback {
  void OnCell(int64_t v) { value = v; }
  void OnCell(double v) { value = v; }
  void OnCell(NullTermStringView v) { value = std::string(v.data(), v.size()); }
  void OnCell(std::nullptr_t) { value = std::monostate(); }
  void OnCell(uint32_t v) { value = static_cast<int64_t>(v); }
  void OnCell(int32_t v) { value = static_cast<int64_t>(v); }
  Cell value;
};

void ExpectTable(const core::dataframe::Dataframe& df,
                 const std::vector<std::string>& cols,
                 const std::vector<std::vector<Cell>>& expected) {
  ASSERT_EQ(df.row_count(), expected.size());
  for (uint32_t r = 0; r < expected.size(); ++r) {
    std::vector<Cell> actual;
    for (const std::string& c : cols) {
      auto idx = df.IndexOfColumnLegacy(c);
      ASSERT_TRUE(idx.has_value()) << c;
      CellCollector collector;
      df.GetCell(r, *idx, collector);
      actual.push_back(collector.value);
    }
    EXPECT_EQ(actual, expected[r]) << "row " << r;
  }
}

// Path -> (self, cumulative, sorted constituent names): everything a test
// asserts on, independent of node and frame numbering.
using TreeSummary =
    std::map<std::string,
             std::tuple<std::vector<double>, std::vector<double>,
                        std::vector<std::string>>>;

class FlamegraphTest : public ::testing::Test {
 protected:
  Forest MakeForest(const std::vector<FrameSpec>& frames,
                    uint32_t metric_count = 1) {
    Forest f;
    f.metric_count = metric_count;
    std::map<std::string, uint32_t> name_idx;
    for (const FrameSpec& fr : frames) {
      auto [it, inserted] = name_idx.insert(
          {fr.name, static_cast<uint32_t>(f.name_table.size())});
      if (inserted) {
        f.name_table.push_back(
            pool_.InternString(base::StringView(fr.name)));
      }
      f.name.push_back(it->second);
      f.key.push_back(it->second);
      f.parent.push_back(fr.parent ? *fr.parent : kNoParent);
      PERFETTO_CHECK(fr.metrics.size() == metric_count);
      for (double m : fr.metrics) {
        f.metrics.push_back(m);
      }
    }
    return f;
  }

  Flamegraph BuildOk(const Forest& forest, const Config& config) {
    auto res = Build(forest, config, pool_);
    PERFETTO_CHECK(res.ok());
    return std::move(*res);
  }

  std::string NameOf(const Forest& forest, uint32_t frame) {
    return pool_.Get(forest.name_table[forest.name[frame]]).ToStdString();
  }

  TreeSummary Summarize(const Tree& tree, const Forest& forest) {
    uint32_t k = tree.metric_count;
    std::vector<std::string> path(tree.size());
    TreeSummary out;
    for (uint32_t i = 0; i < tree.size(); ++i) {
      path[i] = (tree.parent[i] == kNoParent ? ""
                                             : path[tree.parent[i]] + "/") +
                NameOf(forest, tree.rep_frame[i]);
      std::vector<double> self(tree.self.data() + i * k,
                               tree.self.data() + (i + 1) * k);
      std::vector<double> cum(tree.cumulative.data() + i * k,
                              tree.cumulative.data() + (i + 1) * k);
      std::vector<std::string> cons;
      for (uint32_t c = tree.constituents_offset[i];
           c < tree.constituents_offset[i + 1]; ++c) {
        cons.push_back(NameOf(forest, tree.constituents[c]));
      }
      std::sort(cons.begin(), cons.end());
      out[path[i]] = {self, cum, cons};
    }
    return out;
  }

  StringPool pool_;
};

TEST_F(FlamegraphTest, TopDownMergesSiblingsByKey) {
  // main -> {b, b, c}; the two b siblings merge, c stays under the merged
  // b node's sibling set.
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"b", 0, {2}},
      {"b", 0, {3}},
      {"c", 1, {4}},
  });
  Flamegraph fg = BuildOk(f, Config{});
  EXPECT_EQ(fg.up.size(), 0u);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s["main"],
            (std::tuple{std::vector<double>{1}, std::vector<double>{10},
                        std::vector<std::string>{"main"}}));
  EXPECT_EQ(s["main/b"],
            (std::tuple{std::vector<double>{5}, std::vector<double>{9},
                        std::vector<std::string>{"b", "b"}}));
  EXPECT_EQ(s["main/b/c"],
            (std::tuple{std::vector<double>{4}, std::vector<double>{4},
                        std::vector<std::string>{"c"}}));
}

TEST_F(FlamegraphTest, TopDownMergesRoots) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"main", std::nullopt, {2}},
      {"a", 1, {4}},
  });
  Flamegraph fg = BuildOk(f, Config{});
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<0>(s["main"]), std::vector<double>{3});
  EXPECT_EQ(std::get<1>(s["main"]), std::vector<double>{7});
  EXPECT_EQ(std::get<1>(s["main/a"]), std::vector<double>{4});
}

TEST_F(FlamegraphTest, NonTopologicalOrderMatchesTopological) {
  Forest ordered = MakeForest({
      {"main", std::nullopt, {1}},
      {"b", 0, {2}},
      {"b", 0, {3}},
      {"c", 1, {4}},
  });
  // The same forest with children listed before their parents.
  Forest shuffled = MakeForest({
      {"c", 3, {4}},
      {"b", 2, {3}},
      {"main", std::nullopt, {1}},
      {"b", 2, {2}},
  });
  Flamegraph a = BuildOk(ordered, Config{});
  Flamegraph b = BuildOk(shuffled, Config{});
  EXPECT_EQ(Summarize(a.down, ordered), Summarize(b.down, shuffled));
}

TEST_F(FlamegraphTest, DanglingParentsAndCyclesAreIgnored) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"dangling", 100, {10}},
      {"cycle_a", 3, {10}},
      {"cycle_b", 2, {10}},
  });
  Flamegraph fg = BuildOk(f, Config{});
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(std::get<1>(s["main"]), std::vector<double>{1});
}

TEST_F(FlamegraphTest, BottomUp) {
  // main -> a -> b; every frame has a value, so every frame anchors a
  // caller chain.
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"a", 0, {2}},
      {"b", 1, {4}},
  });
  Config config;
  config.view = Config::View::kBottomUp;
  Flamegraph fg = BuildOk(f, config);
  EXPECT_EQ(fg.down.size(), 0u);
  TreeSummary s = Summarize(fg.up, f);
  ASSERT_EQ(s.size(), 6u);
  EXPECT_EQ(std::get<1>(s["main"]), std::vector<double>{1});
  EXPECT_EQ(std::get<1>(s["a"]), std::vector<double>{2});
  EXPECT_EQ(std::get<1>(s["a/main"]), std::vector<double>{2});
  EXPECT_EQ(std::get<1>(s["b"]), std::vector<double>{4});
  EXPECT_EQ(std::get<1>(s["b/a"]), std::vector<double>{4});
  EXPECT_EQ(std::get<1>(s["b/a/main"]), std::vector<double>{4});
  // Self on an up node sums the frames at that chain position.
  EXPECT_EQ(std::get<0>(s["b/a"]), std::vector<double>{2});
  EXPECT_EQ(std::get<2>(s["b/a"]), std::vector<std::string>{"a"});
}

TEST_F(FlamegraphTest, BottomUpMergesSharedSuffixes) {
  // Two different b frames under main: their caller chains merge under
  // the same b root, and the shared main suffix appears once per anchor
  // in the constituents.
  Forest f = MakeForest({
      {"main", std::nullopt, {0}},
      {"b", 0, {2}},
      {"b", 0, {3}},
  });
  Config config;
  config.view = Config::View::kBottomUp;
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.up, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<1>(s["b"]), std::vector<double>{5});
  EXPECT_EQ(std::get<0>(s["b"]), std::vector<double>{5});
  EXPECT_EQ(std::get<1>(s["b/main"]), std::vector<double>{5});
  EXPECT_EQ(std::get<2>(s["b/main"]),
            (std::vector<std::string>{"main", "main"}));
}

TEST_F(FlamegraphTest, Pivot) {
  // main -> a -> x -> c and main -> x: pivoting on x roots both halves at
  // the merged x.
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"a", 0, {2}},
      {"x", 1, {3}},
      {"c", 2, {4}},
      {"x", 0, {5}},
  });
  Config config;
  config.view = Config::View::kPivot;
  config.pivot_pattern = "x";
  Flamegraph fg = BuildOk(f, config);

  TreeSummary down = Summarize(fg.down, f);
  ASSERT_EQ(down.size(), 2u);
  EXPECT_EQ(std::get<0>(down["x"]), std::vector<double>{8});
  EXPECT_EQ(std::get<1>(down["x"]), std::vector<double>{12});
  EXPECT_EQ(std::get<1>(down["x/c"]), std::vector<double>{4});

  TreeSummary up = Summarize(fg.up, f);
  ASSERT_EQ(up.size(), 4u);
  // Each anchor carries its own subtree total up its caller chain.
  EXPECT_EQ(std::get<1>(up["x"]), std::vector<double>{12});
  EXPECT_EQ(std::get<1>(up["x/a"]), std::vector<double>{7});
  EXPECT_EQ(std::get<1>(up["x/a/main"]), std::vector<double>{7});
  EXPECT_EQ(std::get<1>(up["x/main"]), std::vector<double>{5});
}

TEST_F(FlamegraphTest, PivotReRootsNestedMatches) {
  // x -> a -> x: the nested x starts its own root; the outer x's subtree
  // stops at it.
  Forest f = MakeForest({
      {"x", std::nullopt, {1}},
      {"a", 0, {2}},
      {"x", 1, {4}},
  });
  Config config;
  config.view = Config::View::kPivot;
  config.pivot_pattern = "x";
  Flamegraph fg = BuildOk(f, config);
  TreeSummary down = Summarize(fg.down, f);
  ASSERT_EQ(down.size(), 2u);
  EXPECT_EQ(std::get<0>(down["x"]), std::vector<double>{5});
  EXPECT_EQ(std::get<1>(down["x"]), std::vector<double>{7});
  EXPECT_EQ(std::get<1>(down["x/a"]), std::vector<double>{2});
  // Upward, the outer x weighs 3 (itself + a, stopping at the nested x)
  // and the nested x weighs 4; both merge into the x root.
  TreeSummary up = Summarize(fg.up, f);
  EXPECT_EQ(std::get<1>(up["x"]), std::vector<double>{7});
  EXPECT_EQ(std::get<1>(up["x/a"]), std::vector<double>{4});
  EXPECT_EQ(std::get<1>(up["x/a/x"]), std::vector<double>{4});
}

TEST_F(FlamegraphTest, HideFrameFoldsValuesIntoKeptAncestor) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"hideme", 0, {2}},
      {"b", 1, {4}},
  });
  Config config;
  config.filters.push_back({Config::FilterKind::kHideFrame, "hideme"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s["main"],
            (std::tuple{std::vector<double>{3}, std::vector<double>{7},
                        std::vector<std::string>{"main"}}));
  EXPECT_EQ(std::get<1>(s["main/b"]), std::vector<double>{4});
}

TEST_F(FlamegraphTest, HiddenRootValuesAreDropped) {
  Forest f = MakeForest({
      {"hideme", std::nullopt, {1}},
      {"b", 0, {4}},
  });
  Config config;
  config.filters.push_back({Config::FilterKind::kHideFrame, "hideme"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(std::get<1>(s["b"]), std::vector<double>{4});
}

TEST_F(FlamegraphTest, ShowStackDropsValuelessSubtrees) {
  Forest f = MakeForest({
      {"root", std::nullopt, {1}},
      {"keepme", 0, {2}},
      {"other", 0, {4}},
  });
  Config config;
  config.filters.push_back({Config::FilterKind::kShowStack, "keepme"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  // root's own value still shows as self, but does not count towards any
  // cumulative value (its stack prefix has no match).
  EXPECT_EQ(std::get<0>(s["root"]), std::vector<double>{1});
  EXPECT_EQ(std::get<1>(s["root"]), std::vector<double>{2});
  EXPECT_EQ(std::get<1>(s["root/keepme"]), std::vector<double>{2});
}

TEST_F(FlamegraphTest, HideStack) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"bad", 0, {2}},
      {"c", 1, {3}},
      {"ok", 0, {4}},
  });
  Config config;
  config.filters.push_back({Config::FilterKind::kHideStack, "bad"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<1>(s["main"]), std::vector<double>{5});
  EXPECT_EQ(std::get<1>(s["main/ok"]), std::vector<double>{4});
}

TEST_F(FlamegraphTest, ShowFromFrame) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"start", 0, {2}},
      {"b", 1, {3}},
  });
  Config config;
  config.filters.push_back({Config::FilterKind::kShowFromFrame, "start"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<1>(s["start"]), std::vector<double>{5});
  EXPECT_EQ(std::get<1>(s["start/b"]), std::vector<double>{3});
}

TEST_F(FlamegraphTest, MultipleMetrics) {
  Forest f = MakeForest(
      {
          {"main", std::nullopt, {1, 0}},
          {"a", 0, {0, 2}},
          {"b", 0, {0, 0}},
      },
      2);
  Flamegraph fg = BuildOk(f, Config{});
  TreeSummary s = Summarize(fg.down, f);
  // b is zero on every metric and is dropped; a survives via metric 1.
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<0>(s["main"]), (std::vector<double>{1, 0}));
  EXPECT_EQ(std::get<1>(s["main"]), (std::vector<double>{1, 2}));
  EXPECT_EQ(std::get<1>(s["main/a"]), (std::vector<double>{0, 2}));
}

TEST_F(FlamegraphTest, EmptyForest) {
  Forest f = MakeForest({});
  Flamegraph fg = BuildOk(f, Config{});
  EXPECT_EQ(fg.down.size(), 0u);
  EXPECT_EQ(fg.up.size(), 0u);
  ASSERT_EQ(fg.down.constituents_offset.size(), 1u);
  EXPECT_EQ(fg.down.constituents_offset[0], 0u);
}

TEST_F(FlamegraphTest, FiltersMatchExtraStrings) {
  // Filters also match the extra strings attached to a name entry (how
  // builders expose property values folded into the key).
  Forest f = MakeForest({
      {"root", std::nullopt, {1}},
      {"aa", 0, {2}},
      {"bb", 0, {4}},
  });
  f.match_offset.push_back(0);  // root: no extras.
  f.match_offset.push_back(0);  // aa: one extra, "libkeep.so".
  f.match_offset.push_back(1);
  f.match_offset.push_back(1);  // bb: none.
  f.match_strings.push_back(pool_.InternString("libkeep.so"));
  Config config;
  config.filters.push_back({Config::FilterKind::kShowStack, "libkeep"});
  Flamegraph fg = BuildOk(f, config);
  TreeSummary s = Summarize(fg.down, f);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(std::get<1>(s["root"]), std::vector<double>{2});
  EXPECT_EQ(std::get<1>(s["root/aa"]), std::vector<double>{2});
}

TEST_F(FlamegraphTest, ToDataframe) {
  Forest f = MakeForest({
      {"main", std::nullopt, {1}},
      {"b", 0, {2}},
      {"b", 0, {3}},
  });
  Flamegraph fg = BuildOk(f, Config{});
  auto df = ToDataframe(fg, f, &pool_);
  ASSERT_TRUE(df.ok());
  ExpectTable(
      *df,
      {"id", "parentId", "depth", "name", "selfValue", "cumulativeValue"},
      {
          {I(0), I(-1), I(1), S("main"), I(1), I(6)},
          {I(1), I(0), I(2), S("b"), I(5), I(5)},
      });
}

TEST_F(FlamegraphTest, ToDataframeBottomUpAndDoubles) {
  // A fractional value makes the metric's columns DOUBLE; the up tree
  // emits negative depths.
  Forest f = MakeForest({
      {"main", std::nullopt, {1.5}},
      {"a", 0, {2}},
  });
  Config config;
  config.view = Config::View::kBottomUp;
  Flamegraph fg = BuildOk(f, config);
  auto df = ToDataframe(fg, f, &pool_);
  ASSERT_TRUE(df.ok());
  ExpectTable(
      *df,
      {"id", "parentId", "depth", "name", "selfValue", "cumulativeValue"},
      {
          {I(0), I(-1), I(-1), S("main"), D(1.5), D(1.5)},
          {I(1), I(-1), I(-1), S("a"), D(2), D(2)},
          {I(2), I(1), I(-2), S("main"), D(1.5), D(2)},
      });
}

TEST_F(FlamegraphTest, ToDataframeMultipleMetrics) {
  Forest f = MakeForest(
      {
          {"main", std::nullopt, {1, 0}},
          {"a", 0, {0, 2.5}},
      },
      2);
  Flamegraph fg = BuildOk(f, Config{});
  auto df = ToDataframe(fg, f, &pool_);
  ASSERT_TRUE(df.ok());
  ExpectTable(*df,
              {"id", "parentId", "depth", "name", "selfValue0",
               "cumulativeValue0", "selfValue1", "cumulativeValue1"},
              {
                  {I(0), I(-1), I(1), S("main"), I(1), I(1), D(0), D(2.5)},
                  {I(1), I(0), I(2), S("a"), I(0), I(0), D(2.5), D(2.5)},
              });
}

TEST_F(FlamegraphTest, ConfigValidation) {
  Forest f = MakeForest({{"main", std::nullopt, {1}}});
  Config pivot_without_pattern;
  pivot_without_pattern.view = Config::View::kPivot;
  EXPECT_FALSE(Build(f, pivot_without_pattern, pool_).ok());

  Config pattern_without_pivot;
  pattern_without_pivot.pivot_pattern = "x";
  EXPECT_FALSE(Build(f, pattern_without_pivot, pool_).ok());

  Forest no_metrics;
  no_metrics.metric_count = 0;
  EXPECT_FALSE(Build(no_metrics, Config{}, pool_).ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::flamegraph
