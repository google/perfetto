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

#include <functional>
#include <string>

#include "perfetto/base/logging.h"

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

template <class T>
std::function<bool(T, T)> GetPredicateForOp(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return std::equal_to<T>();
    case SQLITE_INDEX_CONSTRAINT_GE:
      return std::greater_equal<T>();
    case SQLITE_INDEX_CONSTRAINT_GT:
      return std::greater<T>();
    case SQLITE_INDEX_CONSTRAINT_LE:
      return std::less_equal<T>();
    case SQLITE_INDEX_CONSTRAINT_LT:
      return std::less<T>();
    case SQLITE_INDEX_CONSTRAINT_NE:
      return std::not_equal_to<T>();
    default:
      PERFETTO_CHECK(false);
  }
}

template <typename T>
T ExtractSqliteValue(sqlite3_value* value);

template <>
inline uint32_t ExtractSqliteValue(sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return static_cast<uint32_t>(sqlite3_value_int64(value));
}

template <>
inline uint64_t ExtractSqliteValue(sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return static_cast<uint64_t>(sqlite3_value_int64(value));
}

template <>
inline int64_t ExtractSqliteValue(sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return static_cast<int64_t>(sqlite3_value_int64(value));
}

template <>
inline double ExtractSqliteValue(sqlite3_value* value) {
  auto type = sqlite3_value_type(value);
  PERFETTO_DCHECK(type == SQLITE_FLOAT || type == SQLITE_INTEGER);
  return sqlite3_value_double(value);
}

// On MacOS size_t !== uint64_t
#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
template <>
inline size_t ExtractSqliteValue(sqlite3_value* value) {
  PERFETTO_DCHECK(sqlite3_value_type(value) == SQLITE_INTEGER);
  return static_cast<size_t>(sqlite3_value_int64(value));
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

template <typename T>
void ReportSqliteResult(sqlite3_context*, T value);

template <>
inline void ReportSqliteResult(sqlite3_context* ctx, int32_t value) {
  sqlite3_result_int(ctx, value);
}

template <>
inline void ReportSqliteResult(sqlite3_context* ctx, int64_t value) {
  sqlite3_result_int64(ctx, value);
}

template <>
inline void ReportSqliteResult(sqlite3_context* ctx, uint32_t value) {
  sqlite3_result_int64(ctx, value);
}

template <>
inline void ReportSqliteResult(sqlite3_context* ctx, uint64_t value) {
  sqlite3_result_int64(ctx, static_cast<sqlite_int64>(value));
}

template <>
inline void ReportSqliteResult(sqlite3_context* ctx, double value) {
  sqlite3_result_double(ctx, value);
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_UTILS_H_
