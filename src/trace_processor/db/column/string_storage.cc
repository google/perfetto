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

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/glob.h"
#include "src/trace_processor/util/regex.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor::column {

namespace {

struct Greater {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return lhs != StringPool::Id::Null() && pool_->Get(lhs) > rhs;
  }
  const StringPool* pool_;
};

struct GreaterEqual {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return lhs != StringPool::Id::Null() && pool_->Get(lhs) >= rhs;
  }
  const StringPool* pool_;
};

struct Less {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return lhs != StringPool::Id::Null() && pool_->Get(lhs) < rhs;
  }
  const StringPool* pool_;
};

struct LessEqual {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return lhs != StringPool::Id::Null() && pool_->Get(lhs) <= rhs;
  }
  const StringPool* pool_;
};

struct NotEqual {
  bool operator()(StringPool::Id lhs, StringPool::Id rhs) {
    return lhs != StringPool::Id::Null() && lhs != rhs;
  }
};

struct Glob {
  bool operator()(StringPool::Id lhs, util::GlobMatcher& matcher) const {
    return lhs != StringPool::Id::Null() && matcher.Matches(pool_->Get(lhs));
  }
  const StringPool* pool_;
};

struct GlobFullStringPool {
  GlobFullStringPool(StringPool* pool, util::GlobMatcher& matcher)
      : pool_(pool), matches_(pool->MaxSmallStringId().raw_id()) {
    PERFETTO_DCHECK(!pool->HasLargeString());
    for (auto it = pool->CreateIterator(); it; ++it) {
      auto id = it.StringId();
      matches_[id.raw_id()] = matcher.Matches(pool->Get(id));
    }
  }
  bool operator()(StringPool::Id lhs, StringPool::Id) {
    return lhs != StringPool::Id::Null() && matches_[lhs.raw_id()];
  }
  StringPool* pool_;
  std::vector<uint8_t> matches_;
};

struct Regex {
  bool operator()(StringPool::Id lhs, regex::Regex& pattern) const {
    return lhs != StringPool::Id::Null() &&
           pattern.Search(pool_->Get(lhs).c_str());
  }
  const StringPool* pool_;
};

struct RegexFullStringPool {
  RegexFullStringPool(StringPool* pool, const regex::Regex& regex)
      : pool_(pool), matches_(pool->MaxSmallStringId().raw_id()) {
    PERFETTO_DCHECK(!pool->HasLargeString());
    for (auto it = pool->CreateIterator(); it; ++it) {
      auto id = it.StringId();
      matches_[id.raw_id()] =
          id != StringPool::Id::Null() && regex.Search(pool_->Get(id).c_str());
    }
  }
  bool operator()(StringPool::Id lhs, StringPool::Id) {
    return matches_[lhs.raw_id()];
  }
  StringPool* pool_;
  std::vector<uint8_t> matches_;
};

struct IsNull {
  bool operator()(StringPool::Id lhs, StringPool::Id) const {
    return lhs == StringPool::Id::Null();
  }
};

struct IsNotNull {
  bool operator()(StringPool::Id lhs, StringPool::Id) const {
    return lhs != StringPool::Id::Null();
  }
};

uint32_t LowerBoundIntrinsic(StringPool* pool,
                             const StringPool::Id* data,
                             NullTermStringView val,
                             Range search_range) {
  Less comp{pool};
  const auto* lower =
      std::lower_bound(data + search_range.start, data + search_range.end, val,
                       [comp](StringPool::Id id, NullTermStringView val) {
                         return comp(id, val);
                       });
  return static_cast<uint32_t>(std::distance(data, lower));
}

uint32_t UpperBoundIntrinsic(StringPool* pool,
                             const StringPool::Id* data,
                             NullTermStringView val,
                             Range search_range) {
  Greater comp{pool};
  const auto* upper =
      std::upper_bound(data + search_range.start, data + search_range.end, val,
                       [comp](NullTermStringView val, StringPool::Id id) {
                         return comp(id, val);
                       });
  return static_cast<uint32_t>(std::distance(data, upper));
}

