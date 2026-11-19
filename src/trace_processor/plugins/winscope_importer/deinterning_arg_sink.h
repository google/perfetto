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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_DEINTERNING_ARG_SINK_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_DEINTERNING_ARG_SINK_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor::winscope {

// Wraps a sink and resolves interned strings: an integer arg whose key ends in
// "_iid" is replaced by the string it refers to, under the key without the
// suffix. If the string cannot be resolved, an error string is added under
// that key and the integer is kept.
class DeinterningArgSink : public ArgSink {
 public:
  // Returns the string |iid| refers to for the arg |key|, or nullopt.
  using Lookup =
      std::function<std::optional<base::StringView>(ArgKey key, uint64_t iid)>;

  DeinterningArgSink(ArgSink& inner, StringPool& pool, Lookup lookup);
  ~DeinterningArgSink() override;

  void Add(ArgKey key, Variadic value) override;
  size_t ArrayIndex(StringPool::Id key) override {
    return inner_.ArrayIndex(key);
  }
  size_t IncrementArrayIndex(StringPool::Id key) override {
    return inner_.IncrementArrayIndex(key);
  }
  bool WantsDefault(ArgKey key) override { return inner_.WantsDefault(key); }

 private:
  ArgSink& inner_;
  StringPool& pool_;
  Lookup lookup_;
};

}  // namespace perfetto::trace_processor::winscope

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_DEINTERNING_ARG_SINK_H_
