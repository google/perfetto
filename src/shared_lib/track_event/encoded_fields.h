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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_ENCODED_FIELDS_H_
#define SRC_SHARED_LIB_TRACK_EVENT_ENCODED_FIELDS_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/ext/base/small_vector.h"
#include "perfetto/public/abi/track_event_hl_abi.h"

namespace perfetto::shlib {

// A nested message containing IID slots. The original length-prefix width is
// retained in output; only the payload shrinks when IIDs are compacted.
struct EncodedFieldLength {
  size_t offset;
  size_t width;
  uint32_t payload_size;
  size_t first_slot;
  size_t end_slot;
};
using EncodedFieldLengths = base::SmallVector<EncodedFieldLength, 8>;

// extra_bytes accounts for the event name in the shared transport budget.
// If supplied, lengths receives prefixes in increasing source-offset order.
bool ValidateEncodedFields(const PerfettoTeHlEventFields* fields,
                           size_t extra_bytes,
                           EncodedFieldLengths* lengths = nullptr);
size_t WriteEncodedIid(uint64_t iid,
                       uint8_t destination[PERFETTO_TE_HL_IID_SLOT_SIZE]);

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_ENCODED_FIELDS_H_
