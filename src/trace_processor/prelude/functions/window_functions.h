/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_WINDOW_FUNCTIONS_H_
#define SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_WINDOW_FUNCTIONS_H_

#include <sqlite3.h>
#include <unordered_map>
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/trace_processor/demangle.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "src/trace_processor/export_json.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/prelude/functions/create_function_internal.h"
#include "src/trace_processor/util/status_macros.h"

#include "src/trace_processor/prelude/functions/register_function.h"

namespace perfetto {
namespace trace_processor {
// Keeps track of the latest non null value and its position withing the
// window. Every time the window shrinks (`xInverse` is called) the window size
// is reduced by one and the position of the value moves one back, if it gets
// out of the window the value is discarded.
class LastNonNullAggregateContext {
 public:
  static LastNonNullAggregateContext* Get(sqlite3_context* ctx) {
    return reinterpret_cast<LastNonNullAggregateContext*>(
        sqlite3_aggregate_context(ctx, 0));
  }

  static LastNonNullAggregateContext* GetOrCreate(sqlite3_context* ctx) {
    return reinterpret_cast<LastNonNullAggregateContext*>(
        sqlite3_aggregate_context(ctx, sizeof(LastNonNullAggregateContext)));
  }

  inline void PopFront() {
    PERFETTO_CHECK(window_size_ > 0);
    --window_size_;
    if (!last_non_null_value_) {
      return;
    }
    if (value_index_ == 0) {
      sqlite3_value_free(last_non_null_value_);
      last_non_null_value_ = nullptr;
      return;
    }
    PERFETTO_CHECK(value_index_ > 0);
    --value_index_;
  }

  inline void PushBack(sqlite3_value* value) {
    ++window_size_;
    if (sqlite3_value_type(value) == SQLITE_NULL) {
      return;
    }

    Destroy();
    last_non_null_value_ = sqlite3_value_dup(value);
    value_index_ = window_size_ - 1;
  }

  inline void Destroy() {
    if (last_non_null_value_) {
      sqlite3_value_free(last_non_null_value_);
    }
  }

  sqlite3_value* last_non_null_value() const { return last_non_null_value_; }

 private:
  int64_t window_size_;
  // Index within the window of the last non null value. Only valid if `value`
  // is set.
  int64_t value_index_;
  // Actual value
  sqlite3_value* last_non_null_value_;
};

static_assert(std::is_standard_layout<LastNonNullAggregateContext>::value,
              "Must be able to be initialized by sqlite3_aggregate_context "
              "(similar to calloc, i.e. no constructor called)");
static_assert(std::is_trivial<LastNonNullAggregateContext>::value,
              "Must be able to be destroyed by just calling free (i.e. no "
              "destructor called)");

inline void LastNonNullStep(sqlite3_context* ctx,
                            int argc,
                            sqlite3_value** argv) {
  if (argc != 1) {
    sqlite3_result_error(
        ctx, "Unsupported number of args passed to LAST_NON_NULL", -1);
    return;
  }

  auto* ptr = LastNonNullAggregateContext::GetOrCreate(ctx);
  if (!ptr) {
    sqlite3_result_error(ctx, "LAST_NON_NULL: Failed to allocate context", -1);
    return;
  }

  ptr->PushBack(argv[0]);
}

inline void LastNonNullInverse(sqlite3_context* ctx, int, sqlite3_value**) {
  auto* ptr = LastNonNullAggregateContext::GetOrCreate(ctx);
  PERFETTO_CHECK(ptr != nullptr);
  ptr->PopFront();
}

inline void LastNonNullValue(sqlite3_context* ctx) {
  auto* ptr = LastNonNullAggregateContext::GetOrCreate(ctx);
  if (!ptr || !ptr->last_non_null_value()) {
    sqlite3_result_null(ctx);
  } else {
    sqlite3_result_value(ctx, ptr->last_non_null_value());
  }
}

inline void LastNonNullFinal(sqlite3_context* ctx) {
  auto* ptr = LastNonNullAggregateContext::Get(ctx);
  if (!ptr || !ptr->last_non_null_value()) {
    sqlite3_result_null(ctx);
  } else {
    sqlite3_result_value(ctx, ptr->last_non_null_value());
    ptr->Destroy();
  }
}

inline void RegisterLastNonNullFunction(sqlite3* db) {
  auto ret = sqlite3_create_window_function(
      db, "LAST_NON_NULL", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
      &LastNonNullStep, &LastNonNullFinal, &LastNonNullValue,
      &LastNonNullInverse, nullptr);
  if (ret) {
    PERFETTO_ELOG("Error initializing LAST_NON_NULL");
  }
}
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_WINDOW_FUNCTIONS_H_
