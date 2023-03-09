/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_PB_DECODER_H_
#define INCLUDE_PERFETTO_PUBLIC_PB_DECODER_H_

#include "perfetto/public/abi/pb_decoder_abi.h"

struct PerfettoPbDecoderIterator {
  struct PerfettoPbDecoder decoder;
  struct PerfettoPbDecoderField field;
};

static inline struct PerfettoPbDecoderIterator PerfettoPbDecoderIterateBegin(
    const uint8_t* start,
    const uint8_t* end) {
  struct PerfettoPbDecoderIterator ret;
  ret.decoder.read_ptr = start;
  ret.decoder.end_ptr = end;
  ret.field = PerfettoPbDecoderParseField(&ret.decoder);
  return ret;
}

static inline struct PerfettoPbDecoderIterator
PerfettoPbDecoderIterateNestedStart(
    struct PerfettoPbDecoderDelimitedField val) {
  struct PerfettoPbDecoderIterator ret;
  ret.decoder.read_ptr = val.start;
  ret.decoder.end_ptr = val.start + val.len;
  ret.field = PerfettoPbDecoderParseField(&ret.decoder);
  return ret;
}

static inline void PerfettoPbDecoderIterateNext(
    struct PerfettoPbDecoderIterator* iterator) {
  iterator->field = PerfettoPbDecoderParseField(&iterator->decoder);
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_DECODER_H_