uint32_t LowerBoundExtrinsic(StringPool* pool,
                             const StringPool::Id* data,
                             NullTermStringView val,
                             const uint32_t* indices,
                             uint32_t indices_count) {
  Less comp{pool};
  const auto* lower =
      std::lower_bound(indices, indices + indices_count, val,
                       [comp, data](uint32_t index, NullTermStringView val) {
                         return comp(data[index], val);
                       });
  return static_cast<uint32_t>(std::distance(indices, lower));
}

uint32_t UpperBoundExtrinsic(StringPool* pool,
                             const StringPool::Id* data,
                             NullTermStringView val,
                             const uint32_t* indices,
                             uint32_t indices_count) {
  Greater comp{pool};
  const auto* upper =
      std::upper_bound(indices, indices + indices_count, val,
                       [comp, data](NullTermStringView val, uint32_t index) {
                         return comp(data[index], val);
                       });
  return static_cast<uint32_t>(std::distance(indices, upper));
}

}  // namespace

StringStorage::StringStorage(StringPool* string_pool,
                             const std::vector<StringPool::Id>* data,
                             bool is_sorted)
    : data_(data), string_pool_(string_pool), is_sorted_(is_sorted) {}

StringStorage::ChainImpl::ChainImpl(StringPool* string_pool,
                                    const std::vector<StringPool::Id>* data,
                                    bool is_sorted)
    : data_(data), string_pool_(string_pool), is_sorted_(is_sorted) {}

SingleSearchResult StringStorage::ChainImpl::SingleSearch(FilterOp op,
                                                          SqlValue sql_val,
                                                          uint32_t i) const {
  if (sql_val.type == SqlValue::kNull) {
    if (op == FilterOp::kIsNull) {
      return IsNull()((*data_)[i], StringPool::Id::Null())
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    }
    if (op == FilterOp::kIsNotNull) {
      return IsNotNull()((*data_)[i], StringPool::Id::Null())
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    }
    return SingleSearchResult::kNeedsFullSearch;
  }

  if (sql_val.type != SqlValue::kString) {
    return SingleSearchResult::kNeedsFullSearch;
  }

  switch (op) {
    case FilterOp::kEq: {
      std::optional<StringPool::Id> id =
          string_pool_->GetId(base::StringView(sql_val.string_value));
      return id && std::equal_to<>()((*data_)[i], *id)
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    }
    case FilterOp::kNe: {
      std::optional<StringPool::Id> id =
          string_pool_->GetId(base::StringView(sql_val.string_value));
      return !id || NotEqual()((*data_)[i], *id) ? SingleSearchResult::kMatch
                                                 : SingleSearchResult::kNoMatch;
    }
    case FilterOp::kGe:
      return GreaterEqual{string_pool_}(
                 (*data_)[i], NullTermStringView(sql_val.string_value))
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    case FilterOp::kGt:
      return Greater{string_pool_}((*data_)[i],
                                   NullTermStringView(sql_val.string_value))
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    case FilterOp::kLe:
      return LessEqual{string_pool_}((*data_)[i],
                                     NullTermStringView(sql_val.string_value))
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    case FilterOp::kLt:
      return Less{string_pool_}((*data_)[i],
                                NullTermStringView(sql_val.string_value))
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    case FilterOp::kGlob: {
      util::GlobMatcher matcher =
          util::GlobMatcher::FromPattern(sql_val.string_value);
      return Glob{string_pool_}((*data_)[i], matcher)
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    }
    case FilterOp::kRegex: {
      // Caller should ensure that the regex is valid.
      base::StatusOr<regex::Regex> regex =
          regex::Regex::Create(sql_val.AsString());
      PERFETTO_CHECK(regex.status().ok());
      return Regex{string_pool_}((*data_)[i], regex.value())
                 ? SingleSearchResult::kMatch
                 : SingleSearchResult::kNoMatch;
    }
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Already handled above");
  }
  PERFETTO_FATAL("For GCC");
}

