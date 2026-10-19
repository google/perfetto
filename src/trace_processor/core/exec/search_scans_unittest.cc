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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/eq_scans.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/index_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Value = std::variant<std::nullptr_t, int64_t, double, std::string>;

// Bigger than an int32, so a column holding it is stored as an int64.
constexpr int64_t kBig = int64_t{1} << 40;

// Gives each filter value slot its value.
struct Values : ValueFetcher {
  using Type = size_t;
  static constexpr Type kNull = base::variant_index<Value, std::nullptr_t>();
  static constexpr Type kInt64 = base::variant_index<Value, int64_t>();
  static constexpr Type kDouble = base::variant_index<Value, double>();
  static constexpr Type kString = base::variant_index<Value, std::string>();

  int64_t GetInt64Value(uint32_t i) const {
    return std::get<int64_t>((*values)[i]);
  }
  double GetDoubleValue(uint32_t i) const {
    return std::get<double>((*values)[i]);
  }
  const char* GetStringValue(uint32_t i) const {
    return std::get<std::string>((*values)[i]).c_str();
  }
  Type GetValueType(uint32_t i) const { return (*values)[i].index(); }
  bool IteratorInit(uint32_t) { PERFETTO_FATAL("No IN"); }
  bool IteratorNext(uint32_t) { PERFETTO_FATAL("No IN"); }

  const std::vector<Value>* values = nullptr;
};

enum Col : uint32_t {
  kTs,
  kSet,
  kSmall,
  kNeg,
  kDbl,
  kName,
  kDur,
  kMaybe,
  kTag,
  kId
};

class SearchScansDiffTest : public ::testing::Test {
 protected:
  void Build(uint32_t rows) {
    dataframe::AdhocDataframeBuilder builder(
        {"ts", "set", "small", "neg", "dbl", "name", "dur", "maybe", "tag"},
        &pool_);
    int64_t ts = int64_t{1} << 33;
    uint32_t set = 0;
    uint32_t small = 1;
    int64_t neg = -1000;
    double dbl = 0;
    uint32_t name = 0;
    maybe_.clear();
    for (uint32_t i = 0; i < rows; ++i) {
      ts += rng_() % 3;
      // A SetId column holds, for each run of rows, the first row of the run.
      if (rng_() % 3 == 0) {
        set = i;
      }
      small += 1 + rng_() % 3;
      neg += rng_() % 2;
      dbl += (rng_() % 2) * 0.5;
      name += rng_() % 2;
      PERFETTO_CHECK(builder.PushNonNull(kTs, ts));
      PERFETTO_CHECK(builder.PushNonNull(kSet, int64_t{set}));
      PERFETTO_CHECK(builder.PushNonNull(kSmall, int64_t{small}));
      PERFETTO_CHECK(builder.PushNonNull(kNeg, neg));
      PERFETTO_CHECK(builder.PushNonNull(kDbl, dbl));
      PERFETTO_CHECK(builder.PushNonNull(
          kName, pool_.InternString(
                     base::StackString<16>("s%05u", name).string_view())));
      PERFETTO_CHECK(builder.PushNonNull(kDur, int64_t(rng_() % 1000)));
      if (rng_() % 4 == 0) {
        builder.PushNull(kMaybe);
        maybe_.emplace_back(std::nullopt);
      } else {
        maybe_.emplace_back(kBig + int64_t(rng_() % 50));
        PERFETTO_CHECK(builder.PushNonNull(kMaybe, *maybe_.back()));
      }
      PERFETTO_CHECK(builder.PushNonNull(
          kTag,
          pool_.InternString(
              base::StackString<16>("t%u", static_cast<uint32_t>(rng_() % 20))
                  .string_view())));
    }
    auto df = std::move(builder).Build();
    PERFETTO_CHECK(df.ok());
    // Indexes on unsorted columns of each kind: an integer, a sparse nullable
    // integer, and a string, the last two with a second column.
    for (std::vector<uint32_t> cols : std::vector<std::vector<uint32_t>>{
             {kDur}, {kMaybe, kDur}, {kTag, kDur}}) {
      auto index = df->BuildIndex(cols.data(), cols.data() + cols.size());
      PERFETTO_CHECK(index.ok());
      *df = df->AddIndex(std::move(*index));
    }
    df_ = std::make_unique<dataframe::Dataframe>(std::move(*df));
    PERFETTO_CHECK(df_->column_names()[kId] == dataframe::kAutoIdColumnName);
  }

