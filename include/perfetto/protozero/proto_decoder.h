/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_
#define INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_

#include <stdint.h>
#include <array>
#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

// A generic protobuf decoder. Doesn't require any knowledge about the proto
// schema. It tokenizes fields, retrieves their ID and type and exposes
// accessors to retrieve its values.
// It does NOT recurse in nested submessages, instead it just computes their
// boundaries, recursion is left to the caller.
// This class is designed to be used in perf-sensitive contexts. It does not
// allocate and does not perform any proto semantic checks (e.g. repeated /
// required / optional). It's supposedly safe wrt out-of-bounds memory accesses
// (see proto_decoder_fuzzer.cc).
// This class serves also as a building block for TypedProtoDecoder, used when
// the schema is known at compile time.
class ProtoDecoder {
 public:
  // Creates a ProtoDecoder using the given |buffer| with size |length| bytes.
  inline ProtoDecoder(const uint8_t* buffer, size_t length)
      : begin_(buffer), end_(buffer + length), read_ptr_(buffer) {}

  // Reads the next field from the buffer and advances the read cursor. If a
  // full field cannot be read, the returned Field will be invalid (i.e.
  // field.valid() == false).
  Field ReadField();

  // Finds the first field with the given id. Doesn't affect the read cursor.
  Field FindField(uint32_t field_id);

  // Resets the read cursor to the start of the buffer.
  inline void Reset() { read_ptr_ = begin_; }

  // Resets the read cursor to the given position (must be within the buffer).
  inline void Reset(const uint8_t* pos) {
    PERFETTO_DCHECK(pos >= begin_ && pos < end_);
    read_ptr_ = pos;
  }

  // Returns the position of read cursor, relative to the start of the buffer.
  inline size_t read_offset() const {
    return static_cast<size_t>(read_ptr_ - begin_);
  }

  inline size_t bytes_left() const {
    PERFETTO_DCHECK(read_ptr_ <= end_);
    return static_cast<size_t>(end_ - read_ptr_);
  }

  const uint8_t* begin() const { return begin_; }
  const uint8_t* end() const { return end_; }

 protected:
  const uint8_t* const begin_;
  const uint8_t* const end_;
  const uint8_t* read_ptr_ = nullptr;
};

// An iterator-like class used to iterate through repeated fields. Used by
// TypedProtoDecoder. The iteration sequence is a bit counter-intuitive due to
// the fact that fields_[field_id] holds the *last* value of the field, not the
// first, but the remaining storage holds repeated fields in FIFO order.
// Assume that we push the 10,11,12 into a repeated field with ID=1.
//
// Decoder memory layout:  [  fields storage  ] [ repeated fields storage]
// 1st iteration:           10
// 2nd iteration:           11                   10
// 3rd iteration:           12                   10 11
//
// We start the iteration @ fields_[num_fields], which is the start of the
// repeated fields storage, proceed until the end and lastly jump @ fields_[id].
class RepeatedFieldIterator {
 public:
  RepeatedFieldIterator(uint32_t field_id,
                        const Field* begin,
                        const Field* end,
                        const Field* last)
      : field_id_(field_id), iter_(begin), end_(end), last_(last) {
    FindNextMatchingId();
  }

  inline const Field* operator->() const { return &*iter_; }
  inline const Field& operator*() const { return *iter_; }
  inline explicit operator bool() const { return iter_ != end_; }

  RepeatedFieldIterator& operator++() {
    PERFETTO_DCHECK(iter_ != end_);
    if (iter_ == last_) {
      iter_ = end_;
      return *this;
    }
    ++iter_;
    FindNextMatchingId();
    return *this;
  }

 private:
  inline void FindNextMatchingId() {
    PERFETTO_DCHECK(iter_ != last_);
    for (; iter_ != end_; ++iter_) {
      if (iter_->id() == field_id_)
        return;
    }
    iter_ = last_->valid() ? last_ : end_;
  }

  uint32_t field_id_;

  // Initially points to the beginning of the repeated field storage, then is
  // incremented as we call operator++().
  const Field* iter_;

  // Always points to fields_[size_], i.e. past the end of the storage.
  const Field* end_;

  // Always points to fields_[field_id].
  const Field* last_;
};