SearchValidationResult StringStorage::ChainImpl::ValidateSearchConstraints(
    FilterOp op,
    SqlValue val) const {
  // Type checks.
  switch (val.type) {
    case SqlValue::kNull:
    case SqlValue::kString:
      break;
    case SqlValue::kLong:
    case SqlValue::kDouble:
      // Any string is always more than any numeric.
      if (op == FilterOp::kGt || op == FilterOp::kGe) {
        return SearchValidationResult::kAllData;
      }
      return SearchValidationResult::kNoData;
    case SqlValue::kBytes:
      return SearchValidationResult::kNoData;
  }

  return SearchValidationResult::kOk;
}

RangeOrBitVector StringStorage::ChainImpl::SearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Range search_range) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "StringStorage::ChainImpl::Search",
                    [&search_range, op](metatrace::Record* r) {
                      r->AddArg("Start", std::to_string(search_range.start));
                      r->AddArg("End", std::to_string(search_range.end));
                      r->AddArg("Op",
                                std::to_string(static_cast<uint32_t>(op)));
                    });

  if (is_sorted_) {
    switch (op) {
      case FilterOp::kEq:
      case FilterOp::kGe:
      case FilterOp::kGt:
      case FilterOp::kLe:
      case FilterOp::kLt:
        return RangeOrBitVector(
            BinarySearchIntrinsic(op, sql_val, search_range));
      case FilterOp::kNe: {
        // Not equal is a special operation on binary search, as it doesn't
        // define a range, and rather just `not` range returned with `equal`
        // operation.
        Range r = BinarySearchIntrinsic(FilterOp::kEq, sql_val, search_range);
        BitVector bv(r.start, true);
        bv.Resize(r.end);
        bv.Resize(search_range.end, true);
        return RangeOrBitVector(std::move(bv));
      }
      case FilterOp::kGlob:
      case FilterOp::kRegex:
      case FilterOp::kIsNull:
      case FilterOp::kIsNotNull:
        // Those operations can't be binary searched so we fall back on not
        // sorted algorithm.
        break;
    }
  }
  return RangeOrBitVector(LinearSearch(op, sql_val, search_range));
}

RangeOrBitVector StringStorage::ChainImpl::IndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  PERFETTO_DCHECK(indices.size <= size());
  PERFETTO_TP_TRACE(
      metatrace::Category::DB, "StringStorage::ChainImpl::IndexSearch",
      [indices, op](metatrace::Record* r) {
        r->AddArg("Count", std::to_string(indices.size));
        r->AddArg("Op", std::to_string(static_cast<uint32_t>(op)));
      });
  return RangeOrBitVector(
      IndexSearchInternal(op, sql_val, indices.data, indices.size));
}

