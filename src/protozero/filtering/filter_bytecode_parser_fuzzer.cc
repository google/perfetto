/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/hash.h"

#include "perfetto/protozero/packed_repeated_fields.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"

namespace protozero {
namespace {

// This function gives a little help to the fuzzer. The bytecode is really a
// sequence of varint-encoded uint32 words, with a FNV1a checksum at the end.
// It's very unlikely that the fuzzer on its own can work out the checksum, so
// most fuzzer inputs are doomed to fail the checksum verification.
// This takes the fuzzer input and builds a more plausible bytecode.
void LoadBytecodeWithChecksum(FilterBytecodeParser* parser,
                              const uint8_t* data,
                              size_t size) {
  protozero::PackedVarInt words;
  perfetto::base::Hasher hasher;
  for (size_t i = 0; i < size; i += sizeof(uint32_t)) {
    uint32_t word = 0;
    memcpy(&word, data, sizeof(uint32_t));
    words.Append(word);
    hasher.Update(word);
  }
  words.Append(static_cast<uint32_t>(hasher.digest()));
  parser->Load(words.data(), words.size());
}

int FuzzBytecodeParser(const uint8_t* data, size_t size) {
  FilterBytecodeParser parser;
  parser.set_suppress_logs_for_fuzzer(true);

  if (size > 4 && data[0] < 192) {
    // 75% of the times use the LoadBytecodeWithChecksum() which helps the
    // fuzzer passing the checksum verification.
    LoadBytecodeWithChecksum(&parser, data + 1, size - 1);
  } else {
    // In the remaining 25%, pass completely arbitrary inputs.
    parser.Load(data, size);
  }

  // Smoke testing with known problematic values
  for (uint32_t msg_index = 0; msg_index < 3; ++msg_index) {
    parser.Query(msg_index, 0);
    parser.Query(msg_index, 1);
    parser.Query(msg_index, 127);
    parser.Query(msg_index, 128);
    parser.Query(msg_index, 129);
    parser.Query(msg_index, 65536);
    parser.Query(msg_index, 65536);
    parser.Query(msg_index, 1u << 28);
    parser.Query(msg_index, 1u << 31);
  }

  // Query using the random data at the end of the random buffer.
  if (size > 8) {
    uint32_t msg_index = 0;
    uint32_t field_id = 0;
    memcpy(&msg_index, &data[size - 8], 4);
    memcpy(&field_id, &data[size - 4], 4);
    parser.Query(msg_index, field_id);
  }

  return 0;
}

}  // namespace
}  // namespace protozero

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return protozero::FuzzBytecodeParser(data, size);
}
