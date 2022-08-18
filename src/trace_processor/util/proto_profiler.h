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

#ifndef SRC_TRACE_PROCESSOR_UTIL_PROTO_PROFILER_H_
#define SRC_TRACE_PROCESSOR_UTIL_PROTO_PROFILER_H_

#include <algorithm>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"

#include "perfetto/protozero/field.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto {
namespace trace_processor {
namespace util {

class SizeProfileComputer {
 public:
  using FieldPath = std::vector<std::string>;
  using SizeSamples = std::vector<size_t>;

  struct FieldPathHasher {
    using argument_type = std::vector<std::string>;
    using result_type = size_t;

    result_type operator()(const argument_type& p) const {
      size_t h = 0u;
      for (auto v : p)
        h ^= std::hash<std::string>{}(v);
      return h;
    }
  };

  explicit SizeProfileComputer(DescriptorPool* pool);

  // Returns a list of samples (i.e. all encountered field sizes) for each
  // field path in trace proto.
  // TODO(kraskevich): consider switching to internal DescriptorPool.
  base::FlatHashMap<FieldPath, SizeSamples, FieldPathHasher>
  Compute(const uint8_t* ptr, size_t size, const std::string& message_type);

 private:
  void ComputeInner(const uint8_t* ptr,
                    size_t size,
                    const std::string& message_type);
  void Sample(size_t size);
  size_t GetFieldSize(const protozero::Field& f);

  DescriptorPool* pool_;
  // The current 'stack' we're considering as we parse the protobuf.
  // For example if we're currently looking at the varint field baz which is
  // nested inside message Bar which is in turn a field named bar on the message
  // Foo. Then the stack would be: Foo, #bar, Bar, #baz, int
  // We keep track of both the field names (#bar, #baz) and the field types
  // (Foo, Bar, int) as sometimes we are intrested in which fields are big
  // and sometimes which types are big.
  std::vector<std::string> stack_;

  // Information about each field path seen.
  base::FlatHashMap<FieldPath, SizeSamples, FieldPathHasher> path_to_samples_;
};

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_PROTO_PROFILER_H_