BitVector StringStorage::ChainImpl::LinearSearch(FilterOp op,
                                                 SqlValue sql_val,
                                                 Range range) const {
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));

  const StringPool::Id* start = data_->data() + range.start;

  BitVector::Builder builder(range.end, range.start);
  switch (op) {
    case FilterOp::kEq:
      utils::LinearSearchWithComparator(val, start, std::equal_to<>(), builder);
      break;
    case FilterOp::kNe:
      utils::LinearSearchWithComparator(val, start, NotEqual(), builder);
      break;
    case FilterOp::kLe:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        LessEqual{string_pool_}, builder);
      break;
    case FilterOp::kLt:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        Less{string_pool_}, builder);
      break;
    case FilterOp::kGt:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        Greater{string_pool_}, builder);
      break;
    case FilterOp::kGe:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        GreaterEqual{string_pool_}, builder);
      break;
    case FilterOp::kGlob: {
      util::GlobMatcher matcher =
          util::GlobMatcher::FromPattern(sql_val.AsString());

      // If glob pattern doesn't involve any special characters, the function
      // called should be equality.
      if (matcher.IsEquality()) {
        utils::LinearSearchWithComparator(val, start, std::equal_to<>(),
                                          builder);
        break;
      }

      // For very big string pools (or small ranges) or pools with large strings
      // run a standard glob function.
      if (range.size() < string_pool_->size() ||
          string_pool_->HasLargeString()) {
        utils::LinearSearchWithComparator(std::move(matcher), start,
                                          Glob{string_pool_}, builder);
        break;
      }

      utils::LinearSearchWithComparator(
          StringPool::Id::Null(), start,
          GlobFullStringPool{string_pool_, matcher}, builder);
      break;
    }
    case FilterOp::kRegex: {
      // Caller should ensure that the regex is valid.
      base::StatusOr<regex::Regex> regex =
          regex::Regex::Create(sql_val.AsString());
      PERFETTO_CHECK(regex.status().ok());

      // For very big string pools (or small ranges) or pools with large
      // strings run a standard regex function.
      if (range.size() < string_pool_->size() ||
          string_pool_->HasLargeString()) {
        utils::LinearSearchWithComparator(std::move(regex.value()), start,
                                          Regex{string_pool_}, builder);
        break;
      }
      utils::LinearSearchWithComparator(
          StringPool::Id::Null(), start,
          RegexFullStringPool{string_pool_, regex.value()}, builder);
      break;
    }
    case FilterOp::kIsNull:
      utils::LinearSearchWithComparator(val, start, IsNull(), builder);
      break;
    case FilterOp::kIsNotNull:
      utils::LinearSearchWithComparator(val, start, IsNotNull(), builder);
  }

  return std::move(builder).Build();
}

Range StringStorage::ChainImpl::OrderedIndexSearchValidated(
    FilterOp op,
    SqlValue sql_val,
    Indices indices) const {
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));
  NullTermStringView val_str = string_pool_->Get(val);

  switch (op) {
    case FilterOp::kEq:
      return {LowerBoundExtrinsic(string_pool_, data_->data(), val_str,
                                  indices.data, indices.size),
              UpperBoundExtrinsic(string_pool_, data_->data(), val_str,
                                  indices.data, indices.size)};
    case FilterOp::kLe:
      return {0, UpperBoundExtrinsic(string_pool_, data_->data(), val_str,
                                     indices.data, indices.size)};
    case FilterOp::kLt:
      return {0, LowerBoundExtrinsic(string_pool_, data_->data(), val_str,
                                     indices.data, indices.size)};
    case FilterOp::kGe:
      return {LowerBoundExtrinsic(string_pool_, data_->data(), val_str,
                                  indices.data, indices.size),
              indices.size};
    case FilterOp::kGt:
      return {UpperBoundExtrinsic(string_pool_, data_->data(), val_str,
                                  indices.data, indices.size),
              indices.size};
    case FilterOp::kIsNull: {
      // Assuming nulls are at the front.
      IsNull comp;
      const auto* first_non_null = std::partition_point(
          indices.data, indices.data + indices.size, [comp, this](uint32_t i) {
            return comp((*data_)[i], StringPool::Id::Null());
          });
      return Range(0, static_cast<uint32_t>(
                          std::distance(indices.data, first_non_null)));
    }
    case FilterOp::kIsNotNull: {
      // Assuming nulls are at the front.
      IsNull comp;
      const auto* first_non_null = std::partition_point(
          indices.data, indices.data + indices.size, [comp, this](uint32_t i) {
            return comp((*data_)[i], StringPool::Id::Null());
          });
      return Range(
          static_cast<uint32_t>(std::distance(indices.data, first_non_null)),
          indices.size);
    }

    case FilterOp::kNe:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Not supported for OrderedIndexSearch");
  }
  PERFETTO_FATAL("For GCC");
}

