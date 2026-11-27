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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_RAW_PROTO_FIELDS_H_
#define SRC_SHARED_LIB_TRACK_EVENT_RAW_PROTO_FIELDS_H_

#include <stddef.h>

namespace perfetto::shlib {

// Validates size budget (`size + extra_bytes`) and top-level protobuf framing
// in `data`. Length-delimited payloads are not inspected.
bool ValidateRawProtoFields(const void* data, size_t size, size_t extra_bytes);

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_RAW_PROTO_FIELDS_H_
