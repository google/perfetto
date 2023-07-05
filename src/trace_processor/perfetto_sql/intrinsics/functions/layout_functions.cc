// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/trace_processor/perfetto_sql/intrinsics/functions/layout_functions.h"

#include <queue>
#include <vector>
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {

namespace {

constexpr char kFunctionName[] = "INTERNAL_LAYOUT";

// A helper class for tracking which depths are available at a given time
// and which slices are occupying each depths.
class SlicePacker {
 public:
  SlicePacker() = default;

  // |dur| can be 0 for instant events and -1 for slices which do not end.
  base::Status AddSlice(int64_t ts, int64_t dur) {
    if (last_call_ == LastCall::kAddSlice) {
      return base::ErrStatus(R"(
Incorrect window clause (observed two consecutive calls to "step" function).
The window clause should be "rows between unbounded preceding and current row".
)");
    }
    last_call_ = LastCall::kAddSlice;
    if (ts < last_seen_ts_) {
      return base::ErrStatus(R"(
Passed slices are in incorrect order: %s requires timestamps to be sorted.
Please specify "ORDER BY ts" in the window clause.
)",
                             kFunctionName);
    }
    last_seen_ts_ = ts;
    ProcessPrecedingEvents(ts);
    // If the event is instant, do not mark this depth as occupied as it
    // becomes immediately available again.
    bool is_busy = dur != 0;
    size_t depth = SelectAvailableDepth(is_busy);
    // If the slice has an end and is not an instant, schedule this depth
    // to be marked available again when it ends.
    if (dur > 0) {
      slice_ends_.push({ts + dur, depth});
    }
    last_depth_ = depth;
    return base::OkStatus();
  }

  size_t GetLastDepth() {
    last_call_ = LastCall::kQuery;
    return last_depth_;
  }

 private:
  struct SliceEnd {
    int64_t ts;
    size_t depth;
  };

  struct SliceEndGreater {
    bool operator()(const SliceEnd& lhs, const SliceEnd& rhs) {
      return lhs.ts > rhs.ts;
    }
  };

  void ProcessPrecedingEvents(int64_t ts) {
    while (!slice_ends_.empty() && slice_ends_.top().ts <= ts) {
      is_depth_busy_[slice_ends_.top().depth] = false;
      slice_ends_.pop();
    }
  }

  size_t SelectAvailableDepth(bool new_state) {
    for (size_t i = 0; i < is_depth_busy_.size(); ++i) {
      if (!is_depth_busy_[i]) {
        is_depth_busy_[i] = new_state;
        return i;
      }
    }
    size_t depth = is_depth_busy_.size();
    is_depth_busy_.push_back(new_state);
    return depth;
  }

  enum class LastCall {
    kAddSlice,
    kQuery,
  };
  // The first call will be "add slice" and the calls are expected to
  // interleave, so set initial value to "query".
  LastCall last_call_ = LastCall::kQuery;

  int64_t last_seen_ts_ = 0;
  std::vector<bool> is_depth_busy_;
  // A list of currently open slices, ordered by end timestamp (ascending).
  std::priority_queue<SliceEnd, std::vector<SliceEnd>, SliceEndGreater>
      slice_ends_;
  size_t last_depth_ = 0;
};

base::StatusOr<SlicePacker*> GetOrCreateAggregationContext(
    sqlite3_context* ctx) {
  SlicePacker** packer = static_cast<SlicePacker**>(
      sqlite3_aggregate_context(ctx, sizeof(SlicePacker*)));
  if (!packer) {
    return base::ErrStatus("Failed to allocate aggregate context");
  }

  if (!*packer) {
    *packer = new SlicePacker();
  }
  return *packer;
}

base::Status Step(sqlite3_context* ctx, size_t argc, sqlite3_value** argv) {
  base::StatusOr<SlicePacker*> slice_packer =
      GetOrCreateAggregationContext(ctx);
  RETURN_IF_ERROR(slice_packer.status());

  base::StatusOr<SqlValue> ts =
      sqlite_utils::ExtractArgument(argc, argv, "ts", 0, SqlValue::kLong);
  RETURN_IF_ERROR(ts.status());

  base::StatusOr<SqlValue> dur =
      sqlite_utils::ExtractArgument(argc, argv, "dur", 1, SqlValue::kLong);
  RETURN_IF_ERROR(dur.status());

  return slice_packer.value()->AddSlice(ts->AsLong(), dur.value().AsLong());
}

void StepWrapper(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  PERFETTO_CHECK(argc >= 0);

  base::Status status = Step(ctx, static_cast<size_t>(argc), argv);
  if (!status.ok()) {
    sqlite_utils::SetSqliteError(ctx, kFunctionName, status);
    return;
  }
}

void FinalWrapper(sqlite3_context* ctx) {
  SlicePacker** slice_packer = static_cast<SlicePacker**>(
      sqlite3_aggregate_context(ctx, sizeof(SlicePacker*)));
  if (!slice_packer || !*slice_packer) {
    return;
  }
  sqlite3_result_int64(ctx,
                       static_cast<int64_t>((*slice_packer)->GetLastDepth()));
  delete *slice_packer;
}

void ValueWrapper(sqlite3_context* ctx) {
  base::StatusOr<SlicePacker*> slice_packer =
      GetOrCreateAggregationContext(ctx);
  if (!slice_packer.ok()) {
    sqlite_utils::SetSqliteError(ctx, kFunctionName, slice_packer.status());
    return;
  }
  sqlite3_result_int64(
      ctx, static_cast<int64_t>(slice_packer.value()->GetLastDepth()));
}

void InverseWrapper(sqlite3_context* ctx, int, sqlite3_value**) {
  sqlite_utils::SetSqliteError(ctx, kFunctionName, base::ErrStatus(R"(
The inverse step is not supported: the window clause should be
"BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW".
)"));
}

}  // namespace

base::Status LayoutFunctions::Register(sqlite3* db,
                                       TraceProcessorContext* context) {
  int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
  int ret = sqlite3_create_window_function(
      db, kFunctionName, 2, flags, context, StepWrapper, FinalWrapper,
      ValueWrapper, InverseWrapper, nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Unable to register function with name %s",
                           kFunctionName);
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