  // A value near one in a random row of `col`, sometimes of another type.
  Value RandomValue(uint32_t col) {
    uint32_t row = static_cast<uint32_t>(rng_() % df_->row_count());
    if (col == kMaybe) {
      // A sparse column without a popcount has no random access.
      return maybe_[row] ? Value(*maybe_[row]) : Value(nullptr);
    }
    struct Cell : dataframe::CellCallback {
      void OnCell(int64_t v) { out = v; }
      void OnCell(double v) { out = v; }
      void OnCell(NullTermStringView v) { out = std::string(v.c_str()); }
      void OnCell(std::nullptr_t) { out = nullptr; }
      void OnCell(uint32_t v) { out = int64_t{v}; }
      void OnCell(int32_t v) { out = int64_t{v}; }
      Value out;
    } cell;
    df_->GetCell(row, col, cell);
    switch (rng_() % 8) {
      case 0:
        // Between two integers, or two doubles.
        if (auto* i = std::get_if<int64_t>(&cell.out)) {
          return static_cast<double>(*i) + 0.5;
        }
        if (auto* d = std::get_if<double>(&cell.out)) {
          return *d + 0.25;
        }
        return std::string("s00000x");
      case 1:
        // The wrong type.
        return std::holds_alternative<std::string>(cell.out)
                   ? Value(int64_t{3})
                   : Value(std::string("s00001"));
      case 2:
        // Out of range either way.
        return rng_() % 2 ? Value(int64_t{-(int64_t{1} << 40)})
                          : Value(int64_t{1} << 40);
      case 3:
        if (auto* i = std::get_if<int64_t>(&cell.out)) {
          return *i + 1;
        }
        return cell.out;
      default:
        return cell.out;
    }
  }

  // Runs `specs` with `values` through the interpreter, returning the rows.
  std::vector<uint32_t> RunInterpreter(std::vector<dataframe::FilterSpec> specs,
                                       const std::vector<Value>& slots) {
    auto plan = df_->PlanQuery(specs, {}, {}, {}, uint64_t{1} << kMaybe);
    PERFETTO_CHECK(plan.ok());
    dataframe::Cursor<Values> cursor;
    df_->PrepareCursor(*plan, cursor);
    Values fetcher;
    fetcher.values = &slots;
    cursor.Execute(fetcher);
    std::vector<uint32_t> rows;
    for (; !cursor.Eof(); cursor.Next()) {
      rows.push_back(cursor.RowIndex());
    }
    return rows;
  }

