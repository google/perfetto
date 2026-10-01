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

#ifndef SRC_TRACE_PROCESSOR_UTIL_DECOMPRESSOR_H_
#define SRC_TRACE_PROCESSOR_UTIL_DECOMPRESSOR_H_

#include "src/base/decompressor.h"

namespace perfetto::trace_processor::util {

using base::CompressionCodecInfo;
using base::CompressionType;
using base::CreateDecompressor;
using base::DecompressedBuffer;
using base::Decompressor;
using base::DecompressToBuffer;
using base::FrameMode;
using base::GetCompressionCodecInfo;
using base::IsCompressionSupported;
using base::IsGzipSupported;
using base::IsZstdSupported;

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_DECOMPRESSOR_H_
