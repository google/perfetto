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
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

using Indices = DataLayerChain::Indices;
using OrderedIndices = DataLayerChain::OrderedIndices;

TEST(StringStorage, SearchOneElement) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  // 0:"cheese", 1:"pasta", 2:"pizza", 3:NULL, 4:"pierogi", 5:"onion", 6:"fries"
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::String("pierogi"), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::String("pierogi"), 3),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::String("foo"), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::String("pierogi"), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kNe, SqlValue::String("pierogi"), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::String("pierogi"), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::String("pierogi"), 1),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::String("pierogi"), 2),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGt, SqlValue::String("pierogi"), 5),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::String("pierogi"), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLe, SqlValue::String("pierogi"), 2),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::String("pierogi"), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kLt, SqlValue::String("pierogi"), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 4),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 4),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 3),
            SingleSearchResult::kNoMatch);

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGlob, SqlValue::String("p*"), 1),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGlob, SqlValue::String("p*"), 0),
            SingleSearchResult::kNoMatch);
}

TEST(StringStorage, Search) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  StringPool pool;
  ids.reserve(strings.size());
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::String("pierogi");
  Range filter_range(0, 7);

  FilterOp op = FilterOp::kEq;
  auto res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(4));

  op = FilterOp::kNe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 5, 6));

  op = FilterOp::kLt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 5, 6));

  op = FilterOp::kLe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 6));

  op = FilterOp::kGt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2));

  op = FilterOp::kGe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 4));

  op = FilterOp::kIsNull;
  res = chain->Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));

  op = FilterOp::kIsNotNull;
  res = chain->Search(op, SqlValue(), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4, 5, 6));

  op = FilterOp::kGlob;
  res = chain->Search(op, SqlValue::String("p*"), filter_range);
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
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::String("pierogi");
  // "fries", "onion", "pierogi", NULL, "pizza", "pasta", "cheese"
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {6, 5, 4, 3, 2, 1, 0}, Indices::State::kNonmonotonic);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 4, 5, 6));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 5, 6));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 5, 6));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(4));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2, 4));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kIsNull, SqlValue(), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices),
              ElementsAre(0, 1, 2, 4, 5, 6));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGlob, SqlValue::String("p*"), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2, 4, 5));
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
  auto chain = storage.MakeChain();
  BitVector bv =
      chain->Search(FilterOp::kRegex, SqlValue::String(".*zz.*"), Range(0, 7))
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
  auto chain = storage.MakeChain();
  BitVector bv =
      chain->Search(FilterOp::kRegex, SqlValue::String("*"), Range(0, 7))
          .TakeIfBitVector();

  ASSERT_EQ(bv.CountSetBits(), 0u);
}
#endif

TEST(StringStorage, SearchEmptyString) {
  std::vector<std::string> strings{"", "apple"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids, true);
  auto chain = storage.MakeChain();

  auto res = chain->Search(FilterOp::kEq, SqlValue::String(""), {0, 5});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3));
}

TEST(StringStorage, SearchEmptyStringIsNull) {
  std::vector<std::string> strings{"", "apple"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids, true);
  auto chain = storage.MakeChain();

  auto res = chain->Search(FilterOp::kIsNull, SqlValue(), {0, 5});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));
}

TEST(StringStorage, SearchSorted) {
  std::vector<std::string> strings{"apple",    "burger",   "cheese",
                                   "doughnut", "eggplant", "fries"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids, true);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::String("cheese");
  // 1:NULL, 2:NULL, 3:apple, 4:burger, 5:cheese, 6:doughnut, 7:eggplant,
  Range filter_range(1, 8);

  FilterOp op = FilterOp::kEq;
  auto res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5));

  op = FilterOp::kNe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 6, 7));

  op = FilterOp::kLt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));

  op = FilterOp::kLe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 5));

  op = FilterOp::kGt;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6, 7));

  op = FilterOp::kGe;
  res = chain->Search(op, val, filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(5, 6, 7));

  op = FilterOp::kGlob;
  res = chain->Search(op, SqlValue::String("*e"), filter_range);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 5));
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
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::String("cheese");
  Indices common_indices = Indices::CreateWithIndexPayloadForTesting(
      {5, 4, 2, 1}, Indices::State::kNonmonotonic);

  auto indices = common_indices;
  chain->IndexSearch(FilterOp::kEq, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kNe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kLe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2, 3));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGt, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGe, val, indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));

  indices = common_indices;
  chain->IndexSearch(FilterOp::kGlob, SqlValue::String("*e"), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(2));
}

TEST(StringStorage, OrderedIndexSearch) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids(1, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();
  SqlValue val = SqlValue::String("pierogi");
  std::vector<uint32_t> indices_vec{0, 6, 5, 2, 4, 3};
  OrderedIndices indices{indices_vec.data(), 6, Indices::State::kNonmonotonic};

  FilterOp op = FilterOp::kEq;
  Range res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 4u);
  ASSERT_EQ(res.end, 5u);

  op = FilterOp::kLt;
  res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 4u);

  op = FilterOp::kLe;
  res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 5u);

  op = FilterOp::kGt;
  res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 5u);
  ASSERT_EQ(res.end, 6u);

  op = FilterOp::kGe;
  res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 4u);
  ASSERT_EQ(res.end, 6u);

  op = FilterOp::kIsNull;
  val = SqlValue();
  res = chain->OrderedIndexSearch(op, val, indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 1u);
}

TEST(StringStorage, OrderedIndexSearchLowerBoundWithNulls) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();

  // NULL, NULL, cheese, pizza
  std::vector<uint32_t> indices_vec{0, 2, 3, 7};
  OrderedIndices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};
  auto res = chain->OrderedIndexSearch(FilterOp::kEq,
                                       SqlValue::String("cheese"), indices);
  ASSERT_EQ(res.start, 2u);
  ASSERT_EQ(res.end, 3u);
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
  auto chain = storage.MakeChain();

  std::vector<uint32_t> indices_vec{0, 2, 5, 7};
  OrderedIndices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};
  auto res = chain->OrderedIndexSearch(FilterOp::kIsNull, SqlValue(), indices);
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
  auto chain = storage.MakeChain();

  std::vector<uint32_t> indices_vec{0, 2, 5, 7};
  OrderedIndices indices{indices_vec.data(), 4, Indices::State::kNonmonotonic};
  auto res =
      chain->OrderedIndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 2u);
  ASSERT_EQ(res.end, 4u);
}

TEST(StringStorage, StableSort) {
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids(3, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();
  auto make_tokens = []() {
    return std::vector{
        Token{0, 0}, Token{1, 1}, Token{2, 2}, Token{3, 3}, Token{4, 4},
        Token{5, 5}, Token{6, 6}, Token{7, 7}, Token{8, 8},
    };
  };
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kAscending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(0, 1, 2, 3, 8, 7, 4, 6, 5));
  }
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      SortDirection::kDescending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(5, 6, 4, 7, 8, 3, 0, 1, 2));
  }
}

TEST(StringStorage, DistinctFromIndexVector) {
  std::vector<std::string> strings{"cheese", "pasta", "cheese",
                                   "fries",  "pasta", "fries"};
  std::vector<StringPool::Id> ids(2, StringPool::Id::Null());
  StringPool pool;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  StringStorage storage(&pool, &ids);
  auto chain = storage.MakeChain();

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {1, 0, 6, 3}, Indices::State::kNonmonotonic);
  chain->Distinct(indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::column