  // An executor plan for a logical plan which only filters: a scan doing the
  // searches, if any, then a Filter for the rest. A plan using an index gets
  // an IndexScan; one equality search which needs no dispatch gets its own
  // scan; anything else gets a SortedScan.
  struct ExecPlan {
    std::unique_ptr<Source> scan;
    std::unique_ptr<Pipeline> pipeline;
    std::vector<FilterValueCaster<Values>> casters;
  };
  std::optional<ExecPlan> Lower(const dataframe::LogicalPlan& plan) {
    std::vector<std::shared_ptr<const dataframe::Column>> columns;
    for (uint32_t i = 0; i < df_->column_names().size(); ++i) {
      columns.push_back(df_->shared_column(i));
    }
    ExecPlan out;
    out.casters.resize(plan.filter_value_count);
    std::vector<SortedScan::Search> searches;
    std::vector<Filter::Condition> searches_as_conditions;
    std::vector<Filter::Condition> conditions;
    std::vector<IndexScan::Equality> equalities;
    const dataframe::Index* index = nullptr;
    bool empty = false;
    for (const auto& op : plan.ops) {
      if (const auto* f = std::get_if<dataframe::logical::Filter>(&op)) {
        if (!f->op.IsAnyOf<NonStringOp>() && !f->op.IsAnyOf<NullOp>()) {
          return std::nullopt;
        }
        if (f->value_index && !f->op.IsAnyOf<NullOp>()) {
          out.casters[*f->value_index] =
              GetFilterValueCaster<Values>(f->storage, f->op);
        }
        using K = SortedScan::Search::Kind;
        std::optional<K> kind;
        switch (f->strategy.index()) {
          case base::variant_index<dataframe::logical::FilterStrategy,
                                   dataframe::logical::BinarySearch>():
            kind = K::kBinarySearch;
            ++binary_searches_;
            break;
          case base::variant_index<dataframe::logical::FilterStrategy,
                                   dataframe::logical::SetIdSortedSearch>():
            kind = K::kSetIdEq;
            ++setid_searches_;
            break;
          case base::variant_index<dataframe::logical::FilterStrategy,
                                   dataframe::logical::SmallValueLookup>():
            kind = K::kSmallValueEq;
            ++small_value_searches_;
            break;
          default:
            break;
        }
        if (kind) {
          searches.push_back(
              {df_->shared_column(f->col), *kind, f->op, *f->value_index});
          searches_as_conditions.push_back({f->col, f->op, *f->value_index});
        } else {
          conditions.push_back({f->col, f->op, f->value_index.value_or(0)});
        }
      } else if (const auto* idx =
                     std::get_if<dataframe::logical::IndexFilter>(&op)) {
        for (const auto& p : idx->predicates) {
          if (!p.op.Is<Eq>()) {
            return std::nullopt;
          }
          out.casters[*p.value_index] =
              GetFilterValueCaster<Values>(p.storage, p.op);
          equalities.push_back({df_->shared_column(p.col), *p.value_index});
        }
        index = &df_->indexes()[idx->index];
      } else if (std::holds_alternative<dataframe::logical::Empty>(op)) {
        empty = true;
      } else if (!std::holds_alternative<dataframe::logical::Scan>(op) &&
                 !std::holds_alternative<dataframe::logical::Output>(op)) {
        return std::nullopt;
      }
    }
    uint32_t rows = empty ? 0 : df_->row_count();
    using K = SortedScan::Search::Kind;
    const SortedScan::Search* one =
        searches.size() == 1 && searches[0].op.Is<Eq>() ? &searches[0]
                                                        : nullptr;
    if (empty) {
      // Provably empty, whatever else the plan does.
      out.scan = std::make_unique<DataframeScan>(std::move(columns), 0);
    } else if (index) {
      // Through an index, any search of a sorted column is left to the Filter.
      conditions.insert(conditions.end(), searches_as_conditions.begin(),
                        searches_as_conditions.end());
      out.scan = std::make_unique<IndexScan>(std::move(columns), *index,
                                             std::move(equalities), &pool_);
      ++index_scans_;
    } else if (searches.empty()) {
      out.scan = std::make_unique<DataframeScan>(std::move(columns), rows);
    } else if (one && one->kind == K::kSetIdEq) {
      out.scan = std::make_unique<SetIdEqScan>(std::move(columns), rows,
                                               one->column, one->value);
      ++set_id_eq_scans_;
    } else if (one && one->kind == K::kSmallValueEq) {
      out.scan = std::make_unique<SmallValueEqScan>(std::move(columns), rows,
                                                    one->column, one->value);
      ++small_value_eq_scans_;
    } else if (one && one->column->storage.type().Is<Id>()) {
      out.scan =
          std::make_unique<IdEqScan>(std::move(columns), rows, one->value);
      ++id_eq_scans_;
    } else {
      out.scan = std::make_unique<SortedScan>(std::move(columns), rows,
                                              std::move(searches), &pool_);
      ++sorted_scans_;
    }
    std::vector<Pipeline::Step> steps;
    if (!conditions.empty()) {
      steps.emplace_back(
          std::make_unique<Filter>(std::move(conditions), &pool_));
    }
    out.pipeline = std::make_unique<Pipeline>(*out.scan, std::move(steps));
    return out;
  }

  StringPool pool_;
  std::minstd_rand rng_{42};
  std::unique_ptr<dataframe::Dataframe> df_;
  // What each row of column kMaybe holds.
  std::vector<std::optional<int64_t>> maybe_;
  uint32_t binary_searches_ = 0;
  uint32_t setid_searches_ = 0;
  uint32_t small_value_searches_ = 0;
  uint32_t id_eq_scans_ = 0;
  uint32_t set_id_eq_scans_ = 0;
  uint32_t small_value_eq_scans_ = 0;
  uint32_t sorted_scans_ = 0;
  uint32_t index_scans_ = 0;
};

