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
#include "src/trace_processor/db/storage/string_storage.h"

#include "src/trace_processor/db/storage/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using Range = RowMap::Range;

TEST(StringStorageUnittest, LinearSearchEq) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kEq, SqlValue::String("pizza"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 1u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 2u);
}

TEST(StringStorageUnittest, LinearSearchNe) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kNe, SqlValue::String("pizza"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 5u);
}

TEST(StringStorageUnittest, LinearSearchLe) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kLe, SqlValue::String("noodles"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 2u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 0u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 5u);
}

TEST(StringStorageUnittest, LinearSearchLt) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kLt, SqlValue::String("pasta"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 3u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 0u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 4u);
  ASSERT_EQ(bv.IndexOfNthSet(2), 5u);
}

TEST(StringStorageUnittest, LinearSearchGe) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kGe, SqlValue::String("noodles"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 4u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 1u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 2u);
  ASSERT_EQ(bv.IndexOfNthSet(2), 3u);
  ASSERT_EQ(bv.IndexOfNthSet(3), 4u);
}

TEST(StringStorageUnittest, LinearSearchGt) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kGt, SqlValue::String("pasta"), Range(0, 6))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 2u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 2u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 3u);
}

TEST(StringStorageUnittest, LinearSearchIsNull) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 7);
  BitVector bv =
      storage.Search(FilterOp::kIsNull, SqlValue::String("pasta"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 1u);
}

TEST(StringStorageUnittest, LinearSearchIsNotNull) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage
          .Search(FilterOp::kIsNotNull, SqlValue::String("pasta"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 6u);
}

TEST(StringStorageUnittest, LinearSearchGlob) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kGlob, SqlValue::String("p*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 3u);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST(StringStorageUnittest, LinearSearchRegex) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kRegex, SqlValue::String(".*zz.*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 1u);
}

TEST(StringStorageUnittest, LinearSearchRegexMalformed) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 6);
  BitVector bv =
      storage.Search(FilterOp::kRegex, SqlValue::String("*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 0u);
}
#endif

TEST(StringStorageUnittest, IndexSearchEq) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());

  StringStorage storage(&pool, ids.data(), 7);
  std::vector<uint32_t> indices{6, 5, 4, 3, 2, 1, 0};
  BitVector bv = storage
                     .IndexSearch(FilterOp::kEq, SqlValue::String("pasta"),
                                  indices.data(), 7)
                     .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 1u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 5u);
}

}  // namespace
}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
