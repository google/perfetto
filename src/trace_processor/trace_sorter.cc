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
#include <utility>

#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/trace_sorter.h"

namespace perfetto {
namespace trace_processor {

namespace {

inline void MoveToTraceParser(ProtoTraceParser* proto_parser,
                              TraceSorter::EventsMap::iterator* it) {
  if ((*it)->second.is_ftrace) {
    proto_parser->ParseFtracePacket((*it)->second.cpu,
                                    (*it)->first /*timestamp*/,
                                    std::move((*it)->second.blob_view));
  } else {
    proto_parser->ParseTracePacket(std::move((*it)->second.blob_view));
  }
}

}  // namespace

TraceSorter::TraceSorter(TraceProcessorContext* context,
                         uint64_t window_size_ns)
    : context_(context), window_size_ns_(window_size_ns){};

void TraceSorter::PushTracePacket(uint64_t timestamp,
                                  TraceBlobView trace_view) {
  TimestampedTracePiece ttp(
      std::move(trace_view), false /* is_ftrace */,
      0 /* cpu - this field should never be used for non-ftrace packets */);
  events_.emplace(timestamp, std::move(ttp));
  MaybeFlushEvents();
}

void TraceSorter::PushFtracePacket(uint32_t cpu,
                                   uint64_t timestamp,
                                   TraceBlobView trace_view) {
  TimestampedTracePiece ttp(std::move(trace_view), true /* is_ftrace */, cpu);
  events_.emplace(timestamp, std::move(ttp));
  MaybeFlushEvents();
}

void TraceSorter::MaybeFlushEvents() {
  if (events_.empty())
    return;
  uint64_t most_recent_timestamp = events_.rbegin()->first;
  auto it = events_.begin();
  for (; it != events_.end(); it++) {
    uint64_t cur_timestamp = it->first;

    // Only flush if there is an event older than the window size or
    // if we are force flushing.
    if (most_recent_timestamp - cur_timestamp < window_size_ns_)
      break;
    MoveToTraceParser(context_->proto_parser.get(), &it);
  }
  events_.erase(events_.begin(), it);
}

void TraceSorter::FlushEventsForced() {
  for (auto it = events_.begin(); it != events_.end(); it++) {
    MoveToTraceParser(context_->proto_parser.get(), &it);
  }
  events_.clear();
}

}  // namespace trace_processor
}  // namespace perfetto