TEST_F(SearchScansDiffTest, FindsWhatTheInterpreterFinds) {
  const std::vector<Op> kOps = {Eq{}, Ne{}, Lt{},     Le{},
                                Gt{}, Ge{}, IsNull{}, IsNotNull{}};
  uint32_t compared = 0;
  uint32_t found = 0;
  for (uint32_t table = 0; table < 8; ++table) {
    // Small tables, and tables spanning several batches.
    Build(table % 2 ? 1 + rng_() % 50 : kMaxBatchRows * 2 + rng_() % 3000);
    for (uint32_t query = 0; query < 250; ++query) {
      std::vector<dataframe::FilterSpec> specs;
      std::vector<std::pair<uint32_t, Op>> shape;
      uint32_t filters = 1 + rng_() % 3;
      for (uint32_t i = 0; i < filters; ++i) {
        // Favour the sorted and indexed columns: they are what is searched.
        static constexpr uint32_t kSorted[] = {kTs,  kSet,  kSmall, kNeg,
                                               kDbl, kName, kId};
        static constexpr uint32_t kIndexed[] = {kDur, kMaybe, kTag};
        uint32_t col;
        switch (rng_() % 4) {
          case 0:
          case 1:
            col = kSorted[rng_() % 7];
            break;
          case 2:
            col = kIndexed[rng_() % 3];
            break;
          default:
            col = static_cast<uint32_t>(rng_() % 10);
            break;
        }
        Op op = kOps[rng_() % (rng_() % 4 ? 6 : kOps.size())];
        specs.push_back({col, i, op, std::nullopt});
        shape.emplace_back(col, op);
      }
      std::vector<dataframe::FilterSpec> planned = specs;
      auto logical = df_->PlanQueryLogicalForTesting(planned, {}, {}, {},
                                                     uint64_t{1} << kMaybe);
      ASSERT_TRUE(logical.ok());
      std::optional<ExecPlan> exec = Lower(*logical);
      if (!exec) {
        continue;
      }
      // The same plan runs with several sets of values through one cursor, as
      // a query does.
      RowCursor cursor(*exec->pipeline);
      for (uint32_t run = 0; run < 3; ++run) {
        std::vector<Value> by_source;
        for (const auto& [col, op] : shape) {
          by_source.push_back(RandomValue(col));
        }
        // The planner numbers the values in the order it applies them.
        std::vector<Value> slots(logical->filter_value_count);
        for (const auto& spec : planned) {
          if (spec.value_index) {
            slots[*spec.value_index] = by_source[spec.source_index];
          }
        }
        std::vector<uint32_t> expected = RunInterpreter(specs, slots);

        Values fetcher;
        fetcher.values = &slots;
        std::vector<FilterValue> cast(slots.size());
        for (uint32_t i = 0; i < slots.size(); ++i) {
          if (exec->casters[i]) {
            exec->casters[i](fetcher, i, pool_, &cast[i]);
          }
        }
        std::vector<uint32_t> actual;
        for (bool ok = cursor.Open(Span<const FilterValue>(
                 cast.data(), cast.data() + cast.size()));
             ok; ok = cursor.Next()) {
          uint32_t row = cursor.Value<uint32_t>(kId);
          actual.push_back(row);
          // A column without a slot per row reads its own row's value.
          std::optional<int64_t> maybe;
          if (cursor.batch().IsValid(kMaybe, cursor.row())) {
            maybe = cursor.Value<int64_t>(kMaybe);
          }
          ASSERT_EQ(maybe, maybe_[row]) << row;
        }
        ASSERT_TRUE(cursor.status().ok());
        ASSERT_EQ(actual, expected) << logical->ToString();
        ++compared;
        found += !expected.empty();
      }
    }
  }
  // Enough queries found rows, and every kind of search ran, for the
  // comparison to mean something.
  EXPECT_GT(found, compared / 4);
  EXPECT_GT(binary_searches_, 0u);
  EXPECT_GT(setid_searches_, 0u);
  EXPECT_GT(small_value_searches_, 0u);
  EXPECT_GT(id_eq_scans_, 0u);
  EXPECT_GT(set_id_eq_scans_, 0u);
  EXPECT_GT(small_value_eq_scans_, 0u);
  EXPECT_GT(sorted_scans_, 0u);
  EXPECT_GT(index_scans_, 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
