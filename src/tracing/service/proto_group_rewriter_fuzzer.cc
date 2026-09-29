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
#include <string.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "src/tracing/service/proto_group_rewriter.h"
#include "src/tracing/service/proto_group_rewriter_reference_for_testing.h"

namespace perfetto::tracing_v2 {
namespace {

// Checks arbitrary producer input:
// - No crash, and output only on kRewritten.
// - The same result and bytes as the reference rewriter.
// - The same again when the input is split into slices.
//
// The first input byte picks the split:
// - Bits 0-5: use slices of 1 to 64 bytes.
// - Bit 7: an empty slice after each slice.
//
// Short slices exercise reads and skips across slice boundaries.
int FuzzProtoGroupRewriter(const uint8_t* data, size_t size) {
  if (size == 0)
    return 0;
  const size_t step = 1 + (data[0] & 0x3f);
  const bool add_empty_slices = data[0] & 0x80;
  const std::vector<uint8_t> input(data + 1, data + size);

  std::vector<Slice> whole;
  if (!input.empty())
    whole.emplace_back(input.data(), input.size());
  std::vector<Slice> split;
  for (size_t begin = 0; begin < input.size(); begin += step) {
    split.emplace_back(input.data() + begin,
                       std::min(step, input.size() - begin));
    if (add_empty_slices)
      split.emplace_back(input.data() + begin, 0);
  }

  std::vector<uint8_t> expected;
  const RewriteResult expected_result =
      ReferenceRewriteForTesting(input, &expected);

  ProtoGroupRewriter rewriter;
  for (const std::vector<Slice>* slices : {&whole, &split}) {
    std::optional<Slice> output;
    const RewriteResult result = rewriter.Rewrite(*slices, &output);
    PERFETTO_CHECK(result == expected_result);
    PERFETTO_CHECK(output.has_value() == (result == RewriteResult::kRewritten));
    if (output) {
      PERFETTO_CHECK(output->size == expected.size());
      PERFETTO_CHECK(!memcmp(output->start, expected.data(), expected.size()));
    }
  }
  return 0;
}

}  // namespace
}  // namespace perfetto::tracing_v2

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return perfetto::tracing_v2::FuzzProtoGroupRewriter(data, size);
}
