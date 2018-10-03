/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_UTILS_H_
#define SRC_TRACE_PROCESSOR_SQLITE_UTILS_H_

#include <sqlite3.h>
#include <algorithm>
#include <deque>
#include <iterator>

#include "perfetto/base/logging.h"
#include "src/trace_processor/query_constraints.h"

namespace perfetto {
namespace trace_processor {
namespace sqlite_utils {

inline bool IsOpEq(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_EQ;
}

inline bool IsOpGe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GE;
}

inline bool IsOpGt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GT;
}

inline bool IsOpLe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LE;
}

inline bool IsOpLt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LT;
}

inline std::string OpToString(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return "=";
    case SQLITE_INDEX_CONSTRAINT_NE:
      return "!=";
    case SQLITE_INDEX_CONSTRAINT_GE:
      return ">=";
    case SQLITE_INDEX_CONSTRAINT_GT:
      return ">";
    case SQLITE_INDEX_CONSTRAINT_LE:
      return "<=";
    case SQLITE_INDEX_CONSTRAINT_LT:
      return "<";
    default:
      PERFETTO_FATAL("Operator to string conversion not impemented for %d", op);
  }
}

template <typename F>
bool Compare(uint32_t actual, sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<uint32_t>(sqlite3_value_int64(value)));
}

template <typename F>
bool Compare(uint64_t actual, sqlite3_value* value) {
  PERFETTO_CHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<uint64_t>(sqlite3_value_int64(value)));
}

template <class RandomAccessIterator>
void FilterColumn(RandomAccessIterator begin,
                  RandomAccessIterator end,
                  const QueryConstraints::Constraint& constraint,
                  sqlite3_value* argv,
                  std::vector<bool>* row_filter) {
  using T = typename RandomAccessIterator::value_type;
  PERFETTO_DCHECK(static_cast<size_t>(std::distance(begin, end)) ==
                  row_filter->size());

  auto it = std::find(row_filter->begin(), row_filter->end(), true);
  while (it != row_filter->end()) {
    auto index = std::distance(row_filter->begin(), it);
    switch (constraint.op) {
      case SQLITE_INDEX_CONSTRAINT_EQ:
        *it = Compare<std::equal_to<T>>(begin[index], argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_GE:
        *it = Compare<std::greater_equal<T>>(begin[index], argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_GT:
        *it = Compare<std::greater<T>>(begin[index], argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_LE:
        *it = Compare<std::less_equal<T>>(begin[index], argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_LT:
        *it = Compare<std::less<T>>(begin[index], argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_NE:
        *it = Compare<std::not_equal_to<T>>(begin[index], argv);
        break;
      default:
        PERFETTO_CHECK(false);
    }
    it = std::find(it + 1, row_filter->end(), true);
  }
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_UTILS_H_
