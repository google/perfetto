/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/args_tracker.h"

namespace perfetto {
namespace trace_processor {

ArgsTracker::ArgsTracker(TraceProcessorContext* context) : context_(context) {}

void ArgsTracker::AddArg(RowId row_id,
                         StringId flat_key,
                         StringId key,
                         Variadic value) {
  args_.emplace_back();

  auto* rid_arg = &args_.back();
  rid_arg->row_id = row_id;
  rid_arg->flat_key = flat_key;
  rid_arg->key = key;
  rid_arg->value = value;
}

void ArgsTracker::Flush() {
  using Arg = TraceStorage::Args::Arg;

  // We sort here because a single packet may add multiple args with different
  // rowids.
  auto comparator = [](const Arg& f, const Arg& s) {
    return f.row_id < s.row_id;
  };
  std::sort(args_.begin(), args_.end(), comparator);

  auto* storage = context_->storage.get();
  for (uint32_t i = 0; i < args_.size();) {
    const auto& args = args_[i];
    RowId rid = args.row_id;

    uint32_t next_rid_idx = i + 1;
    while (next_rid_idx < args_.size() && rid == args_[next_rid_idx].row_id)
      next_rid_idx++;

    auto set_id = storage->mutable_args()->AddArgSet(args_, i, next_rid_idx);
    auto pair = TraceStorage::ParseRowId(rid);
    switch (pair.first) {
      case TableId::kRawEvents:
        storage->mutable_raw_events()->set_arg_set_id(pair.second, set_id);
        break;
      case TableId::kCounters:
        storage->mutable_counters()->set_arg_set_id(pair.second, set_id);
        break;
      default:
        PERFETTO_FATAL("Unsupported table to insert args into");
    }
    i = next_rid_idx;
  }
  args_.clear();
}

}  // namespace trace_processor
}  // namespace perfetto
