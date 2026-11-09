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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_HL_H_
#define SRC_SHARED_LIB_TRACK_EVENT_HL_H_

#include <cstdint>
#include <vector>

#include "perfetto/protozero/field.h"
#include "perfetto/public/abi/track_event_hl_abi.h"

// Internal helpers of the high level track event ABI implementation (hl.cc).

namespace perfetto::shlib {

// Like PerfettoTeHlEmitImpl(), but only emits the event on the data source
// instances in `instance_mask`, and appends `raw_fields` (serialized
// perfetto.protos.TrackEvent fields, see SerializeTeHlEventFields()) to it.
void TeHlEmit(struct PerfettoTeCategoryImpl* cat,
              int32_t type,
              const char* name,
              struct PerfettoTeHlExtra* const* extra_data,
              uint32_t instance_mask,
              protozero::ConstBytes raw_fields);

// Serializes `name` and `extra_data` as perfetto.protos.TrackEvent fields that
// don't depend on any sequence (without interning). Interned proto fields are
// not supported.
std::vector<uint8_t> SerializeTeHlEventFields(
    const char* name,
    struct PerfettoTeHlExtra* const* extra_data);

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_HL_H_
