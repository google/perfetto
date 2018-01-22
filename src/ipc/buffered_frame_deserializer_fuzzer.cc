/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/base/utils.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/ipc/wire_protocol.pb.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  perfetto::ipc::BufferedFrameDeserializer bfd;
  auto rbuf = bfd.BeginReceive();
  memcpy(rbuf.data, data, size);
  ::perfetto::base::ignore_result(bfd.EndReceive(size));
  // TODO(fmayer): Determine if this has value.
  // This slows down fuzzing from 190k / s to 140k / sec.
  while (bfd.PopNextFrame() != nullptr) {
  }
  return 0;
}
