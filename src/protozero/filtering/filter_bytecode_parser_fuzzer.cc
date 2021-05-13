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

#include "src/protozero/filtering/filter_bytecode_parser.h"

namespace protozero {
namespace {

int FuzzBytecodeParser(const uint8_t* data, size_t size) {
  FilterBytecodeParser parser;
  parser.Load(data, size > 8 ? size - 8 : size);

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
