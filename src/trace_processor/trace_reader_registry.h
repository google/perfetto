/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_READER_REGISTRY_H_
#define SRC_TRACE_PROCESSOR_TRACE_READER_REGISTRY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

class ChunkedTraceReader;
class TraceProcessorContext;

// Maps `TraceType` values to `ChunkedTraceReader` subclasses.
// This class is used to create `ChunkedTraceReader` instances for a given
// `TraceType`.
class TraceReaderRegistry {
 public:
  TraceReaderRegistry() = default;

  // Registers a mapping from `TraceType` value to `ChunkedTraceReader`
  // subclass. Only one such mapping can be registered per `TraceType` value.
  template <typename Reader>
  void RegisterTraceReader(TraceType trace_type) {
    RegisterFactory(trace_type, [](TraceProcessorContext* ctxt, uint32_t) {
      return std::make_unique<Reader>(ctxt);
    });
  }

  // Like RegisterTraceReader, but for readers whose constructor also takes the
  // trace_file_table id of the file being read (e.g. to use as a clock owner).
  template <typename Reader>
  void RegisterTraceReaderWithFileId(TraceType trace_type) {
    RegisterFactory(trace_type,
                    [](TraceProcessorContext* ctxt, uint32_t file_id) {
                      return std::make_unique<Reader>(ctxt, file_id);
                    });
  }

  // Registers a trace reader factory that captures its own state (e.g. a
  // plugin's `this` pointer). The TraceProcessorContext* passed at creation
  // time is ignored by the wrapper.
  void RegisterPluginTraceReader(
      TraceType trace_type,
      std::function<std::unique_ptr<ChunkedTraceReader>()> factory);

  // Creates a new `ChunkedTraceReader` instance for the given `type`,
  // `file_id` being the trace_file_table id of the file being read. Returns
  // an error if no mapping has been previously registered.
  base::StatusOr<std::unique_ptr<ChunkedTraceReader>> CreateTraceReader(
      TraceType type,
      TraceProcessorContext* context,
      uint32_t file_id);

 private:
  using Factory = std::function<std::unique_ptr<ChunkedTraceReader>(
      TraceProcessorContext*,
      uint32_t)>;
  void RegisterFactory(TraceType trace_type, Factory factory);

  base::FlatHashMap<TraceType, Factory> factories_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TRACE_READER_REGISTRY_H_
