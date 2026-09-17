/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/trace_processor/trace_blob.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/trace_processor/ref_counted.h"

namespace perfetto {
namespace trace_processor {
namespace {

void DeleteHeapBuf(void* ctx) {
  delete[] static_cast<uint8_t*>(ctx);
}

}  // namespace

// static
TraceBlob TraceBlob::Allocate(size_t size) {
  uint8_t* data = new uint8_t[size];
  PERFETTO_CHECK(data);
  return FromHeapBuf(data, size);
}

// static
TraceBlob TraceBlob::CopyFrom(const void* src, size_t size) {
  TraceBlob blob = Allocate(size);
  const uint8_t* src_u8 = static_cast<const uint8_t*>(src);
  std::copy(src_u8, src_u8 + size, blob.mutable_data());
  return blob;
}

// static
TraceBlob TraceBlob::TakeOwnership(std::unique_ptr<uint8_t[]> buf,
                                   size_t size) {
  PERFETTO_CHECK(buf);
  return FromHeapBuf(buf.release(), size);
}

// static
TraceBlob TraceBlob::FromMmap(base::ScopedMmap mapped) {
  PERFETTO_CHECK(mapped.IsValid());
  uint8_t* data = static_cast<uint8_t*>(mapped.data());
  size_t size = mapped.length();
  return Adopt(data, size, std::move(mapped));
}

// static
TraceBlob TraceBlob::Adopt(const uint8_t* data,
                           size_t size,
                           void* ctx,
                           Deleter deleter) {
  PERFETTO_CHECK(data);
  PERFETTO_CHECK(deleter);
  return TraceBlob(data, nullptr, size, ctx, deleter);
}

// static
TraceBlob TraceBlob::FromHeapBuf(uint8_t* buf, size_t size) {
  return TraceBlob(buf, buf, size, buf, &DeleteHeapBuf);
}

TraceBlob::TraceBlob(const uint8_t* data,
                     uint8_t* mutable_data,
                     size_t size,
                     void* ctx,
                     Deleter deleter)
    : data_(data),
      mutable_data_(mutable_data),
      size_(size),
      ctx_(ctx),
      deleter_(deleter) {}

uint8_t* TraceBlob::mutable_data() const {
  PERFETTO_DCHECK(mutable_data_);
  return mutable_data_;
}

TraceBlob::~TraceBlob() {
  if (deleter_)
    deleter_(ctx_);
}

TraceBlob::TraceBlob(TraceBlob&& other) noexcept
    : RefCounted(std::move(other)),
      data_(std::exchange(other.data_, nullptr)),
      mutable_data_(std::exchange(other.mutable_data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      ctx_(std::exchange(other.ctx_, nullptr)),
      deleter_(std::exchange(other.deleter_, nullptr)) {}

TraceBlob& TraceBlob::operator=(TraceBlob&& other) noexcept {
  if (this == &other)
    return *this;
  this->~TraceBlob();
  new (this) TraceBlob(std::move(other));
  return *this;
}

}  // namespace trace_processor
}  // namespace perfetto
