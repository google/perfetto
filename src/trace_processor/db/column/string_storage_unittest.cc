/*
 * Copyright (C) 2023 The Android Open Source Project
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
#include "src/trace_processor/db/column/string_storage.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(StringStorage, Search) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();
  SqlValue val = SqlValue::String("pierogi");
  Range filter_range(0, 7);

  FilterOp op = FilterOp::kEq;
  auto res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  op = FilterOp::kNe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 5, 6));

  op = FilterOp::kLt;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 5, 6));

  op = FilterOp::kLe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 6));

  op = FilterOp::kGt;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kGe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  op = FilterOp::kIsNull;
  res = queriable->Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kIsNotNull;
  res = queriable->Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4, 5, 6));

  op = FilterOp::kGlob;
  res = queriable->Search(op, SqlValue::String("p*"), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 2, 4));
}

TEST(StringStorage, IndexSearch) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();
  SqlValue val = SqlValue::String("pierogi");
  // "fries", "onion", "pierogi", NULL, "pizza", "pasta", "cheese"
  std::vector<uint32_t> indices_vec{6, 5, 4, 3, 2, 1, 0};
  Indices indices{indices_vec.data(), 7, Indices::State::kNonmonotonic};

  FilterOp op = FilterOp::kEq;
  auto res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kNe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 6));

  op = FilterOp::kLt;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 5, 6));

  op = FilterOp::kLe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 5, 6));

  op = FilterOp::kGt;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  op = FilterOp::kGe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  op = FilterOp::kIsNull;
  res = queriable->IndexSearch(op, SqlValue(), indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kIsNotNull;
  res = queriable->IndexSearch(op, SqlValue(), indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4, 5, 6));

  op = FilterOp::kGlob;
  res = queriable->IndexSearch(op, SqlValue::String("p*"), indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4, 5));
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST(StringStorage, LinearSearchRegex) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();
  BitVector bv =
      queriable
          ->Search(FilterOp::kRegex, SqlValue::String(".*zz.*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 1u);
}

TEST(StringStorage, LinearSearchRegexMalformed) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();
  BitVector bv =
      queriable->Search(FilterOp::kRegex, SqlValue::String("*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 0u);
}
#endif

TEST(StringStorage, SearchSorted) {
  std::vector<std::string> strings{"apple",    "burger",   "cheese",
                                   "doughnut", "eggplant", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids, true);
  auto queriable = storage.MakeQueryable();
  SqlValue val = SqlValue::String("cheese");
  Range filter_range(0, 6);

  FilterOp op = FilterOp::kEq;
  auto res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kNe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3, 4, 5));

  op = FilterOp::kLt;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  op = FilterOp::kLe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));

  op = FilterOp::kGt;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));

  op = FilterOp::kGe;
  res = queriable->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3, 4, 5));

  op = FilterOp::kGlob;
  res = queriable->Search(op, SqlValue::String("*e"), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

TEST(StringStorage, IndexSearchSorted) {
  std::vector<std::string> strings{"apple",    "burger",   "cheese",
                                   "doughnut", "eggplant", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids, true);
  auto queriable = storage.MakeQueryable();
  SqlValue val = SqlValue::String("cheese");
  // fries, eggplant, cheese, burger
  std::vector<uint32_t> indices_vec{5, 4, 2, 1};
  Indices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};

  FilterOp op = FilterOp::kEq;
  auto res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kNe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3));

  op = FilterOp::kLt;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kLe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3));

  op = FilterOp::kGt;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  op = FilterOp::kGe;
  res = queriable->IndexSearch(op, val, indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));

  op = FilterOp::kGlob;
  res = queriable->IndexSearch(op, SqlValue::String("*e"), indices);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));
}

TEST(StringStorage, OrderedIndexSearch) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();
  SqlValue val = SqlValue::String("pierogi");
  // cheese, fries, onion, pasta, pierogi, pizza
  std::vector<uint32_t> indices_vec{0, 5, 4, 1, 3, 2};
  Indices indices{indices_vec.data(), 6, Indices::State::kNonmonotonic};

  FilterOp op = FilterOp::kEq;
  Range res = queriable->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 4u);
  ASSERT_EQ(res.end, 5u);

  op = FilterOp::kLt;
  res = queriable->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 4u);

  op = FilterOp::kLe;
  res = queriable->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 5u);

  op = FilterOp::kGt;
  res = queriable->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 5u);
  ASSERT_EQ(res.end, 6u);

  op = FilterOp::kGe;
  res = queriable->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 4u);
  ASSERT_EQ(res.end, 6u);
}

TEST(StringStorage, OrderedIndexSearchIsNull) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();

  std::vector<uint32_t> indices_vec{0, 2, 5, 7};
  Indices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};
  auto res =
      queriable->OrderedIndexSearch(FilterOp::kIsNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 2u);
}

TEST(StringStorage, OrderedIndexSearchIsNotNull) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto queriable = storage.MakeQueryable();

  std::vector<uint32_t> indices_vec{0, 2, 5, 7};
  Indices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};
  auto res =
      queriable->OrderedIndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 2u);
  ASSERT_EQ(res.end, 4u);
}

}  // namespace
}  // namespace perfetto::trace_processor::column
