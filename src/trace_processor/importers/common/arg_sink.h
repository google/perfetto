/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARG_SINK_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARG_SINK_H_

#include <cstddef>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

// The key of one arg. |flat_key| is the path without array indices; |key|
// includes them. The two are equal when no array is on the path.
struct ArgKey {
  StringId flat_key;
  StringId key;
};

// Receives args produced by a parser, e.g. ProtoToArgsParser or the JSON args
// parser.
class ArgSink {
 public:
  virtual ~ArgSink();

  virtual void Add(ArgKey key, Variadic value) = 0;

  // Array indices under |key| are tracked by the sink so that arrays built
  // across several parses into the same sink continue each other.
  virtual size_t ArrayIndex(StringId) {
    PERFETTO_FATAL("Arrays not supported by this sink");
  }
  virtual size_t IncrementArrayIndex(StringId) {
    PERFETTO_FATAL("Arrays not supported by this sink");
  }

  // Whether a default value should be added for |key| when parsing with
  // defaults.
  virtual bool WantsDefault(ArgKey) { return true; }
};

// Writes args into an ArgsInserter.
class ArgsInserterSink : public ArgSink {
 public:
  explicit ArgsInserterSink(ArgsInserter& inserter) : inserter_(inserter) {}
  ~ArgsInserterSink() override;

  void Add(ArgKey key, Variadic value) override {
    inserter_.AddArg(key.flat_key, key.key, value);
  }
  size_t ArrayIndex(StringId key) override {
    return inserter_.GetNextArrayEntryIndex(key);
  }
  size_t IncrementArrayIndex(StringId key) override {
    return inserter_.IncrementArrayEntryIndex(key);
  }

 private:
  ArgsInserter& inserter_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARG_SINK_H_
