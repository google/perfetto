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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "perfetto/public/abi/track_event_hl_abi.h"

namespace perfetto::shlib {

// Internal (not part of the ABI) extra type: already serialized
// perfetto.protos.TrackEvent fields, appended verbatim to the event.
inline constexpr uint32_t kTeHlExtraTypeRawFields = 0x10000;
struct TeHlExtraRawFields {
  struct PerfettoTeHlExtra header;
  const void* data;
  size_t size;
};

// Emits an event (see PerfettoTeHlEmitImpl()) only on the data source instances
// in `instance_mask`.
void TeHlEmit(struct PerfettoTeCategoryImpl* cat,
              int32_t type,
              const char* name,
              struct PerfettoTeHlExtra* const* extra_data,
              uint32_t instance_mask);

// Serializes `name` and `extra_data` as perfetto.protos.TrackEvent fields that
// don't depend on any sequence (no interning), to be emitted later with a
// TeHlExtraRawFields extra.
std::vector<uint8_t> SerializeTeHlEventFields(
    const char* name,
    struct PerfettoTeHlExtra* const* extra_data);

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_HL_H_
