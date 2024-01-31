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

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {
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
  SqlValue val = SqlValue::String("pierogi");
  Range filter_range(0, 7);

  FilterOp op = FilterOp::kEq;
  auto res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  op = FilterOp::kNe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 5, 6));

  op = FilterOp::kLt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 5, 6));

  op = FilterOp::kLe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 6));

  op = FilterOp::kGt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kGe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  op = FilterOp::kIsNull;
  res = storage.Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kIsNotNull;
  res = storage.Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4, 5, 6));

  op = FilterOp::kGlob;
  res = storage.Search(op, SqlValue::String("p*"), filter_range);
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
  SqlValue val = SqlValue::String("pierogi");
  // "fries", "onion", "pierogi", NULL, "pizza", "pasta", "cheese"
  std::vector<uint32_t> indices{6, 5, 4, 3, 2, 1, 0};

  FilterOp op = FilterOp::kEq;
  auto res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kNe;
  res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 6));

  op = FilterOp::kLt;
  res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 5, 6));

  op = FilterOp::kLe;
  res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 5, 6));

  op = FilterOp::kGt;
  res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  op = FilterOp::kGe;
  res = storage.IndexSearch(op, val, indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  op = FilterOp::kIsNull;
  res = storage.IndexSearch(op, SqlValue(), indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kIsNotNull;
  res = storage.IndexSearch(op, SqlValue(), indices.data(), 7);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4, 5, 6));

  op = FilterOp::kGlob;
  res = storage.IndexSearch(op, SqlValue::String("p*"), indices.data(), 7);
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
  BitVector bv =
      storage.Search(FilterOp::kRegex, SqlValue::String(".*zz.*"), Range(0, 7))
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
  BitVector bv =
      storage.Search(FilterOp::kRegex, SqlValue::String("*"), Range(0, 7))
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
  SqlValue val = SqlValue::String("cheese");
  Range filter_range(0, 6);

  FilterOp op = FilterOp::kEq;
  auto res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kNe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3, 4, 5));

  op = FilterOp::kLt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1));

  op = FilterOp::kLe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));

  op = FilterOp::kGt;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));

  op = FilterOp::kGe;
  res = storage.Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 3, 4, 5));

  op = FilterOp::kGlob;
  res = storage.Search(op, SqlValue::String("*e"), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
