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

template <class D>
int CompareValues(const D& deque, size_t a, size_t b, bool desc) {
  const auto& first = deque[a];
  const auto& second = deque[b];
  if (first < second) {
    return desc ? 1 : -1;
  } else if (first > second) {
    return desc ? -1 : 1;
  }
  return 0;
}

// On MacOS size_t !== uint64_t
#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
template <typename F>
bool CompareToSqliteValue(size_t actual, sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<size_t>(sqlite3_value_int64(value)));
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

template <typename F>
bool CompareToSqliteValue(uint32_t actual, sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<uint32_t>(sqlite3_value_int64(value)));
}

template <typename F>
bool CompareToSqliteValue(uint64_t actual, sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<uint64_t>(sqlite3_value_int64(value)));
}

template <typename F>
bool CompareToSqliteValue(int64_t actual, sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return F()(actual, static_cast<int64_t>(sqlite3_value_int64(value)));
}

template <typename F>
bool CompareToSqliteValue(double actual, sqlite3_value* value) {
  auto type = sqlite3_value_type(value);
  PERFETTO_DCHECK(type == SQLITE_FLOAT || type == SQLITE_INTEGER);
  return F()(actual, sqlite3_value_double(value));
}

template <class D>
void FilterColumn(const D& deque,
                  size_t offset,
                  const QueryConstraints::Constraint& constraint,
                  sqlite3_value* argv,
                  std::vector<bool>* filter) {
  using T = typename D::value_type;

  auto it = std::find(filter->begin(), filter->end(), true);
  while (it != filter->end()) {
    auto filter_idx = static_cast<size_t>(std::distance(filter->begin(), it));
    T actual = deque[offset + filter_idx];
    switch (constraint.op) {
      case SQLITE_INDEX_CONSTRAINT_EQ:
        *it = CompareToSqliteValue<std::equal_to<T>>(actual, argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_GE:
        *it = CompareToSqliteValue<std::greater_equal<T>>(actual, argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_GT:
        *it = CompareToSqliteValue<std::greater<T>>(actual, argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_LE:
        *it = CompareToSqliteValue<std::less_equal<T>>(actual, argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_LT:
        *it = CompareToSqliteValue<std::less<T>>(actual, argv);
        break;
      case SQLITE_INDEX_CONSTRAINT_NE:
        *it = CompareToSqliteValue<std::not_equal_to<T>>(actual, argv);
        break;
      default:
        PERFETTO_CHECK(false);
    }
    it = std::find(it + 1, filter->end(), true);
  }
}

template <class Comparator>
std::vector<uint32_t> CreateSortedIndexFromFilter(
    uint32_t offset,
    const std::vector<bool>& filter,
    Comparator comparator) {
  auto set_bits = std::count(filter.begin(), filter.end(), true);

  std::vector<uint32_t> sorted_rows(static_cast<size_t>(set_bits));
  auto it = std::find(filter.begin(), filter.end(), true);
  for (size_t i = 0; it != filter.end(); i++) {
    auto filter_idx = static_cast<uint32_t>(std::distance(filter.begin(), it));
    sorted_rows[i] = offset + filter_idx;
    it = std::find(it + 1, filter.end(), true);
  }
  std::sort(sorted_rows.begin(), sorted_rows.end(), comparator);
  return sorted_rows;
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_UTILS_H_
