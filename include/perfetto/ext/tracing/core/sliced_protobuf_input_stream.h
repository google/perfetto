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

#ifndef INCLUDE_PERFETTO_EXT_TRACING_CORE_SLICED_PROTOBUF_INPUT_STREAM_H_
#define INCLUDE_PERFETTO_EXT_TRACING_CORE_SLICED_PROTOBUF_INPUT_STREAM_H_

#include "perfetto/ext/tracing/core/slice.h"

#include <stdint.h>
#include <utility>

#include <google/protobuf/io/zero_copy_stream.h>

#include "perfetto/base/export.h"

namespace perfetto {

using ZeroCopyInputStream = google::protobuf::io::ZeroCopyInputStream;

// Wraps a sequence of Slice(s) in a protobuf ZeroCopyInputStream that can be
// passed to protobuf::Message::ParseFromZeroCopyStream().
class PERFETTO_EXPORT SlicedProtobufInputStream : public ZeroCopyInputStream {
 public:
  // This indirection deals with the fact that the public protobuf library and
  // the internal one diverged on this type. The internal doesn's use a custom
  // defined type. The public one uses a typedef that isn't compatible with
  // stdint's int64_t (long long vs long). So insted of trying to use
  // google::protobuf::int64, infer the type from the return value of the
  // ByteCount().
  using int64 = decltype(std::declval<ZeroCopyInputStream>().ByteCount());

  explicit SlicedProtobufInputStream(const Slices*);
  ~SlicedProtobufInputStream() override;

  // ZeroCopyInputStream implementation. See zero_copy_stream.h for the API
  // contract of the methods below.
  bool Next(const void** data, int* size) override;
  void BackUp(int count) override;
  bool Skip(int count) override;
  int64 ByteCount() const override;

 private:
  bool Validate() const;

  const Slices* const slices_;
  Slices::const_iterator cur_slice_;
  size_t pos_in_cur_slice_ = 0;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_TRACING_CORE_SLICED_PROTOBUF_INPUT_STREAM_H_
