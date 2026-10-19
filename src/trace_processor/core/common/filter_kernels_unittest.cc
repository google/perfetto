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

#include "src/trace_processor/core/common/filter_kernels.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::filter {
namespace {

const RangeOp kOps[] = {Eq{}, Lt{}, Le{}, Gt{}, Ge{}};

// What SearchSorted should keep of `range`, by std::lower_bound and
// std::upper_bound over `values`.
template <typename T, typename Less>
Range Expected(const std::vector<T>& values,
               RangeOp op,
               const T& value,
               Range range,
               Less less) {
  auto b = values.begin() + range.b;
  auto e = values.begin() + range.e;
  auto lower = static_cast<uint32_t>(std::lower_bound(b, e, value, less) -
                                     values.begin());
  auto upper = static_cast<uint32_t>(std::upper_bound(b, e, value, less) -
                                     values.begin());
  switch (op.index()) {
    case RangeOp::GetTypeIndex<Eq>():
      return {lower, upper};
    case RangeOp::GetTypeIndex<Lt>():
      return {range.b, lower};
    case RangeOp::GetTypeIndex<Le>():
      return {range.b, upper};
    case RangeOp::GetTypeIndex<Gt>():
      return {upper, range.e};
    default:
      return {lower, range.e};
  }
}

// An empty range is any empty range: which row it starts at is not part of
// the answer.
void ExpectSame(Range actual, Range expected) {
  if (expected.empty()) {
    ASSERT_TRUE(actual.empty()) << actual.b << " " << actual.e;
  } else {
    ASSERT_EQ(actual.b, expected.b);
    ASSERT_EQ(actual.e, expected.e);
  }
}

template <typename T>
std::vector<T> SortedValues(std::minstd_rand& rng, uint32_t size) {
  std::vector<T> values;
  T v = static_cast<T>(rng() % 10);
  for (uint32_t i = 0; i < size; ++i) {
    // Runs of equal values, so searches meet duplicates.
    v += static_cast<T>(rng() % 3 == 0);
    values.push_back(v);
  }
  return values;
}

std::vector<uint32_t> Sizes() {
  std::vector<uint32_t> sizes;
  for (uint32_t i = 0; i <= 70; ++i) {
    sizes.push_back(i);
  }
  for (uint32_t s : {127u, 128u, 129u, 1000u, 4096u, 100000u}) {
    sizes.push_back(s);
  }
  return sizes;
}

template <typename T>
void CheckNumeric(StorageType type) {
  std::minstd_rand rng(1);
  for (uint32_t size : Sizes()) {
    std::vector<T> values = SortedValues<T>(rng, size);
    SortedColumn column{type, values.data(), nullptr};
    for (uint32_t i = 0; i < 40; ++i) {
      uint32_t b = size ? static_cast<uint32_t>(rng() % (size + 1)) : 0;
      uint32_t e =
          b + (size - b ? static_cast<uint32_t>(rng() % (size - b + 1)) : 0);
      // Below, at, between and above the values.
      T value = size ? values[rng() % size] : T{};
      value += static_cast<T>(static_cast<int>(rng() % 5) - 2);
      for (RangeOp op : kOps) {
        Range actual = SearchSorted(
            column, op, CastFilterValueResult::Value(value), {b, e});
        ExpectSame(actual, Expected(values, op, value, {b, e}, std::less<>()));
      }
    }
  }
}

// The searches of numbers: every type runs the same code.
TEST(SearchSortedTest, Int64) {
  CheckNumeric<int64_t>(Int64{});
}

TEST(SearchSortedTest, String) {
  std::minstd_rand rng(2);
  StringPool pool;
  for (uint32_t size : Sizes()) {
    if (size > 4096) {
      continue;
    }
    std::vector<std::string> text;
    for (uint32_t v : SortedValues<uint32_t>(rng, size)) {
      // Zero-padded, so the strings sort as their numbers do.
      text.push_back(base::StackString<16>("s%06u", 2 * v).ToStdString());
    }
    std::vector<StringPool::Id> ids;
    for (const std::string& t : text) {
      ids.push_back(pool.InternString(base::StringView(t)));
    }
    SortedColumn column{String{}, ids.data(), &pool};
    for (uint32_t i = 0; i < 20; ++i) {
      uint32_t b = size ? static_cast<uint32_t>(rng() % (size + 1)) : 0;
      uint32_t e =
          b + (size - b ? static_cast<uint32_t>(rng() % (size - b + 1)) : 0);
      // In the column, or between two of its strings and so in no row.
      uint32_t n =
          size ? *base::StringToUInt32(text[rng() % size].substr(1)) : 0;
      n += static_cast<uint32_t>(rng() % 3) - 1;
      std::string value = base::StackString<16>("s%06u", n).ToStdString();
      for (RangeOp op : kOps) {
        Range actual = SearchSorted(
            column, op, CastFilterValueResult::Value(value.c_str()), {b, e});
        ExpectSame(actual, Expected(text, op, value, {b, e}, std::less<>()));
      }
    }
  }
}

// -----------------------------------------------------------------------------
// SearchIndexEqual
// -----------------------------------------------------------------------------

// A column of `rows` rows, a value in some and null in the rest, laid out for
// each nullability, and an index of it.
template <typename T>
struct IndexedColumn {
  std::vector<std::optional<T>> rows;
  std::vector<T> dense;
  std::vector<T> packed;
  BitVector present;
  std::vector<uint32_t> popcount;
  // The rows in order of their values, nulls first, equal ones by row: as
  // Dataframe::BuildIndex orders them.
  std::vector<uint32_t> index;