// This decoder loads all fields upfront, without recursing in nested messages.
// It is used as a base class for typed decoders generated by the pbzero plugin.
// The split between TypedProtoDecoderBase and TypedProtoDecoder<> is to have
// unique definition of functions like ParseAllFields() and ExpandHeapStorage().
// The storage (either on-stack or on-heap) for this class is organized as
// follows:
// |-------------------------- fields_ ----------------------|
// [ field 0 (invalid) ] [ fields 1 .. N ] [ repeated fields ]
//                                        ^                  ^
//                                        num_fields_        size_
class TypedProtoDecoderBase : public ProtoDecoder {
 public:
  // If the field |id| is known at compile time, prefer the templated
  // specialization at<kFieldNumber>().
  inline const Field& Get(uint32_t id) {
    return PERFETTO_LIKELY(id < num_fields_) ? fields_[id] : fields_[0];
  }

  // Returns an object that allows to iterate over all instances of a repeated
  // field given its id. Example usage:
  // for (auto it = decoder.GetRepeated(N); it; ++it) { ... }
  inline RepeatedFieldIterator GetRepeated(uint32_t field_id) const {
    return RepeatedFieldIterator(field_id, &fields_[num_fields_],
                                 &fields_[size_], &fields_[field_id]);
  }

 protected:
  TypedProtoDecoderBase(Field* storage,
                        uint32_t num_fields,
                        uint32_t capacity,
                        const uint8_t* buffer,
                        size_t length)
      : ProtoDecoder(buffer, length),
        fields_(storage),
        num_fields_(num_fields),
        size_(num_fields),
        capacity_(capacity) {
    // The reason why Field needs to be trivially de/constructible is to avoid
    // implicit initializers on all the ~1000 entries. We need it to initialize
    // only on the first |max_field_id| fields, the remaining capacity doesn't
    // require initialization.
    static_assert(std::is_trivially_constructible<Field>::value &&
                      std::is_trivially_destructible<Field>::value &&
                      std::is_trivial<Field>::value,
                  "Field must be a trivial aggregate type");
    memset(fields_, 0, sizeof(Field) * num_fields_);
  }

  void ParseAllFields();

  // Called when the default on-stack storage is exhausted and new repeated
  // fields need to be pushed.
  void ExpandHeapStorage();

  // Used only in presence of a large number of repeated fields, when the
  // default on-stack storage is exhausted.
  std::unique_ptr<Field[]> heap_storage_;

  // Points to the storage, either on-stack (default, provided by the template
  // specialization) or |heap_storage_| after ExpandHeapStorage() is called, in
  // case of a large number of repeated fields.
  Field* fields_;

  // Number of fields without accounting repeated storage. This is equal to
  // MAX_FIELD_ID + 1 (to account for the invalid 0th field).
  // This value is always <= size_ (and hence <= capacity);
  uint32_t num_fields_;

  // Number of active |fields_| entries. This is initially equal to the highest
  // number of fields for the message (num_fields_ == MAX_FIELD_ID + 1) and can
  // grow up to |capacity_| in the case of repeated fields.
  uint32_t size_;

  // Initially equal to kFieldsCapacity of the TypedProtoDecoder
  // specialization. Can grow when falling back on heap-based storage, in which
  // case it represents the size (#fields with each entry of a repeated field
  // counted individually) of the |heap_storage_| array.
  uint32_t capacity_;
};

// Template class instantiated by the auto-generated decoder classes declared in
// xxx.pbzero.h files.
template <int MAX_FIELD_ID, bool HAS_REPEATED_FIELDS>
class TypedProtoDecoder : public TypedProtoDecoderBase {
 public:
  TypedProtoDecoder(const uint8_t* buffer, size_t length)
      : TypedProtoDecoderBase(on_stack_storage_,
                              /*num_fields=*/MAX_FIELD_ID + 1,
                              kCapacity,
                              buffer,
                              length) {
    static_assert(MAX_FIELD_ID <= kMaxDecoderFieldId, "Field ordinal too high");
    TypedProtoDecoderBase::ParseAllFields();
  }

  template <uint32_t FIELD_ID>
  inline const Field& at() const {
    static_assert(FIELD_ID <= MAX_FIELD_ID, "FIELD_ID > MAX_FIELD_ID");
    return fields_[FIELD_ID];
  }

 private:
  // In the case of non-repeated fields, this constant defines the highest field
  // id we are able to decode. This is to limit the on-stack storage.
  // In the case of repeated fields, this constant defines the max number of
  // repeated fields that we'll be able to store before falling back on the
  // heap. Keep this value in sync with the one in protozero_generator.cc.
  static constexpr size_t kMaxDecoderFieldId = 999;

  // If we the message has no repeated fields we need at most N Field entries
  // in the on-stack storage, where N is the highest field id.
  // Otherwise we need some room to store repeated fields.
  static constexpr size_t kCapacity =
      1 + (HAS_REPEATED_FIELDS ? kMaxDecoderFieldId : MAX_FIELD_ID);

  Field on_stack_storage_[kCapacity];
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_