RangeOrBitVector StringStorage::ChainImpl::IndexSearchInternal(
    FilterOp op,
    SqlValue sql_val,
    const uint32_t* indices,
    uint32_t indices_size) const {
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));
  const StringPool::Id* start = data_->data();

  BitVector::Builder builder(indices_size);

  switch (op) {
    case FilterOp::kEq:
      utils::IndexSearchWithComparator(val, start, indices, std::equal_to<>(),
                                       builder);
      break;
    case FilterOp::kNe:
      utils::IndexSearchWithComparator(val, start, indices, NotEqual(),
                                       builder);
      break;
    case FilterOp::kLe:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       LessEqual{string_pool_}, builder);
      break;
    case FilterOp::kLt:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       Less{string_pool_}, builder);
      break;
    case FilterOp::kGt:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       Greater{string_pool_}, builder);
      break;
    case FilterOp::kGe:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       GreaterEqual{string_pool_}, builder);
      break;
    case FilterOp::kGlob: {
      util::GlobMatcher matcher =
          util::GlobMatcher::FromPattern(sql_val.AsString());
      if (matcher.IsEquality()) {
        utils::IndexSearchWithComparator(val, start, indices, std::equal_to<>(),
                                         builder);
        break;
      }
      utils::IndexSearchWithComparator(std::move(matcher), start, indices,
                                       Glob{string_pool_}, builder);
      break;
    }
    case FilterOp::kRegex: {
      base::StatusOr<regex::Regex> regex =
          regex::Regex::Create(sql_val.AsString());
      utils::IndexSearchWithComparator(std::move(regex.value()), start, indices,
                                       Regex{string_pool_}, builder);
      break;
    }
    case FilterOp::kIsNull:
      utils::IndexSearchWithComparator(val, start, indices, IsNull(), builder);
      break;
    case FilterOp::kIsNotNull:
      utils::IndexSearchWithComparator(val, start, indices, IsNotNull(),
                                       builder);
      break;
  }

  return RangeOrBitVector(std::move(builder).Build());
}

Range StringStorage::ChainImpl::BinarySearchIntrinsic(
    FilterOp op,
    SqlValue sql_val,
    Range search_range) const {
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));
  NullTermStringView val_str = string_pool_->Get(val);

  switch (op) {
    case FilterOp::kEq:
      return {LowerBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range),
              UpperBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range)};
    case FilterOp::kLe:
      return {search_range.start,
              UpperBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range)};
    case FilterOp::kLt:
      return {search_range.start,
              LowerBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range)};
    case FilterOp::kGe:
      return {LowerBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range),
              search_range.end};
    case FilterOp::kGt:
      return {UpperBoundIntrinsic(string_pool_, data_->data(), val_str,
                                  search_range),
              search_range.end};
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
    case FilterOp::kRegex:
      PERFETTO_FATAL("Shouldn't be called");
  }
  PERFETTO_FATAL("For GCC");
}

void StringStorage::ChainImpl::StableSort(SortToken* start,
                                          SortToken* end,
                                          SortDirection direction) const {
  switch (direction) {
    case SortDirection::kAscending:
      std::stable_sort(start, end,
                       [this](const SortToken& a, const SortToken& b) {
                         return string_pool_->Get((*data_)[a.index]) <
                                string_pool_->Get((*data_)[b.index]);
                       });
      return;
    case SortDirection::kDescending:
      std::stable_sort(start, end,
                       [this](const SortToken& a, const SortToken& b) {
                         return string_pool_->Get((*data_)[a.index]) >
                                string_pool_->Get((*data_)[b.index]);
                       });
      return;
  }
  PERFETTO_FATAL("For GCC");
}

void StringStorage::ChainImpl::Serialize(StorageProto* msg) const {
  auto* string_storage_msg = msg->set_string_storage();
  string_storage_msg->set_is_sorted(is_sorted_);

  string_storage_msg->set_values(
      reinterpret_cast<const uint8_t*>(data_->data()),
      sizeof(StringPool::Id) * size());
}

}  // namespace perfetto::trace_processor::column