  IndexColumn For(Nullability nullability, StorageType type) const {
    IndexColumn c;
    c.type = type;
    c.nullability = nullability;
    c.nulls = nullability.Is<NonNull>() ? nullptr : &present;
    c.popcount = popcount.data();
    c.data = nullability.Is<SparseNull>() ? packed.data() : dense.data();
    return c;
  }
};

template <typename T>
IndexedColumn<T> MakeIndexed(std::minstd_rand& rng,
                             uint32_t size,
                             bool nullable) {
  IndexedColumn<T> c;
  c.present = BitVector::CreateWithSize(size, false);
  for (uint32_t i = 0; i < size; ++i) {
    if (nullable && rng() % 4 == 0) {
      c.rows.emplace_back(std::nullopt);
      c.dense.push_back(T{});
    } else {
      // Few distinct values, so each is in many rows.
      auto v = static_cast<T>(rng() % 20);
      c.rows.emplace_back(v);
      c.dense.push_back(v);
      c.packed.push_back(v);
      c.present.set(i);
    }
  }
  Slab<uint32_t> popcount = c.present.PrefixPopcount();
  c.popcount.assign(popcount.begin(), popcount.end());
  for (uint32_t i = 0; i < size; ++i) {
    c.index.push_back(i);
  }
  std::stable_sort(c.index.begin(), c.index.end(), [&](uint32_t a, uint32_t b) {
    return c.rows[a] < c.rows[b];
  });
  return c;
}

template <typename T>
void CheckIndexEqual(StorageType type) {
  std::minstd_rand rng(3);
  const Nullability kNullabilities[] = {NonNull{}, DenseNull{}, SparseNull{}};
  for (uint32_t size : Sizes()) {
    for (Nullability n : kNullabilities) {
      IndexedColumn<T> c = MakeIndexed<T>(rng, size, !n.Is<NonNull>());
      IndexColumn column = c.For(n, type);
      for (uint32_t i = 0; i < 20; ++i) {
        // Values in the column, and out of its range either way.
        auto value = static_cast<T>(static_cast<int>(rng() % 24) - 2);
        std::vector<uint32_t> index = c.index;
        Span<uint32_t> entries(index.data(), index.data() + index.size());
        Span<uint32_t> actual = SearchIndexEqual(
            column, CastFilterValueResult::Value(value), entries);
        auto [lb, ub] = std::equal_range(
            index.begin(), index.end(), std::optional<T>(value),
            [&](const auto& a, const auto& b) {
              auto at = [&](const auto& x) {
                if constexpr (std::is_same_v<std::decay_t<decltype(x)>,
                                             uint32_t>) {
                  return c.rows[x];
                } else {
                  return x;
                }
              };
              return at(a) < at(b);
            });
        ASSERT_EQ(actual.b - index.data(), lb - index.begin());
        ASSERT_EQ(actual.e - index.data(), ub - index.begin());
      }
    }
  }
}

// The searches of numbers: every type runs the same code.
TEST(SearchIndexEqualTest, Uint32) {
  CheckIndexEqual<uint32_t>(Uint32{});
}

TEST(SearchIndexEqualTest, String) {
  std::minstd_rand rng(4);
  StringPool pool;
  for (uint32_t size : Sizes()) {
    std::vector<std::string> text;
    std::vector<StringPool::Id> ids;
    std::vector<uint32_t> index;
    for (uint32_t i = 0; i < size; ++i) {
      text.push_back(
          base::StackString<16>("s%02u", static_cast<uint32_t>(rng() % 20))
              .ToStdString());
      ids.push_back(pool.InternString(base::StringView(text.back())));
      index.push_back(i);
    }
    std::stable_sort(index.begin(), index.end(),
                     [&](uint32_t a, uint32_t b) { return text[a] < text[b]; });
    IndexColumn column;
    column.type = String{};
    column.data = ids.data();
    column.pool = &pool;
    for (uint32_t i = 0; i < 20; ++i) {
      std::string value =
          base::StackString<16>("s%02u", static_cast<uint32_t>(rng() % 22))
              .ToStdString();
      std::vector<uint32_t> copy = index;
      Span<uint32_t> actual = SearchIndexEqual(
          column, CastFilterValueResult::Value(value.c_str()),
          Span<uint32_t>(copy.data(), copy.data() + copy.size()));
      uint32_t expected_b = 0;
      while (expected_b < size && text[index[expected_b]] < value) {
        ++expected_b;
      }
      uint32_t expected_e = expected_b;
      while (expected_e < size && text[index[expected_e]] == value) {
        ++expected_e;
      }
      ASSERT_EQ(actual.b - copy.data(), expected_b);
      ASSERT_EQ(actual.e - copy.data(), expected_e);
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::filter
