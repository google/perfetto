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

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/proto_rewriter.h"

namespace perfetto::tracing_v2 {
namespace {

// The bytes come from a producer, so the rewriter has to survive anything. The
// first input byte picks the output bound, so that small inputs reach the
// size-limit paths as well as the malformed ones.
int FuzzProtoRewriter(const uint8_t* data, size_t size) {
  if (size == 0)
    return 0;
  const size_t max_output_size = size_t{data[0]} * 4;
  std::vector<uint8_t> output;
  const RewriteResult result = RewriteProtoGroupToLengthDelimited(
      data + 1, data + size, &output, max_output_size);
  PERFETTO_CHECK(output.size() <= max_output_size);
  PERFETTO_CHECK(result == RewriteResult::kSuccess || output.empty());
  return 0;
}

}  // namespace
}  // namespace perfetto::tracing_v2

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return perfetto::tracing_v2::FuzzProtoRewriter(data, size);
}
