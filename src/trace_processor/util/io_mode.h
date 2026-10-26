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

#ifndef SRC_TRACE_PROCESSOR_UTIL_IO_MODE_H_
#define SRC_TRACE_PROCESSOR_UTIL_IO_MODE_H_

#include "perfetto/base/build_config.h"
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {

// How trace processor reads its input and backs its bulk working memory.
enum class IoMode {
  // Inputs are mmap'd and working memory may be file-backed.
  kNative,
  // Only the heap: what the UI's WASM build has. Native builds emulate it
  // with the "io=wasm" dev flag, so its time and memory can be measured
  // without a browser.
  kWasm,
};

inline IoMode GetIoMode(const Config& config) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  base::ignore_result(config);
  return IoMode::kWasm;
#else
  if (config.enable_dev_features) {
    auto it = config.dev_flags.find("io");
    if (it != config.dev_flags.end() && it->second == "wasm") {
      return IoMode::kWasm;
    }
  }
  return IoMode::kNative;
#endif
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_IO_MODE_H_
